#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json_fwd.hpp>

#include <map>
#include <set>

namespace nix {

struct CellInstance;

/**
 * Operations on files of an unlocked input whose result a traced cell
 * depends on. Each is recorded as "operation on a path = fingerprint of
 * the result" and replayed on the current tree by validation.
 */
enum class FileReadKind : uint8_t {
    /** `import`: the resolved file (a directory resolves to `default.nix`) and its content. */
    Import,
    /** Symlink resolution of a path by `realisePath()`: the resolved path. */
    Resolve,
    /** `readFile`, `hashFile`: the content. */
    Content,
    /** `pathExists`: whether the path exists (as a directory, if requested). */
    Exists,
    /** `pathExists` of a path that must be a directory. */
    ExistsDir,
    /** `readFileType`: the type. */
    Type,
    /** `readDir`: the names and types of the entries. */
    Dir,
    /** Copy to the store without a filter: the resulting store path. */
    Copy,
    /** A path rendered as a string: the store path of the root. */
    Render,
    /** Something that cannot be replayed (a filtered copy): never valid again. */
    Opaque,
};

/**
 * The root of an unlocked input (the flake being evaluated, a
 * `--override-input` pointing to a local tree) in a long-lived evaluator.
 *
 * Every change to such a tree gives it a new store path, so path values,
 * file identities and positions of its files would change with every
 * edit. Instead, the tree is mounted at a virtual store path derived from
 * its identity (the unlocked input), and every string that names a file
 * of the current tree is converted to that virtual path when it becomes a
 * path value (`EvalState::rootPath()`). The reverse conversion happens
 * wherever a path value becomes a string (`EvalState::pathToString()`),
 * so strings, and therefore results, are those of a cold evaluation.
 */
struct StableRoot : SourceAccessor
{
    std::string identity;

    /** `/nix/store/<hash>-<name>`, stable. */
    std::string virtualPrefix;

    /** Store path of the current contents. */
    std::string realPrefix;

    /** The contents of the current generation. */
    ref<SourceAccessor> target;

    /** Generation in which the current contents were mounted. */
    uint64_t generation = 0;

    StableRoot(std::string identity, std::string virtualPrefix, ref<SourceAccessor> target);

    void anchor() override;

    using SourceAccessor::readFile;
    void readFile(const CanonPath & path, Sink & sink, fun<void(uint64_t)> sizeCallback) override;
    bool pathExists(const CanonPath & path) override;
    std::optional<Stat> maybeLstat(const CanonPath & path) override;
    DirEntries readDirectory(const CanonPath & path) override;
    std::string readLink(const CanonPath & path) override;
    std::optional<std::filesystem::path> getPhysicalPath(const CanonPath & path) override;
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;
    void invalidateCache() override;
};

/**
 * The stable roots of an `EvalState`.
 */
struct StableRoots
{
    /** The store directory of the evaluator's store. */
    const std::string storeDir;

    explicit StableRoots(std::string storeDir)
        : storeDir(std::move(storeDir))
    {
    }

    /** `<storeDir>/<hash>-<name>` of a path in the store, or empty. */
    std::string_view storePrefix(std::string_view path) const;

    /** By identity. */
    std::map<std::string, ref<StableRoot>> byIdentity;

    /** By virtual prefix. */
    boost::unordered_flat_map<std::string, StableRoot *> byVirtual;

    /** Current generation's real prefix to root. */
    boost::unordered_flat_map<std::string, StableRoot *> byReal;

    /**
     * Content hashes of evaluated files of stable roots, per generation.
     */
    boost::unordered_flat_map<std::string, std::string> contentHashes;

    /**
     * Parsed files of stable roots by path and content hash, so that an
     * unchanged file keeps its expressions (and thus lambda identities
     * and positions) when its tree changes.
     */
    boost::unordered_flat_map<std::string, std::pair<std::string, Expr *>> parsed;

    /**
     * Mount `accessor` (the contents of an unlocked input whose store path
     * is `storePath`) at the virtual path of `identity`.
     */
    void
    mount(EvalState & state, const std::string & identity, const StorePath & storePath, ref<SourceAccessor> accessor);

    /** The stable root containing `path`, if any. */
    StableRoot * findVirtual(std::string_view path);

