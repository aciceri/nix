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
# The function closes over this `let`: the evaluated file is kept when the
# tree changes (a file cell), unless something it read changed.
let
  top = builtins.readFile ../../top.txt;
in
{ config ? { }, ... }:
# Read the argument right away, so that each call gets its own instance.
assert builtins.isString config.name;
{
  inherit top;
  imported = (import ../../lib.nix).value;
  contents = builtins.readFile ../../data.txt;
  exists = builtins.pathExists ../../maybe.nix;
  listing = builtins.attrNames (builtins.readDir ../../dir);
  copied = "${../../dir}";
  filtered = builtins.path { path = ../../dir; name = "filtered"; filter = p: t: baseNameOf p != "skip"; };
  rendered = toString ../../dir;
  lazy = import ../../lazy.nix;
  # A rendered path that leaves the evaluator: in a derivation, hashed,
  # written to a file. The store path of the current tree must appear.
  inDerivation = (derivation {
    name = "with-path"; system = "x86_64-linux"; builder = "/bin/sh";
    path = toString ../../dir;
    __structuredAttrs = true;
    nested.path = toString ../../dir;
  }).drvPath;
  hashed = builtins.hashString "sha256" (toString ../../dir);
  written = builtins.toFile "path.txt" (toString ../../dir);
  # Names derived from a rendered path (`lib.cleanSource` does this).
  namedCopy = builtins.path { path = ../../dir; name = baseNameOf (toString ../..); };
  namedDrv = (derivation {
    name = "x-" + baseNameOf (toString ../..); system = "x86_64-linux"; builder = "/bin/sh";
  }).drvPath;
  namedFile = builtins.toFile (baseNameOf (toString ../..)) "x";
  # Rendered paths used as strings to reach the tree again.
  readString = builtins.readFile (toString ../../data.txt);
  importString = (import (toString ../../lib.nix)).value;
  existsString = builtins.pathExists (toString ../../maybe.nix);
  hashedFile = builtins.hashFile "sha256" (toString ../../data.txt);
  copiedString = builtins.path { path = toString ../../dir; name = "dir"; };
  traced = builtins.trace "traced path ${toString ../../dir}" (builtins.warn "warned path ${toString ../../dir}" 1);
}
EOF
cat > "$flakeDir/flake.nix" <<'EOF'
{
  outputs = { self }:
    let
      get = name: attr: (import ./pkgs/top-level/impure.nix { config.name = name; }).${attr};
      names = [ "imported" "contents" "exists" "listing" "copied" "filtered" "rendered" "lazy" "top" "inDerivation" "hashed" "written" "namedCopy" "namedDrv" "namedFile" "readString" "importString" "existsString" "hashedFile" "copiedString" "traced" ];
    in
    builtins.listToAttrs (map (name: { inherit name; value = get name name; }) names)
    // {
      importedLazy = get "imported" "lazy";
      # The flake's own store path, as a string with context, in a derivation.
      selfInDerivation = (derivation {
        name = "with-self"; system = "x86_64-linux"; builder = "/bin/sh";
        src = self.outPath;
      }).drvPath;
      selfRendered = "${toString self}/dir";
    };
}
EOF
echo '{ value = "one"; }' > "$flakeDir/lib.nix"
echo -n data1 > "$flakeDir/data.txt"
echo '"lazy1"' > "$flakeDir/lazy.nix"
echo a > "$flakeDir/dir/a"
echo s > "$flakeDir/dir/skip"
echo 1 > "$flakeDir/unrelated.nix"
echo -n top1 > "$flakeDir/top.txt"
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

names="imported contents exists listing copied filtered rendered lazy top inDerivation hashed written namedCopy namedDrv namedFile readString importString existsString hashedFile copiedString traced"

# One instance per attribute, each reading its own file.
for attr in $names; do
    check "$attr" 0 1
done

# The flake's own store path, in the excluded root's outputs (not a cell).
check selfInDerivation 0 0
check selfRendered 0 0

# `trace` and `warn` print the store path of the current contents, in the
# daemon as in the `--verify` child (whose stderr is the same log).
storePath=$(nix flake metadata --json "git+file://$flakeDir" | jq -r .path)
grepQuiet "trace: traced path $storePath/dir" "$daemonLog"
grepQuiet "warned path $storePath/dir" "$daemonLog"
grep 'traced path\|warned path' "$daemonLog" | grepQuietInverse -v "path $storePath/dir"

# A file no instance read: they are all reused although the tree (and its
# store path) changed. The one that rendered a path of the tree is reused
# too: the string is virtual until it leaves the evaluator, and the result
# still shows the current store path (checked against a cold evaluation).
# The ones that let the rendered path leave the evaluator depend on the
# store path and are computed again.
echo 2 > "$flakeDir/unrelated.nix"
for attr in $names; do
    case $attr in
    inDerivation | hashed | written | namedCopy | namedDrv | namedFile) check "$attr" 0 1 ;;
    *) check "$attr" 1 0 ;;
    esac
done
check selfInDerivation 0 0
check selfRendered 0 0

# A file that an instance has not read yet is read from the current tree.
echo '"lazy2"' > "$flakeDir/lazy.nix"
check importedLazy 1 0
check lazy 0 1

# Files that instances read: each instance is reused until its file is edited.
editAndCheck() {
    local attr=$1 file=$2 contents=$3
    check "$attr" 1 0
    echo "$contents" > "$flakeDir/$file"
    # Git flakes only contain tracked files.
    git -C "$flakeDir" add -A
    check "$attr" 0 1
}
editAndCheck imported lib.nix '{ value = "two"; }'
editAndCheck contents data.txt data2
editAndCheck exists maybe.nix '{ }'
# `copied` first: the new file below changes the copied directory too.
editAndCheck copied dir/a a2
editAndCheck listing dir/b b

# A filtered copy is replayed with the same filter: a change to a file it
# excludes keeps the instance, a change to a file it includes does not.
check filtered 0 1
echo s2 > "$flakeDir/dir/skip"
editAndCheck filtered dir/a a3

# A file read by the top level of the file defining the function: the
# evaluated file is dropped, so every instance of its function is new.
editAndCheck top top.txt top2
check contents 0 1

echo quit >&"${DAEMON[1]}"
read -r _ <&"${DAEMON[0]}"
wait "$daemonPid"
