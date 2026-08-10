#!/bin/bash
# ports/netsurf/build.sh -- cross-build NetSurf's libraries for EmbLinkOS.
#
# The source tree lives OUTSIDE this repo (like every other port here: see
# docs/PORTS.md). What lives inside is this script, the two host-tool
# replacements it puts on PATH, our patches, and the code we write ourselves --
# the fetcher, the frontend, and the compat shims.
#
#   NSSRC=... ports/netsurf/build.sh [component ...]
#
# With no arguments it builds the library stack in dependency order. The
# install prefix is inside the source tree, so nothing here touches /opt or
# needs a package manager.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NSSRC="${NSSRC:-/home/motsou/cross/netsurf/netsurf-all-3.11}"
HOST="${HOST:-x86_64-elf}"
EMBLINK_ROOT="${EMBLINK_ROOT:-$HOME/myos}"
# A HOST build reuses every line of this script with a different triple and its
# own prefix, so the SAME frontend can be run under a sanitizer on the build
# machine -- which is the only sane instrument for a wild pointer. See
# frontend/Makefile.tools for what changes when the triple is not ours.
if [ "$HOST" = "x86_64-elf" ]; then
    PREFIX="${PREFIX:-$NSSRC/inst-emblink}"
else
    PREFIX="${PREFIX:-$NSSRC/inst-$HOST}"
fi

[ -d "$NSSRC" ] || { echo "no NetSurf source at $NSSRC (set NSSRC=)" >&2; exit 1; }

# OUR host tools come first: this build machine has neither pkg-config nor
# gperf, and the port answers both itself rather than acquiring dependencies.
export PATH="$HERE/tools:$PATH"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export NSSHARED="$PREFIX/share/netsurf-buildsystem"

# The libraries, in the order each one's headers are needed by the next.
LIBS=(libwapcaplet libparserutils libhubbub libcss libdom libnsgif libnsbmp
      libnsutils libutf8proc libnslog)

# WHAT THIS PORT TURNS OFF, and why. Each of these is a dependency we would
# otherwise have to port for something we do not need or already have.
component_flags() {
    case "$1" in
    libdom)
        # The XML bindings want expat or libxml2. A browser parses HTML with
        # hubbub; the XML path is for XHTML served as XML, which no page we
        # have ever loaded is. Turning it off removes expat from the port.
        echo "WITH_EXPAT_BINDING=no WITH_LIBXML_BINDING=no WITH_HUBBUB_BINDING=yes"
        ;;
    esac
}

# The build machine's gcc is newer than the one NetSurf 3.11 was released
# against, and every component compiles with -Werror. A HOST build is a
# diagnostic tool, not a shipping artifact, so its warnings are not worth
# turning into a porting task -- the cross build keeps -Werror exactly as
# upstream set it.
# Which libc's headers the port's own small pieces (zlib, our iconv) compile
# against. Freestanding newlib for the OS; the build machine's own for a host
# diagnostic build. Getting this wrong links a newlib __errno into a glibc
# binary, which fails at the very last step of a long build.
libc_inc() {
    [ "$HOST" = "x86_64-elf" ] && \
        echo "-isystem ${EMBLINK_NEWLIB:-$HOME/cross/newlib-c99}/x86_64-elf/include"
}

host_relax() {
    [ "$HOST" = "x86_64-elf" ] || echo 'WARNFLAGS=-Wall -w'
}

# The libraries are built by their own Makefiles, which means they miss every
# flag the netsurf build gets from frontend/Makefile.tools -- including the
# auto-init that the rest of this OS's userland has. An uninitialised local in
# libdom or libcss would be invisible to a flag applied only to the core.
# Injected through CC rather than CFLAGS: setting CFLAGS on the command line
# makes it immutable for the whole build, so every `CFLAGS +=` in NetSurf's
# own makefiles is discarded -- including the include paths, which fails at
# the first sibling header.
lib_cflags() {
    [ "$HOST" = "x86_64-elf" ] && echo "-ftrivial-auto-var-init=zero"
}

build_one() {
    local lib="$1"
    echo "=== $lib"
    # shellcheck disable=SC2046  # the flags are deliberately word-split
    make -C "$NSSRC/$lib" install \
        PREFIX="$PREFIX" NSSHARED="$NSSHARED" HOST="$HOST" \
        COMPONENT_TYPE=lib-static \
        $(component_flags "$lib") $(host_relax) \
        CC="$HOST-gcc $(lib_cflags)"
}

