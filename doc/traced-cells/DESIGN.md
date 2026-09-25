# Traced cells: an incrementally invalidated, resident Nix evaluator

Status: design only, nothing implemented. Branch `traced-cells` on top of
upstream `master` (2.36.0 pre-release). Line numbers below refer to this
tree at the branch point.

Companion measurements live in the `universe` repository under
`projects/fasteval/NOTES.md` (evaluation profile of a NixOS workstation,
derivation churn between edits, a userspace stub cache, a resident
`nix repl` experiment that reuses the nixpkgs instance).

## 1. Problem

A cold evaluation of one NixOS workstation (`pike`, 44 k `derivationStrict`
calls, two Home Manager profiles, flake-parts) costs ~10 s of CPU with the
current evaluator, ~14-18 s wall through `nixos-rebuild`/`nh`. Between two
typical edits of a module file, 19 of 15 209 derivations in the closure
change; the other 99.9% are recomputed from scratch on every run. Neither
parallel evaluation (Determinate, `parallel-eval`) nor faster interpretation
changes this: a NixOS toplevel is one demand chain, and no single C++ hot
spot exceeds 20%. Reusing work between runs is the only lever that scales
with the size of the edit instead of the size of the configuration.

Two non-fork approximations were measured:

- an overlay returning cached derivation shells for 99 leaf packages:
  -23% CPU, identical toplevel drvPath;
- a resident `nix repl` re-evaluating the module system with the same
  `pkgs` object: 19.5 s → 6.5 s after a flake reload, identical drvPath.

The second one is what this design makes automatic and safe: reuse every
value whose recorded inputs did not change, not just the nixpkgs instance,
and later persist it.

## 2. Goals and non-goals

Goals

- Same results as a cold evaluation, always. The definition of "same" is
  the toplevel `drvPath` plus every observable value (strings with
  contexts, attrsets, lists, functions by identity).
- Resident process: one `EvalState` survives across evaluations; edits
  invalidate only what depends on them.
- After a module edit on `pike`: evaluation ≤ 3 s (today 10 s CPU).
- Recording overhead on a cold run ≤ 20%.
- No change to the Nix language or to nixpkgs.

Non-goals (for the first iterations)

- Persistence across processes (section 9 keeps the door open).
- Parallel evaluation. The design is single-threaded; the concurrent maps
  in `EvalState` are used as they are.
- Speculation.

## 3. Vocabulary

- **Generation**: one evaluation request served by the resident process.
  Files are re-fingerprinted at the start of a generation.
- **Input**: something the evaluator reads from the outside world: file
  contents, directory listings, path existence, environment variables,
  `currentSystem`, fetcher results, store queries. Inputs are identified by
  content, not by store path (a re-copied source tree has the same inputs).
- **Cell**: a memoised computation with a key, a trace and a result. Cells
  are created at a small number of syntactic points (section 5).
- **Port**: an argument of a cell that is not compared by value when the
  cell is looked up, because it is large, lazy or cyclic (`pkgs`, `config`,
  `lib`, `inputs`, `self`). Ports are compared by what the cell observed
  through them (the trace).
- **Observation**: one read through a port: an attribute path and a summary
  of what was found (a primitive hash, the identity of a cell result, the
  identity of a function, a set of attribute names, "missing").
- **Trace**: the ordered list of observations and input reads made while
  computing a cell, including those made later when unforced parts of the
  result were demanded.

## 4. The core idea

Everything expensive in a NixOS evaluation is the application of a
file-level lambda to an attribute set: `callPackage` applying a package
file to its arguments, `import nixpkgs { ... }`, every NixOS/HM module
applied to `{ config, lib, pkgs, ... }`, `lib.evalModules`. Those
applications are the cells. Their arguments are either small values
(hashed into the key) or ports.

Reusing a cell result across generations is correct if two conditions
hold:

1. every observation the cell ever made through its ports gives the same
   summary in the new generation, and every input it read is unchanged;
2. any *future* observation made by still-unforced thunks inside the
   reused result reads the new generation's data, not the old one.

Condition 1 is checked by replaying the trace against the new ports.
Condition 2 is what makes reuse of a *lazy* value sound: it is obtained by
making ports indirections. A port is a proxy value whose backing value is
looked up in a per-generation table; closures captured the proxy, so old
thunks forced in a new generation read new data. Forced thunks inside the
result were computed from old data, but everything they read was recorded
in the trace and verified by condition 1, so they are consistent with the
new data.

