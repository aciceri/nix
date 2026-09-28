#!/usr/bin/env bash

# `eval-daemon-socket`: `nix build`, `nix eval --json` and `nix path-info`
# ask a running `nix eval-daemon` for the result instead of evaluating,
# with the same results; without a daemon, or with a request the daemon
# does not support, they evaluate locally.

source ./common.sh

requireGit

flakeDir=$TEST_ROOT/eval-daemon-client
sock=$TEST_ROOT/eval-daemon-client.sock
daemonLog=$TEST_ROOT/eval-daemon-client.log

createGitRepo "$flakeDir" ""
cp "${config_nix}" "$flakeDir/"
cat > "$flakeDir/flake.nix" <<'EOF'
{
  outputs = { self }: let inherit (import ./config.nix) mkDerivation; in {
    packages.SYSTEM.default = mkDerivation {
      name = "client-default";
      buildCommand = "echo ${toString ./data.txt} > $out";
    };
    packages.SYSTEM.other = mkDerivation {
      name = "client-other";
      buildCommand = "echo other > $out";
    };
    value = { a = 1; b = "two"; c = [ ./data.txt ]; };
    text = "hello ${self.value.b}";
  };
}
EOF
sed -i "s/SYSTEM/$system/g" "$flakeDir/flake.nix"
echo data1 > "$flakeDir/data.txt"
git -C "$flakeDir" add -A
git -C "$flakeDir" commit -m init

flake="git+file://$flakeDir"

# Local results, before any daemon runs.
localDrv=$(nix path-info --derivation "$flake#default")
localOther=$(nix path-info --derivation "$flake#other")
localValue=$(nix eval --json "$flake#value")
localText=$(nix eval --raw "$flake#text")
localNix=$(nix eval "$flake#value")
localDrvNix=$(nix eval "$flake#default")
localOut=$(nix build --no-link --print-out-paths "$flake#default")

# Without a daemon the setting is harmless: everything is evaluated
# locally and said so at -v.
nix path-info --derivation --extra-experimental-features eval-daemon --option eval-daemon-socket "$sock" \
    "$flake#default" -v 2> "$TEST_ROOT/nodaemon.err"
grepQuiet "evaluating .* locally: no evaluation daemon" "$TEST_ROOT/nodaemon.err"

evalDaemonPid=
stopEvalDaemon() {
    [[ -n $evalDaemonPid ]] || return 0
    kill "$evalDaemonPid" 2> /dev/null || true
    wait "$evalDaemonPid" 2> /dev/null || true
    evalDaemonPid=
}
# Keep the harness's cleanup of the Nix daemon (see `startDaemon`).
trap 'stopEvalDaemon; if [[ -n ${_NIX_TEST_DAEMON_PID-} ]]; then killDaemon; fi' EXIT
startEvalDaemon() {
    nix eval-daemon --extra-experimental-features eval-daemon "$@" --socket "$sock" 2>> "$daemonLog" &
    evalDaemonPid=$!
    for _ in $(seq 1 100); do [[ -S $sock ]] && break; sleep 0.1; done
    [[ -S $sock ]]
}
startEvalDaemon

export NIX_CONFIG="extra-experimental-features = eval-daemon
eval-daemon-socket = $sock"

# Delegated requests give the local results.
nix path-info --derivation "$flake#default" -v 2> "$TEST_ROOT/delegated.err" > "$TEST_ROOT/delegated.out"
grepQuiet "by the evaluation daemon" "$TEST_ROOT/delegated.err"
[[ $(cat "$TEST_ROOT/delegated.out") == "$localDrv" ]]
[[ $(nix path-info --derivation "$flake#other") == "$localOther" ]]
[[ $(nix eval --json "$flake#value") == "$localValue" ]]
[[ $(nix eval --raw "$flake#text") == "$localText" ]]
[[ $(nix eval "$flake#value") == "$localNix" ]]
[[ $(nix eval "$flake#default") == "$localDrvNix" ]]
[[ $(nix build --no-link --print-out-paths "$flake#default") == "$localOut" ]]
[[ $(nix derivation show "$flake#default" | jq -r '.derivations | keys[0]') == "$(basename "$localDrv")" ]]
# Attribute paths are resolved as the command would (`packages.<system>`).
[[ $(nix path-info --derivation "$flake#packages.$system.default") == "$localDrv" ]]

