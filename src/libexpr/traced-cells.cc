#include "nix/expr/traced-cells.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/store/content-address.hh"
#include "nix/util/hash.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/store/store-api.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/util/strings.hh"
#include "nix/util/terminal.hh"

#include <nlohmann/json.hpp>

#include <pthread.h>

#include <algorithm>
#include <bit>
#include <chrono>

namespace nix {

namespace {

/**
 * External value carrying a function port, the first argument of
 * `__portCall` in a proxy function.
 */
struct PortRef : ExternalValueBase, gc
{
    ExprPort * port;

    explicit PortRef(ExprPort * port)
        : port(port)
    {
    }

    std::ostream & print(std::ostream & str) const override
    {
        return str << "«traced-cell port»";
    }

    std::string showType() const override
    {
        return "a traced-cell port";
    }

    std::string typeOf() const override
    {
        return "port";
    }
};

using FormalNames = std::vector<std::pair<Symbol, bool>, gc_allocator<std::pair<Symbol, bool>>>;

/**
 * The formals of a function, looking through proxies of other cells
 * (which records the observation in them).
 */
FormalNames formalsOf(EvalState & state, Value & f0)
{
    Value * f = &f0;
    while (auto backing = state.cells->observeFunctionArgs(state, *f)) {
        state.forceValue(*backing, noPos);
        f = backing;
    }
    FormalNames res;
    if (f->isLambda())
        if (auto formals = f->lambda().fun->getFormals())
            for (auto & i : formals->formals)
                res.emplace_back(i.name, i.def != nullptr);
    return res;
}

bool sameContext(const Value::StringWithContext::Context * a, const Value::StringWithContext::Context * b)
{
    if (!a || !b)
        return a == b;
    if (a->size() != b->size())
        return false;
    for (auto i = a->begin(), j = b->begin(); i != a->end(); ++i, ++j)
        if ((*i)->view() != (*j)->view())
            return false;
    return true;
}

/**
 * Exact equality of two forced non-container values, including string
 * contexts; unlike `==`, never true across types and bitwise for floats.
 */
bool samePrimitive(Value & a, Value & b)
{
    if (a.type() != b.type())
        return false;
    switch (a.type()) {
    case nInt:
        return a.integer() == b.integer();
    case nFloat:
        return std::bit_cast<uint64_t>(a.fpoint()) == std::bit_cast<uint64_t>(b.fpoint());
    case nBool:
        return a.boolean() == b.boolean();
    case nNull:
        return true;
    case nString:
        return a.string_view() == b.string_view() && sameContext(a.context(), b.context());
    case nPath:
        return a.pathAccessor() == b.pathAccessor() && a.pathStrView() == b.pathStrView();
    case nExternal:
        return a.external() == b.external();
    case nAttrs:
    case nList:
    case nFunction:
    case nThunk:
    case nFailed:
        return false;
    }
    unreachable();
}

/**
 * A call site that is stable across copies of the same source tree: the
 * store path hash is dropped, so `/nix/store/<hash>-source/a.nix:3:5`
 * becomes `source/a.nix:3:5`.
 */
std::string renderCallSite(EvalState & state, PosIdx pos)
{
    auto p = state.positions[pos];
    if (!p)
        return "";
    auto path = std::get_if<SourcePath>(&p.origin);
    if (!path)
        return fmt("<internal>:%d:%d", p.line, p.column);
    auto file = path->path.abs();
    auto storePrefix = state.store->storeDir + "/";
    if (file.starts_with(storePrefix)) {
        file = file.substr(storePrefix.size());
        if (auto dash = file.find('-'); dash != std::string::npos)
            file = file.substr(dash + 1);
    }
    return fmt("%s:%d:%d", file, p.line, p.column);
}

/**
 * While validating a cell, errors must not stick to the thunks that were
 * forced speculatively (see `EvalState::handleEvalExceptionForThunk`).
 */
struct Speculation
{
    EvalState & state;

    explicit Speculation(EvalState & state)
        : state(state)
    {
        state.speculative++;
    }

    ~Speculation()
    {
        state.speculative--;
    }
};

/**
 * Whether `p` points into the current thread's stack.
 */
bool onStack(const void * p)
{
    thread_local std::pair<const char *, const char *> bounds = []() {
        pthread_attr_t attr;
        void * addr = nullptr;
        size_t size = 0;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            pthread_attr_getstack(&attr, &addr, &size);
            pthread_attr_destroy(&attr);
        }
        return std::pair{(const char *) addr, (const char *) addr + size};
    }();
    return (const char *) p >= bounds.first && (const char *) p < bounds.second;
}

void prim_portCall(EvalState & state, CallSite callSite, Value * const * args, Value & v)
{
    auto * ref = dynamic_cast<PortRef *>(args[0]->external());
    assert(ref);
    auto & fn = *ref->port;
    auto & cell = *fn.cell;
    cell.noteUse(state);
    if (fn.stale)
        fn.refresh(state);

    /* A call made by ordinary evaluation (not while computing a cell's
       result) puts its result in a value of that evaluation, which is not
       reused: no need to record it, and its argument is new in every
       generation. */
    if (!state.mem.currentOwner) {
        state.callFunction(*fn.backing, *args[1], v, callSite.pos);
        return;
    }

    auto * call = cell.newPort(state, ExprPort::Kind::Call);
    call->fn = &fn;
    if (verbosity >= lvlVomit)
        printMsg(
            lvlVomit,
            "traced cell call port %s in %s context at %s",
            fn.describe(state.symbols),
            state.mem.currentOwner == &cell ? "own" : "another cell's",
            state.positions[callSite.pos]);
    /* Validation applies the function again to the same argument. Keep
       the argument itself (it may still be a black hole, e.g. the `final`
       of an overlay, and its identity matters for aliasing) unless it lives
       on the stack. Values outside the GC heap that are not on the stack
       (constants in the AST, statics) live as long as the process. */
    if (!onStack(args[1]))
        call->callArg = args[1];
    else {
        call->callArg = state.allocValue();
        *call->callArg = *args[1];
    }

    auto * r = state.allocValue();
    try {
        state.callFunction(*fn.backing, *args[1], *r, callSite.pos);
    } catch (Interrupted &) {
        throw;
    } catch (BaseError &) {
        call->summary = ExprPort::Summary::Failed;
        throw;
    }
    call->backing = r;
    call->resolve(state, v);
}

} // namespace

