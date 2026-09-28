#include "nix/expr/traced-cells.hh"
#include "nix/expr/eval-inline.hh"
#include "nix/fetchers/fetch-to-store.hh"
#include "nix/store/content-address.hh"
#include "nix/util/hash.hh"
#include "nix/util/mounted-source-accessor.hh"
#include "nix/store/store-api.hh"
#include "nix/util/finally.hh"
#include "nix/util/signals.hh"
#include "nix/util/terminal.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <sstream>
#include <unordered_map>
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
std::string renderCallSite(const EvalState & state, PosIdx pos)
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
 * A failure as a cold evaluation could observe it: its message, and
 * whether `builtins.tryEval` catches it.
 */
std::string failureOf(const BaseError & e)
{
    return (dynamic_cast<const AssertionError *>(&e) ? "catchable: " : "") + e.info().msg.str();
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
    if (fn.split)
        fn.invalidate(state, "a function called by a reused traced cell is now several functions");

    /* A call made by ordinary evaluation (not while computing a cell's
       result) puts its result in a value of that evaluation, which is not
       reused: no need to record it, and its argument is new in every
       generation. Neither are calls made while replaying this cell's
       calls: the nested calls of the original run have their own ports. */
    if (!state.mem.currentOwner || !static_cast<CellOwner *>(state.mem.currentOwner)->cell || cell.validating
        || cell.gaveUp) {
        state.callFunction(*fn.backing, *args[1], v, callSite.pos);
        return;
    }

    auto * call = cell.newPort(state, ExprPort::Kind::Call);
    call->fn = &fn;
    /* Validation applies the function again to the same argument. Keep
       the argument itself (it may still be a black hole, e.g. the `final`
       of an overlay, and its identity matters for aliasing) if it lives
       in the GC heap; copy it otherwise (the stack, constants). */
#if NIX_USE_BOEHMGC
    if (GC_base(args[1]))
        call->callArg = args[1];
    else
#endif
    {
        call->callArg = state.allocValue();
        *call->callArg = *args[1];
    }

    auto * r = state.allocValue();
    {
        /* Objects the call creates are told apart from the cell's own
           (see `CellOwner`). */
        auto savedOwner = state.mem.currentOwner;
        state.mem.currentOwner = &cell.inCall;
        Finally restoreOwner([&]() { state.mem.currentOwner = savedOwner; });
        try {
            state.callFunction(*fn.backing, *args[1], *r, callSite.pos);
        } catch (Interrupted &) {
            throw;
        } catch (BaseError & e) {
            call->summary = ExprPort::Summary::Failed;
            call->failure = state.symbols.create(failureOf(e));
            throw;
        }
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

std::string StableRoot::showPath(const CanonPath & path)
{
    return target->showPath(path);
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
        root->realPrefix = real;
        /* Two roots with the same contents are known under neither. */
        byReal.clear();
        for (auto & [_, r] : byIdentity)
            if (auto [i, inserted] = byReal.try_emplace(r->realPrefix, &*r); !inserted)
                i->second = nullptr;
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
                    valid = !dropped.contains(std::string(fp.substr(0, fp.find('\n')))) && importValid(state, read);
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
        auto version = fileVersion(path);
        fileHistory[{path, version}] = fileCells[path]->fileReads;
        if (version >= maxFileHistory)
            fileHistory.erase({path, version - maxFileHistory});
        fileVersions[path] = version + 1;
        fileCells.erase(path);
        contentHashes.erase(path);
        state.dropFileCacheEntry(path);
    }
    /* Resolution of imports (`default.nix`) may have changed. */
    state.dropImportResolutionUnder(dir);
}

bool StableRoots::importValid(EvalState & state, const FileRead & read)
{
    if (importValidityGeneration != state.getGeneration()) {
        importValidity.clear();
        importValidityGeneration = state.getGeneration();
    }
    /* `abs\ncontentHash\nversion`, see `fileReadFingerprint()`. */
    std::string recorded(state.symbols[read.fingerprint]);
    auto nl1 = recorded.find('\n');
    auto nl2 = recorded.find('\n', nl1 + 1);
    if (nl1 == std::string::npos || nl2 == std::string::npos)
        return false;
    auto abs = recorded.substr(0, nl1);
    /* The import still resolves to that file (a directory to its
       `default.nix`). Not memoised: other paths may resolve to it. */
    try {
        if (resolveExprPath(SourcePath{state.rootFS, CanonPath(state.symbols[read.path])}).path.abs() != abs)
            return false;
    } catch (Error &) {
        return false;
    }
    auto [i, inserted] = importValidity.try_emplace(recorded, true);
    if (!inserted)
        return i->second;
    auto result = [&]() {
        auto contentHash = recorded.substr(nl1 + 1, nl2 - nl1 - 1);
        auto version = std::stoull(recorded.substr(nl2 + 1));
        std::string now;
        try {
            now = contentFingerprint(SourcePath{state.rootFS, CanonPath(abs)}.resolveSymlinks().readFile());
        } catch (Error &) {
            return false;
        }
        if (now != contentHash)
            return false;
        /* The reads of the version the importer holds: the live file cell
           for the current version, the history for a dropped one. */
        const std::vector<FileRead, gc_allocator<FileRead>> * reads = nullptr;
        if (version == fileVersion(abs)) {
            auto c = fileCells.find(abs);
            if (c == fileCells.end())
                return true; /* not evaluated since: nothing read yet */
            reads = &c->second->fileReads;
        } else {
            auto h = fileHistory.find({abs, version});
            if (h == fileHistory.end())
                return false;
            reads = &h->second;
        }
        for (auto & r : *reads) {
            if (r.root->generation != state.getGeneration())
                return false;
            bool valid = r.kind == FileReadKind::Import ? importValid(state, r)
                                                        : fileReadFingerprint(state, r) == state.symbols[r.fingerprint];
            if (!valid)
                return false;
        }
        return true;
    }();
    /* Positive results reached meanwhile may rest on this one (a cycle). */
    if (!result)
        erase_if(importValidity, [](auto & e) { return e.second; });
    importValidity[recorded] = result;
    return result;
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

bool EvalState::realizeStrings(std::string & s, NixStringContext * context, bool record)
{
    if (!stableRoots || stableRoots->byVirtual.empty())
        return false;
    bool changed = false;
    for (auto & [virtualPrefix, root] : stableRoots->byVirtual) {
        if (root->realPrefix.empty())
            continue;
        /* The whole virtual store path, or its base name alone (`baseNameOf
           (toString src)`, as in `lib.cleanSource`): a 32-character hash
           followed by the name identifies the root unambiguously. */
        auto virtualBase = std::string_view(virtualPrefix).substr(stableRoots->storeDir.size() + 1);
        auto realBase = std::string_view(root->realPrefix).substr(stableRoots->storeDir.size() + 1);
        size_t pos = 0;
        bool found = false;
        while ((pos = s.find(virtualBase, pos)) != std::string::npos) {
            s.replace(pos, virtualBase.size(), realBase);
            pos += realBase.size();
            found = true;
        }
        if (!found)
            continue;
        changed = true;
        if (!record)
            continue;
        recordFileRead(FileReadKind::Render, SourcePath{rootFS, CanonPath(virtualPrefix)}, root->realPrefix);
    }
    if (context)
        realizeContext(*context);
    return changed;
}

bool EvalState::realizeContext(NixStringContext & context)
{
    if (!stableRoots || stableRoots->byVirtual.empty())
        return false;
    std::vector<StorePath> real;
    for (auto i = context.begin(); i != context.end();) {
        auto * o = std::get_if<NixStringContextElem::Opaque>(&i->raw);
        auto * root = o ? stableRoots->findVirtual(store->printStorePath(o->path)) : nullptr;
        if (!root || root->realPrefix.empty()) {
            ++i;
            continue;
        }
        recordFileRead(FileReadKind::Render, SourcePath{rootFS, CanonPath(root->virtualPrefix)}, root->realPrefix);
        real.push_back(store->parseStorePath(root->realPrefix));
        i = context.erase(i);
    }
    for (auto & path : real)
        context.insert(NixStringContextElem::Opaque{.path = path});
    return !real.empty();
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
    /* One read per kind and path, except imports: the value an import
       gives depends on the version of the file (see `importValid()`), and
       a cell that imports the same file again after it was dropped holds
       both versions. */
    auto fingerprintSym = symbols.create(fingerprint);
    auto key = (uint64_t(kind) << 32) | (kind == FileReadKind::Import ? fingerprintSym : pathSym).getId();
    FileRead read{
        .root = root,
        .kind = kind,
        .path = pathSym,
        .fingerprint = fingerprintSym,
        .checkedAt = symbols.create(root->realPrefix),
        .filter = filter,
    };
    for (auto cell = static_cast<CellOwner *>(mem.currentOwner)->cell; cell; cell = cell->parent)
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
                    /* As `addPath()`: the filter sees paths under the stable root. */
                    auto p2 = p.starts_with(real.path.abs()) ? path.path.abs() + p.substr(real.path.abs().size()) : p;
                    return state.callPathFilter(read.filter, {real.accessor, CanonPath(p2)}, noPos);
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
    if (!cell.forgotten)
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
        if (parent->split)
            invalidate(state, "a value observed by a reused traced cell is now several values");
        auto & p = *parent->backing;
        state.forceValue(p, noPos);
        Value * b = nullptr;
        if (p.type() == nAttrs) {
            if (auto a = p.attrs()->get(name))
                b = a->value;
        } else if (p.isList() && !name && index < p.listSize())
            b = p.listView()[index];
        if (!b)
            invalidate(state, "a value observed by a reused traced cell no longer exists");
        backing = b;
        break;
    }

    case Kind::Call: {
        if (fn->stale)
            fn->refresh(state);
        if (fn->split)
            invalidate(state, "a function called by a reused traced cell is now several functions");
        /* The result of an equivalent function stands. */
        if (fn->functionUnchanged)
            break;
        state.forceValue(*fn->backing, noPos);
        auto * r = state.allocValue();
        /* As in `prim_portCall()`: what the call creates is not the cell's own. */
        auto savedOwner = state.mem.currentOwner;
        state.mem.currentOwner = &cell->inCall;
        Finally restoreOwner([&]() { state.mem.currentOwner = savedOwner; });
        state.callFunction(*fn->backing, *callArg, *r, noPos);
        backing = r;
        break;
    }
    }
    stale = false;
    cell->byBacking.try_emplace(backing, this);
}

