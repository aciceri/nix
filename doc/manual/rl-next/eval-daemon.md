---
synopsis: "New experimental command `nix eval-daemon`"
---

[`nix eval-daemon`](@docroot@/command-ref/new-cli/nix3-eval-daemon.md), enabled by the `eval-daemon` experimental feature, keeps one evaluator alive and serves `eval <flakeref>#<attrpath>` requests on standard input or a Unix domain socket.
Every request locks the flake again, so edits are picked up, while files of unchanged inputs (such as Nixpkgs) that earlier requests already evaluated are reused.
Unlocked inputs (the flake being edited, `--override-input` to a local checkout) are mounted at a stable path, so their files keep their identity across edits and only files whose contents or reads changed are evaluated again; paths of such inputs stay symbolic inside the evaluator and take the store path of the current contents where a string leaves it (derivations, `builtins.toFile`, `builtins.hashString`, the output).
Applications of `import nixpkgs { ... }` and the `outputs` of locked flake inputs are reused as *traced cells*: the daemon records what such an application read from its arguments (configuration, overlays, platforms, other inputs) and reuses the result in a later request if those reads give the same answers.
`--verify` compares the result of every `eval` line request with a cold evaluation in a child process.
The daemon only supports pure evaluation.
With the new setting `eval-daemon-socket` (also behind the `eval-daemon` feature), `nix build`, `nix eval`, `nix run`, `nix develop`, `nix path-info`, `nix derivation show` and every other command that builds flake outputs ask a running daemon of the same user for the outputs instead of evaluating them, and evaluate locally when the daemon is not there, its settings differ, or the request uses something it does not support.
