#!/usr/bin/env bash
#
# build.sh - Build jzIntv for Linux and/or Windows.
#
# Executables end up in bin/<platform>/ :
#     bin/linux/jzintv
#     bin/windows/jzintv.exe  (+ SDL2.dll, libwinpthread-1.dll)
#
# Usage:
#     ./build.sh [linux|windows|all]      (default: all)
#
# Designed to be GENERIC (any Linux distro):
#   * Linux   -> uses the system SDL2 via sdl2-config/pkg-config.  If the dev
#                headers are missing (e.g. SteamOS) it falls back to those in
#                build-support/sdl2-linux-headers/ if present.
#   * Windows -> cross-compiles with MinGW-w64.  The SDL2-MinGW dev files are
#                taken, in order, from $SDL2_MINGW_PREFIX, the bundle in
#                build-support/sdl2-mingw/, or downloaded from libsdl.org.
#
# Optional environment variables:
#     CC, CXX            Linux compilers              (default gcc / g++)
#     MINGW              MinGW triplet                (default x86_64-w64-mingw32)
#     SDL2_MINGW_PREFIX  SDL2 mingw dir (include/lib) (else bundle/download)
#     SDL2_VERSION       SDL2 version to download     (default 2.32.8)
#     GNU_READLINE       1 to enable readline         (default 0)
#     STRIP              0 to NOT strip               (default 1)
#     JOBS               make parallelism             (default: nproc)
#
set -euo pipefail

# --- script location (the repo) --------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR/src"
BIN="$SCRIPT_DIR/bin"
SUP="$SCRIPT_DIR/build-support"

# --- parameters ------------------------------------------------------------
TARGET="${1:-all}"
CC="${CC:-gcc}"
CXX="${CXX:-g++}"
MINGW="${MINGW:-x86_64-w64-mingw32}"
SDL2_VERSION="${SDL2_VERSION:-2.32.8}"
GNU_READLINE="${GNU_READLINE:-0}"
STRIP="${STRIP:-1}"
if [ -z "${JOBS:-}" ]; then JOBS="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"; fi