This is the "verifying traces" model of build systems (Salsa, Adapton,
Shake) applied to a lazy language, with two Nix-specific choices: cells are
placed at lambda-with-formals applications, and non-primitive arguments are
ports by default.

## 5. Cells

### 5.1 Kinds

| Kind | Created at | Key | Result |
|---|---|---|---|
| File | `EvalState::evalFile` (`src/libexpr/eval.cc:1160`) | content hash of the file + its path relative to the root of its input (plus `mustBeTrivial`) | the file's value (usually a lambda) |
| Call | `EvalState::callFunction`, lambda branch (`src/libexpr/eval.cc:1608`), only for lambdas **with formals** whose argument is an attrset | lambda identity + argument descriptor | WHNF of the body |
| Derivation | `prim_derivationStrict` (`src/libexpr/primops.cc:1425`) | hash of the fully forced, context-annotated attribute set | `{ drvPath, <outputs> }` |
| Option (phase 3) | inside `lib.modules` merge, see 5.5 | option declaration identity + identities of the definition thunks | merged option value |

File cells replace `fileEvalCache`'s keying by `SourcePath` with keying by
content hash and root-relative path; the `SourcePath → key` map is the
per-generation fingerprint table. This alone makes a re-copied flake source
(new `/nix/store/…-source` path after each edit) hit the cache for every
unchanged file. The relative path is part of the key because a file's
value depends on its location: path literals (`./foo.nix`), `import` and
`__curPos` resolve against it, so two identical `default.nix` files in
different directories must not share a cell. The input root is the flake
source tree or the locked input it belongs to; files outside any input
(absolute paths, `NIX_PATH` lookups) are keyed by absolute path.

Derivation cells are cheap insurance: when a call cell misses (for example
a package whose file changed only in a comment), `mkDerivation` still runs
but `derivationStrict` does not rewrite the `.drv`. They are keyed by the
same content that `hashDerivationModulo` sees. Computing the key already
forces the whole attribute set, so they save only `hashDerivationModulo`
and the store write (~3 s wall on `pike`, 8.6), not evaluation.

### 5.2 Lambda identity

`(file cell key of the file that contains the lambda, byte offset of the
lambda in the file)`. Both are stable across store copies. The offset comes
from `PosIdx`/`PosTable` (`src/libexpr/include/nix/expr/pos-table.hh`)
whose origin is the `SourcePath` of the parse.

Lambdas created by evaluating an expression inside a cell also carry the
id of the **owning cell instance** (the cell whose body created the
closure). This is how functions observed through ports are compared: two
function values are "the same" if they have the same lambda identity and
the same owning cell instance. The instance id, not the key, is required:
several instances can share a key with different traces (5.3), and their
closures capture different port contents. An instance keeps its id for as
long as it is reused; a re-evaluation creates a new instance and therefore
new function identities. Lambdas created outside any cell belong to a
per-generation root instance, so they never match across generations
(conservative).

### 5.3 Argument descriptor

For each formal of the lambda, in order, the argument's value is
summarised as:

- primitive (`int`, `float`, `bool`, `null`, string without context, path):
  its hash;
- string with context: hash of string and context;
- a value that is the result of a cell: the cell id;
- a function: lambda identity + owning cell id;
- anything else (attrset, list, thunk): a **port**.

Ellipsis (`...`) and `@` patterns: the whole argument attrset becomes an
additional port so that `args.foo` accesses through `args` are traced.

The key is the lambda identity plus the hash of the non-port entries plus
the number and positions of the ports. Several cells with the same key but
different traces can coexist (the same package file applied to different
`pkgs`): lookup tries each candidate's trace in LRU order.

### 5.4 Ports

A port is a new `Value` kind `tPort` (a `pdSingleDWord` slot is available,
`src/libexpr/include/nix/expr/value.hh:633-760`; the enum has room). Its
payload is a port id; `EvalState` holds `portTable[generation][id] →
Value *`. Every operation that inspects a value (`forceValue`, `forceAttrs`,
`ExprSelect::eval`, `lookupVar` for `with`, `ExprOpHasAttr`, primops such
as `attrNames`, `functionArgs`, `typeOf`, `isAttrs`, comparison, `toString`,
`derivationStrict`) resolves a `tPort` through the table and records the
observation before proceeding. Resolution yields the backing value for the
current generation; the port itself is never overwritten.