StableRoot::StableRoot(std::string identity, std::string virtualPrefix, ref<SourceAccessor> target)
    : identity(std::move(identity))
    , virtualPrefix(std::move(virtualPrefix))
    , target(target)
{
    displayPrefix.clear();
}

void StableRoot::anchor() {}

void StableRoot::readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback)
{
    target->readFile(path, sink, sizeCallback);
}

bool StableRoot::pathExists(const CanonPath & path)
{
    return target->pathExists(path);
}

std::optional<SourceAccessor::Stat> StableRoot::maybeLstat(const CanonPath & path)
{
    return target->maybeLstat(path);
}

SourceAccessor::DirEntries StableRoot::readDirectory(const CanonPath & path)
{
    return target->readDirectory(path);
}

std::string StableRoot::readLink(const CanonPath & path)
{
    return target->readLink(path);
}

std::optional<std::filesystem::path> StableRoot::getPhysicalPath(const CanonPath & path)
{
    return target->getPhysicalPath(path);
}

std::pair<CanonPath, std::optional<std::string>> StableRoot::getFingerprint(const CanonPath & path)
{
    /* The contents change between generations; the fetcher cache must not
       remember them under a fingerprint of this accessor. */
    return {path, std::nullopt};
}

void StableRoot::invalidateCache()
{
    target->invalidateCache();
}

void StableRoots::mount(
    EvalState & state, const std::string & identity, const StorePath & storePath, ref<SourceAccessor> accessor)
{
    auto real = state.store->printStorePath(storePath);

    StableRoot * root;
    if (auto i = byIdentity.find(identity); i != byIdentity.end())
        root = &*i->second;
    else {
        auto virtualPath = state.store->makeStorePath(
            "traced-cells-root", hashString(HashAlgorithm::SHA256, identity), storePath.name());
        auto r = make_ref<StableRoot>(identity, state.store->printStorePath(virtualPath), accessor);
        state.storeFS->mount(CanonPath(r->virtualPrefix), r);
        state.allowPath(virtualPath);
        byIdentity.emplace(identity, r);
        byVirtual.emplace(r->virtualPrefix, &*r);
        root = &*r;
    }

    bool changed = !root->realPrefix.empty() && root->realPrefix != real;
    if (root->realPrefix != real) {
        if (!root->realPrefix.empty())
            byReal.erase(root->realPrefix);
        root->realPrefix = real;
        byReal.insert_or_assign(real, root);
    }
    root->target = accessor;
    root->generation = state.getGeneration();

    /* The tree changed: keep the evaluated files that did not change and
       did not read anything that changed. Parsed files are kept anyway
       (`parsed`, by content). */
    if (changed)
        revalidateFiles(state, *root);
}

std::string_view StableRoots::storePrefix(std::string_view path) const
{
    if (path.size() <= storeDir.size() + 1 || !path.starts_with(storeDir) || path[storeDir.size()] != '/')
        return {};
    auto end = path.find('/', storeDir.size() + 1);
    return path.substr(0, end == path.npos ? path.size() : end);
}

uint64_t StableRoots::fileVersion(const std::string & path) const
{
    auto i = fileVersions.find(path);
    return i == fileVersions.end() ? 0 : i->second;
}

void StableRoots::revalidateFiles(EvalState & state, StableRoot & root)
{
    auto dir = root.virtualPrefix + "/";

    std::vector<std::string> paths;
    for (auto & [path, cell] : fileCells)
        if (path.starts_with(dir))
            paths.push_back(path);

    boost::unordered_flat_set<std::string> dropped;

    /* Files whose text changed. */
    for (auto & path : paths) {
        auto old = contentHashes.find(path);
        std::optional<std::string> now;
        try {
            now = contentFingerprint(SourcePath{state.rootFS, CanonPath(path)}.resolveSymlinks().readFile());
        } catch (Error &) {
        }
        if (old == contentHashes.end() || !now || *now != old->second)
            dropped.insert(path);
    }

    /* Files whose reads changed, including imports of dropped files, to a
       fixed point. */
    for (bool again = true; again;) {
        again = false;
        for (auto & path : paths) {
            if (dropped.contains(path))
                continue;
            for (auto & read : fileCells[path]->fileReads) {
                bool valid;
                if (read.root->generation != state.getGeneration())
                    valid = false;
                else if (read.kind == FileReadKind::Import) {
                    std::string_view fp = state.symbols[read.fingerprint];
                    valid = !dropped.contains(std::string(fp.substr(0, fp.find('\n'))))
                            && fileReadFingerprint(state, read) == fp;
                } else
                    valid = read.checkedAt == state.symbols.create(read.root->realPrefix)
                            || fileReadFingerprint(state, read) == state.symbols[read.fingerprint];
                if (!valid) {
                    dropped.insert(path);
                    again = true;
                    break;
                }
            }
        }
    }

    for (auto & path : dropped) {
        fileVersions[path]++;
        fileCells.erase(path);
        contentHashes.erase(path);
        state.dropFileCacheEntry(path);
    }
    /* Resolution of imports (`default.nix`) may have changed. */
    state.dropImportResolutionUnder(dir);

    debug(
        "stable root '%s' changed: kept %d evaluated files, dropped %d",
        root.identity,
        paths.size() - dropped.size(),
        dropped.size());
}