    /** The stable root whose current contents contain `path`, if any. */
    StableRoot * findReal(std::string_view path);
};

/**
 * A file read by a traced cell, see `FileReadKind`.
 */
struct FileRead
{
    StableRoot * root;
    FileReadKind kind;
    /** Path relative to the root. */
    Symbol path;
    Symbol fingerprint;
    /** Real prefix of the root when last checked: equal contents. */
    Symbol checkedAt;
    /** `Copy`: the filter function, if any. */
    Value * filter = nullptr;
};

/**
 * The fingerprint of `read` in the current tree, in the format in which it
 * was recorded (see `EvalState::recordFileRead()`).
 */
std::string fileReadFingerprint(EvalState & state, const FileRead & read);

/**
 * Hash of file contents used in fingerprints.
 */
std::string contentFingerprint(std::string_view contents);

/**
 * Fingerprint of a directory listing (names and types).
 */
std::string dirFingerprint(const SourceAccessor::DirEntries & entries);

/**
 * A port of a traced cell (doc/traced-cells/DESIGN.md, sections 4 and 5.4):
 * a value that the cell obtained from its argument. The cell never sees the
 * argument's values directly; it sees `canonical`, a thunk that resolves the
 * port in the generation the cell is bound to.
 *
 * Resolving a port forces its backing value and records a summary the
 * first time: primitives are copied, attribute sets and lists become
 * proxies whose elements are child ports, functions become proxies whose
 * applications are call ports. A reused cell therefore only holds
 * primitives that were checked to be equal to the current ones, and
 * proxies that read the current argument.
 *
 * Child ports are keyed by the address of their backing value, so two
 * paths to the same value give the same `canonical` value, like in a cold
 * evaluation (`==` short-cuts on pointer equality). Validation checks that
 * this aliasing structure is unchanged.
 */
struct ExprPort : Expr, gc
{
    enum class Kind : uint8_t { Root, Child, Call };
    enum class Summary : uint8_t { None, Primitive, Attrs, List, Function, Identity, Failed };

    CellInstance * cell;
    Kind kind;
    Summary summary = Summary::None;

    /**
     * `Kind::Call`: the function port and the argument it was applied to.
     */
    ExprPort * fn = nullptr;
    Value * callArg = nullptr;

    /**
     * `Kind::Child`: the first proxy that contained this port, and the
     * attribute name or list index. Only for diagnostics.
     */
    ExprPort * parent = nullptr;
    Symbol name;
    size_t index = 0;

    /**
     * The thunk through which the cell sees this port.
     */
    Value * canonical;

    /**
     * The value this port denotes in the generation the cell is bound to.
     */
    Value * backing = nullptr;

    /**
     * `backing` is from an earlier binding: validation deferred this port
     * (or an ancestor). Refreshed from the parent or function port when
     * resolved.
     */
    bool stale = false;

    /**
     * `Summary::Primitive`: the observed value. `Attrs`, `List`,
     * `Function`: the proxy handed to the cell.
     */
    Value observed;

    /**
     * `Summary::Identity`: the value is an attribute set or a function
     * created by another cell; the cell reads it directly, and validation
     * only checks that the new value is the same object (the `Bindings`,
     * or the lambda and its environment). Pointer identity is always
     * sound; it is used where it will hold across requests: for values of
     * another cell's result, which are the same objects while that cell is
     * reused.
     */
    const void * identity[2] = {nullptr, nullptr};

    /**
     * Child ports in proxy order (attribute sets: sorted by name; lists:
     * by index).
     */
    std::vector<ExprPort *, gc_allocator<ExprPort *>> slots;
    std::vector<Symbol, gc_allocator<Symbol>> slotNames;

    /**
     * Observed `builtins.functionArgs` of a function port.
     */
    bool functionArgsObserved = false;
    std::vector<std::pair<Symbol, bool>, gc_allocator<std::pair<Symbol, bool>>> functionArgs;

    /**
     * Observed `builtins.unsafeGetAttrPos` results on an attribute set port.
     */
    std::vector<std::pair<Symbol, PosIdx>, gc_allocator<std::pair<Symbol, PosIdx>>> attrPositions;

