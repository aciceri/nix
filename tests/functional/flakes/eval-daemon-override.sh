#!/usr/bin/env bash

# `nix eval-daemon` with `--override-input` to a local checkout: the input is
# unlocked, mounted at a stable root, and its files are edited in place
# without committing. Cells survive edits of files they did not read and
# are recomputed after edits of files they read. `--verify` cannot be
# combined with overrides, so every value is compared with `nix eval` here.

source ./common.sh

requireGit

pkgsDir=$TEST_ROOT/eval-daemon-override-pkgs
flakeDir=$TEST_ROOT/eval-daemon-override
daemonLog=$TEST_ROOT/eval-daemon-override.log

createGitRepo "$pkgsDir" ""
mkdir -p "$pkgsDir/pkgs/top-level"
cat > "$pkgsDir/pkgs/top-level/impure.nix" <<'EOF'
{ config ? { }, ... }:
{
  greeting = "hello-${config.greeting}";
  imported = (import ../../lib.nix).value;
  nested = (import ../../lib.nix).inner;
  rendered = toString ../../lib.nix;
  # Reads made by a function of the imported file, called from here.
  called = (import ../../lib.nix).read "data.txt";
  # A file read through a directory listing and an existence test.
  listed = builtins.attrNames (builtins.readDir ../../dir);
  present = builtins.pathExists ../../maybe.nix;
  # A derivation whose source is a copy of a directory of the tree: its
  # path changes with the directory's contents, not with the tree's.
  src = builtins.path { path = ../../dir; name = "dir"; };
  drv = (derivation {
    name = "with-src"; system = "x86_64-linux"; builder = "/bin/sh";
    src = builtins.path { path = ../../dir; name = "dir"; };
  }).drvPath;
  # An import resolved through a directory (`default.nix`).
  fromDir = (import ../../dir2).v;
}
EOF
mkdir "$pkgsDir/dir2"
echo '{ v = "dir2-1"; }' > "$pkgsDir/dir2/default.nix"
cat > "$pkgsDir/lib.nix" <<'EOF'
{
  value = "lib1";
  inner = (import ./inner.nix).v;
  read = name: builtins.readFile (./. + "/${name}");
}
EOF
echo '{ v = "inner1"; }' > "$pkgsDir/inner.nix"
echo data1 > "$pkgsDir/data.txt"
mkdir "$pkgsDir/dir"
echo a > "$pkgsDir/dir/a"
echo 1 > "$pkgsDir/unrelated.nix"
git -C "$pkgsDir" add -A
git -C "$pkgsDir" commit -m init

createGitRepo "$flakeDir" ""
cat > "$flakeDir/flake.nix" <<EOF
{
  inputs.pkgs = { url = "git+file://$pkgsDir"; flake = false; };
  outputs = { self, pkgs }:
    let p = import (pkgs + "/pkgs/top-level/impure.nix") { config.greeting = "world"; };
    in { inherit (p) greeting imported nested rendered called listed present src drv fromDir; };
}
EOF
git -C "$flakeDir" add -A
nix flake lock "$flakeDir"
git -C "$flakeDir" add flake.lock
git -C "$flakeDir" commit -m init

override=(--override-input pkgs "git+file://$pkgsDir" --no-write-lock-file)

coproc DAEMON { nix eval-daemon --extra-experimental-features eval-daemon "${override[@]}" 2>>"$daemonLog"; }
daemonPid=$DAEMON_PID

# Evaluate an attribute, check the cell hits and misses, and compare the
# value with a cold evaluation.
check() {
    local attr=$1 hits=$2 misses=$3 reply cold
    echo "eval git+file://$flakeDir#$attr" >&"${DAEMON[1]}"
    read -r reply <&"${DAEMON[0]}"
    echo "$attr: $reply" >&2
    [[ $(jq .ok <<< "$reply") == true ]]
    [[ $(jq .stats.cells.hits <<< "$reply") == "$hits" ]]
    [[ $(jq .stats.cells.misses <<< "$reply") == "$misses" ]]
    cold=$(nix eval --no-eval-cache --json "${override[@]}" "git+file://$flakeDir#$attr")
    [[ $(jq -c .value <<< "$reply") == "$cold" ]]
}

