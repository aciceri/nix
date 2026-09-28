#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json_fwd.hpp>

#include <map>
#include <set>
#include <functional>
#include <span>
#include <tuple>
#include <unordered_map>

namespace nix {

struct CellInstance;

/**
 * The root of an unlocked input (the flake being evaluated, a
 * `--override-input` pointing to a local tree) in a long-lived evaluator.
 *
 * Every change to such a tree gives it a new store path, so path values,
 * file identities and positions of its files would change with every
 * edit. Instead, the tree is mounted at a virtual store path derived from
 * its identity (the unlocked input), and every string that names a file
 * of the current tree is converted to that virtual path when it becomes a
 * path value (`EvalState::rootPath()`). Strings keep the virtual path
 * inside the evaluator and are realized where they leave it
 * (`EvalState::realizeStrings()`), so results are those of a cold
 * evaluation.
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

    /**
     * Paths are shown as the current tree's accessor shows them (the
     * checkout, or its store path), as a cold evaluation does.
     */
    std::string showPath(const CanonPath & path) override;
    std::pair<CanonPath, std::optional<std::string>> getFingerprint(const CanonPath & path) override;
    void invalidateCache() override;
};

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
    /** Copy to the store (`builtins.path`, a path in a string): the resulting store path. */
    Copy,
    /** A path rendered as a string: the store path of the root. */
    Render,
    /** Something that cannot be replayed (a copy with references): never valid again. */
    Opaque,
};

/**
 * A file read by a traced cell, see `FileReadKind`.
 */
struct FileRead
{
    StableRoot * root;
    FileReadKind kind;
    /** The (virtual) path. */
    Symbol path;
    Symbol fingerprint;
    /** Real prefix of the root when last checked: equal contents. */
    Symbol checkedAt;
    /** `Copy`: the filter function, if any. */
    Value * filter = nullptr;
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

    /**
     * Real prefix to root; null for a prefix that several roots have
     * (identical contents), which is then not known under either.
     */
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
     * File cells: the context (a `CellInstance` without a function) in
     * which a file of a stable root was evaluated. It collects the file
     * reads made while evaluating the file and forcing thunks created at
     * its top level, so that the evaluated file can be kept when its tree
     * changes. By path.
     */
    boost::unordered_flat_map<
        std::string,
        CellInstance *,
        boost::hash<std::string>,
        std::equal_to<std::string>,
        traceable_allocator<std::pair<const std::string, CellInstance *>>>
        fileCells;

    /**
     * Version of the evaluated value of a file of a stable root: increased
     * whenever it is dropped. An importer's `Import` read records the
     * version of the value it holds; it stays valid while the file's text
     * is the same and the reads of that version are (`importValid()`),
     * even if the file was evaluated again in between (an edit undone).
     */
    boost::unordered_flat_map<std::string, uint64_t> fileVersions;

    /**
     * What a dropped version of a file's value depended on: the reads of
     * its file cell when it was dropped, by path and version.
     */
    std::map<
        std::pair<std::string, uint64_t>,
        std::vector<FileRead, gc_allocator<FileRead>>,
        std::less<std::pair<std::string, uint64_t>>,
        traceable_allocator<
            std::pair<const std::pair<std::string, uint64_t>, std::vector<FileRead, gc_allocator<FileRead>>>>>
        fileHistory;
    static constexpr size_t maxFileHistory = 8;

    /**
     * Whether the value an `Import` read obtained is still what the import
     * gives: the path resolves to the same file, whose text is the
     * recorded one, and every read of that value's version holds.
     * Memoised per generation; a cycle counts as valid.
     */
    bool importValid(EvalState & state, const FileRead & read);
    boost::unordered_flat_map<std::string, bool> importValidity;
    uint64_t importValidityGeneration = 0;

    uint64_t fileVersion(const std::string & path) const;