c_reset=$'\033[0m'; c_bold=$'\033[1m'; c_red=$'\033[31m'; c_grn=$'\033[32m'; c_yel=$'\033[33m'
log()  { printf '%s==> %s%s\n' "$c_bold" "$*" "$c_reset"; }
ok()   { printf '%s    %s%s\n' "$c_grn" "$*" "$c_reset"; }
warn() { printf '%s[!] %s%s\n' "$c_yel" "$*" "$c_reset" >&2; }
die()  { printf '%s[X] %s%s\n' "$c_red" "$*" "$c_reset" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

clean_objs() { find "$SRC" -name '*.o' -delete 2>/dev/null || true; }

# ===========================================================================
#  LINUX
# ===========================================================================
build_linux() {
    log "Build Linux ($(uname -m), release + LTO)"
    have "$CC"  || die "C compiler '$CC' not found"
    have make   || die "'make' not found"

    # Detect the system SDL2; otherwise fall back to the bundled headers (SteamOS).
    local extra="" sdl2_cflags=""
    if have sdl2-config; then sdl2_cflags="$(sdl2-config --cflags 2>/dev/null || true)"
    elif have pkg-config && pkg-config --exists sdl2 2>/dev/null; then sdl2_cflags="$(pkg-config --cflags sdl2)"; fi

    if printf '#include <SDL.h>\nint main(void){return 0;}\n' | "$CC" $sdl2_cflags -x c - -o /dev/null -c 2>/dev/null; then
        ok "System SDL2 found (headers OK)"
    elif [ -f "$SUP/sdl2-linux-headers/SDL2/SDL.h" ]; then
        extra="-I$SUP/sdl2-linux-headers -I$SUP/sdl2-linux-headers/SDL2"
        warn "System SDL2 headers missing -> using the bundle build-support/sdl2-linux-headers"
    else
        die "SDL2 dev not available.  Install the SDL2 development package (e.g. libsdl2-dev / SDL2-devel / sdl2) or provide build-support/sdl2-linux-headers/."
    fi

    clean_objs
    mkdir -p "$BIN"                       # the Makefile links into ../bin/: it must exist
    ( cd "$SRC" && rm -f ../bin/jzintv && \
      make -f Makefile.linux_sdl2 SVN_REV=0 SVN_DTY=0 GNU_READLINE="$GNU_READLINE" \
           EXTRA="$extra" -j"$JOBS" ../bin/jzintv )
    mkdir -p "$BIN/linux"
    mv -f "$BIN/jzintv" "$BIN/linux/jzintv"
    [ "$STRIP" = "1" ] && strip --strip-unneeded "$BIN/linux/jzintv"
    clean_objs
    ok "-> bin/linux/jzintv  ($(du -h "$BIN/linux/jzintv" | cut -f1))"
}

# ===========================================================================
#  WINDOWS (cross-compile with MinGW-w64)
# ===========================================================================
resolve_sdl2_mingw() {
    # Print the prefix (dir with include/ and lib/libSDL2.dll.a).  Empty if unresolved.
    local p
    if [ -n "${SDL2_MINGW_PREFIX:-}" ] && [ -f "$SDL2_MINGW_PREFIX/lib/libSDL2.dll.a" ]; then
        echo "$SDL2_MINGW_PREFIX"; return 0; fi
    if [ -f "$SUP/sdl2-mingw/$MINGW/lib/libSDL2.dll.a" ]; then
        echo "$SUP/sdl2-mingw/$MINGW"; return 0; fi
    # download from libsdl.org (once, into the bundle cache)
    local url="https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-mingw.tar.gz"
    local dl
    if have curl; then dl="curl -fL --retry 3 -o"; elif have wget; then dl="wget -O"; else return 1; fi
    warn "SDL2 mingw not found: downloading SDL2-devel-${SDL2_VERSION}-mingw from libsdl.org ..." >&2
    local tmp; tmp="$(mktemp -d)"
    if $dl "$tmp/sdl2.tgz" "$url" >&2 2>&1 && tar xzf "$tmp/sdl2.tgz" -C "$tmp" 2>/dev/null; then
        mkdir -p "$SUP/sdl2-mingw"
        cp -r "$tmp/SDL2-${SDL2_VERSION}/$MINGW" "$SUP/sdl2-mingw/$MINGW"
        rm -rf "$tmp"
        [ -f "$SUP/sdl2-mingw/$MINGW/lib/libSDL2.dll.a" ] && { echo "$SUP/sdl2-mingw/$MINGW"; return 0; }
    fi
    rm -rf "$tmp"; return 1
}

build_windows() {
    log "Build Windows ($MINGW, release + LTO)"
    local mcc="${MINGW}-gcc" mcxx="${MINGW}-g++" mstrip="${MINGW}-strip"
    have "$mcc"  || die "cross-compiler '$mcc' not found.  Install MinGW-w64 (Arch/SteamOS: pacman -S mingw-w64-gcc ; Debian/Ubuntu: apt install gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 ; Fedora: dnf install mingw64-gcc mingw64-gcc-c++)."
    have "$mcxx" || die "'$mcxx' not found"

    local P; P="$(resolve_sdl2_mingw || true)"
    [ -n "$P" ] && [ -f "$P/lib/libSDL2.dll.a" ] || die "SDL2 dev for MinGW not available.  Provide \$SDL2_MINGW_PREFIX or build-support/sdl2-mingw/$MINGW, or enable the download (curl/wget)."
    ok "SDL2 mingw: $P"

    clean_objs
    mkdir -p "$BIN"                       # the Makefile links into ../bin/: it must exist
    ( cd "$SRC" && rm -f ../bin/jzintv.exe && \
      make -f Makefile.w32_sdl2 \
           CC="$mcc -std=gnu11" \
           CXX="$mcxx -std=c++14 -U__STRICT_ANSI__ -fvisibility=hidden" \
           WARN= WARNXX= GNU_READLINE="$GNU_READLINE" SVN_REV=0 SVN_DTY=0 \
           EXTRA="-I$P/include -I$P/include/SDL2" \
           SDL2_CFLAGS=" " \
           SDL2_LFLAGS="-L$P/lib -lmingw32 -lSDL2main -lSDL2" \
           LFLAGS="-L../lib -static-libgcc -static-libstdc++ -mconsole" \
           -j"$JOBS" ../bin/jzintv.exe )
    mkdir -p "$BIN/windows"
    mv -f "$BIN/jzintv.exe" "$BIN/windows/jzintv.exe"
    [ "$STRIP" = "1" ] && "$mstrip" --strip-all "$BIN/windows/jzintv.exe"

    # Runtime DLLs to ship: SDL2 + libwinpthread (the others are system DLLs).
    cp -f "$P/bin/SDL2.dll" "$BIN/windows/" 2>/dev/null || warn "SDL2.dll not found in $P/bin"
    # Search only directories that exist (an empty sysroot must not reach find,
    # or it errors and, under set -e/pipefail, aborts before the DLL is copied).
    local wp="" sysroot d
    sysroot="$("$mcc" -print-sysroot 2>/dev/null || true)"
    for d in "/usr/$MINGW" "$sysroot"; do
        [ -n "$d" ] && [ -d "$d" ] || continue
        wp="$(find "$d" -name 'libwinpthread-1.dll' 2>/dev/null | head -1)"
        [ -n "$wp" ] && break
    done
    if [ -n "$wp" ]; then cp -f "$wp" "$BIN/windows/"; else warn "libwinpthread-1.dll not found: copy it next to the exe by hand"; fi

    clean_objs
    ok "-> bin/windows/jzintv.exe  ($(du -h "$BIN/windows/jzintv.exe" | cut -f1))  + DLLs"
}

# ===========================================================================
#  MAIN
# ===========================================================================
rc=0
case "$TARGET" in
    linux)   build_linux ;;
    windows) build_windows ;;
    all)
        # In 'all', a target whose toolchain is missing is skipped, not fatal.
        build_linux || rc=1
        if have "${MINGW}-gcc"; then build_windows || rc=1
        else warn "MinGW-w64 (${MINGW}-gcc) not found: skipping Windows"; fi
        ;;
    -h|--help|help) sed -n '2,28p' "$0"; exit 0 ;;
    *) die "unknown target: '$TARGET' (use: linux | windows | all)";;
esac

log "Contents of bin/:"
find "$BIN" -mindepth 2 -maxdepth 2 -type f 2>/dev/null | sed "s#^$SCRIPT_DIR/#  #" | sort
exit $rc
