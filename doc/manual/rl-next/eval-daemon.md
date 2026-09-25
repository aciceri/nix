---
synopsis: "New experimental command `nix eval-daemon`"
---

[`nix eval-daemon`](@docroot@/command-ref/new-cli/nix3-eval-daemon.md), enabled by the `eval-daemon` experimental feature, keeps one evaluator alive and serves `eval <flakeref>#<attrpath>` requests on standard input or a Unix domain socket.
Every request locks the flake again, so edits are picked up, while files of unchanged inputs (such as Nixpkgs) that earlier requests already evaluated are reused.
Applications of `import nixpkgs { ... }` are reused as *traced cells*: the daemon records what Nixpkgs read from its arguments (configuration, overlays, platforms) and reuses the whole package set in a later request if those reads give the same answers.
`--verify` compares every result with a cold evaluation in a child process.
The daemon only supports pure evaluation.
