#!/bin/bash
# Build script for mcp-psd2x with submodule dependencies
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Initialize submodules if needed
git submodule update --init --recursive

# Build qtpsd first
echo "=== Building QtPsd ==="
cmake -S external/qtpsd -B build-qtpsd
cmake --build build-qtpsd

# Build qtmcp (depends on qtpsd for some optional features, but not required)
echo "=== Building QtMcp ==="
cmake -S external/qtmcp -B build-qtmcp
cmake --build build-qtmcp

# Build mcp-psd2x
echo "=== Building mcp-psd2x ==="
cmake -B build -DQT_ADDITIONAL_PACKAGES_PREFIX_PATH="$SCRIPT_DIR/build-qtmcp;$SCRIPT_DIR/build-qtpsd"
cmake --build build

echo "=== Build complete ==="
echo ""
echo "To run mcp-psd2x:"
echo "  export QT_PLUGIN_PATH=\"$SCRIPT_DIR/build-qtmcp/lib64/qt6/plugins:$SCRIPT_DIR/build-qtpsd/lib64/qt6/plugins\""
echo "  ./build/mcp-psd2x"
