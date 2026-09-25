#!/usr/bin/env bash

source ./common.sh

requireGit

libDir=$TEST_ROOT/eval-daemon-lib
flakeDir=$TEST_ROOT/eval-daemon-flake
daemonLog=$TEST_ROOT/eval-daemon.log

# A locked, non-flake input whose only file traces when it is evaluated.
createGitRepo "$libDir" ""
cat > "$libDir/default.nix" <<EOF
builtins.trace "evaluating-lib" { greeting = "hello"; }
EOF
git -C "$libDir" add default.nix
git -C "$libDir" commit -m init

createGitRepo "$flakeDir" ""
cp "${config_nix}" "$flakeDir/"
echo '"one"' > "$flakeDir/value.nix"
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.lib = { url = "git+file://$libDir"; flake = false; };
  outputs = { self, lib }: let inherit (import ./config.nix) mkDerivation; in {
    value = (import lib).greeting + " " + import ./value.nix;
    drv = mkDerivation {
      name = "eval-daemon-" + import ./value.nix;
      buildCommand = "echo > \$out";
    };
    broken = throw "broken on purpose";
  };
}
EOF
git -C "$flakeDir" add flake.nix config.nix value.nix
nix flake lock "$flakeDir"
git -C "$flakeDir" add flake.lock
git -C "$flakeDir" commit -m init

startDaemon() {
    coproc DAEMON { nix eval-daemon --extra-experimental-features eval-daemon "$@" 2>>"$daemonLog"; }
    # Bash unsets DAEMON_PID when the coprocess exits.
    daemonPid=$DAEMON_PID
}

# Coprocess file descriptors are not available in subshells, so the reply is
# returned in a global variable.
request() {
    echo "$1" >&"${DAEMON[1]}"
    read -r reply <&"${DAEMON[0]}"
    echo "reply: $reply" >&2
}

field() {
    jq -r "$1" <<< "$reply"
}

startDaemon

request "eval git+file://$flakeDir#value"
[[ $(field .ok) == true ]]
[[ $(field .kind) == json ]]
[[ $(field .value) == "hello one" ]]
[[ $(field .stats.generation) == 1 ]]

# Uncommitted changes to the flake are picked up.
echo '"two"' > "$flakeDir/value.nix"
request "eval git+file://$flakeDir#value"
[[ $(field .value) == "hello two" ]]
[[ $(field .stats.generation) == 2 ]]

# Restoring the file restores the result.
git -C "$flakeDir" checkout value.nix
request "eval git+file://$flakeDir#value"
[[ $(field .value) == "hello one" ]]

# The locked input did not change, so its file was evaluated only once.
[[ $(grep -c 'evaluating-lib' "$daemonLog") == 1 ]]

# Derivations are reported by their store derivation path.
request "eval git+file://$flakeDir#drv"
[[ $(field .kind) == drvPath ]]
[[ $(field .value) == "$(nix eval --raw "git+file://$flakeDir#drv.drvPath")" ]]

# Evaluation errors are reported and the daemon keeps serving requests.
request "eval git+file://$flakeDir#broken"
[[ $(field .ok) == false ]]
field .error | grepQuiet 'broken on purpose'

# A failed evaluation drops the cached files, so the input is evaluated again.
request "eval git+file://$flakeDir#value"
[[ $(field .value) == "hello one" ]]
[[ $(grep -c 'evaluating-lib' "$daemonLog") == 2 ]]

# `gc` keeps the cached files, `reset` drops them.
request "gc"
[[ $(field .ok) == true ]]
request "eval git+file://$flakeDir#value"
[[ $(grep -c 'evaluating-lib' "$daemonLog") == 2 ]]
request "reset"
[[ $(field .ok) == true ]]
request "eval git+file://$flakeDir#value"
[[ $(grep -c 'evaluating-lib' "$daemonLog") == 3 ]]

request "frobnicate"
[[ $(field .ok) == false ]]

request "quit"
[[ $(field .ok) == true ]]
wait "$daemonPid"

# --verify compares every result with a cold evaluation in a child process.
startDaemon --verify
request "eval git+file://$flakeDir#drv"
[[ $(field .verify.matches) == true ]]
request "eval git+file://$flakeDir#value"
[[ $(field .verify.matches) == true ]]
request "quit"
wait "$daemonPid"

# Cached files are only sound under pure evaluation.
expectStderr 1 nix eval-daemon --extra-experimental-features eval-daemon --impure </dev/null \
    | grepQuiet 'does not support impure evaluation'
