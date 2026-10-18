#!/bin/bash
# ==============================================================================
# /**
#  * dpdu-passthru - Linux MinGW-w64 Cross-Compilation Script
#  * Builds 32-bit Win32 D-PDU API DLL for Tech2Win (emulator.exe)
#  * with native dual-bus GMLAN CAN support (SW-CAN + HS-CAN) for Saab 9-3 NG (9440).
#  *
#  * Built with AI Pair-Programming (Antigravity / Google DeepMind AI).
#  * Inspired by the Saab community - saabiste.net
#  *
#  * Source: https://github.com/oamne/dpdu-passthru
#  */
# ==============================================================================

set -e

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
DRIVER_DIR="$DIR/driver"
INSTALLER_DIR="$DIR/installer"
OUTPUT_DLL="$INSTALLER_DIR/driver.dll"

# Ensure output directory exists
mkdir -p "$INSTALLER_DIR"

# Remove legacy duplicate at root if present
rm -f "$DIR/driver.dll"

echo "=== 1. Preparing build environment ==="
cd "$DRIVER_DIR"

# Detect MinGW-w64 cross-compiler (prefer posix variant for C++20 std::jthread)
if command -v i686-w64-mingw32-g++-posix >/dev/null 2>&1; then
    CXX="i686-w64-mingw32-g++-posix"
elif command -v i686-w64-mingw32-g++ >/dev/null 2>&1; then
    CXX="i686-w64-mingw32-g++"
else
    echo "Error: MinGW-w64 C++ cross-compiler not found."
    echo "Please install it: sudo apt-get install g++-mingw-w64-i686"
    exit 1
fi
echo "  -> Using compiler: $CXX"

# Generate version.h dynamically (matches MSVC PreBuildEvent)
GIT_VER=$(git describe --tags --always --dirty 2>/dev/null || echo "v0.7.0")
echo "#define BUILD_VERSION \"$GIT_VER\"" > version.h
echo "  -> version.h updated: $GIT_VER"

echo "=== 2. Compiling Win32 D-PDU DLL ==="

# Project source files (parité 1:1 avec driver.vcxproj)
SRCS=(
    dpdu.cpp
    dllmain.cpp
    ComLogicalLink.cpp
    ComPrimitive.cpp
    ISO14230ComPrimitive.cpp
    KW82ComPrimitive.cpp
    Logger.cpp
    Settings.cpp
    j2534/shim_loader.cpp
    gmlan_ext/GmlanCanAdapter.cpp
    gmlan_ext/PassThruChannelManager.cpp
    gmlan_ext/Tech2WinMemory.cpp
)

# Compile directly into installer/driver.dll (single binary target, zero intermediate copies)
$CXX -std=c++20 -shared -o "$OUTPUT_DLL" \
    "${SRCS[@]}" \
    dpdu.def \
    -I. \
    -Ij2534 \
    -Igmlan_ext \
    -Wl,--subsystem,windows \
    -Wl,--kill-at \
    -Wl,--enable-stdcall-fixup \
    -O2 \
    -Wall \
    -s \
    -static-libgcc \
    -static-libstdc++ -static

echo "=== 3. Verifying output binary ==="
file "$OUTPUT_DLL"
ls -lh "$OUTPUT_DLL"

echo "=== 4. Packaging deliverable archive ==="
cd "$DIR"
ZIP_NAME="dpdu-passthru-${GIT_VER}.zip"
rm -f "$ZIP_NAME"
python3 -m zipfile -c "$ZIP_NAME" installer
echo "  -> Distribution archive created: $ZIP_NAME"
ls -lh "$ZIP_NAME"

echo "=== Success: installer/driver.dll and $ZIP_NAME built successfully ==="