check greeting 0 1
check imported 1 0

# An uncommitted edit of a file the cell did not read.
echo 2 > "$pkgsDir/unrelated.nix"
check greeting 1 0
# The rendered path shows the store path of the dirty tree, and is reused.
check rendered 1 0

# An uncommitted edit of a file the cell imported.
sed -i 's/lib1/lib2/' "$pkgsDir/lib.nix"
check imported 0 1
check greeting 1 0

# Undone: the earlier instance is found again.
git -C "$pkgsDir" checkout -- lib.nix
check imported 1 0

# A file imported by the imported file (read for the first time now, from
# the reused instance): the importer of `lib.nix` depends on it through
# the reads of `lib.nix`'s value, and is valid again once the edit is
# undone.
check nested 1 0
echo '{ v = "inner2"; }' > "$pkgsDir/inner.nix"
check nested 0 1
check imported 1 0
echo '{ v = "inner1"; }' > "$pkgsDir/inner.nix"
check nested 1 0

# A file read by a function of the imported file, called by the cell:
# the read belongs to the cell, and a change of the file invalidates it.
check called 1 0
echo data2 > "$pkgsDir/data.txt"
check called 0 1
check imported 1 0
git -C "$pkgsDir" checkout -- data.txt
check called 1 0

# A directory listing and an existence test (new files must be added to
# the index: the git fetcher ignores untracked files).
check listed 1 0
echo b > "$pkgsDir/dir/b"
git -C "$pkgsDir" add dir/b
check listed 0 1
git -C "$pkgsDir" rm -q -f dir/b
check listed 1 0
check present 1 0
echo '{ }' > "$pkgsDir/maybe.nix"
git -C "$pkgsDir" add maybe.nix
check present 0 1
git -C "$pkgsDir" rm -q -f maybe.nix
check present 1 0

# The derivation with a copied source: reused after an edit elsewhere,
# recomputed after an edit inside the directory.
check drv 1 0
echo 3 > "$pkgsDir/unrelated.nix"
check drv 1 0
check src 1 0
echo a2 > "$pkgsDir/dir/a"
check src 0 1
check drv 1 0
git -C "$pkgsDir" checkout -- dir/a
check drv 1 0

# An import of a directory reads its `default.nix`.
check fromDir 1 0
echo '{ v = "dir2-2"; }' > "$pkgsDir/dir2/default.nix"
check fromDir 0 1
git -C "$pkgsDir" checkout -- dir2/default.nix
check fromDir 1 0

# `reset` drops every cached file and cell; the next requests compute
# again and the results are the same.
echo reset >&"${DAEMON[1]}"
read -r reply <&"${DAEMON[0]}"
[[ $(jq .ok <<< "$reply") == true ]]
check greeting 0 1
check nested 1 0
echo '{ v = "inner3"; }' > "$pkgsDir/inner.nix"
check nested 0 1
git -C "$pkgsDir" checkout -- inner.nix
check nested 1 0

# A file renamed: the import of the old name fails, the new name works,
# and the rename undone finds the earlier instance.
git -C "$pkgsDir" mv inner.nix inner2.nix
echo "eval git+file://$flakeDir#nested" >&"${DAEMON[1]}"
read -r reply <&"${DAEMON[0]}"
[[ $(jq .ok <<< "$reply") == false ]]
jq -r .error <<< "$reply" | grepQuiet "inner.nix"
git -C "$pkgsDir" mv inner2.nix inner.nix
check nested 1 0

# The edit committed: the tree is clean again at a new revision, and the
# instances of the dirty state are reused.
echo '{ v = "inner4"; }' > "$pkgsDir/inner.nix"
check nested 0 1
git -C "$pkgsDir" commit -qam inner4
check nested 1 0
check greeting 1 0

# The cell site itself edited.
sed -i 's/hello-/hi-/' "$pkgsDir/pkgs/top-level/impure.nix"
check greeting 0 1
check imported 1 0

# The checkout restored to the committed state.
git -C "$pkgsDir" checkout -- .
check greeting 1 0
check rendered 1 0

echo quit >&"${DAEMON[1]}"
read -r _ <&"${DAEMON[0]}"
wait "$daemonPid"