StableRoot * StableRoots::findVirtual(std::string_view path)
{
    if (byVirtual.empty())
        return nullptr;
    auto prefix = storePrefix(path);
    if (prefix.empty())
        return nullptr;
    auto i = byVirtual.find(std::string(prefix));
    return i == byVirtual.end() ? nullptr : i->second;
}

StableRoot * StableRoots::findReal(std::string_view path)
{
    if (byReal.empty())
        return nullptr;
    auto prefix = storePrefix(path);
    if (prefix.empty())
        return nullptr;
    auto i = byReal.find(std::string(prefix));
    return i == byReal.end() ? nullptr : i->second;
}

std::string contentFingerprint(std::string_view contents)
{
    return hashString(HashAlgorithm::SHA256, contents).to_string(HashFormat::Nix32, false);
}

std::string EvalState::pathToString(const SourcePath & path)
{
    auto abs = path.path.abs();
    if (!stableRoots || &*path.accessor != &*rootFS)
        return std::string(abs);
    auto root = stableRoots->findVirtual(abs);
    if (!root)
        return std::string(abs);
    if (mem.currentOwner && verbosity >= lvlVomit) [[unlikely]]
        printMsg(lvlVomit, "traced cell render of '%s'", abs);
    recordFileRead(FileReadKind::Render, SourcePath{rootFS, CanonPath(root->virtualPrefix)}, root->realPrefix);
    return root->realPrefix + std::string(abs.substr(root->virtualPrefix.size()));
}

SourcePath EvalState::toRealPath(const SourcePath & path)
{
    if (!stableRoots || &*path.accessor != &*rootFS)
        return path;
    auto abs = path.path.abs();
    auto root = stableRoots->findVirtual(abs);
    if (!root)
        return path;
    return {rootFS, CanonPath(root->realPrefix + std::string(abs.substr(root->virtualPrefix.size())))};
}

bool EvalState::recordsFileReads(const SourcePath & path)
{
    return mem.currentOwner && stableRoots && &*path.accessor == &*rootFS && stableRoots->findVirtual(path.path.abs());
}

void EvalState::recordFileRead(FileReadKind kind, const SourcePath & path, std::string_view fingerprint, Value * filter)
{
    if (!mem.currentOwner || !stableRoots || &*path.accessor != &*rootFS)
        return;
    auto root = stableRoots->findVirtual(path.path.abs());
    if (!root)
        return;
    auto pathSym = symbols.create(path.path.abs());
    auto key = (uint64_t(kind) << 32) | pathSym.getId();
    FileRead read{
        .root = root,
        .kind = kind,
        .path = pathSym,
        .fingerprint = symbols.create(fingerprint),
        .checkedAt = symbols.create(root->realPrefix),
        .filter = filter,
    };
    for (auto cell = static_cast<CellInstance *>(mem.currentOwner); cell; cell = cell->parent)
        if (kind == FileReadKind::Copy || cell->fileReadKeys.insert(key).second)
            cell->fileReads.push_back(read);
}

std::string dirFingerprint(const SourceAccessor::DirEntries & entries)
{
    std::string res;
    for (auto & [name, type] : entries) {
        res += name;
        res += '\0';
        res += type ? std::to_string(int(*type)) : "?";
        res += '\n';
    }
    return contentFingerprint(res);
}

std::string fileReadFingerprint(EvalState & state, const FileRead & read)
{
    SourcePath path{state.rootFS, CanonPath(state.symbols[read.path])};
    std::string_view recorded = state.symbols[read.fingerprint];
    try {
        switch (read.kind) {

        case FileReadKind::Import: {
            auto resolved = resolveExprPath(path);
            auto abs = std::string(resolved.path.abs());
            return abs + "\n" + contentFingerprint(resolved.resolveSymlinks().readFile()) + "\n"
                   + std::to_string(state.stableRoots->fileVersion(abs));
        }

        case FileReadKind::Resolve: {
            auto mode = recorded.starts_with("F") ? SymlinkResolution::Full : SymlinkResolution::Ancestors;
            return std::string(recorded.substr(0, 1)) + "\n" + path.resolveSymlinks(mode).path.abs();
        }

        case FileReadKind::Content:
            return contentFingerprint(path.readFile());

        case FileReadKind::Exists:
            return path.maybeLstat() ? "1" : "0";

        case FileReadKind::ExistsDir: {
            auto st = path.maybeLstat();
            return st && st->type == SourceAccessor::tDirectory ? "1" : "0";
        }

        case FileReadKind::Type: {
            auto st = path.maybeLstat();
            return st ? std::to_string(int(st->type)) : "missing";
        }

        case FileReadKind::Dir:
            return dirFingerprint(path.readDirectory());

        case FileReadKind::Copy: {
            /* "<method>\n<name>\n<store path>" */
            auto nl1 = recorded.find('\n');
            auto nl2 = recorded.find('\n', nl1 + 1);
            auto method = ContentAddressMethod::parse(recorded.substr(0, nl1));
            auto name = recorded.substr(nl1 + 1, nl2 - nl1 - 1);
            auto real = state.toRealPath(path);
            std::unique_ptr<PathFilter> filter;
            if (read.filter)
                filter = std::make_unique<PathFilter>([&](const std::string & p) {
                    return state.callPathFilter(read.filter, {real.accessor, CanonPath(p)}, noPos);
                });
            auto dst = fetchToStore(
                state.fetchSettings,
                *state.store,
                real.resolveSymlinks(),
                FetchMode::DryRun,
                name,
                method,
                filter.get());
            return std::string(recorded.substr(0, nl2 + 1)) + state.store->printStorePath(dst);
        }

        case FileReadKind::Render:
            return read.root->realPrefix;

        case FileReadKind::Opaque:
            return "";
        }
    } catch (Interrupted &) {
        throw;
    } catch (Error &) {
        return "error";
    }
    unreachable();
}

