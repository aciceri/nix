# Handoff: traced-cells evaluator in the Nix fork

You are working in `~/projects/aciceri/nix`, a fork of NixOS/nix. Read
`doc/traced-cells/DESIGN.md` first; this note gives the context, the
numbers behind the design, the repository state and the rules.

## Repository state

- Remotes: `origin` = git@github.com:aciceri/nix.git, `upstream` =
  git@github.com:NixOS/nix.git. `master` was fast-forwarded to
  `upstream/master` (`18057950c`, version 2.36.0 pre-release).
- Branch `traced-cells` (checked out) = master + doc commits + P1 + P1b. Nothing
  pushed. Do not push, merge or open PRs without explicit authorization
  from Andrea.
- Done: P0 (harness, cold baseline), P1 (`nix eval-daemon`, file cells
  across generations) and P1b (traced cells for `import nixpkgs`), see
  DESIGN.md sections 7, 11 and 12 and `projects/fasteval/NOTES.md`
  sections "P0", "P1" and "P1b" in `universe`. Next: the source root port
  (DESIGN.md 8.3), then P2 cells for module and package applications.
- Tests: `meson test -C build --suite flakes eval-daemon eval-daemon-cells`
  (functional),
  `cycle.py run --daemon ...` (acceptance on `universe`, see NOTES.md).
- Build: `nix develop`, then `meson setup build $mesonFlags` and
  `ninja -C build` (debugoptimized, ~18 min from scratch). Benchmarks use a
  second tree: `meson setup build-release $mesonFlags --buildtype=release
  -Db_lto=true` (`/build-release` is excluded in `.git/info/exclude`).
  The built binary uses the system store and `/etc/nix/nix.conf`.

## Why this exists (measurements on Andrea's workstation `pike`)

Machine: 12 cores, 31 GiB, ZFS, NixOS 26.11, Nix 2.35.2, desktop session
running (wall times noisy; evaluator CPU from `NIX_SHOW_STATS` is the
stable metric). Flake: `~/multiverse/universe` (flake-parts "dendritic",
~200 auto-imported modules, 5 NixOS hosts + 1 darwin, Home Manager as a
NixOS module with two profiles on pike, stylix, niri-flake).

Cold evaluation of `nixosConfigurations.pike.config.system.build.toplevel`:

- `nix-instantiate --readonly-mode --expr`: 9.6-10 s CPU, 10.9 s wall
  (later in the day, under load, 13-15 s; always compare paired runs).
- `nix eval .#nixosConfigurations.pike...` (what `nh`/`nixos-rebuild`
  run): +3.4 s CPU because Nix probes `packages.x86_64-linux.<attr>` first
  and this flake's `packages` filters all `nixosConfigurations` by
  `config.nixpkgs.hostPlatform.system`; writing the .drv files through the
  daemon: +3 s wall.
- 43 940 `derivationStrict` calls, 52 455 `import`s, 2.16 M parsed
  expression nodes, 20 M function calls, 31.8 M thunks, 3.2 M `//`
  copying 46 M attrs, RSS 2.6 GiB.
- Split: without Home Manager 5.4 s; the workstation `home.packages` list
  alone 5.4 s (`claude-desktop` 1.7 s, `telegram-desktop` 1.5 s); second
  HM profile (root) +0.2 s (package thunks shared via `useGlobalPkgs`);
  reaching `nixosConfigurations.pike` through flake-parts 1.6 s.
- Other hosts: sisko 10.2 s, kirk 5.7 s, janeway 3.9 s CPU.
- `GC_INITIAL_HEAP_SIZE=8G`: CPU 14.5 → 9.3 s, wall unchanged (Boehm marks
  in parallel). `eval-attrset-update-layer-rhs-threshold`: 0 is fastest,
  larger values slower (lookups through layers cost more than copying).

perf (cpu_core cycles, sequential eval): interpreter core 19.5%, attrset
build/lookup/sort 15.9%, strings and contexts 15.6%, allocation+GC 13.3%,
JSON/fetchers/sqlite 8.7%, store hashing 5.4%, parsing 5.1%. No single hot
spot. Nix-level profile (`--eval-profiler flamegraph`): 39% self time in
`pkgs/stdenv/generic/make-derivation.nix`, 14% in `lib/modules.nix`; 85% of
samples have a package-instantiation frame on the stack.

Churn: commit `7d4e81e` (4 lines in `modules/niri.nix`) vs parent changes
19 of 15 209 derivations in the toplevel closure.

Alternatives measured and rejected for this workload:

- Determinate Nix 3.22.5 with `--extra-experimental-features parallel-eval
  --eval-cores 12`: no wall gain on the toplevel (one demand chain), 1.3x
  even on an explicit `builtins.parallel` over the package list, with
  system time exploding (shared dependency chains, GC/allocator
  contention). Single-core Determinate is ~1 s faster than upstream.
- psyclyx/fix 0.3.0 (Zig bytecode VM, speculative parallel): fails on
  hosts with stylix (base16.nix YAML parser gets `"\""` from
  `builtins.match`), pure mode broken with tarball inputs, needs
  `rec-set-merges`; on janeway it evaluates but 2-3x slower than cppnix
  (even pinned to one core) and produces a different drvPath; 90-98% of
  speculated thunks never demanded.
- tvix/snix: not at nixpkgs parity.
- Compiling Nix to C/LLVM: attacks only the ~20% interpreter slice,
  ≤1.3x; incremental compilation is not incremental evaluation.