# THE BROWSER ITSELF. Everything turned off here is a dependency we do not
# need or already have:
#   CURL      -- our own fetcher goes in over the OS's TLS stack, so libcurl
#                and openssl leave the port entirely
#   DUKTAPE   -- JavaScript, which also drags in the nsgenbind host tool
#   PNG/JPEG/ -- the image handlers want libpng and libjpeg; the OS has its own
#   WEBP/JXL     decoders and they will be wired to NetSurf's own handlers
#   PSL/NSLOG -- optional, and neither is load-bearing for a first render
#
# PKGCONFIG is named explicitly because the buildsystem only sets it for cross
# triples it recognises, and `x86_64-elf` is not one of them -- it is OUR shim
# on PATH, reading the PKG_CONFIG_PATH exported above.
#
# VLDTARGET is the list of frontends NetSurf will accept, and it is a plain
# assignment in frontends/Makefile.hts -- so naming ours on the COMMAND LINE
# overrides it, which is how a new frontend joins the build without the port
# acquiring its first patch.
build_browser() {
    echo "=== netsurf (TARGET=emblink)"
    make -C "$NSSRC/netsurf" \
        PREFIX="$PREFIX" NSSHARED="$NSSHARED" HOST="$HOST" TARGET=emblink \
        NETSURF_USE_CURL=NO NETSURF_USE_OPENSSL=NO NETSURF_USE_DUKTAPE=NO \
        NETSURF_USE_PNG=NO NETSURF_USE_JPEG=NO NETSURF_USE_JPEGXL=NO \
        NETSURF_USE_WEBP=NO NETSURF_USE_BMP=NO NETSURF_USE_GIF=NO \
        NETSURF_USE_NSPSL=NO NETSURF_USE_NSLOG=NO NETSURF_USE_UTF8PROC=YES \
        NETSURF_USE_NSSVG=NO NETSURF_USE_ROSPRITE=NO NETSURF_USE_VIDEO=NO \
        NETSURF_USE_LIBICONV_PLUG=NO NETSURF_USE_HARU_PDF=NO \
        NETSURF_FB_FONTLIB=internal \
        VLDTARGET="amiga atari beos framebuffer gtk monkey riscos windows emblink" \
        PKGCONFIG=pkg-config PKG_CONFIG=pkg-config \
        $(host_relax) \
        "$@"
    # ONE OUTPUT NAME, TWO BUILDS. NetSurf links its executable to a fixed
    # `nsemblink` at the top of its tree, so a cross build and a host build
    # overwrite each other -- and make then reports the survivor as up to date,
    # because it is NEWER than the other build's objects. That is how a
    # cross-built binary came to be run on the build machine, which fails in a
    # way that looks like the program crashing rather than the wrong file.
    cp -f "$NSSRC/netsurf/nsemblink" "$NSSRC/netsurf/nsemblink-$HOST"
    echo "    -> $NSSRC/netsurf/nsemblink-$HOST"
}

if [ ! -d "$NSSHARED" ]; then
    make -C "$NSSRC/buildsystem" install PREFIX="$PREFIX" >/dev/null
fi

# GPERF, without patching anything. The rule that runs it only fires when its
# output is missing or older than the .gperf, so generating the file here --
# with our own generator, verified against every keyword in the input -- means
# make never reaches for a tool this machine does not have. Zero patches is
# worth a few lines of script; see docs/PORTS.md on why that matters.
gen_gperf() {
    local in="$1" out="$2"
    [ -f "$in" ] || return 0
    if [ ! -f "$out" ] || [ "$in" -nt "$out" ] || [ "$HERE/tools/nsgperf.py" -nt "$out" ]; then
        echo "=== nsgperf $(basename "$in")"
        python3 "$HERE/tools/nsgperf.py" "$in" "$out"
    fi
}
gen_gperf "$NSSRC/libhubbub/src/treebuilder/element-type.gperf" \
          "$NSSRC/libhubbub/src/treebuilder/autogenerated-element-type.c"

