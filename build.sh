#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="${1:-Release}"
BUILD_DIR="${2:-build-linux}"

find_cmake() {
    if command -v cmake &>/dev/null; then
        command -v cmake
        return
    fi
    echo "CMake not found. Install it:" >&2
    echo "  sudo apt-get install cmake" >&2
    exit 1
}

find_ctest() {
    if command -v ctest &>/dev/null; then
        command -v ctest
        return
    fi
    echo "CTest not found. Install CMake:" >&2
    echo "  sudo apt-get install cmake" >&2
    exit 1
}

check_compiler() {
    if ! command -v g++ &>/dev/null && ! command -v clang++ &>/dev/null; then
        echo "No C++ compiler found. Install one:" >&2
        echo "  sudo apt-get install build-essential" >&2
        exit 1
    fi
}

check_wxwidgets() {
    if ! pkg-config --exists wxwidgets-3.0; then
        echo "wxWidgets 3.0 not found. Install it:" >&2
        echo "  sudo apt-get install libwxgtk3.0-gtk3-dev" >&2
        exit 1
    fi
}

CMAKE=$(find_cmake)
CTEST=$(find_ctest)
check_compiler
check_wxwidgets

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_PATH="$SCRIPT_DIR/$BUILD_DIR"
EXECUTABLE="$BUILD_PATH/hello_cross_platform"

echo "Using CMake : $CMAKE"
echo "Configuration: $CONFIGURATION"
echo "Build directory: $BUILD_PATH"

"$CMAKE" -S "$SCRIPT_DIR" -B "$BUILD_PATH" -DCMAKE_BUILD_TYPE="$CONFIGURATION"
"$CMAKE" --build "$BUILD_PATH" --config "$CONFIGURATION"
"$CTEST" --test-dir "$BUILD_PATH" -C "$CONFIGURATION" --output-on-failure

# Run executable with GUI flag
echo ""
echo "=== Running GUI Application ==="
"$EXECUTABLE" --gui

