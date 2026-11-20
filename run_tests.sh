#!/bin/bash
# Bash script to run fast unit tests by default
# Usage: ./run_tests.sh [Debug|Release]
# To run integration tests: ./run_tests.sh -i
# To run all tests: ./run_tests.sh -a

CONFIG="${1:-Debug}"
BUILD_DIR="build"

if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory '$BUILD_DIR' not found. Run cmake first."
    exit 1
fi

cd "$BUILD_DIR" || exit 1

if [ "$1" = "-i" ] || [ "$1" = "--integration" ]; then
    echo "Running integration tests..."
    ctest -C "$CONFIG" -L integration
elif [ "$1" = "-a" ] || [ "$1" = "--all" ]; then
    echo "Running all tests..."
    ctest -C "$CONFIG"
else
    echo "Running fast unit tests (excluding integration tests)..."
    ctest -C "$CONFIG" -LE integration
fi