Port-derived values are ports too. Selecting a non-primitive attribute
from a port yields a **derived port**, a `tPort` whose id names `(parent
port id, attribute name or list index)`, instead of the backing value
itself. The per-generation table resolves derived ports lazily (parent
resolved first, then one selection) and memoises the result for the
generation. So `let cfg = config.services.foo; in ... cfg.enable ...`
stores a derived port in `cfg`; when a thunk of a reused result forces
`cfg.enable` in a later generation it reads the new `config`, and the
read is recorded as the observation `(config, [services, foo, enable])`.
Without this, `cfg` would hold the old generation's attrset, later reads
through it would neither see new data nor be traced, and reuse of lazy
results would be unsound.

Selection results that are *not* wrapped: primitives (returned as they
are, after being observed), cell results (identity observed, 6.2) and
lambdas with an owning cell instance (identity observed). Everything else
(attrsets and lists that are not cell results, primop applications,
lambdas without an owner) is a derived port.

Child extraction: operations that copy children out of a port-backed
container without forcing them (`//`, `attrValues`, `mapAttrs`,
`listToAttrs` over a port list, `++`, `map`, `elemAt`, `head`, `concatLists`,
`genericClosure`, `with` variable lookup) wrap each copied child as a
derived port `(container port, name or index)`, so that the copies stay
indirections. The cost is one small allocation per copied child, paid only
for containers reached through a port.

Passing a port-derived value into another cell puts a "port path"
reference into that cell's descriptor. The inner cell's own trace records
what it observed; the outer cell's trace records that it selected the path.

### 5.5 Option cells (phase 3)

`lib.evalModules` is a call cell whose key contains the identities of all
module values, so any module edit misses it and the whole module system is
recomputed (this is the 6.5 s residual of the resident-repl experiment).
To go below that, the merge of one option becomes a cell:

- key: option declaration identity (module file cell id + attribute path)
  plus the identities of the definition thunks (each definition is
  "attribute path `x.y.z` of the result of module cell M");
- ports: `config`, `options`, `pkgs`, `lib`, `specialArgs`;
- result: the merged value.

Module cells themselves are ordinary call cells (`{ config, lib, pkgs, ...
}: ...` applied to the module arguments, all ports). Editing `niri.nix`
changes one module cell id; only options with a definition from that
module, and cells that observed those options, are recomputed. This
requires cooperation from `lib/modules.nix`: the merge function must be
recognisable (a builtin marker or a `__cell` attribute on the merge
lambda). It is the one place where nixpkgs is involved, and it is opt-in.

## 6. Tracing

### 6.1 What is recorded

