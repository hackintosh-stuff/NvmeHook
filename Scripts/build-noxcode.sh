#!/bin/sh
# build-noxcode.sh — build NvmeHook.kext with only the Command Line Tools
#
# Usage (run on macOS; neither Xcode.app nor sudo is required):
#   sh Scripts/build-noxcode.sh [options]
#   MACKERNELSDK=/path/to/MacKernelSDK LILU=/path/to/Lilu sh Scripts/build-noxcode.sh
#
# Why xcodebuild is not used:
#   The development machine has only the Command Line Tools (/usr/bin/xcodebuild
#   is a stub). A kext needs clang, the MacKernelSDK kernel headers, libkmod and
#   ld -kext, all of which the CLT provides; this script drives them directly.
#
# Dependencies (not vendored in this repository):
#   MacKernelSDK   git submodule, initialise with:
#                  git submodule update --init MacKernelSDK
#   Lilu           source tree, not the released kext: the plugin needs both
#                  Headers/*.hpp (to compile) and Library/plugin_start.cpp (the
#                  glue that provides the kmod entry points and the IOService
#                  shell). A released Lilu.kext ships only the headers.
#                  Place a tree at $HOME/src/Lilu or pass --lilu.
#
# Output: build/noxcode.XXXXXX/NvmeHook.kext, one directory per run, older
#         outputs kept; the newest path is written to build/latest-noxcode-build.txt.
# Note: this script only builds. It does not deploy, does not touch
#       /Library/Extensions and does not rebuild any kernel collection.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

# Single place for the version: it feeds the Info.plist and MODULE_VERSION.
NAME=NvmeHook
BUNDLE_ID=com.ling.NvmeHook
VERSION=1.4.0

SDK_ARG=${MACKERNELSDK:-}
LILU_ARG=${LILU:-}
OUT_ARG=
OPT=-O3
DEBUG_DEFS=

usage() {
    cat <<'EOF'
Usage: sh Scripts/build-noxcode.sh [options]

Options:
  --sdk <dir>    MacKernelSDK root (default: <repo>/MacKernelSDK)
  --lilu <dir>   Lilu source tree, the directory containing Headers/ and
                 Library/plugin_start.cpp (default: $LILU, then <repo>/Lilu,
                 ../sdk/Lilu, $HOME/Lilu, $HOME/src/Lilu)
  --debug        -O0 and DEBUG=1
  --out <dir>    output directory (default: build/noxcode.XXXXXX)
  -h, --help     show this help

Environment: MACKERNELSDK and LILU are used when the matching option is absent.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --sdk)     SDK_ARG="${2:?--sdk requires a value}"; shift 2 ;;
        --lilu)    LILU_ARG="${2:?--lilu requires a value}"; shift 2 ;;
        --out)     OUT_ARG="${2:?--out requires a value}"; shift 2 ;;
        --debug)   OPT=-O0; DEBUG_DEFS="-DDEBUG=1"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1 (see --help)" >&2; exit 1 ;;
    esac
done

[ "$(uname -s)" = "Darwin" ] || { echo "This script can only run on macOS" >&2; exit 1; }
die() { echo "[error] $*" >&2; exit 1; }

