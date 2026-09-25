#pragma once
///@file

#include "nix/expr/eval.hh"
#include "nix/expr/eval-gc.hh"

#include <boost/unordered/unordered_flat_map.hpp>
#include <boost/unordered/unordered_flat_set.hpp>
#include <nlohmann/json_fwd.hpp>

#include <set>

namespace nix {

struct CellInstance;

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
    enum class Summary : uint8_t { None, Primitive, Attrs, List, Function, Failed };

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
     * Validation deferred some observations of the current binding; they
     * are checked by `CellTable::checkDeferred()`.
     */
    bool deferred = false;

    /**
     * Number of ports at the start of the current generation (diagnostics).
     */
    size_t portsAtStart = 0;

    /**
     * Position of the call that bound this instance among the calls at
     * the same call site in its generation. Evaluation is deterministic,
     * so the call with the same ordinal in the next generation is usually
     * the one this instance should serve; it is tried first.
     */
    size_t ordinal = 0;

    /**
     * Set when the result was reached in a generation in which this
     * instance was not bound, i.e. through the result of another reused
     * cell. Such an instance must keep its binding and is never offered for
     * reuse again. Also set when the evaluation of the body failed.
     */
    bool unusable = false;

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
     * Called for every newly evaluated file: registers its value as a cell
     * site if the path matches and the value is a lambda with formals.
     */
    void registerFile(const SourcePath & path, Value & v);

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
    boost::unordered_flat_set<std::pair<ExprLambda *, Env *>> sites;

    /**
     * Call sites whose instances exceeded `maxPorts`: an argument that the
     * cell uses everywhere (for example an overlay that replaces `lib`)
     * makes the trace as large as the evaluation. Calls there take the
     * normal path.
     */
    std::set<std::tuple<ExprLambda *, Env *, Symbol>> untraceable;

    /**
     * Calls per call site in the current generation.
     */
    boost::unordered_flat_map<Symbol, size_t, std::hash<Symbol>> callsInGeneration;

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
