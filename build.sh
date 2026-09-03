#!/usr/bin/env bash
#
# build.sh - Build jzIntv for Linux, Windows, armv7 and/or Sprint.
#
# Executables end up in bin/<platform>/ :
#     bin/linux/jzintv
#     bin/windows/jzintv.exe  (+ SDL2.dll, libwinpthread-1.dll)
#     bin/sprint/jzintv        (+ update.zip)
#     bin/linux-armhf/jzintv
#
# Usage:
#     ./build.sh [linux|windows|linux-armhf|sprint|all]   (default: all)
#
# Designed to be GENERIC (any Linux distro):
#   * Linux   -> uses the system SDL2 via sdl2-config/pkg-config.  If the dev
#                headers are missing (e.g. SteamOS) it falls back to those in
#                build-support/sdl2-linux-headers/ if present.
#   * Windows -> cross-compiles with MinGW-w64.  The SDL2-MinGW dev files are
#                taken, in order, from $SDL2_MINGW_PREFIX, the bundle in
#                build-support/sdl2-mingw/, or downloaded from libsdl.org.
#   * Sprint  -> Intellivision Sprint: cross-compiles ARM 32-bit (armhf) with
#                the arm-linux-gnueabihf-* toolchain (Makefile.sprint) and packs
#                the executable into bin/sprint/update.zip.  Needs the ARM
#                toolchain in PATH (native on Debian, or inside a Debian
#                container on SteamOS) + libsdl2-dev:armhf + zip.  In 'all' it
#                is skipped when the ARM toolchain is absent.
#   * linux-armhf -> a PLAIN armv7 Linux build (same feature set as the
#                'linux' target, just cross-compiled).  This is the one to
#                ship to a Raspberry Pi or any other armhf box; 'sprint' is
#                the Sprint-specific flavour and is not interchangeable.
#                Same toolchain requirements as sprint, minus zip.
#
# Optional environment variables:
#     CC, CXX            Linux compilers              (default gcc / g++)
#     MINGW              MinGW triplet                (default x86_64-w64-mingw32)
#     SDL2_MINGW_PREFIX  SDL2 mingw dir (include/lib) (else bundle/download)
#     SDL2_VERSION       SDL2 version to download     (default 2.32.8)
#     ARM_CROSS          ARM toolchain prefix         (default arm-linux-gnueabihf-)
#     SPRINT_ZIP_TEMPLATE base update.zip for Sprint  (else build-support/../sprint)
#     ARM_PKG_CONFIG_PATH armhf pkgconfig dir       (default /usr/lib/arm-linux-gnueabihf/pkgconfig)
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
ARM_CROSS="${ARM_CROSS:-arm-linux-gnueabihf-}"
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
#  SPRINT  (Intellivision Sprint: cross-compile ARM 32-bit armhf -> update.zip)
# ===========================================================================
build_sprint() {
    log "Build Sprint (Intellivision Sprint, ARM 32-bit armhf) + update.zip"
    local pfx="$ARM_CROSS"
    local acc="${pfx}gcc" acxx="${pfx}g++" astrip="${pfx}strip"
    have "$acc" || die "ARM cross-compiler '$acc' not found.  Sprint needs an ARM
(armhf) toolchain, which is not installed by default on SteamOS/Arch.  Two ways:

  A) Debian/Ubuntu (native):
       sudo dpkg --add-architecture armhf && sudo apt update
       sudo apt install -y crossbuild-essential-armhf libsdl2-dev:armhf make zip
       ./build.sh sprint

  B) SteamOS/Arch (or any distro without armhf) -> throwaway Debian container:
     one line, from the repo directory (outputs stay owned by your user):
       podman run --rm -v \"\$PWD\":/work -w /work docker.io/library/debian:bullseye bash -c \\
         'dpkg --add-architecture armhf; apt-get update -q; apt-get install -y \\
          --no-install-recommends crossbuild-essential-armhf libsdl2-dev:armhf make zip \\
          unzip pkg-config ca-certificates; ./build.sh sprint'

     Persistent (distrobox): distrobox create --name arm-sdl2 --image debian:bullseye ;
       distrobox enter arm-sdl2 ; the apt commands from A ; ./build.sh sprint"
    have "$acxx" || die "'$acxx' not found"
    have make    || die "'make' not found"
    { have zip && have unzip; } || die "'zip' and 'unzip' are required to build update.zip"
    [ -f "$SRC/Makefile.sprint" ] || die "src/Makefile.sprint is missing"

    # SDL2 armhf: Makefile.sprint locates it via pkg-config with the armhf PKG_CONFIG_PATH.
    local pcp="${SPRINT_PKG_CONFIG_PATH:-/usr/lib/arm-linux-gnueabihf/pkgconfig}"
    if have pkg-config && ! PKG_CONFIG_PATH="$pcp" pkg-config --exists sdl2 2>/dev/null; then
        warn "SDL2 armhf not detected in $pcp: if the build fails, install libsdl2-dev:armhf"
    fi

    # update.zip template: holds wbexec.bin/wbgrom.bin/frontend/roms/ and the
    # 'jzintv' file (ARM binary) which we replace with the freshly built one.
    local tpl="${SPRINT_ZIP_TEMPLATE:-}"
    if [ -z "$tpl" ]; then
        for cand in "$SUP/sprint/update.zip" "$SCRIPT_DIR/../sprint/update.zip"; do
            [ -f "$cand" ] && { tpl="$cand"; break; }
        done
    fi
    [ -f "$tpl" ] || die "update.zip template not found.  Set SPRINT_ZIP_TEMPLATE=/path/to/update.zip"
    ok "update.zip template: $tpl"

    clean_objs
    mkdir -p "$BIN"                       # the Makefile links into ../bin/: it must exist
    ( cd "$SRC" && rm -f ../bin/jzintv && \
      make -f Makefile.sprint SVN_REV=0 SVN_DTY=0 -j"$JOBS" ../bin/jzintv )
    [ -f "$BIN/jzintv" ] || die "ARM build failed (no bin/jzintv)"
    [ "$STRIP" = "1" ] && "$astrip" --strip-unneeded "$BIN/jzintv"

    # Package: replace the internal 'jzintv' inside the zip with the ARM binary.
    mkdir -p "$BIN/sprint"
    rm -f "$BIN/sprint/update.zip"
    local wd; wd="$(mktemp -d)"
    ( cd "$wd" && unzip -q "$tpl" && cp -f "$BIN/jzintv" jzintv && chmod +x jzintv \
        && zip -qr "$BIN/sprint/update.zip" . )
    rm -rf "$wd"
    mv -f "$BIN/jzintv" "$BIN/sprint/jzintv"
    clean_objs
    ok "-> bin/sprint/jzintv  ($(du -h "$BIN/sprint/jzintv" | cut -f1))"
    ok "-> bin/sprint/update.zip     ($(du -h "$BIN/sprint/update.zip" | cut -f1))  [rename to update.zip on the USB stick]"
}

