R""(

# Examples

* Evaluate the toplevel of a NixOS configuration, edit a file of the flake
  and evaluate it again, in one process:

  ```console
  # nix eval-daemon
  eval .#nixosConfigurations.machine.config.system.build.toplevel
  {"kind":"drvPath","ok":true,"stats":{...},"value":"/nix/store/...-nixos-system-machine.drv"}
  eval .#nixosConfigurations.machine.config.system.build.toplevel
  {"kind":"drvPath","ok":true,"stats":{...},"value":"/nix/store/...-nixos-system-machine.drv"}
  ```

  The second request reuses every file of the flake's locked inputs (for
  example Nixpkgs) that the first request parsed and evaluated.

* Start a daemon that evaluates a configuration once before serving
  requests, so that the first request is already fast:

  ```console
  # nix eval-daemon --socket "$XDG_RUNTIME_DIR/nix-eval.sock" \
      --warm .#nixosConfigurations.machine.config.system.build.toplevel
  ```

* Serve requests on a Unix domain socket:

  ```console
  # nix eval-daemon --socket "$XDG_RUNTIME_DIR/nix-eval.sock" &
  # echo 'eval .#nixosConfigurations.machine.config.system.build.toplevel' \
      | socat -t 3600 - "UNIX-CONNECT:$XDG_RUNTIME_DIR/nix-eval.sock"
  ```