namespace {

std::string_view fileReadKindName(FileReadKind kind)
{
    switch (kind) {
    case FileReadKind::Import:
        return "import";
    case FileReadKind::Resolve:
        return "symlink resolution";
    case FileReadKind::Content:
        return "contents";
    case FileReadKind::Exists:
    case FileReadKind::ExistsDir:
        return "existence";
    case FileReadKind::Type:
        return "type";
    case FileReadKind::Dir:
        return "directory listing";
    case FileReadKind::Copy:
        return "copy to the store";
    case FileReadKind::Render:
        return "store path";
    case FileReadKind::Opaque:
        return "unreplayable read";
    }
    unreachable();
}

} // namespace

ExprPort::ExprPort(EvalState & state, CellInstance & cell, Kind kind)
    : cell(&cell)
    , kind(kind)
{
    observed.mkNull();
    canonical = state.allocValue();
    canonical->mkThunk(&state.baseEnv, this);
    state.cells->canonicalPorts.emplace(canonical, this);
}

void ExprPort::eval(EvalState & state, Env & env, Value & v)
{
    resolve(state, v);
}

void ExprPort::show(const SymbolTable & symbols, std::ostream & str) const
{
    str << "«traced-cell port»";
}

std::string ExprPort::describe(const SymbolTable & symbols) const
{
    switch (kind) {
    case Kind::Root:
        return "args";
    case Kind::Call:
        return fn->describe(symbols) + "(…)";
    case Kind::Child:
        return parent->describe(symbols) + (name ? "." + std::string(symbols[name]) : fmt("[%d]", index));
    }
    unreachable();
}

void ExprPort::refresh(EvalState & state)
{
    switch (kind) {
    case Kind::Root:
        break;

    case Kind::Child: {
        if (parent->stale)
            parent->refresh(state);
        auto & p = *parent->backing;
        state.forceValue(p, noPos);
        Value * b = nullptr;
        if (p.type() == nAttrs) {
            if (auto a = p.attrs()->get(name))
                b = a->value;
        } else if (p.isList() && !name && index < p.listSize())
            b = p.listView()[index];
        if (!b) {
            cell->unusable = true;
            state.error<CellInvalidated>("a value observed by a reused traced cell no longer exists").debugThrow();
        }
        backing = b;
        break;
    }

    case Kind::Call: {
        if (fn->stale)
            fn->refresh(state);
        state.forceValue(*fn->backing, noPos);
        auto * r = state.allocValue();
        state.callFunction(*fn->backing, *callArg, *r, noPos);
        backing = r;
        break;
    }
    }
    stale = false;
    cell->byBacking.try_emplace(backing, this);
}

void ExprPort::resolve(EvalState & state, Value & v)
{
    cell->noteUse(state);
    if (stale)
        refresh(state);
    assert(backing);

    try {
        state.forceValue(*backing, noPos);
    } catch (Interrupted &) {
        throw;
    } catch (BaseError &) {
        if (summary == Summary::None)
            summary = Summary::Failed;
        throw;
    }

    if (summary == Summary::None)
        observe(state, *backing);

    if (summary == Summary::Primitive || summary == Summary::Identity)
        v = *backing;
    else
        v = observed;
}

namespace {

/**
 * The object behind `v` and the context that created it, for identity
 * summaries: attribute sets and lambdas record their context.
 */
std::tuple<void *, const void *, const void *> objectOf(Value & v)
{
    if (v.type() == nAttrs)
        return {v.attrs()->owner, v.attrs(), nullptr};
    if (v.isLambda())
        return {v.lambda().env->owner, v.lambda().fun, v.lambda().env};
    return {nullptr, nullptr, nullptr};
}

} // namespace

void ExprPort::observe(EvalState & state, Value & b)
{
    /* Not for call results: a call may compute its result afresh, so the
       result of replaying it is a different object even when nothing
       changed. */
    if (auto [owner, object, env] = objectOf(b);
        state.cells->identitySummaries && kind != Kind::Call && owner && owner != cell) {
        identity[0] = object;
        identity[1] = env;
        summary = Summary::Identity;
        return;
    }

    switch (b.type()) {

    case nAttrs: {
        auto bindings = state.buildBindings(b.attrs()->size());
        for (auto & attr : *b.attrs()) {
            auto * child = cell->childFor(state, attr.value, this, attr.name, 0);
            slots.push_back(child);
            slotNames.push_back(attr.name);
            bindings.insert(attr.name, child->canonical, attr.pos);
        }
        observed.mkAttrs(bindings);
        cell->proxies.emplace(observed.attrs(), this);
        summary = Summary::Attrs;
        break;
    }

    case nList: {
        auto size = b.listSize();
        auto list = state.buildList(size);
        for (size_t n = 0; n < size; ++n) {
            auto * child = cell->childFor(state, b.listView()[n], this, Symbol(), n);
            slots.push_back(child);
            list[n] = child->canonical;
        }
        observed.mkList(list);
        summary = Summary::List;
        break;
    }

    case nFunction:
        if (!ref) {
            ref = state.allocValue();
            ref->mkExternal(new PortRef(this));
        }
        observed.mkPrimOpApp(*state.cells->vPortCall, ref);
        summary = Summary::Function;
        break;

    case nInt:
    case nFloat:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nExternal:
        observed = b;
        summary = Summary::Primitive;
        break;

    case nThunk:
    case nFailed:
        unreachable();
    }
}