# ===========================================================================
#  LINUX ARMHF  (plain armv7 cross-build -- NOT the Sprint flavour)
# ===========================================================================
build_linux_armhf() {
    log "Build Linux armv7 (${ARM_CROSS}, release + LTO)"
    local acc="${ARM_CROSS}gcc" acxx="${ARM_CROSS}g++" astrip="${ARM_CROSS}strip"
    have "$acc"  || die "ARM cross-compiler '$acc' not found.  Install an armhf
toolchain and SDL2:  dpkg --add-architecture armhf && apt update &&
apt install -y crossbuild-essential-armhf libsdl2-dev:armhf"
    have "$acxx" || die "'$acxx' not found"
    have make    || die "'make' not found"

    # Makefile.linux_sdl2 finds SDL2 with sdl2-config, which would answer for
    # the HOST.  Resolve the armhf SDL2 through pkg-config instead and hand the
    # result to make, overriding the Makefile's own := assignments.
    local pcp="${ARM_PKG_CONFIG_PATH:-/usr/lib/arm-linux-gnueabihf/pkgconfig}"
    have pkg-config || die "'pkg-config' is required to locate SDL2 for armhf"
    PKG_CONFIG_PATH="$pcp" pkg-config --exists sdl2 2>/dev/null \
        || die "SDL2 for armhf not found in $pcp.  Install libsdl2-dev:armhf, or
point \$ARM_PKG_CONFIG_PATH at the right pkgconfig directory."
    local acflags alflags
    acflags="$(PKG_CONFIG_PATH="$pcp" pkg-config --cflags sdl2) -DUSE_SDL=2"
    alflags="$(PKG_CONFIG_PATH="$pcp" pkg-config --libs sdl2)"
    ok "SDL2 armhf: $(PKG_CONFIG_PATH="$pcp" pkg-config --modversion sdl2)"

    clean_objs
    mkdir -p "$BIN"                       # the Makefile links into ../bin/: it must exist
    # GNU_READLINE is off: readline:armhf is rarely installed alongside the
    # cross toolchain, and the emulator does not need it.
    ( cd "$SRC" && rm -f ../bin/jzintv && \
      make -f Makefile.linux_sdl2 \
           CC="$acc -std=gnu99" \
           CXX="$acxx -std=c++14" \
           SDL2_CFLAGS="$acflags" \
           SDL2_LFLAGS="$alflags" \
           GNU_READLINE=0 SVN_REV=0 SVN_DTY=0 \
           -j"$JOBS" ../bin/jzintv )
    [ -f "$BIN/jzintv" ] || die "armv7 build failed (no bin/jzintv)"
    mkdir -p "$BIN/linux-armhf"
    mv -f "$BIN/jzintv" "$BIN/linux-armhf/jzintv"
    [ "$STRIP" = "1" ] && "$astrip" --strip-unneeded "$BIN/linux-armhf/jzintv"
    clean_objs
    ok "-> bin/linux-armhf/jzintv  ($(du -h "$BIN/linux-armhf/jzintv" | cut -f1))"
}

# ===========================================================================
#  MAIN
# ===========================================================================
rc=0
case "$TARGET" in
    linux)   build_linux ;;
    windows) build_windows ;;
    sprint)  build_sprint ;;
    linux-armhf|armhf|armv7) build_linux_armhf ;;
    all)
        # In 'all', a target whose toolchain is missing is skipped, not fatal.
        build_linux || rc=1
        if have "${MINGW}-gcc"; then build_windows || rc=1
        else warn "MinGW-w64 (${MINGW}-gcc) not found: skipping Windows"; fi
        if have "${ARM_CROSS}gcc"; then build_linux_armhf || rc=1; build_sprint || rc=1
        else warn "ARM toolchain (${ARM_CROSS}gcc) not found: skipping armv7 and Sprint"; fi
        ;;
    -h|--help|help) sed -n '2,45p' "$0"; exit 0 ;;
    *) die "unknown target: '$TARGET' (use: linux | windows | linux-armhf | sprint | all)";;
esac

log "Contents of bin/:"
find "$BIN" -mindepth 2 -maxdepth 2 -type f 2>/dev/null | sed "s#^$SCRIPT_DIR/#  #" | sort
exit $rc