# The daemon served each of them.
generations() { grep -c '^generation [0-9]*: .*: ok in' "$daemonLog"; }
[[ $(generations) == 9 ]]

# Requests the daemon cannot honour are evaluated locally: `--impure`,
# `--expr`, `--recreate-lock-file`, `--apply`, `--override-flake`.
before=$(generations)
nix path-info --derivation --impure "$flake#default" -v 2> "$TEST_ROOT/impure.err" > "$TEST_ROOT/impure.out"
grepQuiet "evaluating .* locally: .*pure" "$TEST_ROOT/impure.err"
[[ $(cat "$TEST_ROOT/impure.out") == "$localDrv" ]]
[[ $(nix eval --expr '1 + 1') == 2 ]]
nix path-info --derivation --recreate-lock-file "$flake#default" -v 2> "$TEST_ROOT/lock.err"
grepQuiet "evaluating .* locally: lock-file flags" "$TEST_ROOT/lock.err"
[[ $(nix eval --apply 'x: x + 1' "$flake#value.a") == 2 ]]
nix path-info --derivation --override-flake foo "$flake" "$flake#default" 2> "$TEST_ROOT/registry.err" > "$TEST_ROOT/registry.out"
grepQuiet "evaluating .* locally: '--override-flake'" "$TEST_ROOT/registry.err"
[[ $(cat "$TEST_ROOT/registry.out") == "$localDrv" ]]
[[ $(generations) == "$before" ]]

# A flake's `nixConfig` is applied by the client. A setting the daemon
# does not depend on is delegated; one it evaluates with (here with
# `accept-flake-config`) makes the client evaluate locally.
configDir=$TEST_ROOT/eval-daemon-client-config
createGitRepo "$configDir" ""
echo '{ nixConfig.bash-prompt-suffix = "x"; outputs = _: { value = 1; }; }' > "$configDir/flake.nix"
git -C "$configDir" add -A
git -C "$configDir" commit -m init
[[ $(nix eval "git+file://$configDir#value") == 1 ]]
[[ $(generations) == $((before + 1)) ]]
echo '{ nixConfig.max-call-depth = 1234; outputs = _: { value = 2; }; }' > "$configDir/flake.nix"
git -C "$configDir" commit -qam depth
[[ $(nix eval --accept-flake-config "git+file://$configDir#value" 2> "$TEST_ROOT/config.err") == 2 ]]
grepQuiet "evaluating .* locally: the flake's 'nixConfig' changes settings" "$TEST_ROOT/config.err"

# An edit is picked up.
echo data2 > "$flakeDir/data.txt"
git -C "$flakeDir" commit -qam data2
newDrv=$(nix path-info --derivation "$flake#default")
[[ $newDrv != "$localDrv" ]]
[[ $newDrv == $(nix path-info --derivation --option eval-daemon-socket "" "$flake#default") ]]

# An evaluation error is reported by the client.
expectStderr 1 nix eval --json "$flake#missing" | grepQuiet "does not provide attribute"

# A read-only daemon only serves read-only clients: derivations are
# evaluated locally, values are still served with `--read-only`.
stopEvalDaemon
startEvalDaemon --read-only
nix path-info --derivation "$flake#default" -v 2> "$TEST_ROOT/readonly.err" > "$TEST_ROOT/readonly.out"
grepQuiet "evaluating .* locally: .*read-only" "$TEST_ROOT/readonly.err"
[[ $(cat "$TEST_ROOT/readonly.out") == "$newDrv" ]]
nix eval --read-only --json "$flake#value" -v 2> "$TEST_ROOT/readonly-json.err" > /dev/null
grepQuiet "by the evaluation daemon" "$TEST_ROOT/readonly-json.err"

stopEvalDaemon
