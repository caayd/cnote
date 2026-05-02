#!/bin/bash
set -euo pipefail

# Build the project
cd "$(dirname "$0")"

# Use make directly if build directory exists with Makefiles
if [ -f build/Makefile ] || [ -f build/makefile ]; then
    cd build
    if ! make -j4 2>&1; then
        echo "METRIC build_success=0"
        echo "METRIC compile_error=1"
        exit 1
    fi
else
    # No build directory - try to configure and build
    rm -rf build
    mkdir -p build
    cd build
    
    # Try cmake if available
    if [ -f "/c/Program Files/CMake/bin/cmake.exe" ]; then
        if ! "/c/Program Files/CMake/bin/cmake.exe" .. -G "MinGW Makefiles" 2>&1; then
            echo "METRIC build_success=0"
            echo "METRIC configure_error=1"
            exit 1
        fi
        if ! make -j4 2>&1; then
            echo "METRIC build_success=0"
            echo "METRIC compile_error=1"
            exit 1
        fi
    else
        echo "METRIC build_success=0"
        echo "METRIC configure_error=1"
        exit 1
    fi
fi

# Success
echo "METRIC build_success=1"
echo "METRIC compile_error=0"
echo "METRIC configure_error=0"