ExprPort * CellInstance::newPort(EvalState & state, ExprPort::Kind kind)
{
    auto * port = new ExprPort(state, *this, kind);
    ports.push_back(port);
    return port;
}

ExprPort * CellInstance::childFor(EvalState & state, Value * backing, ExprPort * parent, Symbol name, size_t index)
{
    if (auto i = byBacking.find(backing); i != byBacking.end())
        return i->second;
    auto * port = newPort(state, ExprPort::Kind::Child);
    port->backing = backing;
    port->parent = parent;
    port->name = name;
    port->index = index;
    byBacking.emplace(backing, port);
    return port;
}

void CellInstance::noteUse(EvalState & state)
{
    if (boundGen != state.getGeneration() && !validating)
        usedNested = true;
}

CellTable::CellTable(EvalState & state, std::vector<std::string> fileSuffixes, size_t maxInstances)
    : fileSuffixes(std::move(fileSuffixes))
    , maxInstances(maxInstances)
{
    auto * v = state.allocValue();
    v->mkPrimOp(new PrimOp{
        .name = "__portCall",
        .arity = 2,
        .impl = prim_portCall,
        .internal = true,
    });
    vPortCall = allocRootValue(v);
}

void CellTable::registerFile(EvalState & state, const SourcePath & path, Expr * e, Value & v)
{
    auto file = path.path.abs();

    if (v.isLambda() && v.lambda().fun->getFormals())
        for (auto & suffix : fileSuffixes)
            if (file.ends_with(suffix)) {
                v.lambda().fun->cellSite = true;
                return;
            }

    if (flakeOutputs && file.ends_with("/flake.nix"))
        if (auto attrs = dynamic_cast<ExprAttrs *>(e))
            if (auto i = attrs->attrs->find(state.symbols.create("outputs")); i != attrs->attrs->end())
                if (auto lambda = dynamic_cast<ExprLambda *>(i->second.e)) {
                    lambda->cellSite = true;
                    flakeOutputSites.insert(lambda);
                }
}

bool CellTable::call(EvalState & state, Value & fun, Value & arg, Value & vRes, PosIdx pos)
{
    auto & lambda = *fun.lambda().fun;
    auto * env = fun.lambda().env;

    if (flakeOutputSites.contains(&lambda) && !excludedRoot.empty()) {
        auto origin = state.positions[lambda.pos].origin;
        if (auto path = std::get_if<SourcePath>(&origin); path && path->path.abs().starts_with(excludedRoot))
            return false;
    }

    try {
        state.forceAttrs(arg, lambda.pos, "while evaluating the value passed for the lambda argument");
    } catch (Error & e) {
        if (pos)
            e.addTrace(state.positions[pos], "from call site");
        throw;
    }

    /* Calls that fail the formals check take the normal path, which
       reports the error. */
    if (auto formals = lambda.getFormals()) {
        size_t used = 0;
        for (auto & formal : formals->formals)
            if (arg.attrs()->get(formal.name))
                used++;
            else if (!formal.def)
                return false;
        if (!formals->ellipsis && used != arg.attrs()->size())
            return false;
    }

    auto callSite = state.symbols.create(renderCallSite(state, pos));
    if (untraceable.contains({&lambda, env, callSite}))
        return false;
    auto gen = state.getGeneration();

    std::vector<CellInstance *> candidates;
    for (auto * cell : instances)
        if (cell->lambda == &lambda && cell->env == env && cell->callSite == callSite && !cell->unusable)
            candidates.push_back(cell);
    auto ordinal = callsInGeneration[{callSite, &lambda}]++;
    std::ranges::sort(candidates, [&](auto * a, auto * b) {
        if ((a->ordinal == ordinal) != (b->ordinal == ordinal))
            return a->ordinal == ordinal;
        return a->boundGen > b->boundGen;
    });

    for (auto * cell : candidates) {
        if (cell->usedNested && cell->ordinal != ordinal)
            continue;
        if (cell->boundGen == gen) {
            /* Already bound in this generation; only the same argument
               may share it. */
            if (cell->boundArgs == arg.attrs()) {
                stats.hits++;
                vRes = *cell->result;
                return true;
            }
            continue;
        }
        std::string why;
        auto start = std::chrono::steady_clock::now();
        auto * rootBacking = state.allocValue();
        *rootBacking = arg;
        bool valid = validate(state, *cell, rootBacking, why, false);
        if (valid) {
            cell->boundGen = gen;
            cell->boundArgs = arg.attrs();
            cell->boundArg = rootBacking;
        }
        stats.validationSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (valid) {
            printMsg(
                lvlTalkative,
                "traced cell %s (%s) #%d: reused instance #%d (%d ports)",
                state.symbols[callSite],
                state.positions[lambda.pos],
                ordinal,
                cell->ordinal,
                cell->ports.size());
            cell->ordinal = ordinal;
            stats.hits++;
            vRes = *cell->result;
            return true;
        }
        stats.rejected++;
        stats.lastRejection = fmt("%s: %s", state.symbols[callSite], why);
        printMsg(
            lvlTalkative,
            "traced cell %s #%d: instance #%d rejected: %s",
            state.symbols[callSite],
            ordinal,
            cell->ordinal,
            why);
    }

    stats.misses++;
    printMsg(
        lvlTalkative,
        "traced cell %s (%s) #%d: new instance",
        state.symbols[callSite],
        state.positions[lambda.pos],
        ordinal);
    return create(state, fun, arg, vRes, callSite, ordinal);
}