# THE PATCHES, applied idempotently -- `patch --forward` treats an
# already-applied hunk as success rather than as a reason to stop, so this is
# safe to run on every build. There is exactly one; see patches/ for why.
for pf in "$HERE"/patches/*.patch; do
    [ -e "$pf" ] || continue
    if ! patch -p1 -d "$NSSRC" --forward --silent --dry-run <"$pf" >/dev/null 2>&1; then
        continue                     # already applied (or does not apply cleanly)
    fi
    echo "=== patch $(basename "$pf")"
    patch -p1 -d "$NSSRC" --forward --silent <"$pf"
done

# ZLIB, built by the port into the port's own prefix. There is a cross-built
# libz.a on this machine already from an earlier port, but it was built WITHOUT
# the gzip file API (gzopen/gzgets/gzclose), which NetSurf's about: fetcher
# uses -- and quietly linking against a zlib that is missing a quarter of
# itself is how you get an undefined symbol at the last step of a long build.
# Building it here means the port owns its copy and nothing else on the machine
# is disturbed.
ZSRC="${ZSRC:-$HOME/cross/zlib-1.3.1}"
build_zlib() {
    local a="$PREFIX/lib/libz.a"
    [ -d "$ZSRC" ] || { echo "no zlib source at $ZSRC (set ZSRC=)" >&2; return 1; }
    [ -f "$a" ] && [ "$a" -nt "$ZSRC/zlib.h" ] && return 0
    echo "=== zlib"
    mkdir -p "$PREFIX/lib" "$PREFIX/include" "$PREFIX/build-zlib"
    local objs=()
    for c in "$ZSRC"/*.c; do
        local o="$PREFIX/build-zlib/$(basename "${c%.c}").o"
        "$HOST-gcc" -O2 -fno-stack-protector -DHAVE_UNISTD_H $(libc_inc) \
            -I"$ZSRC" -c "$c" -o "$o"
        objs+=("$o")
    done
    "$HOST-ar" rcs "$a" "${objs[@]}"
    cp -f "$ZSRC/zlib.h" "$ZSRC/zconf.h" "$PREFIX/include/"
}
build_zlib

# THE OS'S FONT MODULE, as an archive. ui/backend/font.c is a TrueType parser,
# rasteriser and glyph cache that needs nothing but malloc and memcpy -- so the
# browser can render in the system typeface without a second copy of a
# TrueType parser existing. Built here rather than named in the frontend's
# SOURCES because NetSurf's buildsystem resolves source paths relative to its
# own tree, and a path that climbs out of it resolves to nothing and is
# skipped in SILENCE -- the build succeeds and the symbols are simply absent.
build_emfont() {
    local o="$PREFIX/lib/emfont.o" a="$PREFIX/lib/libemfont.a"
    local src="$EMBLINK_ROOT/ui/backend/font.c"
    [ -f "$src" ] || { echo "no font.c at $src" >&2; return 1; }
    if [ ! -f "$a" ] || [ "$src" -nt "$a" ]; then
        echo "=== libemfont (the OS's own font.c)"
        mkdir -p "$PREFIX/lib"
        "$HOST-gcc" -O2 -g -fno-stack-protector -DFONT_NO_BACKEND $(libc_inc) \
            -I"$EMBLINK_ROOT/ui/backend" -I"$EMBLINK_ROOT/ui/scene" \
            -I"$EMBLINK_ROOT/ui/layout" -I"$EMBLINK_ROOT/ui/declare" \
            -c "$src" -o "$o"
        "$HOST-ar" rcs "$a" "$o"
    fi
}
build_emfont

# THE NETWORK FETCHER, and the OS's HTTP/TLS client under it. Cross build only
# -- the host has no kernel TCP stack of ours -- and an archive rather than a
# named source for the same reason libemfont is: a path that climbs out of
# NetSurf's tree is skipped in silence.
build_emnet() {
    [ "$HOST" = "x86_64-elf" ] || return 0
    local a="$PREFIX/lib/libemnet.a"
    local srcs=("$HERE/fetch/emblink_fetch.c" "$HERE/fetch/resolve.c" \
                "$EMBLINK_ROOT/user/web/net.c" "$EMBLINK_ROOT/user/web/url.c" \
                "$EMBLINK_ROOT/user/web/charset.c")
    local newest=0 o objs=()
    for c in "${srcs[@]}"; do [ "$c" -nt "$a" ] && newest=1; done
    [ -f "$a" ] && [ $newest -eq 0 ] && return 0
    echo "=== libemnet (fetcher + the OS's HTTP/TLS client)"
    mkdir -p "$PREFIX/lib"
    for c in "${srcs[@]}"; do
        o="$PREFIX/lib/$(basename "${c%.c}").o"
        "$HOST-gcc" -O2 -g -fno-stack-protector -DEMBLINK_NET $(libc_inc) \
            -I"$EMBLINK_ROOT/user/lib" -I"$EMBLINK_ROOT/user/web" \
            -I"$EMBLINK_ROOT/user/lib/tls" -I"$EMBLINK_ROOT/user/lib/tls/crypto" \
            -I"$EMBLINK_ROOT/user/lib/tls/x509" -I"$EMBLINK_ROOT/user/lib/tls/kshim" \
            -I"$EMBLINK_ROOT/kernel" \
            -I"$NSSRC/netsurf" -I"$NSSRC/netsurf/include" -I"$PREFIX/include" \
            -c "$c" -o "$o"
        objs+=("$o")
    done
    "$HOST-ar" rcs "$a" "${objs[@]}"
}
build_emnet

# OUR ICONV, as an archive on the link line. NetSurf links -liconv when it is
# told the C library does not have iconv inside it (which ours does not: see
# compat/iconv.c for what this is and, more importantly, what it refuses).
# Built here rather than in the OS's Makefile because it is the netsurf link
# that needs the -l form; the OS builds the same source as a plain object.
build_iconv() {
    local o="$PREFIX/lib/ns_iconv.o" a="$PREFIX/lib/libiconv.a"
    if [ ! -f "$a" ] || [ "$HERE/compat/iconv.c" -nt "$a" ]; then
        echo "=== libiconv (ours)"
        mkdir -p "$PREFIX/lib"
        "$HOST-gcc" -O2 -fno-stack-protector $(libc_inc) \
            -c "$HERE/compat/iconv.c" -o "$o"
        "$HOST-ar" rcs "$a" "$o"
    fi
}
build_iconv

# THE STALENESS TRAP, which cost this port two sessions.
#
# patches/0002 edits utils/config.h -- the header that decides which POSIX
# functions NetSurf believes it has. NetSurf's buildsystem does not treat that
# header as a dependency of every object, so applying or changing the patch
# leaves already-compiled objects believing the OLD answers. Half the program
# then thinks the platform has mmap and the other half knows it does not, and
# they disagree about code paths and struct contents.
#
# What that looks like from the outside is not a build error. It is a
# DETERMINISTIC bad pointer at run time -- the same value every boot, surviving
# every recompile of the files you happen to touch, in a program that works
# perfectly when built for the host. It cost a kernel bug hunt, an ASan build,
# and eleven eliminations before a full rebuild made it vanish.
#
# So: stamp what the patches were, and wipe the build directory when they
# change. docs/PORTS.md has the same lesson under its own heading; this is that
# lesson with teeth.
PATCH_STAMP="$NSSRC/.emblink-patch-stamp"
patch_state() { cat "$HERE"/patches/*.patch 2>/dev/null | cksum; }
if [ "$(patch_state)" != "$(cat "$PATCH_STAMP" 2>/dev/null)" ]; then
    echo "=== patches changed -- discarding stale objects"
    rm -rf "$NSSRC/netsurf/build/$HOST-emblink"
    patch_state > "$PATCH_STAMP"
fi

# OUR FRONTEND lives in this repo and is SYMLINKED into NetSurf's tree, not
# copied into it. Copying would fork the file the moment either side changed;
# a link keeps one copy, under our history, and leaves NetSurf's tree pristine
# so the port can keep saying it has no patches.
if [ ! -e "$NSSRC/netsurf/frontends/emblink" ]; then
    ln -s "$HERE/frontend" "$NSSRC/netsurf/frontends/emblink"
fi

declare -a EXTRA=()
targets=("$@")
[ ${#targets[@]} -eq 0 ] && targets=("${LIBS[@]}")
for l in "${targets[@]}"; do
    if [ "$l" = netsurf ]; then build_browser; else build_one "$l"; fi
done

echo
echo "installed into $PREFIX/lib:"
ls -1 "$PREFIX"/lib/*.a 2>/dev/null | sed 's/^/  /'
