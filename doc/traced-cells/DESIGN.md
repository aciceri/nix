# Traced cells: reusing evaluation results across requests

`nix eval-daemon` keeps one evaluator alive and serves evaluation requests
for a flake that is being edited. This document describes how results of
one request are reused in the next, and what limits the reuse. The user
interface is documented in the command's manual page.

## 1. Problem

A NixOS configuration is evaluated whole after every edit, even though an
edit touches one file and most of the evaluation (the package set, the
outputs of input flakes, the module system) does not depend on it. A
resident evaluator can keep values between requests only if it can tell
which of them are still valid; Nix values carry no dependency
information, and an unchanged expression may give a different value when
something it reads (an attribute of an argument, a file of the tree) has
changed.

Goals: results identical to a cold evaluation for every request; work
proportional to the edit for common edits; no changes to the language and
no cooperation from the evaluated code. Non-goal: reusing results across
daemon restarts (see section 9).

## 2. Files: stable roots

Files of locked inputs are content-addressed store paths: an unchanged
input is the same path, and its parsed and evaluated files can be kept
across requests without further checks (`fileEvalCache`, `parsed`).

Files of the flake being edited, or of an input overridden with a local
checkout, get a new store path at every edit. Such an input is mounted at
a *stable root*: a virtual store path derived from the input's identity,
whose accessor forwards to the store path of the current contents
(`StableRoots::mount`). Paths inside the tree are therefore the same
value across edits, and only files whose text changed, or whose reads
changed, are evaluated again (`StableRoots::revalidateFiles`).

Every file of a stable root is evaluated in the context of a *file cell*
that records what the evaluation read from stable roots: imports (with
the imported file's content hash and the version of its value),
contents, existence, symlink resolution, listings and copies
(`FileReadKind`, `EvalState::recordFileRead`). The thunks of the file's
top-level expression live in a copy of the base environment owned by the
file cell, so that reads they make when forced later belong to the file.
When the tree changes, files whose text changed are dropped, then files
whose reads no longer hold, to a fixed point; the version of a dropped
file's value is increased and the reads of the dropped version are kept
(`fileHistory`), so that an importer's `Import` read stays valid when an
edit is undone and the same text comes back (`importValid`).

### Lazy rendering

`toString ./x` and `"${./x}"` inside such a tree yield the virtual path.
Realizing it to the current store path at that moment would make every
string that mentions a path of the tree depend on the whole tree.
Instead strings stay virtual inside the evaluator and are realized where
they leave it (`EvalState::realizeStrings`, `realizeContext`): derivation
attributes, `builtins.toFile`, `builtins.hashString`, names of store
objects derived from a rendered path (`baseNameOf (toString src)`, as
`lib.cleanSource` does), the request's result, error messages and log
messages (which do not record a dependency). Virtual and real prefixes
have the same length, so positions inside strings are stable. A value
that let a rendered path leave the evaluator records a `Render` read and
depends on the tree's store path; a value that only compared or
concatenated paths does not.

## 3. Cells and ports

A *cell site* is a function application chosen for reuse: the function
of `pkgs/top-level/impure.nix` (`import nixpkgs { ... }`) and
the `outputs` function of every locked flake input. An *instance* of a
site is one application: the function, its environment, the argument it
was given and the result (`CellInstance`).

The cell does not receive its argument. It receives a *port*
(`ExprPort`), a proxy that resolves to the argument lazily and records
every observation the cell makes: the type, attribute names, list
length, primitive value, the result of calling a function of the
argument (an overlay, `allowUnfreePredicate`), the position of an
attribute. Selecting an attribute of a port gives a port for the
attribute; calling a function through a port records the call and gives
a port for the result. The records form a tree rooted at the argument
(`ExprPort::Summary`, `ports`).

Reads are *structural*: the summary of a value is what the cell observed
of it, never the identity of the object. Two arguments that answer the
same questions are the same argument. The exception is the cell's own
pre-existing objects (`Summary::Identity`, tracked through `CellOwner`):
a value the cell created itself is compared by identity, since it is the
cell's own and cannot have changed. Ownership is only recorded when
`EvalSettings::traceCells` is set before the evaluator is created (the
daemon does): the owner goes in a word before every `Env`
(`EvalMemory::allocEnv`, `ownerOf`) and in `Bindings::owner`; otherwise
allocating an environment takes one extra branch.

Functions that another cell created (an overlay of an input flake's
`lib`) are not proxied. They are compared as closures
(`CellTable::equivalent`): the same code (text and position) with
equivalent free variables, recursively, within a budget of forced values
(`maxEquivalenceForces`). A comparison never forces a value on the old
side that was not forced before; such a case is undecided and counts as
a difference.

## 4. Lookup and validation

At a cell site (`CellTable::lookup`) the instances of the same function
and call site are candidates, most recently used first. Each candidate is
validated against the new argument (`CellTable::validate`): every
recorded read is replayed against the new value and must give the
recorded answer; every file read must hold in the current tree; closures
must be equivalent. A candidate that passes is *bound* to the new
argument: its ports now resolve to the new argument, so values the cell
had not looked at yet come from it, and further observations are added to
the record. The first candidate that passes is reused; otherwise the
application is computed and becomes a new instance. Instances are bounded
per site and in total (`maxPerCall`, `maxInstances`), oldest dropped.

Instances survive a re-parse of their file: they are indexed by the
function's rendered position and matched by `sameLambda` (same text at
the same position) and, when the function's environment is a new object
too, by environment equivalence.

### Deferred checks