bool CellTable::create(EvalState & state, Value & fun, Value & arg, Value & vRes, Symbol callSite, size_t ordinal)
{
    auto & lambda = *fun.lambda().fun;
    auto * cell = new CellInstance();
    cell->parent = static_cast<CellInstance *>(state.mem.currentOwner);
    cell->lambda = &lambda;
    cell->env = fun.lambda().env;
    cell->callSite = callSite;
    cell->ordinal = ordinal;
    cell->boundGen = state.getGeneration();
    cell->boundArgs = arg.attrs();

    auto * rootBacking = state.allocValue();
    *rootBacking = arg;
    cell->boundArg = rootBacking;
    cell->root = cell->newPort(state, ExprPort::Kind::Root);
    cell->root->backing = rootBacking;
    cell->byBacking.emplace(rootBacking, cell->root);

    Value vArgs;
    cell->root->resolve(state, vArgs);

    /* Bind the formals like `EvalState::callFunction`, but to ports. */
    auto formals = lambda.getFormals();
    Env & env2 = state.mem.allocEnv((lambda.arg ? 1 : 0) + (formals ? formals->formals.size() : 0));
    env2.owner = cell;
    env2.up = fun.lambda().env;
    Displacement displ = 0;
    if (lambda.arg)
        env2.values[displ++] = cell->root->canonical;
    if (formals)
        for (auto & formal : formals->formals) {
            auto j = vArgs.attrs()->get(formal.name);
            env2.values[displ++] = j ? j->value : formal.def->maybeThunk(state, env2);
        }

    /* Registered before the body runs, so that the instance outlives any
       value that refers to its ports even if the body fails. */
    instances.push_back(cell);
    cell->unusable = true;

    auto * result = state.allocValue();
    {
        auto savedOwner = state.mem.currentOwner;
        state.mem.currentOwner = cell;
        Finally restoreOwner([&]() { state.mem.currentOwner = savedOwner; });
        lambda.body->eval(state, env2, *result);
    }

    cell->result = result;
    cell->unusable = false;
    vRes = *result;
    return true;
}