* Let the other `nix` commands use the daemon (see the setting
  [`eval-daemon-socket`](@docroot@/command-ref/conf-file.md#conf-eval-daemon-socket)):

  ```console
  # nix eval-daemon --socket "$XDG_RUNTIME_DIR/nix-eval.sock" &
  # nix build --extra-experimental-features eval-daemon \
      --option eval-daemon-socket "$XDG_RUNTIME_DIR/nix-eval.sock" \
      .#nixosConfigurations.machine.config.system.build.toplevel
  ```

# Description

`nix eval-daemon` keeps one evaluator alive and serves evaluation requests,
one per line, on standard input and output, or on the Unix domain socket
given by `--socket`. Each request starts a new *generation*: flake inputs are
fetched and locked again, so changes to the flake are picked up, while files
that were already evaluated are reused.

Reuse is sound because every file read during a pure evaluation lives in the
Nix store: files of locked inputs are content-addressed, so an unchanged
input is the same path and its evaluated files are kept without further
checks; files of unlocked inputs (see [Traced cells](#traced-cells)) are
checked against the current tree at every request. For this reason the
daemon refuses `--impure`.

Requests are single lines; every response is a single line of JSON.

* `eval` *flakeref*`#`*attrpath*

  Evaluate the attribute *attrpath* of the flake *flakeref*. The attribute
  path is absolute: unlike `nix eval`, no `packages.<system>` or
  `legacyPackages.<system>` prefix is tried. If the value is a derivation the
  response contains its store derivation path (`"kind": "drvPath"`),
  otherwise the value as JSON (`"kind": "json"`). `stats` reports the CPU and
  garbage collector time of this request, the number of cached files, the
  number of evaluations of the request (`attempts`) and, in `cells`, the
  cells reused (`hits`), created (`misses`), rejected by replay
  (`rejected`), checked after the request (`deferred`) and found invalid
  then (`invalidated`). The thunks and function calls are reported when
  [`NIX_SHOW_STATS`](@docroot@/command-ref/env-common.md#env-NIX_SHOW_STATS)
  is set, the heap size when Nix was built with the Boehm garbage
  collector.

  With `--verify`, the daemon then runs `nix eval --no-eval-cache` on the same
  installable in a child process and reports in `verify.matches` whether the
  cold result is identical. The child uses the configuration files and, if
  the daemon is read-only, `--read-only`, but no other command line options
  of the daemon. Requests from other commands (below) are not verified.

  If an evaluation fails, the daemon drops the cached files and the cells
  the request created or reused, because a failed evaluation can leave
  failed thunks behind; cells the request did not touch stay.

* `stats`

  Print the evaluator statistics of the whole process, in the format of
  [`NIX_SHOW_STATS`](@docroot@/command-ref/env-common.md#env-NIX_SHOW_STATS),
  and the totals of the traced cells (`cells`).

* `reset`

  Drop all cached files and cells.

* `quit`

  Stop the daemon.

# Requests from other commands

When the setting `eval-daemon-socket` names the daemon's socket, the
commands that evaluate a flake output (`nix build`, `nix path-info`, `nix
derivation show`, `nix run`, `nix develop`, `nix eval`, and every other
command that builds flake outputs) send their installable to the daemon
as a JSON request and use its answer: derived paths, the app to run, or
the rendered value. The request carries the flake reference, the
attribute paths the command would try, in order, the requested outputs,
the lock flags `--override-input`, `--update-input`,
`--no-write-lock-file`, `--no-update-lock-file` and `--no-registries`,
the system, the store directory, and the settings `read-only`,
`pure-eval`, `restrict-eval`, `allow-import-from-derivation`,
`max-call-depth`, `use-registries`, `flake-registry`, `tarball-ttl` and
`experimental-features`. The daemon evaluates the request with the same
code the command would run, so results are the same; its own
`--no-write-lock-file` and `--no-update-lock-file` apply to every request.

A command evaluates locally when the setting is empty, when nothing
listens on the socket or the daemon is not run by the same user or by
root, when it uses something the daemon does not support (`--impure`,
`--expr`, `--override-flake`, `--recreate-lock-file`, `--commit-lock-file`,
`--reference-lock-file`, `--output-lock-file`, `nix eval --apply`, `nix
eval --write-to`), or when the daemon refuses the request: another
system, store directory or value of one of the settings above, or a
failure of the daemon's own reuse; or when the flake's `nixConfig`, which
the command applies to its own settings as usual, changes one of the
settings above. It says so with an `evaluating ...
locally` message at the informational level: visible by default when
standard error is not a terminal, otherwise at `-v`. Derivations returned
by the daemon are valid in the store because the daemon wrote them and
holds them as temporary roots; a command that finds one missing (a daemon
writing to another store) evaluates locally. An evaluation error is
reported as the daemon rendered it, without the client's own trace.

# Traced cells

The daemon also reuses *applications* of selected functions across requests:
`import nixpkgs { ... }` (the function of `pkgs/top-level/impure.nix`) and
the `outputs` function of every locked flake input. Such a *cell* does
not see its argument directly: it reads it through proxies that record
what it looked at (attribute names, list lengths, primitive values,
results of calling functions of the argument such as overlays or
`allowUnfreePredicate`). Functions that another cell created are called
directly and checked as closures: the same code with equivalent free
variables. In a later request the recorded reads are replayed against the
new argument, and the previous result is reused only if they all give the
same answers. The reused result keeps reading the argument through the
proxies, so values it had not looked at yet come from the new argument.

Reads that cannot be replayed before the result exists (they depend on the
result, for example a NixOS option read by an overlay) are checked when the
request is done; if one changed, the request is evaluated again from
scratch (`stats.attempts`), and that call site no longer defers such reads.
A call site whose cell records more than 100 000 reads (for example
because the argument replaces `lib`) is no longer traced.

The files of unlocked inputs (the flake being evaluated, or an input
overridden with `--override-input` to a local tree) keep their identity
when the tree changes: the daemon mounts such an input at a stable
virtual store path. As with any Git input, files that are not in the
index are not part of the tree: add new files with `git add` for the
daemon (and `nix eval`) to see them. Paths of such an input stay virtual
when they become strings, and take the store path of the current
contents where a string leaves the evaluator (derivation attributes,
`builtins.toFile`, `builtins.hashString`, the result), so results are
those of a cold evaluation. A cell that read files of such a tree (by
importing, reading, listing, testing or copying them) is reused after an
edit only if those operations give the same results in the new tree; a
cell that let a rendered path leave the evaluator depends on the tree's
store path.

Unless `--read-only` is given, derivations are written to the store as
usual; a reused result does not write them again. The daemon keeps every
path it added as a temporary garbage collector root while it runs, so they
stay valid; if the result's store derivation is missing anyway, or
evaluation fails on a missing store path, the request is evaluated again
without reusing anything.

Messages printed by `builtins.trace` and warnings may be printed while
reads are replayed, are not printed again for a reused result, and show
the store path of the current contents of an unlocked input; positions in
error messages that come from the argument may be those of an earlier
request.

)""