void ExprPort::invalidate(EvalState & state, std::string_view reason)
{
    cell->unusable = true;
    state.error<CellInvalidated>("%s", reason).debugThrow();
}

void ExprPort::resolve(EvalState & state, Value & v)
{
    cell->noteUse(state);
    if (stale)
        refresh(state);
    if (split && summary == Summary::None)
        invalidate(state, "a value read by a reused traced cell is now several values");
    assert(backing);

    try {
        state.forceValue(*backing, noPos);
    } catch (Interrupted &) {
        throw;
    } catch (InfiniteRecursionError & e) {
        /* While any validation is in progress, a port resolved for the
           first time (a replayed call forcing thunks, possibly computing
           a nested cell) may hit a value that is being computed (a flake's
           `self`): that is the deferral signal, not a failure of the
           value. */
        if (state.speculative)
            throw;
        if (summary == Summary::None) {
            summary = Summary::Failed;
            failure = state.symbols.create(failureOf(e));
        }
        throw;
    } catch (BaseError & e) {
        if (summary == Summary::None) {
            summary = Summary::Failed;
            failure = state.symbols.create(failureOf(e));
        }
        throw;
    }

    if (summary == Summary::None)
        observe(state, *backing);

    if (summary == Summary::Primitive || summary == Summary::Identity || (summary == Summary::Function && direct))
        v = *backing;
    else
        v = observed;
}

namespace {

/**
 * The object behind `v` and the context that created it: attribute sets
 * and lambdas record their context.
 */
std::tuple<CellOwner *, const void *, const void *> objectOf(EvalState & state, Value & v)
{
    if (v.type() == nAttrs)
        return {static_cast<CellOwner *>(v.attrs()->owner), v.attrs(), nullptr};
    if (v.isLambda())
        return {static_cast<CellOwner *>(state.mem.ownerOf(*v.lambda().env)), v.lambda().fun, v.lambda().env};
    return {nullptr, nullptr, nullptr};
}

} // namespace