# ------------------------------------------------------------------- deps --
find_sdk() {
    for cand in "$SDK_ARG" "$ROOT/MacKernelSDK" "$HOME/MacKernelSDK" "$HOME/src/MacKernelSDK"; do
        [ -n "$cand" ] || continue
        if [ -d "$cand/Headers/IOKit" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

# Needs a source tree: Headers for the includes, Library/plugin_start.cpp for the
# kmod and IOService glue that a plugin cannot do without.
find_lilu() {
    for cand in "$LILU_ARG" "$ROOT/Lilu" "$ROOT/../sdk/Lilu" "$HOME/Lilu" "$HOME/Lilu/Lilu" \
                "$HOME/src/Lilu" "$HOME/src/Lilu/Lilu"; do
        [ -n "$cand" ] || continue
        if [ -f "$cand/Headers/kern_util.hpp" ] && [ -f "$cand/Library/plugin_start.cpp" ]; then
            printf '%s\n' "$cand"
            return 0
        fi
    done
    return 0
}

SDK=$(find_sdk)
[ -n "$SDK" ] || die "MacKernelSDK not found. Initialise the submodule (git submodule update --init MacKernelSDK) or pass --sdk <dir>"
SDK=$(CDPATH= cd -- "$SDK" && pwd)
[ -f "$SDK/Library/universal/libkmodc++.a" ] || die "incomplete MacKernelSDK, missing Library/universal/libkmodc++.a: $SDK"
[ -f "$SDK/Library/x86_64/libkmod.a" ] || die "incomplete MacKernelSDK, missing Library/x86_64/libkmod.a: $SDK"

LILU=$(find_lilu)
if [ -z "$LILU" ]; then
    die "Lilu source tree not found. This plugin needs Headers/*.hpp and
       Library/plugin_start.cpp; a released Lilu.kext has only the headers.
       Clone https://github.com/acidanthera/Lilu to \$HOME/src/Lilu (tag matching
       the installed Lilu) or pass --lilu <dir>."
fi
LILU=$(CDPATH= cd -- "$LILU" && pwd)

echo "== Environment =="
echo "repo   : $ROOT"
xcrun clang --version | head -1
echo "sdk    : $SDK"
echo "lilu   : $LILU"
echo "version: $NAME $VERSION"

# ---------------------------------------------------------------- work dir --
mkdir -p "$ROOT/build"
if [ -n "$OUT_ARG" ]; then
    W="$OUT_ARG"
    mkdir -p "$W"
else
    W=$(mktemp -d "$ROOT/build/noxcode.XXXXXX")
fi
W=$(CDPATH= cd -- "$W" && pwd)
KEXT="$W/$NAME.kext"
OBJ="$W/obj"
BIN="$KEXT/Contents/MacOS/$NAME"
echo "work   : $W"

# ------------------------------------------------------------------ flags --
CFLAGS_COMMON="-arch x86_64 -mkernel -fapple-kext -mno-red-zone
 -nostdinc++ -fno-builtin -fno-common
 -mmmx -msse -msse2 -msse3 -mssse3 -mfpmath=sse -ftree-vectorize
 -fstack-protector -fno-threadsafe-statics -fno-asynchronous-unwind-tables
 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -D__APPLE_KEXT__
 $OPT $DEBUG_DEFS -Wno-unused-parameter -Wno-unused-function -Wno-deprecated-declarations"
CFLAGS_CXX="-std=gnu++17 -fno-exceptions -fno-rtti"
DEFINES="-DPRODUCT_NAME=$NAME -DMODULE_VERSION=$VERSION"
INCLUDES="-I $SDK/Headers -I $SDK -I $LILU"

[ -d "$ROOT/$NAME" ] || die "source directory not found: $ROOT/$NAME"
[ -f "$ROOT/$NAME/$NAME.cpp" ] || die "missing source file: $ROOT/$NAME/$NAME.cpp"
[ -f "$ROOT/$NAME/Info.plist" ] || die "missing file: $ROOT/$NAME/Info.plist"

mkdir -p "$OBJ" "$KEXT/Contents/MacOS"

# ------------------------------------------------------------------ compile --
echo
echo "== Compilation =="
# Explicit list, no globs: an unrelated file dropped into the tree must never end
# up inside the kext.
echo "  CXX $NAME.cpp"
# shellcheck disable=SC2086
xcrun clang++ $CFLAGS_COMMON $CFLAGS_CXX $DEFINES $INCLUDES -I "$ROOT/$NAME" \
    -c "$ROOT/$NAME/$NAME.cpp" -o "$OBJ/$NAME.o"

echo "  CXX plugin_start.cpp"
# shellcheck disable=SC2086
xcrun clang++ $CFLAGS_COMMON $CFLAGS_CXX $DEFINES $INCLUDES \
    -c "$LILU/Library/plugin_start.cpp" -o "$OBJ/plugin_start.o"

# ------------------------------------------------------------------- kmod --
# xcodebuild normally runs CreateKModInfo.perl to emit the translation unit that
# carries kmod_info; without it the kernel rejects the kext. The module entry
# points are the ones plugin_start.cpp exports: <PRODUCT_NAME>_kern_start/_stop.
KMOD="$OBJ/kmod-info.cpp"
cat > "$KMOD" <<KMOD_EOF
#include <mach/kmod.h>
#include <libkern/OSKextLib.h>

extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t ${NAME}_kern_start(kmod_info_t *, void *);
extern kern_return_t ${NAME}_kern_stop(kmod_info_t *, void *);

KMOD_EXPLICIT_DECL($BUNDLE_ID, "$VERSION", _start, _stop);

__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = (kmod_start_func_t *)&${NAME}_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t  *_antimain = (kmod_stop_func_t *)&${NAME}_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
KMOD_EOF

echo "  CXX kmod-info.cpp"
# shellcheck disable=SC2086
xcrun clang++ $CFLAGS_COMMON $CFLAGS_CXX $INCLUDES -c "$KMOD" -o "$OBJ/kmod-info.o"

# -------------------------------------------------------------------- link --
echo
echo "  LD  $NAME"
# -kext leaves kernel and Lilu symbols undefined on purpose; they resolve at load
# against the kernel and the already loaded Lilu.
xcrun ld -kext -arch x86_64 "$OBJ"/*.o \
    "$SDK/Library/universal/libkmodc++.a" \
    "$SDK/Library/x86_64/libkmod.a" \
    -o "$BIN"

# ------------------------------------------------------------ verification --
have_defined_symbol() {
    nm "$1" 2>/dev/null | awk -v s="$2" '$NF == s && $1 != "U" { f = 1 } END { exit !f }'
}

have_defined_symbol "$BIN" _kmod_info || die "$NAME: _kmod_info is missing, the kext cannot be loaded"
have_defined_symbol "$BIN" __start || die "$NAME: _start is missing (libkmod did not provide it)"
have_defined_symbol "$BIN" __stop || die "$NAME: _stop is missing (libkmod did not provide it)"

FILETYPE=$(otool -h "$BIN" | awk 'NR>1{print $5}' | tr -d ' ' | tail -1)
[ "$FILETYPE" = "11" ] || die "$NAME: filetype=$FILETYPE, expected 11 (MH_KEXT_BUNDLE)"

# Symbols of this kext's own sources that stayed undefined mean a source file was
# left out of the list above. Lilu and kernel symbols are expected and stay
# unresolved on purpose.
if nm -u "$BIN" | c++filt | grep -E '(^| )NvmeHook::' > "$W/unresolved.txt"; then
    echo "[error] $NAME: unresolved symbols from its own sources" >&2
    cat "$W/unresolved.txt" >&2
    exit 1
fi

# is the plugin entry actually wired?
have_defined_symbol "$BIN" "_${NAME}_kern_start" || die "$NAME: ${NAME}_kern_start is not exported"

# -------------------------------------------------------------- Info.plist --
expand_plist() { # $1=source plist $2=destination
    sed -e "s/\$(DEVELOPMENT_LANGUAGE)/en/g" \
        -e "s/\$(EXECUTABLE_NAME)/$NAME/g" \
        -e "s/\$(TARGET_NAME)/$NAME/g" \
        -e "s/\$(PRODUCT_NAME)/$NAME/g" \
        -e "s/\$(PRODUCT_BUNDLE_IDENTIFIER)/$BUNDLE_ID/g" \
        -e "s/\$(PRODUCT_BUNDLE_PACKAGE_TYPE)/KEXT/g" \
        -e "s/\$(MARKETING_VERSION)/$VERSION/g" \
        -e "s/\$(CURRENT_PROJECT_VERSION)/$VERSION/g" \
        "$1" > "$2"
    if grep -E '\$\([A-Za-z_]+\)' "$2" >/dev/null; then
        echo "[error] unexpanded Xcode variable in $2:" >&2
        grep -E '\$\([A-Za-z_]+\)' "$2" >&2
        exit 1
    fi
    plutil -lint "$2" >/dev/null || die "Info.plist is not a valid property list: $2"
}

expand_plist "$ROOT/$NAME/Info.plist" "$KEXT/Contents/Info.plist"

# ------------------------------------------------------------------- sign --
codesign --force --sign - --timestamp=none "$KEXT" >/dev/null \
    || die "$NAME: ad-hoc signing failed"
codesign --verify --strict "$KEXT" || die "$NAME: signature verification failed"

# ---------------------------------------------------------------- summary --
echo
echo "== Summary =="
echo "  $KEXT"
echo "    CFBundleIdentifier         : $(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$KEXT/Contents/Info.plist")"
echo "    CFBundleVersion            : $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$KEXT/Contents/Info.plist")"
echo "    CFBundleShortVersionString : $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$KEXT/Contents/Info.plist")"
echo "    architectures              : $(lipo -archs "$BIN")"

printf '%s\n' "$KEXT" > "$ROOT/build/latest-noxcode-build.txt"
echo
echo "BUILD OK: $KEXT ($NAME $VERSION)"
echo "Nothing was deployed: /Library/Extensions and the kernel collections are untouched."
