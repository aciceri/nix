#!/usr/bin/env bash

# Stable roots in `nix eval-daemon`: the files of an unlocked input (here
# the flake being evaluated) keep their identity when the tree changes, so
# a traced cell defined in it is reused after edits to files it did not
# read, and not after edits to files it read (imports, `readFile`,
# `pathExists`, `readDir`, copies to the store, rendered paths). Every
# request is checked against a cold evaluation with `--verify`.

source ./common.sh

requireGit

flakeDir=$TEST_ROOT/eval-daemon-roots
daemonLog=$TEST_ROOT/eval-daemon-roots.log

createGitRepo "$flakeDir" ""
mkdir -p "$flakeDir/pkgs/top-level" "$flakeDir/dir"
cat > "$flakeDir/pkgs/top-level/impure.nix" <<'EOF'
{ config ? { }, ... }:
# Read the argument right away, so that each call gets its own instance.
assert builtins.isString config.name;
{
  imported = (import ../../lib.nix).value;
  contents = builtins.readFile ../../data.txt;
  exists = builtins.pathExists ../../maybe.nix;
  listing = builtins.attrNames (builtins.readDir ../../dir);
  copied = "${../../dir}";
  filtered = builtins.path { path = ../../dir; name = "filtered"; filter = p: t: baseNameOf p != "skip"; };
  rendered = toString ../../dir;
  lazy = import ../../lazy.nix;
}
EOF
cat > "$flakeDir/flake.nix" <<'EOF'
{
  outputs = { self }:
    let
      get = name: attr: (import ./pkgs/top-level/impure.nix { config.name = name; }).${attr};
      names = [ "imported" "contents" "exists" "listing" "copied" "filtered" "rendered" "lazy" ];
    in
    builtins.listToAttrs (map (name: { inherit name; value = get name name; }) names)
    // { importedLazy = get "imported" "lazy"; };
}
EOF
echo '{ value = "one"; }' > "$flakeDir/lib.nix"
echo -n data1 > "$flakeDir/data.txt"
echo '"lazy1"' > "$flakeDir/lazy.nix"
echo a > "$flakeDir/dir/a"
echo s > "$flakeDir/dir/skip"
echo 1 > "$flakeDir/unrelated.nix"
git -C "$flakeDir" add -A
git -C "$flakeDir" commit -m init

coproc DAEMON { nix eval-daemon --extra-experimental-features eval-daemon --verify 2>>"$daemonLog"; }
daemonPid=$DAEMON_PID

# Evaluate an attribute and check the cell hits and misses of the request and
# that a cold evaluation gives the same value.
check() {
    local attr=$1 hits=$2 misses=$3 reply
    echo "eval git+file://$flakeDir#$attr" >&"${DAEMON[1]}"
    read -r reply <&"${DAEMON[0]}"
    echo "$attr: $reply" >&2
    [[ $(jq .ok <<< "$reply") == true ]]
    [[ $(jq .stats.cells.hits <<< "$reply") == "$hits" ]]
    [[ $(jq .stats.cells.misses <<< "$reply") == "$misses" ]]
    [[ $(jq .verify.matches <<< "$reply") == true ]]
}

names="imported contents exists listing copied filtered rendered lazy"

# One instance per attribute, each reading its own file.
for attr in $names; do
    check "$attr" 0 1
done

# A file no instance read: they are all reused although the tree (and its
# store path) changed, except the one that rendered a path of the tree.
echo 2 > "$flakeDir/unrelated.nix"
for attr in $names; do
    if [[ $attr == rendered ]]; then check "$attr" 0 1; else check "$attr" 1 0; fi
done

# A file that an instance has not read yet is read from the current tree.
echo '"lazy2"' > "$flakeDir/lazy.nix"
check importedLazy 1 0
check lazy 0 1

# Files that instances read.
editAndCheck() {
    local attr=$1 file=$2 contents=$3
    echo "$contents" > "$flakeDir/$file"
    # Git flakes only contain tracked files.
    git -C "$flakeDir" add -A
    check "$attr" 0 1
}
editAndCheck imported lib.nix '{ value = "two"; }'
editAndCheck contents data.txt data2
editAndCheck exists maybe.nix '{ }'
editAndCheck listing dir/b b
editAndCheck copied dir/a a2

# A filtered copy is replayed with the same filter: a change to a file it
# excludes keeps the instance, a change to a file it includes does not.
check filtered 0 1
echo s2 > "$flakeDir/dir/skip"
check filtered 1 0
editAndCheck filtered dir/a a3

echo quit >&"${DAEMON[1]}"
read -r _ <&"${DAEMON[0]}"
wait "$daemonPid"