bool CellTable::validate(EvalState & state, CellInstance & cell, Value * rootBacking, std::string & why, bool final)
{
    Speculation speculation(state);
    cell.validating = true;
    Finally doneValidating([&]() { cell.validating = false; });

    /* The new binding is built on the side and committed only on success:
       a nested use of this instance may still read the current one. */
    boost::unordered_flat_map<ExprPort *, Value *> next;
    decltype(cell.byBacking) byBacking;
    bool deferred = false;

    /* Bind `port` to `backing`; fails if the aliasing between ports
       changed. */
    /* Identity is observable only through the pointer-equality short cut
       of `==`, which gives the same answer as comparing contents except
       for functions (and containers holding them). So aliasing between
       ports must be preserved unless the port was observed to be a
       primitive; a primitive port reached through two paths only needs
       equal contents. */
    std::string conflict;

    /* Whether `v` is a primitive, forcing it if needed (only on an
       aliasing conflict, where the type of an unobserved port decides). */
    auto isPrimitive = [&](Value & v) {
        try {
            state.forceValue(v, noPos);
        } catch (Interrupted &) {
            throw;
        } catch (BaseError &) {
            return false;
        }
        switch (v.type()) {
        case nAttrs:
        case nList:
        case nFunction:
        case nThunk:
        case nFailed:
            return false;
        case nInt:
        case nFloat:
        case nBool:
        case nString:
        case nPath:
        case nNull:
        case nExternal:
            return true;
        }
        unreachable();
    };

    auto assign = [&](ExprPort * port, Value * backing) {
        auto [i, inserted] = next.try_emplace(port, backing);
        if (!inserted) {
            if (i->second == backing)
                return true;
            /* Reached through two paths that now lead to different values:
               fine for equal primitives. */
            if (isPrimitive(*backing) && isPrimitive(*i->second) && samePrimitive(*backing, *i->second)
                && (port->summary != ExprPort::Summary::Primitive || samePrimitive(*backing, port->observed)))
                return true;
            conflict = fmt("%s is also bound to another value", port->describe(state.symbols));
            return false;
        }
        auto [j, inserted2] = byBacking.try_emplace(backing, port);
        if (inserted2 || (!port->identityObserved && !j->second->identityObserved) || isPrimitive(*backing))
            return true;
        conflict =
            fmt("%s now shares its value with %s", port->describe(state.symbols), j->second->describe(state.symbols));
        return false;
    };

    /* An infinite recursion while replaying means that the observation
       depends on a value that is being computed right now (typically the
       cell's own result, read through the argument after the cell
       returned). The eager pass defers it; the final pass, run when the
       request is done, must see it succeed. */
    enum class Outcome { Ok, Deferred, Failed };

    auto force = [&](ExprPort & port, Value & b) {
        std::string error;
        try {
            state.forceValue(b, noPos);
            return port.summary == ExprPort::Summary::Failed ? (why = "a value no longer fails", Outcome::Failed)
                                                             : Outcome::Ok;
        } catch (Interrupted &) {
            throw;
        } catch (InfiniteRecursionError & e) {
            if (!final)
                return Outcome::Deferred;
            error = e.info().msg.str();
        } catch (BaseError & e) {
            error = e.info().msg.str();
        }
        if (port.summary == ExprPort::Summary::Failed)
            return Outcome::Ok;
        why = "a value now fails: " + filterANSIEscapes(error, true);
        return Outcome::Failed;
    };

    auto check = [&](ExprPort & port, Value & b) {
        switch (port.summary) {

        case ExprPort::Summary::Primitive:
            if (!samePrimitive(b, port.observed)) {
                why = "a value changed";
                return false;
            }
            break;

        case ExprPort::Summary::Attrs:
            if (b.type() != nAttrs || b.attrs()->size() != port.slots.size()) {
                why = "attribute names changed";
                return false;
            }
            for (size_t n = 0; n < port.slots.size(); ++n) {
                auto a = b.attrs()->get(port.slotNames[n]);
                if (!a) {
                    why = fmt("attribute '%s' disappeared", state.symbols[port.slotNames[n]]);
                    return false;
                }
                if (!assign(port.slots[n], a->value)) {
                    why = fmt("aliasing changed at attribute '%s' (%s)", state.symbols[port.slotNames[n]], conflict);
                    return false;
                }
            }
            for (auto & [name, pos] : port.attrPositions) {
                auto a = b.attrs()->get(name);
                if (!a || a->pos != pos) {
                    why = fmt("position of attribute '%s' changed", state.symbols[name]);
                    return false;
                }
            }
            break;

        case ExprPort::Summary::List:
            if (!b.isList() || b.listSize() != port.slots.size()) {
                why = "list length changed";
                return false;
            }
            for (size_t n = 0; n < port.slots.size(); ++n)
                if (!assign(port.slots[n], b.listView()[n])) {
                    why = fmt("aliasing changed at element %d (%s)", n, conflict);
                    return false;
                }
            break;

        case ExprPort::Summary::Function:
            if (b.type() != nFunction) {
                why = "a function is no longer a function";
                return false;
            }
            if (port.functionArgsObserved && formalsOf(state, b) != port.functionArgs) {
                why = "function arguments changed";
                return false;
            }
            break;

        case ExprPort::Summary::Identity: {
            auto [owner, object, env] = objectOf(b);
            if (object != port.identity[0] || env != port.identity[1]) {
                why = "a value of another cell is a different object";
                return false;
            }
            break;
        }

        case ExprPort::Summary::None:
        case ExprPort::Summary::Failed:
            break;
        }
        return true;
    };

    /* Files of unlocked inputs read while computing the result. Cheap
       unless the input changed since the read was last checked. */
    for (auto & read : cell.fileReads) {
        auto & root = *read.root;
        if (root.generation != state.getGeneration()) {
            why = fmt("input '%s' was not mounted in this evaluation", root.identity);
            return false;
        }
        auto real = state.symbols.create(root.realPrefix);
        if (read.checkedAt == real)
            continue;
        auto now = fileReadFingerprint(state, read);
        if (now != state.symbols[read.fingerprint]) {
            why = fmt("%s of '%s' changed", fileReadKindName(read.kind), state.symbols[read.path]);
            return false;
        }
        read.checkedAt = real;
    }

    try {
        assign(cell.root, rootBacking);

        /* By index: replaying calls may force the instance's own thunks,
           which resolve ports for the first time (against the old
           binding) and append them. They are checked against the new
           binding when the loop reaches them; their parent or function
           port comes earlier, so their new backing is known by then. */
        for (size_t n = 0; n < cell.ports.size(); ++n) {
            auto * port = cell.ports[n];

            if (port->kind == ExprPort::Kind::Call) {
                auto fn = next.find(port->fn);
                if (fn == next.end()) {
                    /* The function port was deferred. */
                    if (final) {
                        why = "a function port has no value";
                        return false;
                    }
                    continue;
                }
                auto * r = state.allocValue();
                try {
                    state.callFunction(*fn->second, *port->callArg, *r, noPos);
                } catch (Interrupted &) {
                    throw;
                } catch (InfiniteRecursionError &) {
                    if (final && port->summary != ExprPort::Summary::Failed) {
                        why = fmt("%s: a function call now fails", port->describe(state.symbols));
                        return false;
                    }
                    deferred = deferred || !final;
                    continue;
                } catch (BaseError &) {
                    if (port->summary != ExprPort::Summary::Failed) {
                        why = fmt("%s: a function call now fails", port->describe(state.symbols));
                        return false;
                    }
                    next.emplace(port, r);
                    continue;
                }
                assign(port, r);
            }

            if (port->summary == ExprPort::Summary::None)
                continue;
            auto i = next.find(port);
            if (i == next.end()) {
                /* Below a deferred port. */
                if (final) {
                    why = fmt("%s: an observed value is no longer reachable", port->describe(state.symbols));
                    return false;
                }
                continue;
            }
            switch (force(*port, *i->second)) {
            case Outcome::Ok:
                break;
            case Outcome::Deferred:
                deferred = true;
                /* Its children stay unassigned, hence deferred too. */
                continue;
            case Outcome::Failed:
                why = port->describe(state.symbols) + ": " + why;
                return false;
            }
            if (!check(*port, *i->second)) {
                why = port->describe(state.symbols) + ": " + why;
                return false;
            }
        }
    } catch (Interrupted &) {
        throw;
    } catch (BaseError & e) {
        why = e.msg();
        return false;
    }

    if (final)
        return true;

    for (auto * port : cell.ports) {
        auto i = next.find(port);
        port->stale = i == next.end();
        if (!port->stale)
            port->backing = i->second;
    }
    cell.byBacking = std::move(byBacking);
    cell.deferred = deferred;
    if (deferred)
        stats.deferred++;
    return true;
}