    /**
     * After `root` changed: keep the evaluated files under it whose
     * contents and reads (including the versions of the files they
     * import) are unchanged, drop the others.
     */
    void revalidateFiles(EvalState & state, StableRoot & root);

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
 * A port of a traced cell (doc/traced-cells/DESIGN.md, section 3):
 * a value that the cell obtained from its argument. The cell never sees the
 * argument's values directly; it sees `canonical`, a thunk that resolves the
 * port in the generation the cell is bound to.
 *
 * Resolving a port forces its backing value and records a summary the
 * first time: primitives are copied, attribute sets and lists become
 * proxies whose elements are child ports, functions become proxies whose
 * applications are call ports, and the cell's own pre-existing objects
 * (reached again through a call's result, e.g. `prev.stdenv` in an
 * overlay) are read directly and must be the same objects. A reused cell
 * therefore only holds primitives that were checked to be equal to the
 * current ones, and proxies that read the current argument. Calls of a
 * function whose closure is equivalent (`CellTable::equivalent()`) are
 * not applied again.
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
     * `Summary::Failed`: the error, as validation compares it: its message
     * and whether `builtins.tryEval` catches it.
     */
    Symbol failure;

    /**
     * `Kind::Call`: the function port and the argument it was applied to.
     */
    ExprPort * fn = nullptr;
    Value * callArg = nullptr;

    /**
     * `Kind::Child`: the first proxy that contained this port, and the
     * attribute name or list index.
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
     * `Summary::Function`: the function last validated, and whether the
     * current binding's function is an equivalent closure, in which case
     * the call ports of this function keep their results.
     */
    Value function;
    bool functionUnchanged = false;

    /**
     * `Summary::Function`: the cell got the function itself rather than a
     * proxy (a function of another cell, or a primop): its calls are not
     * recorded, and validation requires an equivalent closure.
     */
    bool direct = false;

    /**
     * `Summary::Identity`: the value is an object of this cell's own
     * (created in its context, not by a call through one of its ports),
     * reached through a port. The cell reads it directly; validation
     * checks that the same object is reached: the `Bindings`, or the
     * lambda and its environment.
     */
    const void * identity[2] = {nullptr, nullptr};

    /**
     * Observed `builtins.functionArgs` of a function port.
     */
    bool functionArgsObserved = false;
    std::vector<std::pair<Symbol, bool>, gc_allocator<std::pair<Symbol, bool>>> functionArgs;

    /**
     * Child ports in proxy order (attribute sets: sorted by name; lists:
     * by index).
     */
    std::vector<ExprPort *, gc_allocator<ExprPort *>> slots;
    std::vector<Symbol, gc_allocator<Symbol>> slotNames;

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

    /**
     * In the current binding, the paths that led to one value lead to
     * different values, each checked against the observations. The port
     * then stands for no single object: a new read through it or through
     * a stale child invalidates the cell.
     */
    bool split = false;

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

    /**
     * Mark the instance unusable and throw `CellInvalidated`.
     */
    [[noreturn]] void invalidate(EvalState & state, std::string_view reason);

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
    CellOwner self{this, false}, inCall{this, true};

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
    const void * boundArgs = nullptr;
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
     * The body is being evaluated; and the instance was given up while
     * computing (`CellTable::giveUp()`).
     */
    bool computing = false;
    bool gaveUp = false;

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

    /**
     * Dropped from `CellTable::instances` (`CellTable::forget()`): its
     * ports are not registered in `CellTable::canonicalPorts`, which
     * would keep their addresses after they are collected.
     */
    bool forgotten = false;

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

    /**
     * Instances kept overall.
     */
    static constexpr size_t maxInstances = 128;

    static constexpr size_t maxPorts = 100000;
    /**
     * Instances kept per function, call site and call ordinal.
     */
    static constexpr size_t maxPerCall = 32;
    std::vector<CellInstance *, traceable_allocator<CellInstance *>> instances;

    /**
     * `instances` by site (function and call site), rebuilt whenever
     * `instances` is pruned.
     */
    std::map<std::pair<Symbol, Symbol>, std::vector<CellInstance *>> instancesBySite;

    /**
     * Key of `instancesBySite`: the function's position rendered without
     * the tree's store path, and the call site. A file parsed again (after
     * a retry from scratch, or an edit elsewhere in it) yields a new
     * `ExprLambda` for the same code; instances are matched by
     * `sameLambda()` and, when the function's environment is a new object
     * too (a file evaluated again), by `equivalentEnvironments()`.
     */
    std::pair<Symbol, Symbol> siteKeyOf(EvalState & state, const ExprLambda & lambda, Symbol callSite);