Experiments that support the design (all files under
`~/multiverse/universe/projects/fasteval/`, notes in `NOTES.md`):

- `_experiments/stub-cache.nix`: overlay replacing 99 leaf packages with
  cached derivation shells (drvPath, per-output outPath, outputs, name,
  meta, string contexts rebuilt with `builtins.appendContext`, lazy
  `override`/`overrideAttrs`/`overrideDerivation`/`passthru`/`src`
  fallbacks): identical toplevel drvPath, CPU -23%. Consumers that inspect
  packages beyond outPath/meta were found this way: `qemu.override` in
  all-packages.nix, niri-flake reads `package.cargoBuildFeatures`,
  home-manager reads `package.meta.description`, buildEnv reads
  `meta.priority`; stubbing core packages breaks stdenv assertions.
- `_experiments/resident-repl.txt`: `nix repl` keeping one EvalState,
  `p = nixosConfigurations.pike.pkgs`, then after `:lf` reload evaluate
  `pike.extendModules { modules = [{ nixpkgs.pkgs = p; nixpkgs.hostPlatform
  = "x86_64-linux"; nixpkgs.config = lib.mkForce {}; nixpkgs.overlays =
  lib.mkForce []; }]; }`: cold 19.5 s → 6.5 s, identical drvPath. This is
  the number the design must reproduce automatically (phase P1b) and then
  beat (P2, P3).
- `_experiments/bench.sh <label> [ENV=V ...] -- <cmd>`: appends wall/CPU/
  GC/heap/thunks to `_experiments/results.tsv`; `_experiments/
  profile-summary.py <folded-profile>` summarises `--eval-profiler
  flamegraph` output. Useful binaries already in the store:
  Determinate Nix `/nix/store/zdhkfdp7ggk3ai25r4m0fwqahabaxjvb-determinate-nix-3.22.5/bin/nix`,
  fix `/nix/store/b3amdyr47zi4lsipc82d2kwf8kr22lqd-fix-0.3.0/bin/fix`,
  perf `/nix/store/12ns24zpsfih98b0q7k44n5ys86l789i-perf-linux-7.2.7/bin/perf`
  (`kernel.perf_event_paranoid=2`, user-space sampling works).

## The design in one paragraph

Cells = applications of file-level lambdas with formals to attrsets
(`callPackage`, `import nixpkgs {…}`, modules, `evalModules`), plus file
cells keyed by content hash and root-relative path, and derivation cells.
Small arguments go into the key; large/lazy/cyclic ones (`pkgs`, `config`,
`lib`, `inputs`) are ports, compared by the trace of what the cell observed
through them. A cell is reused in a new generation if replaying its trace
against the new ports and inputs gives the same summaries (verifying
traces, bottom-up, early cutoff). Soundness for unforced thunks inside a
reused result comes from ports and values derived from them being
indirections (`tPort` value kind resolved through a per-generation table,
DESIGN.md 5.4), so old closures read new data. Resident process
(`nix eval-daemon`, built from `repl.cc` pieces), `--verify` mode compares
with a cold evaluation. Persistence later: keys/traces are pointer-free;
results stored as WHNF with lazy-fallback children.

Hook points in this tree: `EvalState::callFunction` lambda branch
`src/libexpr/eval.cc:1608`; `ExprSelect::eval` `eval.cc:1462`;
`lookupVar` (`with`) `eval.cc:933`; `evalFile`/`fileEvalCache`
`eval.cc:1160`, `resetFileCache` `eval.cc:1197`; `forceValue`
`src/libexpr/include/nix/expr/eval-inline.hh:~95-120`; `mkPos`
`eval.cc:990` (positions embed the store path: `__curPos` is used all over
the universe flake, so lazy-trees or relative rendering is needed);
`prim_derivationStrict` `src/libexpr/primops.cc:1425`; `Value` layout
`src/libexpr/include/nix/expr/value.hh:633-760` (a `pdSingleDWord` slot is
free for `tPort`); `RootValue` `src/libexpr/include/nix/expr/value.hh`, `allocRootValue`
(side tables holding `Value *` must use `RootValue` or
`traceable_allocator`); `PrimOp` struct in `eval.hh` (add `readsInput`);
`NixRepl::processLine`/`loadFlake` `src/libcmd/repl.cc:352,94`.

## Expected outcome and acceptance

- Common case (edit one module file on pike): today 10 s CPU / 14-18 s wall
  via `nh`; target ≤ 6.5 s after P2, ≤ 3 s after P3. Cold-run recording
  overhead ≤ 20%.
- Every phase: drvPath identical to `nix-instantiate --readonly-mode
  --expr '(builtins.getFlake "git+file:///home/ccr/multiverse/universe")
  .nixosConfigurations.pike.config.system.build.toplevel'` over repeated
  edit/restore cycles on all hosts (pike, sisko, kirk, janeway, picard).

## Rules

- Write code comments, docs and commit messages in English; keep prose
  concise; no em dashes in new text.
- Do not push, merge, deploy or publish without explicit authorization.
  Committing on `traced-cells` is fine.
- Do not modify `~/multiverse/universe` for this work except under
  `projects/fasteval/` (experiments and notes); never run `nh os switch`.
- Benchmarks: use paired runs and CPU time; the desktop adds several
  seconds of wall noise.
