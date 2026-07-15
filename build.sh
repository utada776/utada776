#!/usr/bin/env bash
set -euo pipefail

CONFIGURATION="Release"
BUILD_DIR="build-linux"
RUN_MODE="auto"
RUN_AFTER_BUILD="1"
RUN_TESTS="1"

usage() {
    cat <<'EOF'
Usage: ./build.sh [options]

Options:
  -c, --configuration <Debug|Release>  Build type (default: Release)
  -b, --build-dir <dir>                Build directory (default: build-linux)
      --gui                            Run GUI after build
      --cli                            Run CLI after build
      --no-run                         Build/test only, do not start the app
      --no-test                        Skip ctest
  -h, --help                           Show this help

Examples:
  ./build.sh
  ./build.sh --configuration Debug
  ./build.sh --build-dir out/linux-release --no-run
  ./build.sh --gui
  ./build.sh --cli
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--configuration)
            CONFIGURATION="${2:-}"
            shift 2
            ;;
        -b|--build-dir)
            BUILD_DIR="${2:-}"
            shift 2
            ;;
        --gui)
            RUN_MODE="gui"
            shift
            ;;
        --cli)
            RUN_MODE="cli"
            shift
            ;;
        --no-run)
            RUN_AFTER_BUILD="0"
            shift
            ;;
        --no-test)
            RUN_TESTS="0"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        Debug|Release)
            CONFIGURATION="$1"
            shift
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "$CONFIGURATION" != "Debug" && "$CONFIGURATION" != "Release" ]]; then
    echo "Invalid configuration: $CONFIGURATION" >&2
    echo "Use Debug or Release." >&2
    exit 1
fi

find_cmake() {
    if command -v cmake >/dev/null 2>&1; then
        command -v cmake
        return
    fi
    echo "CMake not found. Install it:" >&2
    echo "  sudo apt-get update && sudo apt-get install -y cmake" >&2
    exit 1
}

find_ctest() {
    if command -v ctest >/dev/null 2>&1; then
        command -v ctest
        return
    fi
    echo "CTest not found. Install CMake:" >&2
    echo "  sudo apt-get update && sudo apt-get install -y cmake" >&2
    exit 1
}

check_compiler() {
    if command -v g++ >/dev/null 2>&1; then
        echo "Compiler     : $(command -v g++)"
        return
    fi
    if command -v clang++ >/dev/null 2>&1; then
        echo "Compiler     : $(command -v clang++)"
        return
    fi

    echo "No C++ compiler found. Install one:" >&2
    echo "  sudo apt-get update && sudo apt-get install -y build-essential" >&2
    exit 1
}

check_wxwidgets() {
    if command -v wx-config >/dev/null 2>&1; then
        echo "wxWidgets    : $(wx-config --version)"
        return
    fi

    if command -v pkg-config >/dev/null 2>&1; then
        if pkg-config --exists wxgtk3u-3.2; then
            echo "wxWidgets    : $(pkg-config --modversion wxgtk3u-3.2)"
            return
        fi
        if pkg-config --exists wxwidgets-3.2; then
            echo "wxWidgets    : $(pkg-config --modversion wxwidgets-3.2)"
            return
        fi
        if pkg-config --exists wxgtk3u-3.0; then
            echo "wxWidgets    : $(pkg-config --modversion wxgtk3u-3.0)"
            return
        fi
        if pkg-config --exists wxwidgets-3.0; then
            echo "wxWidgets    : $(pkg-config --modversion wxwidgets-3.0)"
            return
        fi
    fi

    echo "wxWidgets development files were not found. Install one of:" >&2
    echo "  sudo apt-get update && sudo apt-get install -y libwxgtk3.2-dev" >&2
    echo "  sudo apt-get update && sudo apt-get install -y libwxgtk3.0-gtk3-dev" >&2
    exit 1
}

print_linux_hints() {
    cat <<'EOF'

Helpful Ubuntu packages:
  sudo apt-get update
  sudo apt-get install -y build-essential cmake libwxgtk3.2-dev libvtk9-dev

If GUI launch fails in a headless shell:
  - Use --cli to run in terminal mode
  - Or export DISPLAY and start an X/Wayland session
EOF
}

CMAKE="$(find_cmake)"
CTEST="$(find_ctest)"
check_compiler
check_wxwidgets

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_PATH="$SCRIPT_DIR/$BUILD_DIR"
EXECUTABLE="$BUILD_PATH/hello_cross_platform"

mkdir -p "$BUILD_PATH"

echo "Using CMake  : $CMAKE"
echo "Using CTest  : $CTEST"
echo "Configuration: $CONFIGURATION"
echo "Build dir    : $BUILD_PATH"

"$CMAKE" -S "$SCRIPT_DIR" -B "$BUILD_PATH" -DCMAKE_BUILD_TYPE="$CONFIGURATION"
"$CMAKE" --build "$BUILD_PATH" --config "$CONFIGURATION"

if [[ "$RUN_TESTS" == "1" ]]; then
    "$CTEST" --test-dir "$BUILD_PATH" --output-on-failure
fi

if [[ "$RUN_AFTER_BUILD" != "1" ]]; then
    exit 0
fi

if [[ ! -x "$EXECUTABLE" ]]; then
    echo "Executable was not produced at: $EXECUTABLE" >&2
    exit 1
fi

if [[ "$RUN_MODE" == "auto" ]]; then
    if [[ -n "${DISPLAY:-}" || -n "${WAYLAND_DISPLAY:-}" ]]; then
        RUN_MODE="gui"
    else
        RUN_MODE="cli"
    fi
fi

echo
if [[ "$RUN_MODE" == "gui" ]]; then
    echo "=== Running GUI Application ==="
    if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo "No GUI display detected. Falling back to CLI mode." >&2
        print_linux_hints
        "$EXECUTABLE"
    else
        "$EXECUTABLE"
    fi
else
    echo "=== Running CLI Application ==="
    "$EXECUTABLE"
fi