void CellTable::startGeneration(EvalState & state)
{
    callsInGeneration.clear();

    auto forget = [&](CellInstance * cell) {
        for (auto * port : cell->ports)
            canonicalPorts.erase(port->canonical);
    };

    if (verbosity >= lvlTalkative)
        for (auto * cell : instances) {
            printMsg(
                lvlTalkative,
                "traced cell %s #%d: %d ports (%d new)",
                state.symbols[cell->callSite],
                cell->ordinal,
                cell->ports.size(),
                cell->ports.size() - cell->portsAtStart);
            if (verbosity >= lvlChatty)
                for (auto n = cell->portsAtStart; n < cell->ports.size() && n < cell->portsAtStart + 20; ++n)
                    printMsg(lvlChatty, "  new port %s", cell->ports[n]->describe(state.symbols));
            cell->portsAtStart = cell->ports.size();
        }

    /* Dropping an instance only unroots it: ports still reachable from
       other results keep it alive. */
    std::erase_if(instances, [&](auto * cell) {
        if (cell->ports.size() > maxPorts) {
            forget(cell);
            untraceable.emplace(cell->lambda, cell->env, cell->callSite);
            stats.untraceable++;
            printMsg(
                lvlTalkative,
                "traced cell %s: %d ports, not traced any more",
                state.symbols[cell->callSite],
                cell->ports.size());
            return true;
        }
        if (cell->unusable)
            forget(cell);
        return cell->unusable;
    });

    /* Most recently bound first. Keep a few instances per function and
       call site (an edit is often undone, and a site may serve several
       calls), and at most `maxInstances` overall. */
    std::ranges::stable_sort(instances, [](auto * a, auto * b) { return a->boundGen > b->boundGen; });
    std::map<std::tuple<ExprLambda *, Symbol, size_t>, size_t> perCall;
    std::erase_if(instances, [&](auto * cell) {
        if (++perCall[{cell->lambda, cell->callSite, cell->ordinal}] <= maxPerCall)
            return false;
        forget(cell);
        return true;
    });
    if (instances.size() <= maxInstances)
        return;
    for (size_t n = maxInstances; n < instances.size(); ++n)
        forget(instances[n]);
    instances.resize(maxInstances);
}

bool CellTable::checkDeferred(EvalState & state)
{
    bool ok = true;
    for (auto * cell : instances) {
        if (!cell->deferred || cell->unusable || cell->boundGen != state.getGeneration())
            continue;
        cell->deferred = false;
        std::string why;
        if (validate(state, *cell, cell->boundArg, why, true))
            continue;
        cell->unusable = true;
        stats.invalidated++;
        stats.lastRejection = fmt("%s (deferred): %s", state.symbols[cell->callSite], why);
        printMsg(lvlTalkative, "traced cell %s: deferred check failed: %s", state.symbols[cell->callSite], why);
        ok = false;
    }
    return ok;
}

void CellTable::retry()
{
    for (auto * cell : instances)
        if (!cell->unusable)
            cell->boundGen = 0;
    callsInGeneration.clear();
}

void CellTable::clear()
{
    instances.clear();
    canonicalPorts.clear();
}

Value * CellTable::observeFunctionArgs(EvalState & state, Value & v)
{
    if (!v.isPrimOpApp() || v.primOpApp().left != *vPortCall)
        return nullptr;
    auto & port = *dynamic_cast<PortRef &>(*v.primOpApp().right->external()).port;
    port.cell->noteUse(state);
    if (!port.functionArgsObserved) {
        port.functionArgs = formalsOf(state, *port.backing);
        port.functionArgsObserved = true;
    }
    return port.backing;
}

std::optional<PosIdx> CellTable::attrPos(const Bindings * attrs, Symbol name)
{
    for (auto * cell : instances)
        if (auto i = cell->proxies.find(attrs); i != cell->proxies.end()) {
            auto & port = *i->second;
            auto a = port.backing->attrs()->get(name);
            auto pos = a ? a->pos : noPos;
            if (std::ranges::find(port.attrPositions, std::pair{name, pos}) == port.attrPositions.end())
                port.attrPositions.emplace_back(name, pos);
            return pos;
        }
    return std::nullopt;
}

void CellTable::observeIdentity(const Value & v)
{
    if (auto i = canonicalPorts.find(&v); i != canonicalPorts.end())
        i->second->identityObserved = true;
}

bool CellTable::sameBacking(EvalState & state, const Value & a, const Value & b)
{
    auto i = canonicalPorts.find(&a);
    auto j = canonicalPorts.find(&b);
    if (i == canonicalPorts.end() || j == canonicalPorts.end())
        return false;
    for (auto * port : {i->second, j->second})
        if (port->stale)
            port->refresh(state);
    return i->second->backing == j->second->backing;
}

nlohmann::json CellTable::instancesJson(const EvalState & state) const
{
    auto res = nlohmann::json::array();
    for (auto * cell : instances)
        res.push_back({
            {"callSite", state.symbols[cell->callSite]},
            {"boundGeneration", cell->boundGen},
            {"ports", cell->ports.size()},
            {"unusable", cell->unusable},
        });
    return res;
}

nlohmann::json CellTable::statsJson() const
{
    size_t ports = 0;
    for (auto * cell : instances)
        ports += cell->ports.size();
    return {
        {"instances", instances.size()},
        {"ports", ports},
        {"hits", stats.hits},
        {"misses", stats.misses},
        {"rejected", stats.rejected},
        {"deferred", stats.deferred},
        {"invalidated", stats.invalidated},
        {"untraceable", stats.untraceable},
        {"validationTime", stats.validationSeconds},
        {"lastRejection", stats.lastRejection},
    };
}

} // namespace nix