A read may depend on the request's result (a NixOS option read by an
overlay, `self` of the flake being evaluated): replaying it while the
result is being computed would recurse. Such reads are deferred and
checked when the request is done (`CellTable::checkDeferred`). If one
fails, the request is evaluated again from scratch: nothing computed in
the attempt can be kept, since it may derive from the invalid instance.
The cached files and the instances created or bound in that generation
are dropped (`dropGeneration`); instances the request did not touch
stay. During the retry no check may be deferred (a cell whose check
would be deferred is computed again), and the site that failed rejects
eagerly from then on (`noDeferralSites`), so a request costs at most two
evaluations. An infinite recursion hit by a port resolved while a
validation is in progress is the signal to defer, not a failed value.

### Lazy invalidation

A port bound to a new argument may find, when resolved later, that the
value it stands for is now several values, or no longer exists. The
instance is then marked unusable and the request is evaluated again
(`CellInvalidated`). A request makes at most `maxAttempts` (4)
evaluations: the last one runs without cells, and if it still fails the
daemon refuses the request. A reused result whose store derivations were
garbage collected is evaluated once more from scratch.

### Untraceable sites

A site whose instance records too many reads (the argument replaces
`lib`, so every function of `lib` is called through a port) is given up
(`maxPorts`); the application is evaluated directly from then on.

## 5. Exactness

The daemon's result must equal a cold evaluation, and `--verify` checks
it by running `nix eval` in a child process. Exactness follows from:
values are reused only when every observation the cell made gives the
same answer; the reused value keeps reading through ports; `==` between
a port and the value it denotes is true through chains of nested cells
(`sameBacking`); no context is added to strings that a cold evaluation
would leave without context; store objects created by the cell
(derivations, copies) keep their store paths, and a reused result does
not write them again (they are held as temporary roots).

Differences from a cold evaluation, documented as limits:

- messages of `builtins.trace` may be printed while reads are replayed;
- positions in error messages that come from the argument may be those
  of an earlier request;
- a string containing a virtual path is compared, sorted and split as the
  virtual string: its order relative to other store paths and substrings
  that cut into the hash part differ from a cold evaluation.
  `realizeStrings` only substitutes whole `<hash>-<name>` base names.

## 6. Cost model

A request costs the reads replayed (proportional to what the cells
observed, not to what they computed), the files whose text is compared
(the tree's files that were evaluated), and the evaluation of whatever
is not reused. For a NixOS configuration the reused part is the package
set and the outputs of input flakes; what remains is the module system
of the edited host, which is evaluated whole. On a workstation
configuration this leaves a warm request at about a quarter of the cold
time, and memory stable over distinct edits.

Where an application's argument depends on its own result (the module
system's `config`), a cell site there does not pay: its checks are
deferred, and the site is recomputed whenever the result changes. This
is why the sites stop at `import nixpkgs` and the flake outputs.

## 7. Diagnostics

Levels are Nix's; the default is `info` when stderr is not a terminal
and `notice` on one, and each `-v` raises it by one.

- `notice`: one line per request (`generation N: ...`) and retries.
- `talkative`: cell decisions (new instance, reused, rejected and why,
  given up, deferred check failed), instances dropped with a generation,
  slow validations, computations and replays.
- `stats` in each reply: hits, misses, rejections, deferred and
  invalidated checks, instances, ports, calls, file reads, validation
  time, attempts, `lastRejection`.

## 8. Other commands as clients

The setting `eval-daemon-socket` (gated on the `eval-daemon` experimental
feature) makes the commands that evaluate flake outputs clients of the
daemon (`eval_daemon::` in libcmd, hooked in
`InstallableFlake::toDerivedPaths()`, `nix eval` and `nix run`). A
connection carries one request, a JSON object with a protocol `version`
(1) and what determines the evaluation: the flake reference as parsed,
the attribute paths in the order the command tries them, the requested
outputs, the lock flags (`--override-input`, `--update-input`,
`--no-write-lock-file`, `--no-update-lock-file`, `--no-registries`), the
system, the store directory and the settings that change the result
(`forwardedSettings`: `read-only`, `pure-eval`, `restrict-eval`,
`allow-import-from-derivation`, `max-call-depth`, `use-registries`,
`flake-registry`, `tarball-ttl`, `experimental-features`), which must
equal the daemon's. The daemon builds the same `InstallableFlake` and
runs the same code (`toDerivedPaths`, `toValue`, `toApp`), with the
on-disk evaluation cache disabled and delegation disabled in its own
process, and answers with derived paths (as strings), the app, or the
rendered value (JSON, a raw string, or Nix syntax as `nix eval` prints
it).

The client only talks to a daemon run by the same user or by root
(peer credentials of the socket), and never guesses: anything the daemon
cannot honour exactly is evaluated locally, saying so at `info` level
(`printInfo`, shown by default unless stderr is a terminal). This covers
impure evaluation, installables that are not flakes (`--expr`,
`--file`), other lock-file flags,
`--override-flake` (the flag registry is not sent), `nix eval --apply`
or `--write-to`, a different protocol version, system, store or
setting, and a flake whose `nixConfig` changes one of those settings:
the daemon returns the flake's `nixConfig`, the client applies it as a
local evaluation would (prompting for untrusted settings), and falls back
if the forwarded settings are no longer those it sent. Derivations returned by
the daemon must be valid in the client's store, else the client falls
back; the daemon holds them as temporary roots. Evaluation errors are
thrown with the daemon's rendering.

## 9. Not done

- Persistence across daemon restarts: the first request is a cold
  evaluation. Instances, ports and file cells are ordinary heap objects
  with no serialization.
- Cells below the module system: a cut where the argument is not the
  whole `config` (option merges, module function applications) needs a
  design of its own.
- `--verify` refuses flags that change the lock file (`--override-input`
  with `--no-write-lock-file`); the override workflow is covered by a
  functional test that compares with `nix eval` instead.
- Clients get one connection per request, served in sequence; a request
  waits for the one before it.
