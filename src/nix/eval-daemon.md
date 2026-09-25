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

* Serve requests on a Unix domain socket:

  ```console
  # nix eval-daemon --socket /tmp/eval.sock &
  # echo 'eval .#nixosConfigurations.machine.config.system.build.toplevel' | socat - UNIX-CONNECT:/tmp/eval.sock
  ```

# Description

`nix eval-daemon` keeps one evaluator alive and serves evaluation requests,
one per line, on standard input and output, or on the Unix domain socket
given by `--socket`. Each request starts a new *generation*: flake inputs are
fetched and locked again, so changes to the flake are picked up, while files
that were already evaluated are reused.

Reuse is sound because every file read during a pure evaluation lives in the
Nix store or in an input mounted at a store path derived from its contents,
so a reused file cannot have changed. For this reason the daemon refuses
`--impure`.

Requests are single lines; every response is a single line of JSON.

* `eval` *flakeref*`#`*attrpath*

  Evaluate the attribute *attrpath* of the flake *flakeref*. The attribute
  path is absolute: unlike `nix eval`, no `packages.<system>` or
  `legacyPackages.<system>` prefix is tried. If the value is a derivation the
  response contains its store derivation path (`"kind": "drvPath"`),
  otherwise the value as JSON (`"kind": "json"`). `stats` reports the CPU and
  garbage collector time, thunks and function calls of this request, the heap
  size and the number of cached files.

  With `--verify`, the daemon then runs `nix eval --no-eval-cache` on the same
  installable in a child process and reports in `verify.matches` whether the
  cold result is identical. The child uses the configuration files and
  `--read-only`, but no other command line options of the daemon.

  If an evaluation fails, the daemon drops all cached files, because a failed
  evaluation can leave failed thunks behind.

* `stats`

  Print the evaluator statistics of the whole process, in the format of
  [`NIX_SHOW_STATS`](@docroot@/command-ref/env-common.md#env-NIX_SHOW_STATS).

* `gc`

  Run a full garbage collection and report the heap size and the bytes
  still in use (`liveBytes`).

* `reset`

  Drop all cached files, then behave like `gc`.

* `quit`

  Stop the daemon.

)""
