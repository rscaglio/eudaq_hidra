#!/bin/bash
# Build script for HidraDryXDCProducer tests
# Usage: ./build_tests.sh [basic|advanced|all]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"
HIDRA_INCLUDE="${SCRIPT_DIR}/../include"
EUDAQ_INCLUDE="${SCRIPT_DIR}/../../../include"

# Check if gtest is available
if ! pkg-config --exists gtest; then
    echo "Error: Google Test not found. Install with:"
    echo "  Ubuntu/Debian: sudo apt-get install libgtest-dev"
    echo "  macOS: brew install googletest"
    exit 1
fi

# Get gtest flags
GTEST_CFLAGS=$(pkg-config --cflags gtest 2>/dev/null || echo "-I/usr/include")
GTEST_LIBS=$(pkg-config --libs gtest gtest_main 2>/dev/null || echo "-lgtest -lgtest_main -lpthread")

# Compiler settings
CXX=${CXX:-g++}
CXXFLAGS="-std=c++17 -Wall -Wextra -O2 -fPIC"
INCLUDES="-I${HIDRA_INCLUDE} -I${EUDAQ_INCLUDE} -I/usr/include ${GTEST_CFLAGS}"

# Source files
DRY_SOURCES="${SCRIPT_DIR}/../src/HidraDryXDCProducer.cc"
TEST_TYPE=${1:-all}

# Create build directory
mkdir -p "${BUILD_DIR}/test"
cd "${BUILD_DIR}/test"

echo "Building HidraDryXDCProducer tests..."
echo "  C++ Compiler: ${CXX}"
echo "  Flags: ${CXXFLAGS}"
echo "  Includes: ${INCLUDES}"
echo "  GTest Libs: ${GTEST_LIBS}"

# Build basic tests
if [[ "${TEST_TYPE}" == "basic" || "${TEST_TYPE}" == "all" ]]; then
    echo ""
    echo "Building basic tests..."
    ${CXX} ${CXXFLAGS} \
        ${INCLUDES} \
        "${SCRIPT_DIR}/HidraDryXDCProducerTest.cc" \
        ${DRY_SOURCES} \
        -o hidra_dry_xdc_producer_test \
        -L/usr/lib -L/usr/local/lib \
        ${GTEST_LIBS} -ldl
    echo "✓ Created: hidra_dry_xdc_producer_test"
fi

# Build advanced tests
if [[ "${TEST_TYPE}" == "advanced" || "${TEST_TYPE}" == "all" ]]; then
    echo ""
    echo "Building advanced tests..."
    ${CXX} ${CXXFLAGS} \
        ${INCLUDES} \
        "${SCRIPT_DIR}/HidraDryXDCProducerAdvancedTest.cc" \
        ${DRY_SOURCES} \
        -o hidra_dry_xdc_producer_advanced_test \
        -L/usr/lib -L/usr/local/lib \
        ${GTEST_LIBS} -ldl
    echo "✓ Created: hidra_dry_xdc_producer_advanced_test"
fi

echo ""
echo "Build completed successfully!"
echo ""
echo "Run tests with:"
echo "  ./hidra_dry_xdc_producer_test"
echo "  ./hidra_dry_xdc_producer_advanced_test"