void ExprPort::observe(EvalState & state, Value & b)
{
    /* A cell that reads its argument everywhere (an overlay's `final`, a
       module system's `config`) would trace the whole evaluation: give
       up while computing, before the ports exhaust memory. */
    if (cell->ports.size() > CellTable::maxPorts) [[unlikely]] {
        if (!cell->gaveUp)
            state.cells->giveUp(state, *cell);
        summary = Summary::Identity;
        return;
    }

    /* An object of the cell's own, reached again through a port: not
       part of the argument, so reading it directly loses nothing, and it
       stops the proxies from spreading over the cell's own values. */
    if (auto [owner, object, env] = objectOf(state, b); owner && owner->cell == cell && !owner->inCall) {
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

    case nFunction: {
        /* A function of another cell is as stable as that cell's result;
           a primop is a constant. Calling such a function directly costs
           nothing per call; validation checks that the new function is an
           equivalent closure. Functions of ordinary evaluation (new every
           request, e.g. overlays from a module system) and functions
           created by a call are proxied so that their calls can be
           replayed. */
        auto [owner, object, env] = objectOf(state, b);
        /* A function whose environment is the base environment (a file's
           top-level function) is equivalent to itself as long as its file
           is unchanged. */
        direct = !b.isLambda()
                 || (((owner && !owner->inCall) || b.lambda().env == &state.baseEnv)
                     && !state.cells->proxiedLambdas.contains(b.lambda().fun));
        if (!direct) {
            if (!ref) {
                ref = state.allocValue();
                ref->mkExternal(new PortRef(this));
            }
            observed.mkPrimOpApp(*state.cells->vPortCall, ref);
        }
        function = b;
        summary = Summary::Function;
        break;
    }

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

CellTable::CellTable(EvalState & state)
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

    if (v.isLambda() && v.lambda().fun->getFormals() && file.ends_with("/pkgs/top-level/impure.nix")) {
        v.lambda().fun->cellSite = true;
        return;
    }

    if (file.ends_with("/flake.nix"))
        if (auto attrs = dynamic_cast<ExprAttrs *>(e))
            if (auto i = attrs->attrs->find(state.symbols.create("outputs")); i != attrs->attrs->end())
                if (auto lambda = dynamic_cast<ExprLambda *>(i->second.e)) {
                    lambda->cellSite = true;
                    flakeOutputSites.insert(lambda);
                }
}

bool CellTable::call(EvalState & state, Value & fun, Value & arg, Value & vRes, PosIdx pos)
{
    if (suspended)
        return false;
    auto & lambda = *fun.lambda().fun;

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
    return lookup(
        state, fun, arg, arg.attrs(), callSite, vRes, [&](CellInstance & cell, Value & vArgs, Value & result) {
            /* Bind the formals like `EvalState::callFunction`, but to
               ports. */
            auto formals = lambda.getFormals();
            Env & env2 = state.mem.allocEnv((lambda.arg ? 1 : 0) + (formals ? formals->formals.size() : 0));
            env2.up = fun.lambda().env;
            Displacement displ = 0;
            if (lambda.arg)
                env2.values[displ++] = cell.root->canonical;
            if (formals)
                for (auto & formal : formals->formals) {
                    auto j = vArgs.attrs()->get(formal.name);
                    env2.values[displ++] = j ? j->value : formal.def->maybeThunk(state, env2);
                }
            lambda.body->eval(state, env2, result);
        });
}

std::pair<Symbol, Symbol> CellTable::deferralKey(EvalState & state, const CellInstance & cell)
{
    return {state.symbols.create(renderCallSite(state, cell.lambda->pos)), cell.callSite};
}

void CellTable::giveUp(EvalState & state, CellInstance & cell)
{
    cell.gaveUp = true;
    untraceable.emplace(cell.lambda, cell.env, cell.callSite);
    stats.untraceable++;
    printMsg(
        lvlTalkative,
        "traced cell %s: %d ports while computing, not traced any more",
        state.symbols[cell.callSite],
        cell.ports.size());
}

bool CellTable::lookup(
    EvalState & state,
    Value & fun,
    Value & arg,
    const void * argsId,
    Symbol callSite,
    Value & vRes,
    std::function<void(CellInstance &, Value & vArgs, Value & result)> compute)
{
    auto & lambda = *fun.lambda().fun;
    auto * env = fun.lambda().env;
    if (untraceable.contains({&lambda, env, callSite}))
        return false;
    /* A recursive application of the same site inside its own computation
       (an overlay chain, a fixpoint) would receive the enclosing cell's
       proxies as its argument and stack proxies on proxies. */
    if (state.mem.currentOwner)
        for (auto * outer = static_cast<CellOwner *>(state.mem.currentOwner)->cell; outer; outer = outer->parent)
            if (outer->computing && outer->lambda == &lambda && outer->callSite == callSite)
                return false;
    auto gen = state.getGeneration();

    /* Instances of this function at this call site: the same closure, or
       the same code with an equivalent environment (the file was parsed
       or evaluated again since the instance was made; only instances of
       earlier requests, whose environments are settled). Comparing may
       evaluate cell sites, which add instances: iterate over a copy. */
    std::vector<CellInstance *> candidates;
    if (auto i = instancesBySite.find(siteKeyOf(state, lambda, callSite)); i != instancesBySite.end())
        for (auto * cell : std::vector(i->second)) {
            if (cell->unusable)
                continue;
            if (cell->lambda != &lambda || cell->env != env) {
                if (cell->boundGen >= gen || !sameLambda(state, cell->lambda, &lambda))
                    continue;
                bool same = cell->env == env;
                if (!same)
                    try {
                        Comparison comparison(state, *this);
                        same = equivalentEnvironments(state, &lambda, *cell->env, *env);
                    } catch (Interrupted &) {
                        throw;
                    } catch (BaseError &) {
                    }
                if (!same)
                    continue;
                cell->lambda = &lambda;
                cell->env = env;
            }
            candidates.push_back(cell);
        }
    auto ordinal = callsInGeneration[{callSite, &lambda}]++;
    auto before = [&](auto * a, auto * b) {
        if ((a->ordinal == ordinal) != (b->ordinal == ordinal))
            return a->ordinal == ordinal;
        return a->boundGen > b->boundGen;
    };
    if (candidates.size() > maxCandidatesTried) {
        std::ranges::partial_sort(candidates, candidates.begin() + maxCandidatesTried, before);
        candidates.resize(maxCandidatesTried);
    } else
        std::ranges::sort(candidates, before);

    for (auto * cell : candidates) {
        if (cell->usedNested && cell->ordinal != ordinal)
            continue;
        if (cell->boundGen == gen) {
            /* Already bound in this generation; only the same argument
               may share it. */
            if (cell->boundArgs == argsId) {
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
            cell->boundArgs = argsId;
            cell->boundArg = rootBacking;
        }
        auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        stats.validationSeconds += seconds;
        if (seconds > 0.5)
            printMsg(
                lvlTalkative,
                "traced cell %s #%d: validating instance #%d took %.1f s (%s)",
                state.symbols[callSite],
                ordinal,
                cell->ordinal,
                seconds,
                valid ? "valid" : why);
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

    auto * cell = new CellInstance();
    cell->parent = state.mem.currentOwner ? static_cast<CellOwner *>(state.mem.currentOwner)->cell : nullptr;
    cell->lambda = &lambda;
    cell->env = env;
    cell->callSite = callSite;
    cell->ordinal = ordinal;
    cell->boundGen = gen;
    cell->boundArgs = argsId;

    auto * rootBacking = state.allocValue();
    *rootBacking = arg;
    cell->boundArg = rootBacking;
    cell->root = cell->newPort(state, ExprPort::Kind::Root);
    cell->root->backing = rootBacking;
    cell->byBacking.emplace(rootBacking, cell->root);

    Value vArgs;
    cell->root->resolve(state, vArgs);

    /* Registered before the body runs, so that the instance outlives any
       value that refers to its ports even if the body fails. */
    instances.push_back(cell);
    instancesBySite[siteKeyOf(state, lambda, callSite)].push_back(cell);
    cell->unusable = true;

    auto * result = state.allocValue();
    {
        auto savedOwner = state.mem.currentOwner;
        state.mem.currentOwner = &cell->self;
        cell->computing = true;
        auto start = std::chrono::steady_clock::now();
        Finally restoreOwner([&]() {
            state.mem.currentOwner = savedOwner;
            cell->computing = false;
            auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (seconds > 0.3)
                printMsg(
                    lvlTalkative,
                    "traced cell %s #%d: computing took %.1f s (%d ports)",
                    state.symbols[callSite],
                    ordinal,
                    seconds,
                    cell->ports.size());
        });
        compute(*cell, vArgs, *result);
    }

    cell->result = result;
    cell->unusable = cell->gaveUp;
    vRes = *result;
    return true;
}

bool CellTable::validate(EvalState & state, CellInstance & cell, Value * rootBacking, std::string & why, bool final)
{
    Comparison comparison(state, *this);
    cell.validating = true;
    Finally doneValidating([&]() { cell.validating = false; });

    /* The new binding is built on the side and committed only on success:
       a nested use of this instance may still read the current one. */
    boost::unordered_flat_map<ExprPort *, Value *> next;
    decltype(cell.byBacking) byBacking;
    bool deferred = false;

    /* Ports bound to a second value (split), and bindings still to check:
       the second values of split ports and, while draining, ports bound
       for the first time. */
    boost::unordered_flat_set<ExprPort *> splitPorts;
    std::vector<std::pair<ExprPort *, Value *>> pending;
    bool draining = false;

    /* Function ports whose new value is an equivalent closure. */
    boost::unordered_flat_map<ExprPort *, bool> unchanged;

    /* Functions the cell calls directly whose check was deferred: they
       keep the function last validated, for the final pass to compare. */
    boost::unordered_flat_set<ExprPort *> uncheckedDirect;

    /* Ports not observed yet when the loop reached them. */
    std::vector<ExprPort *> unobserved;

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
            /* Reached through two paths that now lead to different values.
               Nothing was read through an unobserved port, so any value
               will do (and it must not be forced: the cell never did).
               Otherwise the second value must satisfy the observations
               too, unless the cell compared the identity of the port. */
            if (port->summary == ExprPort::Summary::None)
                return true;
            if (port->identityObserved) {
                conflict = fmt("%s is also bound to another value", port->describe(state.symbols));
                return false;
            }
            splitPorts.insert(port);
            byBacking.try_emplace(backing, port);
            pending.emplace_back(port, backing);
            return true;
        }
        auto [j, inserted2] = byBacking.try_emplace(backing, port);
        if (!inserted2 && port->identityObserved && j->second->identityObserved && !isPrimitive(*backing)) {
            conflict = fmt(
                "%s now shares its value with %s", port->describe(state.symbols), j->second->describe(state.symbols));
            return false;
        }
        if (draining)
            pending.emplace_back(port, backing);
        return true;
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
            error = failureOf(e);
        } catch (BaseError & e) {
            error = failureOf(e);
        }
        bool failed = port.summary == ExprPort::Summary::Failed;
        if (failed && error == state.symbols[port.failure])
            return Outcome::Ok;
        why = (failed ? "a value fails differently: " : "a value now fails: ") + filterANSIEscapes(error, true);
        return Outcome::Failed;
    };

    /* After a comparison found a pair different, or undecided: positive
       results memoised meanwhile may rest on the assumption that it was
       equivalent (coinduction). */
    auto forgetPositives = [&]() {
        erase_if(equivalentValues, [](auto & e) { return e.second; });
        erase_if(equivalentEnvs, [](auto & e) { return e.second; });
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
            /* An equivalent closure gives the same results: its call ports
               keep theirs. Comparing may force values that depend on the
               result of the cell being validated (infinite recursion):
               then the calls are applied again instead, or, for a function
               the cell calls directly, the check is deferred. */
            try {
                unchanged[&port] = equivalent(state, port.function, b);
            } catch (InfiniteRecursionError &) {
                if (port.direct) {
                    if (final) {
                        why = "a function depends on a value that is not available";
                        return false;
                    }
                    forgetPositives();
                    deferred = true;
                    uncheckedDirect.insert(&port);
                    break;
                }
                unchanged[&port] = false;
            }
            if (!unchanged[&port]) {
                if (port.direct) {
                    if (port.function.isLambda())
                        proxiedLambdas.insert(port.function.lambda().fun);
                    why = "a function changed";
                    return false;
                }
                forgetPositives();
            }
            break;

        case ExprPort::Summary::Identity: {
            auto [owner, object, env] = objectOf(state, b);
            if (object != port.identity[0] || env != port.identity[1]) {
                why = "an object of the cell's own is a different object";
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

    /* Force `b` and check it against the observations of `port`. */
    auto checkBinding = [&](ExprPort & port, Value & b) {
        switch (force(port, b)) {
        case Outcome::Ok:
            break;
        case Outcome::Deferred:
            deferred = true;
            if (port.direct)
                uncheckedDirect.insert(&port);
            /* Its children stay unassigned, hence deferred too. */
            return true;
        case Outcome::Failed:
            why = port.describe(state.symbols) + ": " + why;
            return false;
        }
        if (!check(port, b)) {
            why = port.describe(state.symbols) + ": " + why;
            return false;
        }
        return true;
    };

    /* Replay the call of `port` against the function `fn` and bind the
       result. */
    auto replay = [&](ExprPort & port, Value & fn) {
        auto * r = state.allocValue();
        auto start = std::chrono::steady_clock::now();
        Finally timing([&]() {
            auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (seconds > 0.5)
                printMsg(lvlTalkative, "traced cell: replaying %s took %.1f s", port.describe(state.symbols), seconds);
        });
        try {
            auto savedOwner = state.mem.currentOwner;
            state.mem.currentOwner = &cell.inCall;
            Finally restoreOwner([&]() { state.mem.currentOwner = savedOwner; });
            state.callFunction(fn, *port.callArg, *r, noPos);
        } catch (Interrupted &) {
            throw;
        } catch (InfiniteRecursionError &) {
            if (final && port.summary != ExprPort::Summary::Failed) {
                why = fmt("%s: a function call now fails", port.describe(state.symbols));
                return false;
            }
            deferred = deferred || !final;
            return true;
        } catch (BaseError & e) {
            if (port.summary != ExprPort::Summary::Failed || dynamic_cast<RecoverableEvalError *>(&e)) {
                why = fmt("%s: a function call now fails", port.describe(state.symbols));
                return false;
            }
            /* Checked against the recorded failure like any value. */
            r->mkFailed(std::current_exception(), nullptr);
        }
        if (!assign(&port, r)) {
            why = fmt("%s: %s", port.describe(state.symbols), conflict);
            return false;
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
        bool valid = read.kind == FileReadKind::Import
                         ? state.stableRoots->importValid(state, read)
                         : fileReadFingerprint(state, read) == state.symbols[read.fingerprint];
        if (!valid) {
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
                /* Same function: the result stands, and so does everything
                   read from it. The eager pass leaves those ports out of
                   the binding (they refresh from their parents when used);
                   the final pass, which must see every observation, checks
                   them against the same values. A stale result (its replay
                   was deferred) was computed from an older function and is
                   replayed. */
                if (auto u = unchanged.find(port->fn); u != unchanged.end() && u->second && !port->stale) {
                    if (port->backing)
                        next.try_emplace(port, port->backing);
                    if (!final)
                        continue;
                } else if (!replay(*port, *fn->second))
                    return false;
            }

            if (port->summary == ExprPort::Summary::None) {
                unobserved.push_back(port);
                continue;
            }
            auto i = next.find(port);
            if (i == next.end()) {
                /* Below a deferred port. */
                if (final) {
                    why = fmt("%s: an observed value is no longer reachable", port->describe(state.symbols));
                    return false;
                }
                continue;
            }
            if (!checkBinding(*port, *i->second))
                return false;
        }

        /* Second values of split ports: checked like the first, with the
           calls of split function ports replayed against them. */
        draining = true;
        std::optional<std::unordered_multimap<ExprPort *, ExprPort *>> callsByFn;
        while (!pending.empty()) {
            auto [port, b] = pending.back();
            pending.pop_back();
            if (port->summary == ExprPort::Summary::None) {
                unobserved.push_back(port);
                continue;
            }
            if (!checkBinding(*port, *b))
                return false;
            if (port->summary != ExprPort::Summary::Function)
                continue;
            unchanged[port] = false;
            if (!callsByFn) {
                callsByFn.emplace();
                for (auto * p : cell.ports)
                    if (p->kind == ExprPort::Kind::Call)
                        callsByFn->emplace(p->fn, p);
            }
            for (auto [i, e] = callsByFn->equal_range(port); i != e; ++i)
                if (!replay(*i->second, *b))
                    return false;
        }

        /* A port that a replayed call observed (forcing the cell's own
           thunks) after the loop passed it was observed against the old
           binding only. */
        for (auto * port : unobserved)
            if (port->summary != ExprPort::Summary::None) {
                why = fmt("%s: observed while validating", port->describe(state.symbols));
                return false;
            }
    } catch (Interrupted &) {
        throw;
    } catch (BaseError & e) {
        why = e.msg();
        return false;
    }

    if (final)
        return true;

    if (deferred && (noDeferrals || noDeferralSites.contains(deferralKey(state, cell)))) {
        why = noDeferrals ? "a check would be deferred in a retry"
                          : "a check would be deferred, and a deferred check failed here before";
        return false;
    }

    for (auto * port : cell.ports) {
        auto i = next.find(port);
        port->stale = i == next.end();
        if (!port->stale) {
            port->backing = i->second;
            if (port->summary == ExprPort::Summary::Function) {
                /* A deferred check compares with the function last validated. */
                if (!uncheckedDirect.contains(port))
                    port->function = *port->backing;
                auto u = unchanged.find(port);
                port->functionUnchanged = u != unchanged.end() && u->second;
                /* The cell calls a direct function through copies of what
                   it saw; give it the new one from now on. */
                if (port->direct)
                    *port->canonical = *port->backing;
            }
        }
        port->split = splitPorts.contains(port);
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

    /* Dropping an instance only unroots it: ports still reachable from
       other results keep it alive. */
    std::erase_if(instances, [&](auto * cell) {
        if (cell->ports.size() > maxPorts) {
            forget(*cell);
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
            forget(*cell);
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
        forget(*cell);
        return true;
    });
    if (instances.size() > maxInstances) {
        for (size_t n = maxInstances; n < instances.size(); ++n)
            forget(*instances[n]);
        instances.resize(maxInstances);
    }
    indexInstances(state);
}

std::pair<Symbol, Symbol> CellTable::siteKeyOf(EvalState & state, const ExprLambda & lambda, Symbol callSite)
{
    return {state.symbols.create(renderCallSite(state, lambda.pos)), callSite};
}

void CellTable::indexInstances(EvalState & state)
{
    instancesBySite.clear();
    for (auto * cell : instances)
        instancesBySite[siteKeyOf(state, *cell->lambda, cell->callSite)].push_back(cell);
}

bool CellTable::checkDeferred(EvalState & state)
{
    bool ok = true;
    /* Over a copy: final validations may create instances, and bind
       others, whose checks may be deferred too. */
    for (bool again = true; again;) {
        again = false;
        for (auto * cell : std::vector(instances)) {
            if (!cell->deferred || cell->unusable || cell->boundGen != state.getGeneration())
                continue;
            again = true;
            cell->deferred = false;
            std::string why;
            if (validate(state, *cell, cell->boundArg, why, true))
                continue;
            cell->unusable = true;
            noDeferralSites.insert(deferralKey(state, *cell));
            stats.invalidated++;
            stats.lastRejection = fmt("%s (deferred): %s", state.symbols[cell->callSite], why);
            printMsg(
                lvlTalkative,
                "traced cell %s (%s): deferred check failed: %s",
                state.symbols[cell->callSite],
                state.positions[cell->lambda->pos],
                why);
            ok = false;
        }
    }
    return ok;
}

void CellTable::dropGeneration(EvalState & state, uint64_t gen)
{
    size_t dropped = 0;
    std::erase_if(instances, [&](auto * cell) {
        if (cell->boundGen != gen && !cell->unusable)
            return false;
        forget(*cell);
        dropped++;
        return true;
    });
    indexInstances(state);
    callsInGeneration.clear();
    printMsg(
        lvlTalkative, "traced cells: dropped %d instances of generation %d, %d kept", dropped, gen, instances.size());
}

void CellTable::clear()
{
    for (auto * cell : instances)
        forget(*cell);
    instances.clear();
    instancesBySite.clear();
    callsInGeneration.clear();
}

void CellTable::forget(CellInstance & cell)
{
    cell.forgotten = true;
    for (auto * port : cell.ports)
        canonicalPorts.erase(port->canonical);
}

CellTable::Comparison::Comparison(EvalState & state, CellTable & table)
    : state(state)
    , table(table)
    , forces(std::exchange(table.equivalenceForces, 0))
{
    state.speculative++;
    values.swap(table.equivalentValues);
    envs.swap(table.equivalentEnvs);
}

CellTable::Comparison::~Comparison()
{
    state.speculative--;
    values.swap(table.equivalentValues);
    envs.swap(table.equivalentEnvs);
    table.equivalenceForces = forces;
}

namespace {

/**
 * Collect the variables `e` reads from environments outside itself:
 * mirrors `Expr::bindVars`, counting the environments each construct
 * introduces (`depth`), so that a variable with `level >= depth` reaches
 * out of `e`.
 */
void collectFreeVars(const Expr * e, Level depth, CellTable::FreeVars & out)
{
    using AttrKind = ExprAttrs::AttrDef::Kind;

    /* A variable, with the attributes selected from it if `e` is a static
       selection. */
    auto variable = [&](const ExprVar & var, Level depth, std::vector<Symbol> path) {
        if (var.level >= depth) {
            out.vars.push_back({var.level - depth, var.displ, var.fromWith, var.name, std::move(path)});
            return;
        }
        /* A `with` inside `e` may not define the name: the lookup goes on
           in the enclosing `with`s, from the first one outside `e`. */
        auto level = var.level;
        for (auto * with = var.fromWith; with && with->parentWith; with = with->parentWith)
            if ((level += with->prevWith) >= depth) {
                out.vars.push_back({level - depth, 0, with->parentWith, var.name, std::move(path)});
                return;
            }
    };

    /* `x.a.b` with static attributes: the variable and the path (`x` may
       be the slot of an `inherit (x) …` source). */
    auto staticPath = [&](const Expr & e) -> std::optional<std::pair<const ExprVar *, std::vector<Symbol>>> {
        if (auto * var = dynamic_cast<const ExprVar *>(&e))
            return std::pair{var, std::vector<Symbol>{}};
        auto * sel = dynamic_cast<const ExprSelect *>(&e);
        if (!sel || sel->def)
            return std::nullopt;
        auto * var = dynamic_cast<const ExprVar *>(sel->e);
        if (!var)
            return std::nullopt;
        std::vector<Symbol> path;
        for (auto & i : sel->getAttrPath()) {
            if (!i.symbol)
                return std::nullopt;
            path.push_back(i.symbol);
        }
        return std::pair{var, std::move(path)};
    };

    auto select = [&](const ExprSelect & sel, Level depth) {
        if (auto sp = staticPath(sel)) {
            variable(*sp->first, depth, std::move(sp->second));
            return;
        }
        collectFreeVars(sel.e, depth, out);
        if (sel.def)
            collectFreeVars(sel.def, depth, out);
        for (auto & i : sel.getAttrPath())
            if (!i.symbol)
                collectFreeVars(i.expr, depth, out);
    };

    auto attrsAt = [&](const ExprAttrs & attrs, Level plain, Level inherited, Level inheritedFrom) {
        /* `inherit (from) a b;`: `from` is evaluated once into an
           environment of its own; when it is a variable, only the named
           attributes are read from it. */
        std::vector<bool> fromUsedWhole(attrs.inheritFromExprs ? attrs.inheritFromExprs->size() : 0, false);
        for (auto & [name, def] : *attrs.attrs) {
            if (def.kind != AttrKind::InheritedFrom) {
                collectFreeVars(def.e, def.kind == AttrKind::Plain ? plain : inherited, out);
                continue;
            }
            auto * sel = dynamic_cast<const ExprSelect *>(def.e);
            auto * from = sel ? dynamic_cast<const ExprInheritFrom *>(sel->e) : nullptr;
            if (from && from->displ < fromUsedWhole.size()) {
                auto * fromExpr = (*attrs.inheritFromExprs)[from->displ];
                if (auto sp = staticPath(*fromExpr);
                    sp && sel->getAttrPath().size() == 1 && sel->getAttrPath()[0].symbol) {
                    sp->second.push_back(sel->getAttrPath()[0].symbol);
                    variable(*sp->first, plain, std::move(sp->second));
                    continue;
                }
                fromUsedWhole[from->displ] = true;
                continue;
            }
            collectFreeVars(def.e, inheritedFrom, out);
        }
        if (attrs.inheritFromExprs)
            for (size_t n = 0; n < attrs.inheritFromExprs->size(); ++n)
                if (fromUsedWhole[n])
                    collectFreeVars((*attrs.inheritFromExprs)[n], plain, out);
        for (auto & dyn : *attrs.dynamicAttrs) {
            collectFreeVars(dyn.nameExpr, plain, out);
            collectFreeVars(dyn.valueExpr, plain, out);
        }
    };

    if (auto * var = dynamic_cast<const ExprVar *>(e)) {
        variable(*var, depth, {});
    } else if (
        dynamic_cast<const ExprInt *>(e) || dynamic_cast<const ExprFloat *>(e) || dynamic_cast<const ExprString *>(e)
        || dynamic_cast<const ExprPath *>(e) || dynamic_cast<const ExprPos *>(e)
        || dynamic_cast<const ExprBlackHole *>(e)) {
    } else if (auto * sel = dynamic_cast<const ExprSelect *>(e)) {
        select(*sel, depth);
    } else if (auto * has = dynamic_cast<const ExprOpHasAttr *>(e)) {
        collectFreeVars(has->e, depth, out);
        for (auto & i : has->attrPath)
            if (!i.symbol)
                collectFreeVars(i.expr, depth, out);
    } else if (auto * attrs = dynamic_cast<const ExprAttrs *>(e)) {
        if (attrs->recursive)
            attrsAt(*attrs, depth + 1, depth, depth + 2);
        else
            attrsAt(*attrs, depth, depth, depth + 1);
    } else if (auto * list = dynamic_cast<const ExprList *>(e)) {
        for (auto * i : list->elems)
            collectFreeVars(i, depth, out);
    } else if (auto * lambda = dynamic_cast<const ExprLambda *>(e)) {
        if (auto formals = lambda->getFormals())
            for (auto & i : formals->formals)
                if (i.def)
                    collectFreeVars(i.def, depth + 1, out);
        collectFreeVars(lambda->body, depth + 1, out);
    } else if (auto * call = dynamic_cast<const ExprCall *>(e)) {
        collectFreeVars(call->fun, depth, out);
        for (auto * i : *call->args)
            collectFreeVars(i, depth, out);
    } else if (auto * let = dynamic_cast<const ExprLet *>(e)) {
        attrsAt(*let->attrs, depth + 1, depth, depth + 2);
        collectFreeVars(let->body, depth + 1, out);
    } else if (auto * with = dynamic_cast<const ExprWith *>(e)) {
        collectFreeVars(with->attrs, depth, out);
        collectFreeVars(with->body, depth + 1, out);
    } else if (auto * if_ = dynamic_cast<const ExprIf *>(e)) {
        collectFreeVars(if_->cond, depth, out);
        collectFreeVars(if_->then, depth, out);
        collectFreeVars(if_->else_, depth, out);
    } else if (auto * assert_ = dynamic_cast<const ExprAssert *>(e)) {
        collectFreeVars(assert_->cond, depth, out);
        collectFreeVars(assert_->body, depth, out);
    } else if (auto * not_ = dynamic_cast<const ExprOpNot *>(e)) {
        collectFreeVars(not_->e, depth, out);
    } else if (auto * concat = dynamic_cast<const ExprConcatStrings *>(e)) {
        for (auto & [pos, i] : concat->es)
            collectFreeVars(i, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpEq *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpNEq *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpAnd *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpOr *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpImpl *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpConcatLists *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (auto * op = dynamic_cast<const ExprOpUpdate *>(e)) {
        collectFreeVars(op->e1, depth, out);
        collectFreeVars(op->e2, depth, out);
    } else if (dynamic_cast<const ExprLazyApp *>(e)) {
        /* `EvalState::mkLazyApp()`: the function and its argument. */
        if (depth == 0) {
            out.vars.push_back({0, 0, nullptr, Symbol(), {}});
            out.vars.push_back({0, 1, nullptr, Symbol(), {}});
        }
    } else {
        out.unknown = true;
    }
}

} // namespace

const CellTable::FreeVars & CellTable::freeVars(const Expr * e)
{
    auto [i, inserted] = freeVarsCache.try_emplace(e);
    if (inserted)
        collectFreeVars(e, 0, i->second);
    return i->second;
}

bool CellTable::equivalentEnvironments(EvalState & state, const Expr * e, Env & before, Env & now)
{
    if (&before == &now)
        return true;
    auto & fv = freeVars(e);
    if (fv.unknown)
        return false;
    auto key = std::tuple{(const Env *) &before, (const Env *) &now, e};
    if (auto [i, inserted] = equivalentEnvs.try_emplace(key, true); !inserted)
        return i->second;
    /* An infinite recursion (a value that depends on the result of the
       cell being validated) propagates; the pair is then undecided. */
    Finally undo([&]() {
        if (std::uncaught_exceptions())
            equivalentEnvs.erase(key);
    });

    auto result = [&]() {
        for (auto & var : fv.vars) {
            Env *a = &before, *b = &now;
            for (auto l = var.level; l; --l) {
                a = a->up;
                b = b->up;
                if (!a || !b)
                    return a == b;
            }
            if (a == b)
                continue;
            if (!var.fromWith) {
                Value *va = a->values[var.displ], *vb = b->values[var.displ];
                if (va != vb && !(va && vb && equivalentAt(state, *va, *vb, var.path)))
                    return false;
                continue;
            }
            /* A `with` variable: the attribute in the innermost enclosing
               `with` that defines it (see `EvalState::lookupVar`). */
            for (auto * with = var.fromWith;;) {
                Value *wa = a->values[0], *wb = b->values[0];
                try {
                    state.forceAttrs(*wa, noPos, "");
                    state.forceAttrs(*wb, noPos, "");
                } catch (Interrupted &) {
                    throw;
                } catch (InfiniteRecursionError &) {
                    throw;
                } catch (BaseError &) {
                    return false;
                }
                auto ja = wa->attrs()->get(var.name);
                auto jb = wb->attrs()->get(var.name);
                if (bool(ja) != bool(jb))
                    return false;
                if (ja) {
                    if (ja->value != jb->value && !equivalentAt(state, *ja->value, *jb->value, var.path))
                        return false;
                    break;
                }
                if (!with->parentWith)
                    break;
                for (auto l = with->prevWith; l; --l) {
                    a = a->up;
                    b = b->up;
                    if (!a || !b)
                        return a == b;
                }
                if (a == b)
                    break;
                with = with->parentWith;
            }
        }
        return true;
    }();
    equivalentEnvs[key] = result;
    return result;
}

bool CellTable::equivalentAt(EvalState & state, Value & before, Value & now, std::span<const Symbol> path)
{
    /* Follow the attributes as far as both sides are forced attribute
       sets; from there the values are compared as a whole (an unforced
       old value is compared as a thunk, which never forces it). */
    Value *a = &before, *b = &now;
    for (auto name : path) {
        if (a == b)
            return true;
        if (a->type() != nAttrs)
            break;
        /* The old value was forced, so the new one may be. */
        try {
            state.forceValue(*b, noPos);
        } catch (Interrupted &) {
            throw;
        } catch (InfiniteRecursionError &) {
            throw;
        } catch (BaseError &) {
            return false;
        }
        if (b->type() != nAttrs)
            return false;
        auto ja = a->attrs()->get(name);
        auto jb = b->attrs()->get(name);
        if (!ja || !jb)
            return bool(ja) == bool(jb);
        a = ja->value;
        b = jb->value;
    }
    return equivalent(state, *a, *b);
}

bool CellTable::sameLambda(EvalState & state, const ExprLambda * a, const ExprLambda * b)
{
    if (a == b)
        return true;
    if (state.positions[a->pos] != state.positions[b->pos])
        return false;
    auto shown = [&](const ExprLambda * e) -> const std::string & {
        auto [i, inserted] = shownCache.try_emplace(e);
        if (inserted) {
            std::ostringstream str;
            e->show(state.symbols, str);
            i->second = str.str();
        }
        return i->second;
    };
    auto & sa = shown(a);
    /* `__curPos` is the one construct whose value is not determined by
       the text and the starting position. */
    if (sa != shown(b) || sa.find("__curPos") != std::string::npos)
        return false;
    /* The same text may read other slots of the enclosing environments
       (an edited `let` around it), and callers compare environments with
       the free variables of one side. */
    auto sameVar = [](const FreeVar & x, const FreeVar & y) {
        if (x.level != y.level || x.displ != y.displ || x.name != y.name || x.path != y.path)
            return false;
        /* `with` variables: the same chain of enclosing `with`s. */
        auto *wx = x.fromWith, *wy = y.fromWith;
        for (; wx && wy; wx = wx->parentWith, wy = wy->parentWith)
            if (wx->prevWith != wy->prevWith)
                return false;
        return wx == wy;
    };
    auto &fa = freeVars(a), &fb = freeVars(b);
    return fa.unknown == fb.unknown && std::ranges::equal(fa.vars, fb.vars, sameVar);
}

bool CellTable::equivalent(EvalState & state, Value & before, Value & now)
{
    if (&before == &now)
        return true;
    auto key = std::pair<const void *, const void *>{&before, &now};
    if (auto [i, inserted] = equivalentValues.try_emplace(key, true); !inserted)
        return i->second;
    bool undecided = false;
    Finally undo([&]() {
        if (undecided || std::uncaught_exceptions())
            equivalentValues.erase(key);
    });
    if (equivalenceDepth >= maxEquivalenceDepth) {
        undecided = true;
        return false;
    }
    equivalenceDepth++;
    Finally shallower([&]() { equivalenceDepth--; });
    auto result = compareValues(state, before, now);
    /* Out of budget, the pair is undecided, like one too deep. */
    undecided = !result && equivalenceForces > maxEquivalenceForces;
    equivalentValues[key] = result;
    return result;
}

bool CellTable::compareValues(EvalState & state, Value & before, Value & now)
{
    if (before.isBlackhole() || now.isBlackhole())
        return false;

    auto unforced = [](Value & v) { return v.isThunk() || v.isApp(); };

    if (unforced(before) && unforced(now)) {
        if (before.isThunk() && now.isThunk() && before.thunk().expr == now.thunk().expr)
            return equivalentEnvironments(state, before.thunk().expr, *before.thunk().env, *now.thunk().env);
        if (before.isApp() && now.isApp())
            return equivalent(state, *before.app().left, *now.app().left)
                   && equivalent(state, *before.app().right, *now.app().right);
        /* Different expressions (typically an edited file): compare what
           they evaluate to. */
    } else if (unforced(before)) {
        /* Nothing forced the old value; forcing it now may do anything
           (fetch an input, fail): undecided, hence not equivalent. */
        return false;
    } else if (unforced(now)) {
        /* The old value was forced, so the new one may be, within the
           validation's budget. */
        if (++equivalenceForces > maxEquivalenceForces)
            return false;
    }

    /* The failure of `v`, if forcing it fails. */
    auto force = [&](Value & v) -> std::optional<std::string> {
        try {
            state.forceValue(v, noPos);
            return std::nullopt;
        } catch (Interrupted &) {
            throw;
        } catch (InfiniteRecursionError &) {
            throw;
        } catch (BaseError & e) {
            return failureOf(e);
        }
    };
    auto failedBefore = force(before), failedNow = force(now);
    if (failedBefore || failedNow)
        return failedBefore == failedNow;

    /* A proxy function of some cell's port stands for the function that
       port currently denotes. */
    auto unwrap = [&](Value & v) -> Value * {
        if (!v.isPrimOpApp() || v.primOpApp().left != *vPortCall)
            return &v;
        auto & port = *dynamic_cast<PortRef &>(*v.primOpApp().right->external()).port;
        if (port.stale)
            port.refresh(state);
        return force(*port.backing) ? nullptr : port.backing;
    };
    auto *a = unwrap(before), *b = unwrap(now);
    if (!a || !b)
        return false;
    if (a != &before || b != &now)
        return equivalent(state, *a, *b);

    if (before.type() != now.type())
        return false;

    switch (before.type()) {
    case nInt:
    case nFloat:
    case nBool:
    case nString:
    case nPath:
    case nNull:
    case nExternal:
        return samePrimitive(before, now);

    case nAttrs: {
        auto *a = before.attrs(), *b = now.attrs();
        if (a == b)
            return true;
        auto key = std::pair<const void *, const void *>{a, b};
        if (auto [i, inserted] = equivalentValues.try_emplace(key, true); !inserted)
            return i->second;
        Finally undo([&]() {
            if (std::uncaught_exceptions())
                equivalentValues.erase(key);
        });
        auto result = [&]() {
            if (a->size() != b->size())
                return false;
            for (auto i = a->begin(), j = b->begin(); i != a->end(); ++i, ++j) {
                /* Positions are observable (`unsafeGetAttrPos`); an
                   expression parsed again (e.g. `call-flake.nix`) has new
                   indices for the same locations. */
                if (i->name != j->name || (i->pos != j->pos && state.positions[i->pos] != state.positions[j->pos]))
                    return false;
                if (i->value != j->value && !equivalent(state, *i->value, *j->value))
                    return false;
            }
            return true;
        }();
        equivalentValues[key] = result;
        return result;
    }

    case nList: {
        if (before.listSize() != now.listSize())
            return false;
        auto va = before.listView(), vb = now.listView();
        for (size_t n = 0; n < va.size(); ++n)
            if (va[n] != vb[n] && !equivalent(state, *va[n], *vb[n]))
                return false;
        return true;
    }

    case nFunction:
        if (before.isLambda() && now.isLambda()) {
            if (!sameLambda(state, before.lambda().fun, now.lambda().fun))
                return false;
            return equivalentEnvironments(state, before.lambda().fun, *before.lambda().env, *now.lambda().env);
        }
        if (before.isPrimOp() && now.isPrimOp())
            return before.primOp() == now.primOp();
        if (before.isPrimOpApp() && now.isPrimOpApp())
            return equivalent(state, *before.primOpApp().left, *now.primOpApp().left)
                   && equivalent(state, *before.primOpApp().right, *now.primOpApp().right);
        return false;

    case nThunk:
    case nFailed:
        return false;
    }
    unreachable();
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
    /* The value a port denotes, for a port; the value itself otherwise
       (a cell's own value compared with a port that denotes it: the same
       object in a cold evaluation). */
    auto underlying = [&](const Value & v) -> const Value * {
        /* Ports of nested cells denote ports of the enclosing cell:
           follow the chain to the end. */
        const Value * u = &v;
        for (size_t hops = 0; hops < 1000; ++hops) {
            auto i = canonicalPorts.find(u);
            if (i == canonicalPorts.end())
                break;
            auto * port = i->second;
            if (port->stale)
                port->refresh(state);
            if (port->split)
                port->invalidate(state, "a value compared by a reused traced cell is now several values");
            u = port->backing;
        }
        return u;
    };
    if (&a == &b)
        return true;
    auto *ua = underlying(a), *ub = underlying(b);
    return (ua == &a && ub == &b) ? false : ua == ub;
}

nlohmann::json CellTable::statsJson() const
{
    size_t ports = 0, calls = 0, fileReads = 0;
    for (auto * cell : instances) {
        ports += cell->ports.size();
        fileReads += cell->fileReads.size();
        for (auto * port : cell->ports)
            calls += port->kind == ExprPort::Kind::Call;
    }
    return {
        {"instances", instances.size()},
        {"ports", ports},
        {"calls", calls},
        {"fileReads", fileReads},
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
