# Google Test Suite

## Overview
Unit tests for the TCP client and server implementations using Google Test (gtest) framework.

## Building Tests

Enable tests when configuring CMake:
```bash
cmake -DTLCFI_BUILD_TESTS=ON -B build
cmake --build build
```

## Running Tests

### Run all tests via CTest:
```bash
cd build
ctest -C Debug    # For Debug configuration
# or
ctest -C Release  # For Release configuration
```
Note: this runs the whole suite as 4 tests, whereas in reality each executable contains multiple tests. If this fails, run the specific executable mentioned below to find out the exact cause.

### Run specific test executable directly:
```bash
# Windows (Debug)
.\build\tests\tcp\Debug\test_tcp_server.exe

# Windows (Release)
.\build\tests\tcp\Release\test_tcp_server.exe

# Linux/Mac
./build/tests/tcp/test_tcp_server
```

### Run with gtest filters:
```bash
.\build\tests\tcp\Debug\test_tcp_server.exe --gtest_filter=TcpServerTest.*
```

## Test Structure

- `tests/tcp/test_tcp_server.cpp` - TcpServer tests using Google Test
- Other files in the same folder have other tests
- Uses test helpers from `tests/tcp/test_helpers.hpp`

## Current Test Coverage

Most of the code should currently be covered.