    /**
     * External value carrying this port, argument of `__portCall`.
     */
    Value * ref = nullptr;

    /**
     * `==` looked at the identity of `canonical` (pointer equality, or a
     * comparison of functions). Only then must validation preserve which
     * ports share a value.
     */
    bool identityObserved = false;

    ExprPort(EvalState & state, CellInstance & cell, Kind kind);

    void eval(EvalState & state, Env & env, Value & v) override;
    void show(const SymbolTable & symbols, std::ostream & str) const override;

    /**
     * A path like `args.config.overlays[2](…)`, for diagnostics.
     */
    std::string describe(const SymbolTable & symbols) const;

    /**
     * Force the backing value, record the summary if this is the first
     * resolution, and store what the cell sees in `v`.
     */
    void resolve(EvalState & state, Value & v);

    /**
     * Recompute `backing` from the parent or function port.
     */
    void refresh(EvalState & state);

private:
    void observe(EvalState & state, Value & b);
};

/**
 * A memoised application of a cell site (a file-level lambda with formals)
 * to an attribute set. Garbage collected: ports point to their instance,
 * so an instance stays alive while any of its ports is reachable, even
 * after `CellTable` dropped it.
 */
struct CellInstance : gc
{
    ExprLambda * lambda;
    Env * env;
    Symbol callSite;

    Value * result = nullptr;

    /**
     * All ports in creation order; parents and function ports precede the
     * ports derived from them.
     */
    std::vector<ExprPort *, gc_allocator<ExprPort *>> ports;
    ExprPort * root = nullptr;

    using PortMap = boost::unordered_flat_map<
        Value *,
        ExprPort *,
        std::hash<Value *>,
        std::equal_to<Value *>,
        gc_allocator<std::pair<Value * const, ExprPort *>>>;

    /**
     * Ports by backing value, for the current binding.
     */
    PortMap byBacking;

    /**
     * Proxy attribute sets, for `unsafeGetAttrPos`.
     */
    boost::unordered_flat_map<
        const Bindings *,
        ExprPort *,
        std::hash<const Bindings *>,
        std::equal_to<const Bindings *>,
        gc_allocator<std::pair<const Bindings * const, ExprPort *>>>
        proxies;

    uint64_t boundGen = 0;
    const Bindings * boundArgs = nullptr;
    Value * boundArg = nullptr;

    /**
     * The cell in whose context this one was created; file reads are also
     * charged to it, because its result may contain this one's.
     */
    CellInstance * parent = nullptr;

    /**
     * Files of unlocked inputs read while computing this cell's result
     * (see `FileReadKind`), and a set to skip duplicates.
     */
    std::vector<FileRead, gc_allocator<FileRead>> fileReads;
    boost::unordered_flat_set<uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, gc_allocator<uint64_t>>
        fileReadKeys;

    /**
     * Validation deferred some observations of the current binding; they
     * are checked by `CellTable::checkDeferred()`.
     */
    bool deferred = false;

    /**
     * Number of ports at the start of the current generation (diagnostics).
     */
    size_t portsAtStart = 0;

    /**
     * Position of the call that bound this instance among the calls of
     * the same function at the same call site in its generation. Evaluation is deterministic,
     * so the call with the same ordinal in the next generation is usually
     * the one this instance should serve; it is tried first.
     */
    size_t ordinal = 0;

    /**
     * Set while the body is evaluated and when that fails.
     */
    bool unusable = false;

    /**
     * The result was reached in a generation before this instance was
     * bound in it, i.e. through the result of another reused cell. Its
     * ports were then resolved against the previous binding; validation
     * replays those reads too, so rebinding is sound, but only for the
     * same logical call: such an instance is only offered to the call
     * with the same ordinal at its site.
     */
    bool usedNested = false;

    /**
     * True while this instance is being validated: replaying calls forces
     * the instance's own thunks, which may resolve its ports.
     */
    bool validating = false;

    ExprPort * newPort(EvalState & state, ExprPort::Kind kind);
    ExprPort * childFor(EvalState & state, Value * backing, ExprPort * parent, Symbol name, size_t index);
    void noteUse(EvalState & state);
};

/**
 * Traced cells of a long-lived `EvalState` (`nix eval-daemon`).
 */
struct CellTable
{
    struct Stats
    {
        uint64_t hits = 0, misses = 0, rejected = 0, deferred = 0, invalidated = 0, untraceable = 0;
        double validationSeconds = 0;
        std::string lastRejection;
    };

