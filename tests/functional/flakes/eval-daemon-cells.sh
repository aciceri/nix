#!/usr/bin/env bash

# Traced cells in `nix eval-daemon`: applications of a Nixpkgs-like
# `pkgs/top-level/impure.nix` are reused across edits when everything they
# observed of their arguments is unchanged. Every request is checked against
# a cold evaluation with `--verify`.

source ./common.sh

requireGit

pkgsDir=$TEST_ROOT/eval-daemon-pkgs
flakeDir=$TEST_ROOT/eval-daemon-cells
daemonLog=$TEST_ROOT/eval-daemon-cells.log

createGitRepo "$pkgsDir" ""
mkdir -p "$pkgsDir/pkgs/top-level"
cat > "$pkgsDir/pkgs/top-level/impure.nix" <<'EOF'
{ config ? { }, overlays ? [ ], ... }:
builtins.trace "instantiating" (
  let
    base = final: {
      name = "base";
      greeting = "hello-${config.greeting}";
      unfree = config.allowUnfreePredicate { name = "x"; };
      lazy = config.lazyValue;
      same = config.a == config.b;
      args = builtins.attrNames (builtins.functionArgs config.fn);
      line = (builtins.unsafeGetAttrPos "greeting" config).line;
    };
    extend = f: o: final: let prev = f final; in prev // o final prev;
    fix = f: let x = f x; in x;
  in
  fix (builtins.foldl' extend base overlays)
)
EOF
git -C "$pkgsDir" add -A
git -C "$pkgsDir" commit -m init

createGitRepo "$flakeDir" ""
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.pkgs = { url = "git+file://$pkgsDir"; flake = false; };
  outputs = { self, pkgs }:
    let
      shared = x: x;
      p = import (pkgs + "/pkgs/top-level/impure.nix") {
        config = import ./config.nix // { a = { f = shared; }; b = { f = shared; }; };
        overlays = [ (import ./overlay.nix) ];
      };
    in { inherit (p) greeting unfree lazy same extra args line; };
}
EOF
cat > "$flakeDir/config.nix" <<'EOF'
{
  greeting = "world";
  allowUnfreePredicate = p: p.name == "x";
  lazyValue = "lazy1";
  fn = { u, v ? 1 }: u;
}
EOF
echo 'final: prev: { extra = "extra-${prev.greeting}"; }' > "$flakeDir/overlay.nix"
echo 1 > "$flakeDir/unrelated.nix"
git -C "$flakeDir" add -A
nix flake lock "$flakeDir"
git -C "$flakeDir" add flake.lock
git -C "$flakeDir" commit -m init

coproc DAEMON { nix eval-daemon --extra-experimental-features eval-daemon --verify 2>>"$daemonLog"; }
daemonPid=$DAEMON_PID

# Evaluate an attribute and check its value, the cell hits and misses of the
# request, and that a cold evaluation gives the same value.
check() {
    local attr=$1 value=$2 hits=$3 misses=$4 reply
    echo "eval git+file://$flakeDir#$attr" >&"${DAEMON[1]}"
    read -r reply <&"${DAEMON[0]}"
    echo "$attr: $reply" >&2
    [[ $(jq -c .value <<< "$reply") == "$value" ]]
    [[ $(jq .stats.cells.hits <<< "$reply") == "$hits" ]]
    [[ $(jq .stats.cells.misses <<< "$reply") == "$misses" ]]
    [[ $(jq .verify.matches <<< "$reply") == true ]]
}

check greeting '"hello-world"' 0 1
# Two attribute sets sharing a function value are equal, as in a cold evaluation.
check same true 1 0

# An edit that the cell did not observe keeps the instance.
echo 2 > "$flakeDir/unrelated.nix"
check greeting '"hello-world"' 1 0

# A changed value that the cell never read is read from the new argument.
sed -i 's/lazy1/lazy2/' "$flakeDir/config.nix"
check lazy '"lazy2"' 1 0

# `builtins.functionArgs` through the argument is observed.
check args '["u","v"]' 1 0
sed -i 's/{ u, v ? 1 }/{ u, w ? 1 }/' "$flakeDir/config.nix"
check args '["u","w"]' 0 1

# Observed values invalidate the instance; values it had not read do not.
sed -i 's/world/mars/' "$flakeDir/config.nix"
check greeting '"hello-mars"' 1 0
sed -i 's/mars/venus/' "$flakeDir/config.nix"
check greeting '"hello-venus"' 0 1

# Overlays are functions of the argument: their results are observed.
check extra '"extra-hello-venus"' 1 0
echo 'final: prev: { extra = "EXTRA-${prev.greeting}"; }' > "$flakeDir/overlay.nix"
check extra '"EXTRA-hello-venus"' 0 1
check unfree true 1 0

# Attribute positions are observed: moving the attribute invalidates.
check line 2 1 0
sed -i 's/^{$/{\n  # moved/' "$flakeDir/config.nix"
check line 3 0 1

# A value of the argument that depends on the cell's own result cannot be
# checked when the cell is looked up (the result is being computed); it is
# checked when the request is done, and the request is evaluated again if
# it changed.
flake2Dir=$TEST_ROOT/eval-daemon-cells-2
createGitRepo "$flake2Dir" ""
cat > "$flake2Dir/flake.nix" <<EOF
{
  inputs.pkgs = { url = "git+file://$pkgsDir"; flake = false; };
  outputs = { self, pkgs }:
    let
      p = import (pkgs + "/pkgs/top-level/impure.nix") {
        config = { greeting = p.name + "-" + builtins.readFile ./suffix.txt; };
      };
    in { inherit (p) greeting; };
}
EOF
printf x > "$flake2Dir/suffix.txt"
echo 1 > "$flake2Dir/unrelated.nix"
git -C "$flake2Dir" add -A
nix flake lock "$flake2Dir"
git -C "$flake2Dir" add flake.lock
git -C "$flake2Dir" commit -m init

check2() {
    local value=$1 deferred=$2 invalidated=$3 attempts=$4 reply
    echo "eval git+file://$flake2Dir#greeting" >&"${DAEMON[1]}"
    read -r reply <&"${DAEMON[0]}"
    echo "greeting (2): $reply" >&2
    [[ $(jq -c .value <<< "$reply") == "$value" ]]
    [[ $(jq .stats.cells.deferred <<< "$reply") == "$deferred" ]]
    [[ $(jq .stats.cells.invalidated <<< "$reply") == "$invalidated" ]]
    [[ $(jq .stats.attempts <<< "$reply") == "$attempts" ]]
    [[ $(jq .verify.matches <<< "$reply") == true ]]
}

check2 '"hello-base-x"' 0 0 1
echo 2 > "$flake2Dir/unrelated.nix"
check2 '"hello-base-x"' 1 0 1
printf y > "$flake2Dir/suffix.txt"
check2 '"hello-base-y"' 1 1 2

echo quit >&"${DAEMON[1]}"
read -r _ <&"${DAEMON[0]}"
wait "$daemonPid"