    void indexInstances(EvalState & state);

    /**
     * Instances tried per application: the one with the same ordinal and
     * the most recently bound ones. Trying every instance of a site with
     * thousands of them (per-option cells) is quadratic.
     */
    static constexpr size_t maxCandidatesTried = maxPerCall;

    RootValue vPortCall;
    Stats stats;

    explicit CellTable(EvalState & state);

    /**
     * No instance is looked up or created (an evaluation that gave up on
     * cells after repeated invalidations).
     */
    bool suspended = false;

    /**
     * No validation may defer a check (a request being evaluated again
     * after a deferred check failed: another deferred failure would cost
     * a third evaluation; cells whose checks cannot be done eagerly are
     * computed again instead).
     */
    bool noDeferrals = false;

    /**
     * Give up on `cell` (too many ports while computing): its site takes
     * the normal path from now on, and its remaining reads are direct.
     */
    void giveUp(EvalState & state, CellInstance & cell);

    /**
     * Directory of the flake being evaluated by the current request; its
     * `outputs` are not a cell (its `self` changes with every edit).
     */
    std::string excludedRoot;

    /**
     * Called for every newly evaluated file with its parsed expression
     * and value: registers cell sites (the file-level lambda with formals
     * of `pkgs/top-level/impure.nix`, i.e. `import nixpkgs { ... }`, and
     * the `outputs` lambda of every `flake.nix`).
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
     * be evaluated again after `dropGeneration()`.
     */
    bool checkDeferred(EvalState & state);

    void clear();

    /**
     * Drop the instances created or bound in generation `gen` (a request
     * being evaluated again from scratch: they may have been computed
     * from, or validated against, an instance that turned out invalid).
     * Instances of earlier generations that the request did not touch
     * stay; their own validation protects them.
     */
    void dropGeneration(EvalState & state, uint64_t gen);

    /**
     * Whether `now` evaluates like `before` did: a bisimulation over the
     * two value graphs. Forced values must have equal contents, element by
     * element; an unforced thunk must have the same expression and an
     * equivalent environment (in the slots the expression can reach), or
     * both sides are forced and compared. Lambdas are the same expression
     * with equivalent environments. Results are memoised for the
     * `Comparison`, and a pair under comparison counts as equivalent
     * (coinduction), so cyclic graphs terminate.
     *
     * Sound because evaluation is deterministic in the expression, the
     * environment and the file system, which reused cells check
     * separately.
     */
    bool equivalent(EvalState & state, Value & before, Value & now);

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

    /**
     * A variable an expression reads from the environment it is evaluated
     * in (or, for `with` variables, from the enclosing `with` expressions).
     */
    struct FreeVar
    {
        Level level;
        Displacement displ;
        ExprWith * fromWith;
        Symbol name;
        /**
         * Attributes selected statically from the variable (`lib.foo.bar`,
         * `inherit (lib) foo`): only that part of the value is compared.
         * Empty: the whole value.
         */
        std::vector<Symbol> path;
    };

    struct FreeVars
    {
        std::vector<FreeVar> vars;
        /**
         * The expression has a kind this analysis does not know; its
         * environments are only equivalent if they are the same.
         */
        bool unknown = false;
    };

    const FreeVars & freeVars(const Expr * e);

    nlohmann::json statsJson() const;

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
     * Call sites where a deferred check failed once (typically a flake's
     * `outputs` reading its own `self.outPath`, which changes with every
     * edit of an unlocked input). A deferred check that fails at the end
     * of the request costs a second evaluation, so instances there are
     * rejected as soon as a check has to be deferred.
     */
    std::set<std::pair<Symbol, Symbol>> noDeferralSites;

    /**
     * Key of `noDeferralSites`: the function's position and the call
     * site, both rendered without the tree's store path, so that the
     * entry survives a re-parse of the file (a retry from scratch drops
     * the parsed files).
     */
    std::pair<Symbol, Symbol> deferralKey(EvalState & state, const CellInstance & cell);

    struct PointerPairHash
    {
        size_t operator()(const std::pair<const void *, const void *> & p) const noexcept
        {
            return std::hash<const void *>{}(p.first) * 0x9E3779B97F4A7C15ull ^ std::hash<const void *>{}(p.second);
        }
    };