    std::vector<std::string> fileSuffixes;
    size_t maxInstances;
    static constexpr size_t maxPorts = 100000;
    std::vector<CellInstance *, traceable_allocator<CellInstance *>> instances;
    RootValue vPortCall;
    Stats stats;

    CellTable(EvalState & state, std::vector<std::string> fileSuffixes, size_t maxInstances);

    /**
     * Also make the `outputs` function of every `flake.nix` a cell site,
     * except for the flake being evaluated (`excludedRoot`): the outputs
     * of unchanged locked inputs are then reused across requests.
     */
    bool flakeOutputs = true;

    /**
     * Directory of the flake being evaluated by the current request; its
     * `outputs` are not a cell (its `self` changes with every edit).
     */
    std::string excludedRoot;

    /**
     * Called for every newly evaluated file with its parsed expression
     * and value: registers cell sites (the file-level lambda with formals
     * of files whose path matches, the `outputs` lambda of `flake.nix`).
     */
    void registerFile(EvalState & state, const SourcePath & path, Expr * e, Value & v);

    /**
     * Apply the cell site `fun` to `arg`, reusing an instance if one
     * validates. Returns false if the call must take the normal path (for
     * example because it fails the formals check).
     */
    bool call(EvalState & state, Value & fun, Value & arg, Value & vRes, PosIdx pos);

    /**
     * Evict least recently used instances. Only safe between evaluations.
     */
    void startGeneration(EvalState & state);

    /**
     * Check the observations that validation deferred because they could
     * not be evaluated yet (infinite recursion: they depend on a value that
     * was being computed when the cell was looked up). Call after the
     * request's result is computed. Returns false if an instance turned
     * out to be invalid; such instances are dropped, and the request must
     * be evaluated again after `retry()`.
     */
    bool checkDeferred(EvalState & state);

    /**
     * Release the bindings of the current generation so that a new
     * evaluation of the same request can bind instances again.
     */
    void retry();

    void clear();

    /**
     * If `v` is a proxy function, record a `functionArgs` observation and
     * return the backing function.
     */
    Value * observeFunctionArgs(EvalState & state, Value & v);

    /**
     * If `attrs` is a proxy, record an `unsafeGetAttrPos` observation and
     * return the position of `name` in the backing attribute set.
     */
    std::optional<PosIdx> attrPos(const Bindings * attrs, Symbol name);

    /**
     * Called by `EvalState::eqValues` when its result depends on the
     * identity of `v`.
     */
    void observeIdentity(const Value & v);

    /**
     * Whether `a` and `b` are port values that currently denote the same
     * value; `==` must then treat them as identical, as a cold evaluation
     * would.
     */
    bool sameBacking(EvalState & state, const Value & a, const Value & b);

    /**
     * Ports by the address of their canonical value.
     */
    boost::unordered_flat_map<const Value *, ExprPort *> canonicalPorts;

    nlohmann::json statsJson() const;

    nlohmann::json instancesJson(const EvalState & state) const;

private:
    boost::unordered_flat_set<ExprLambda *> flakeOutputSites;

    /**
     * Call sites whose instances exceeded `maxPorts`: an argument that the
     * cell uses everywhere (for example an overlay that replaces `lib`)
     * makes the trace as large as the evaluation. Calls there take the
     * normal path.
     */
    std::set<std::tuple<ExprLambda *, Env *, Symbol>> untraceable;

    /**
     * Calls per call site and function in the current generation.
     */
    std::map<std::pair<Symbol, ExprLambda *>, size_t> callsInGeneration;

    /**
     * Replay the observations of `cell` against `rootBacking`. Eager
     * (`final` false): observations that hit infinite recursion are
     * deferred and the binding is committed. Final: everything must check;
     * nothing is committed.
     */
    bool validate(EvalState & state, CellInstance & cell, Value * rootBacking, std::string & why, bool final);
    bool create(EvalState & state, Value & fun, Value & arg, Value & vRes, Symbol callSite, size_t ordinal);
};

} // namespace nix
