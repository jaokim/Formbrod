#!/usr/bin/env bash
#
# fetch-deps.sh - download and unpack Formbrod's third-party libraries.
#
#   tools/fetch-deps.sh             fetch everything in tools/deps.list
#   tools/fetch-deps.sh ffmpeg      fetch only the named dependencies
#   tools/fetch-deps.sh -f ffmpeg   re-unpack even if already installed
#   tools/fetch-deps.sh -l          list dependencies and their state
#
# Result: deps/<name>/include and deps/<name>/lib. Downloads are cached in
# deps/cache/. If a site can't be reached, download the archive yourself
# and put it in deps/cache/ under the file name from the URL.
#
# Environment:
#   DEPS_DIR    where to install (default: <repo>/deps)
#   DEPS_LIST   which list to read (default: tools/deps.list)
#   DEPS_CRT    C runtime to prefer when an archive has several builds:
#               newlib (default, what the Makefile builds with), clib4 or
#               clib2. Libraries must match the program's runtime.
#
# Needs curl or wget, sha256sum or shasum, and for .lha archives lha or
# lhasa (Debian/Ubuntu: apt install lhasa).

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS_DIR=${DEPS_DIR:-$ROOT/deps}
DEPS_LIST=${DEPS_LIST:-$ROOT/tools/deps.list}
DEPS_CRT=${DEPS_CRT:-newlib}
case $DEPS_CRT in newlib|clib4|clib2) ;; *) echo "fetch-deps: DEPS_CRT must be newlib, clib4 or clib2" >&2; exit 1 ;; esac
CACHE=$DEPS_DIR/cache

force=0
list_only=0
WORK=
trap '[ -n "$WORK" ] && rm -rf "$WORK"' EXIT

die()  { echo "fetch-deps: $*" >&2; exit 1; }
info() { echo "fetch-deps: $*"; }

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

while getopts "flh" opt; do
    case $opt in
        f) force=1 ;;
        l) list_only=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))

[ -f "$DEPS_LIST" ] || die "no dependency list at $DEPS_LIST"

download() {
    local url=$1 out=$2
    if command -v curl >/dev/null; then
        curl -fL --retry 3 -o "$out.part" "$url"
    elif command -v wget >/dev/null; then
        wget -O "$out.part" "$url"
    else
        die "need curl or wget to download $url"
    fi
    mv "$out.part" "$out"
}

sha256() {
    if command -v sha256sum >/dev/null; then
        sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum >/dev/null; then
        shasum -a 256 "$1" | cut -d' ' -f1
    else
        die "need sha256sum or shasum"
    fi
}

unpack() {
    local archive=$1 dest=$2
    mkdir -p "$dest"
    case $archive in
        *.lha|*.lzh|*.LHA|*.LZH)
            if command -v lha >/dev/null; then
                (cd "$dest" && lha xq "$archive")
            elif command -v lhasa >/dev/null; then
                (cd "$dest" && lhasa -xq "$archive")
            else
                die "need lha or lhasa to unpack $(basename "$archive") (apt install lhasa)"
            fi ;;
        *.tar|*.tar.*|*.tgz)
            tar -xf "$archive" -C "$dest" ;;
        *.zip|*.ZIP)
            unzip -q "$archive" -d "$dest" ;;
        *)
            die "don't know how to unpack $(basename "$archive")" ;;
    esac
}

# Prints the best match for a path suffix inside a tree: builds for
# $DEPS_CRT first, then paths naming no runtime, then other runtimes; the
# shortest path wins a tie.
best_match() {
    local tree=$1 suffix=$2
    find "$tree" -type f -path "*/$suffix" 2>/dev/null |
        awk -v crt="$DEPS_CRT" -v skip=$((${#tree} + 1)) '{
            rel = substr($0, skip)     # judge only the path inside the archive
            rank = 1
            if (rel ~ /newlib|clib[24]/) rank = 2
            if (index(rel, crt))         rank = 0
            printf "%d %05d %s\n", rank, length(rel), $0
        }' | sort | head -n 1 | cut -d' ' -f3-
}

install_dep() {
    local name=$1 url=$2 want=$3 header=$4 lib=$5
    local dest=$DEPS_DIR/$name
    local file=$CACHE/$(basename "${url%%\?*}")
    local work hdr libfile inc_root lib_dir got

    if [ $force -eq 0 ] && [ -f "$dest/include/$header" ] && [ -f "$dest/lib/$lib" ]; then
        info "$name is already installed in $dest (use -f to reinstall)"
        return
    fi

    mkdir -p "$CACHE"
    if [ ! -f "$file" ]; then
        info "downloading $name from $url"
        download "$url" "$file" || die "download of $name failed; put the archive at $file and run again"
    fi

    got=$(sha256 "$file")
    if [ "$want" = "-" ]; then
        info "$name sha256 $got (not pinned; add it to $(basename "$DEPS_LIST") to check it)"
    elif [ "$got" != "$want" ]; then
        die "$name checksum mismatch: expected $want, got $got (delete $file to download again)"
    fi

    work=$(mktemp -d "$DEPS_DIR/.unpack-$name.XXXXXX")
    WORK=$work
    unpack "$file" "$work"

    hdr=$(best_match "$work" "$header")
    libfile=$(best_match "$work" "$lib")
    [ -n "$hdr" ] || die "$name: no $header in the archive"
    [ -n "$libfile" ] || die "$name: no $lib in the archive"

    inc_root=${hdr%/$header}
    lib_dir=$(dirname "$libfile")

    rm -rf "$dest"
    mkdir -p "$dest/include" "$dest/lib"
    cp -R "$inc_root/." "$dest/include/"
    # Static libraries only: linking a .so on AmigaOS 4 needs extra steps.
    find "$lib_dir" -maxdepth 1 -type f -name '*.a' -exec cp {} "$dest/lib/" \;

    {
        echo "url=$url"
        echo "sha256=$got"
        echo "crt=$DEPS_CRT"
        echo "include_from=${inc_root#$work/}"
        echo "lib_from=${lib_dir#$work/}"
    } > "$dest/.installed"

    rm -rf "$work"
    WORK=
    info "$name installed: $(ls "$dest/lib" | wc -l | tr -d ' ') libraries in $dest/lib"
}

found=0
while read -r name url sha header lib extra; do
    case $name in ''|'#'*) continue ;; esac
    [ -n "${lib:-}" ] || die "bad line for '$name' in $DEPS_LIST (need 5 columns)"

    if [ $# -gt 0 ]; then
        wanted=0
        for arg in "$@"; do [ "$arg" = "$name" ] && wanted=1; done
        [ $wanted -eq 1 ] || continue
    fi
    found=$((found + 1))

    if [ $list_only -eq 1 ]; then
        if [ -f "$DEPS_DIR/$name/.installed" ]; then state=installed; else state=missing; fi
        printf '%-12s %-9s %s\n' "$name" "$state" "$url"
        continue
    fi
    install_dep "$name" "$url" "$sha" "$header" "$lib"
done < "$DEPS_LIST"

[ $found -gt 0 ] || die "no matching dependencies in $DEPS_LIST"