While a cell is being computed (a stack of "current cell" in `EvalState`,
pushed at cell entry and at every force of a thunk that belongs to a
cell's result), the following append to the current cell's trace:

- port observations (5.4);
- input reads: `readFile`, `readDir`, `pathExists`, `readFileType`,
  `findFile`/`nixPath`, `getEnv`, `currentSystem`, `currentTime`,
  `fetchTree`/`fetchGit`/`fetchTarball`/`fetchurl`/`fetchClosure`
  (`src/libexpr/primops.cc`, `src/libexpr/primops/fetchTree.cc`; each
  primop gets a `bool readsInput` flag next to `experimentalFeature` in
  `PrimOp`, `src/libexpr/include/nix/expr/eval.hh`), `import` (a file cell
  reference), `storePath`, `unsafeGetAttrPos`/`__curPos` (section 8.3);
- identities of cells it called (so that validation can short-circuit:
  if a called cell is still valid in the new generation, its result
  identity is unchanged).

Thunks belong to the cell in which they were allocated (`allocValue` is
tagged with the current cell; a side table or a bit in the allocation
batch). Forcing a thunk of cell C while cell D is current pushes C: the
observation is charged to C, and D records "observed C's result", which is
an identity comparison at validation time.

### 6.2 Summaries

An observation's summary must be comparable across generations without
holding the old value:

- primitives: 64-bit hash of type + value (+ context for strings);
- attrset: the sorted list of attribute names (hash) — only the names, the
  values are observed separately when selected;
- list: length; elements observed separately when indexed or iterated
  (`map`, `foldl'` etc. iterate: each element access is an observation
  `(port, path ++ [i])`);
- cell result: cell id;
- function: lambda identity + owning cell id;
- missing attribute: a distinguished value.

### 6.3 Validation

At lookup of a cell in generation g:

1. compute the key from the current arguments (descriptor); candidates =
   cells with that key, most recently valid first;
2. for a candidate, if it was already validated in g, reuse;
3. otherwise replay the trace: for each input read, compare the current
   fingerprint; for each observation, resolve the path on the *current*
   port backing value and compare summaries. Resolving a path may force
   thunks in the new generation; those forcings are themselves cells or
   cheap selections, so the cost is proportional to what changed
   (bottom-up verification with early cutoff);
4. on success mark valid-in-g and return the old result object;
   on failure try the next candidate, then evaluate normally, creating a
   new cell (the old one stays for LRU eviction).

Trace replay must stop at the first mismatch to bound the cost of a
guaranteed miss.

## 7. Resident process

- A `nix eval-daemon` command (new file `src/nix/eval-daemon.cc`) built on
  the same pieces as `nix repl` (`src/libcmd/repl.cc`: one `EvalState`,
  `loadFlake`, `processLine`): a Unix socket, one request per line,
  `eval <flakeref>#<attrpath>` → prints the value (or drvPath), `stats`,
  `gc`, `quit`.
- Per request: lock the flake, fingerprint the repository files (a few
  hundred `readFile` + hash), map input store paths to their narHash from
  the lock (nixpkgs is never re-hashed), bump the generation, evaluate.
- `nixos-rebuild`/`nh` integration: a wrapper asks the daemon for the
  drvPath, then runs `nix build <drvPath>` and the normal activation.
  A `--verify` mode also runs a cold evaluation in a child process and
  compares drvPaths; this is the acceptance test during development.
- Memory: cells hold `RootValue`s (`src/libexpr/include/nix/expr/root-value.hh`)
  so Boehm keeps results alive. Eviction: cells not validated for N
  generations are dropped; an explicit `gc` request drops everything but
  the last generation's valid set.

## 8. Complications and how each is handled

### 8.1 `with`

`lookupVar` (`src/libexpr/eval.cc:933`) resolves `with`-bound names by
`attrs()->get` on the with-target. If the target is port-derived, that is
an observation `(port, path ++ [name])`, including the "missing" outcome for
inner `with` levels that shadow. No semantic change.

### 8.2 Functors, primop applications, partial application

`callFunction` handles `__functor` by recursion and primops by arity; only
the lambda-with-formals branch creates cells. A partially applied lambda
(`tApp` chain) is forced through `forceValue` (`eval-inline.hh:115`) into
`callFunction`, so it is covered.

### 8.3 Positions and `__curPos`

`mkPos` (`src/libexpr/eval.cc:990`) renders `file` as the accessor-absolute
path, which contains the store hash of the copied source. Two consecutive
generations copy the repository to different store paths, so any value
derived from `__curPos` or `unsafeGetAttrPos` differs and would invalidate
its dependents (the `universe` flake uses `getCurrentDir __curPos` in
every project). Options: (a) evaluate the repository through `lazy-trees`
(no copy, stable virtual path); (b) render positions relative to the
flake root inside the daemon. (a) is the intended path; (b) is a fallback
that changes observable strings and is therefore off by default.

### 8.4 Errors, `tryEval`, `abort`, `throw`

A cell whose evaluation throws is stored with the error as its result
(the `isFailed` state already exists in `Value`, `eval-inline.hh:120`) and
replays the same error on reuse. `tryEval` observes the failure as a
primitive. `builtins.break` and `trace` are side effects: `trace` replays
nothing on reuse (documented), `break` disables cell creation while a
debugger is attached.

### 8.5 String contexts

Contexts are part of the value summary. `unsafeDiscardStringContext`
produces a different summary from its input, as it should.

### 8.6 Derivation writing

`derivationStrict` writes the `.drv` to the store on a miss. On reuse the
daemon does not touch the store; before returning a toplevel drvPath the
front end runs one batched `queryValidPaths` on the cached drv closure and
re-writes any path the store lost (garbage collected). This replaces the
~3 s of per-derivation writes measured on `pike`.

### 8.7 Fetchers and IFD

`fetchTree` results are inputs keyed by the locked input attributes; the
existing `inputCache` (`src/libexpr/eval.hh`) already memoises them per
process. Import-from-derivation is `import` of a store path: a file cell
whose input is the built output; the build itself is not the daemon's
business.

### 8.8 Impure evaluation

`getEnv`, `currentTime`, `currentSystem` are inputs on an environment port
whose fingerprint is taken per generation. `currentTime` makes every cell
that reads it miss on every generation, which is correct.

### 8.9 GC and identity

Cell results are kept alive by `RootValue`; everything else is ordinary
Boehm-managed memory. `Value *` addresses are never identities: port and
derived-port ids index the per-generation resolution table (whose entries
are `RootValue`s or `traceable_allocator` storage), and cross-generation
identities are cell instance ids and lambda identities.

### 8.10 Threads

Upstream `master` has concurrent maps but a single evaluation thread. The
design assumes one thread; the trace stack is a plain member of
`EvalState`.

## 9. Persistence (later)

Keys, traces and summaries are already pointer-free, so they serialise as
they are. Results are not: a cell result is a live graph with unforced
thunks. The persistent form stores the WHNF of the result with children as
either primitives, cell references, or "lazy fallback" markers that force a
cold evaluation of that thunk when first demanded (the same shape as the
userspace stub experiment). A cold start then loads keys and traces from
SQLite, validates against the current inputs, and materialises results on
demand. Nothing in sections 4-8 has to change; persistence is a second
backing store for the cell table.

## 10. Changes to upstream code, by file

| File | Change |
|---|---|
| `src/libexpr/include/nix/expr/value.hh` | new `tPort` internal type (single-dword slot) |
| `src/libexpr/include/nix/expr/eval.hh` | `CellTable`, `PortTable`, `TraceStack`, `Fingerprints` members; `PrimOp::readsInput` |
| `src/libexpr/include/nix/expr/eval-inline.hh` | `forceValue`: resolve ports, push cell ownership on thunk force |
| `src/libexpr/eval.cc` | `evalFile` → file cells by content hash; `callFunction` lambda branch → call cells; `ExprSelect::eval`, `lookupVar`, `ExprOpHasAttr::eval` → observations; `ExprLambda::eval` → owning cell tag; `mkPos` (8.3) |
| `src/libexpr/primops.cc`, `primops/fetchTree.cc`, `primops/context.cc` | input flags and recording; `derivationStrict` cells; `attrNames`/`functionArgs`/`typeOf`/… observations on ports |
| `src/libexpr/traced-cells.{hh,cc}` (new) | cell/trace/summary data structures, validation, eviction, statistics |
| `src/libcmd/repl.cc` | factor the resident `EvalState` + flake loading for reuse |
| `src/nix/eval-daemon.cc` (new) | socket front end, generations, `--verify` |
| `doc/manual/source/command-ref/new-cli/nix3-eval-daemon.md` | documentation |
| `tests/functional/traced-cells/` | see section 11 |

Rough size: 4-6 k lines including tests. The evaluator core is touched in
five functions; the rest is additive.

## 11. Plan and experiments

Each phase ends with a measurement on `pike` and a drvPath comparison
against a cold `nix-instantiate --readonly-mode`.

- **P0** (harness): `--verify` comparator and a benchmark script that
  edits a module, requests an evaluation, restores the file, over the
  `universe` hosts. Acceptance: reproducible cold numbers.
  Done as `projects/fasteval/_experiments/cycle.py` in `universe`: edits
  are patch files applied to a scratch clone, evaluators are compared in
  paired ABBA order, and every result is checked against a cold reference
  drvPath cached per git tree hash (`refs.tsv`). That check is the
  comparator until the daemon exists; `nix eval-daemon --verify` (a cold
  evaluation in a child process) comes with P1.
- **P1** (resident + file cells): daemon skeleton, file cells keyed by
  content hash and root-relative path, generations, fingerprinting.
  Expected gain: parsing only (~5%). Acceptance: identical drvPath over 20
  edit/restore cycles; RSS stable.
- **P1b** (nixpkgs instance cell, no traces): a special case of a call cell
  for the top-level `import nixpkgs { ... }` keyed structurally (overlay
  owning file hashes + config hash + system). Reproduces the resident-repl
  result (19.5 s → 6.5 s) automatically. Acceptance: same numbers,
  identical drvPath, invalidation when an overlay file changes.
- **P2** (general call cells, ports, traces, validation): the real thing
  for packages and everything outside the module system. Expected:
  ≤ 6.5 s after a module edit, misses limited to the changed files and
  their dependents. Measure recording overhead on cold runs (target ≤ 20%).
- **P3** (option cells): needs the `lib/modules.nix` marker. Expected:
  ≤ 3 s after a module edit.
- **P4** (persistence): cold start from SQLite ≤ 1.5x warm.

Risks, in order: trace size on cells that iterate large ports
(`lib.mapAttrs` over `pkgs`); allocation cost of derived ports on child
extraction (5.4); validation cost approaching evaluation cost
on deep `config` observation chains; `__curPos` without lazy trees;
memory growth without aggressive eviction; subtle identity bugs producing
different drvPaths (mitigated by `--verify` on every deploy during
development).