    struct EnvPairHash
    {
        size_t operator()(const std::tuple<const Env *, const Env *, const Expr *> & t) const noexcept
        {
            auto [a, b, e] = t;
            return (std::hash<const void *>{}(a) * 0x9E3779B97F4A7C15ull ^ std::hash<const void *>{}(b))
                       * 0x9E3779B97F4A7C15ull
                   ^ std::hash<const void *>{}(e);
        }
    };

    /**
     * Memo of `equivalent()` for the current `Comparison`: pairs of values,
     * and pairs of environments with the expression they were compared
     * for.
     */
    boost::unordered_flat_map<std::pair<const void *, const void *>, bool, PointerPairHash> equivalentValues;
    boost::unordered_flat_map<std::tuple<const Env *, const Env *, const Expr *>, bool, EnvPairHash> equivalentEnvs;

    std::unordered_map<const Expr *, FreeVars> freeVarsCache;

    /**
     * Whether two lambda expressions are the same code at the same
     * position: the same expression, or (a file parsed again after an
     * edit elsewhere in it) the same text and the same location, so that
     * every position inside is the same too.
     */
    bool sameLambda(EvalState & state, const ExprLambda * a, const ExprLambda * b);
    std::unordered_map<const Expr *, std::string> shownCache;

    /**
     * Nesting of `equivalent()`; beyond `maxEquivalenceDepth` a pair is
     * left undecided (not equivalent, not memoised) rather than risking
     * the stack, which the evaluation that triggered the validation is
     * already using.
     */
    size_t equivalenceDepth = 0;
    static constexpr size_t maxEquivalenceDepth = 400;

    /**
     * Values forced on behalf of comparisons during one `Comparison`: a new
     * value may be forced where the old one was (the previous evaluation
     * did that work, this one would too), within this budget; beyond it a
     * pair is undecided.
     */
    size_t equivalenceForces = 0;
    static constexpr size_t maxEquivalenceForces = 50000;

    /**
     * A validation, or a comparison of environments by `lookup()`. The
     * thunks it forces must not keep their errors (see
     * `EvalState::handleEvalExceptionForThunk`). It has a memo of its own,
     * whose keys are addresses of values that may be collected after it,
     * and a forcing budget of its own. Comparisons nest (forcing a value
     * may validate another cell): the enclosing one's are restored after.
     */
    struct Comparison
    {
        EvalState & state;
        CellTable & table;
        decltype(equivalentValues) values;
        decltype(equivalentEnvs) envs;
        size_t forces;
        Comparison(EvalState & state, CellTable & table);
        ~Comparison();
    };

    /**
     * Drop `cell`'s ports from `canonicalPorts`, for good (see
     * `CellInstance::forgotten`).
     */
    void forget(CellInstance & cell);

public:
    /**
     * Lambdas that a cell once called directly and that then turned out
     * not to be equivalent closures across requests (typically because
     * their environment holds per-request metadata, like a dirty tree's
     * `lastModified` in `lib.version`). Ports for them are proxies from
     * then on, so that their calls can be replayed instead of rejecting
     * the cell.
     */
    boost::unordered_flat_set<const ExprLambda *> proxiedLambdas;

private:

    bool equivalentEnvironments(EvalState & state, const Expr * e, Env & before, Env & now);
    bool equivalentAt(EvalState & state, Value & before, Value & now, std::span<const Symbol> path);
    bool compareValues(EvalState & state, Value & before, Value & now);

    /**
     * Replay the observations of `cell` against `rootBacking`. Eager
     * (`final` false): observations that hit infinite recursion are
     * deferred and the binding is committed. Final: everything must check;
     * nothing is committed.
     */
    bool validate(EvalState & state, CellInstance & cell, Value * rootBacking, std::string & why, bool final);

    /**
     * Find or create the instance of `fun` at `callSite` for the root
     * value `arg` (`argsId` identifies equal arguments within a
     * generation). `compute` evaluates the body given the cell and what
     * the root port resolved to.
     */
    bool lookup(
        EvalState & state,
        Value & fun,
        Value & arg,
        const void * argsId,
        Symbol callSite,
        Value & vRes,
        std::function<void(CellInstance &, Value & vArgs, Value & result)> compute);
};

} // namespace nix
