# OpenItsContainer

OpenItsContainer is an open-source TLC-FI / GEN-FI runtime container for traffic
light controllers. It implements the TLC-FI and GEN-FI protocols over JSON-RPC
2.0, handles TCP/TLS transport, manages the control-loop state machine, and
provides `main()` — so a controller developer only needs to supply the controller
logic itself.

The CVN-C coupling (CIF host and bridge) is one included implementation of that
boundary, targeting the procedural CVN-C / CCOL programming model used in Dutch
traffic control. But the container is not CVN-C-specific: any C or C++ controller
can be linked in and hosted — whether that is a traditional procedural CCOL
controller, a modern event-driven design, or a more functionally-oriented
implementation. The only requirement is providing the three C symbols the
container calls across the boundary (`applicatieprogramma`, `getCifItemName`, and
the CIF buffer definitions).

The project can also be packaged as an **SDK**, shipping prebuilt static
libraries and public headers so third-party developers can integrate their own
controller without building the container from source.

---

## Dependencies

| Library | Version | How obtained |
|---|---|---|
| [Asio](https://github.com/chriskohlhoff/asio) | 1.36.0 | FetchContent (or system package if found) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.12.0 | FetchContent (or system package if found) |
| [OpenSSL](https://www.openssl.org/) | Any supported | System / vcpkg — **required**, must be findable at configure time |
| [Google Test](https://github.com/google/googletest) | 1.17.0 | FetchContent — only when `TLCFI_BUILD_TESTS=ON` |

Asio and nlohmann/json are fetched automatically by CMake if not found on the
system. On Linux/WSL you can instead install them via your package manager
(`libasio-dev`, `nlohmann-json3-dev`) and the build will use those. OpenSSL mus
always be present; on Windows set `OPENSSL_ROOT_DIR` if it is not in a standard
location or managed by vcpkg.

---

## Building

### Prerequisites

| Requirement | Windows | Linux / WSL |
|---|---|---|
| C++ compiler | MSVC (Visual Studio 2022, v143) | GCC 12+ or Clang 15+ |
| CMake | 3.20+ | 3.20+ |
| Build system | Visual Studio generator (built-in) | Ninja |
| OpenSSL | Required (set `OPENSSL_ROOT_DIR` if not on PATH) | Required (`libssl-dev`) |

### Installing OpenSSL on Windows (vcpkg)

The easiest way to get OpenSSL on Windows is via [vcpkg](https://vcpkg.io):

```powershell
# 1. Clone and bootstrap vcpkg (once, anywhere on your machine)
git clone https://github.com/microsoft/vcpkg.git C:/vcpkg
C:/vcpkg/bootstrap-vcpkg.bat

# 2. Install OpenSSL (x64)
C:/vcpkg/vcpkg.exe install openssl:x64-windows

# 3. Tell CMake where vcpkg is (add to your configure command, or set globally)
# -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

When the toolchain file is passed to CMake, `find_package(OpenSSL)` resolves
automatically — no need to set `OPENSSL_ROOT_DIR` manually. You can also set
`VCPKG_ROOT` as a system environment variable and many editors (Visual Studio,
VS Code) will pick it up automatically.

### Windows (Visual Studio 2022)

Use one of the provided CMake presets. Each preset selects the protocol version
and build type:

| Preset | Protocol | Config |
|---|---|---|
| `vs2022-x64-debug-1.1.0` | TLC-FI 1.1.0 | Debug |
| `vs2022-x64-release-1.1.0` | TLC-FI 1.1.0 | Release |
| `vs2022-x64-debug-2.0.1` | TLC-FI 2.0.1 | Debug |
| `vs2022-x64-release-2.0.1` | TLC-FI 2.0.1 | Release |

```powershell
# Configure (add -DCMAKE_TOOLCHAIN_FILE=... if using vcpkg for OpenSSL)
cmake --preset vs2022-x64-debug-2.0.1 -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build (Visual Studio multi-config generator: specify --config)
cmake --build build-vs/x64-Debug-2.0.1 --config Debug
```

Or open the generated `.sln` in Visual Studio and build from there.

To enable unit tests, add `-DTLCFI_BUILD_TESTS=ON` to the configure step:

```powershell
cmake --preset vs2022-x64-debug-2.0.1 -DTLCFI_BUILD_TESTS=ON
```

### Linux / WSL

Two Ninja presets are available. Both export `compile_commands.json` for clangd.

| Preset | Protocol | Config |
|---|---|---|
| `linux-ninja-debug` | TLC-FI 1.1.0 | Debug |
| `linux-ninja-release` | TLC-FI 1.1.0 | Release |

```bash
# Configure
cmake --preset linux-ninja-debug

# Build
cmake --build ~/build/OpenItsContainer-linux-ninja-debug-1.1.0

# Copy compile_commands.json to the project root for clangd (once after configure)
cp ~/build/OpenItsContainer-linux-ninja-debug-1.1.0/compile_commands.json .
```

To enable unit tests:

```bash
cmake --preset linux-ninja-debug -DTLCFI_BUILD_TESTS=ON
```

### Running tests

After a tests-enabled build, use the provided scripts. Pass `-BuildDir` to match the preset build directory:

```powershell
# Windows — unit tests only (default)
.\run_tests.ps1 -BuildDir build-vs\x64-Debug-2.0.1

# Windows — integration tests
.\run_tests.ps1 -BuildDir build-vs\x64-Debug-2.0.1 -Integration

# Windows — all tests
.\run_tests.ps1 -BuildDir build-vs\x64-Debug-2.0.1 -All

# Windows — Release config
.\run_tests.ps1 -BuildDir build-vs\x64-Release-2.0.1 -Config Release -All
```

```bash
# Linux — unit tests only (default)
./run_tests.sh

# Linux — integration tests
./run_tests.sh -i

# Linux — all tests
./run_tests.sh -a
```

See `tests/README.md` for more detail on test structure and running specific
test executables directly.

---

## SDK

The container can be packaged as an SDK so third-party developers can link their
own CVN-C controller against the prebuilt static libraries, without needing to
build the full project.

### Building the SDK payload

```powershell
# 1. Configure (tests OFF: googletest install rules pollute the SDK layout)
cmake -B build-sdk -G "Visual Studio 17 2022" -DTLCFI_BUILD_TESTS=OFF

# 2. Build the container libs
#    tlcfi_ptp is a standalone, force-link-free static lib (PTP stays optional
#    at zero cost), so it is NOT pulled in by tlcfi_main and must be named
#    explicitly — otherwise the install step below cannot find tlcfi_ptp.lib.
cmake --build build-sdk --config Release --target tlcfi_main tlcfi_ptp

# 3. Install to the SDK output directory
cmake --install build-sdk --config Release --prefix C:/out/OpenItsContainer-SDK
```

This produces an `OpenItsContainer-SDK/` directory with `include/`, `lib/x64/`,
`cmake/`, `msvc/`, `template/`, and `README.md`.

> **Note:** Build the SDK with a *released* MSVC toolset (VS2022 / v143). Static
> libs carry their compiled objects, and building with a preview toolset stamps
> them with a runtime requirement that no public VC++ redistributable can
> satisfy on a target machine.

### Using the SDK

See `sdk/README.md` for the consumer-facing guide: the CIF integration contract,
ABI rules, `tlcfi.json` configuration, and both the CMake and plain-MSVC
consumption paths.

---

## Project structure

```
cmake/          CMake helper modules installed with the SDK package
docs/           Protocol and design documentation
examples/       Example applications (monitor consumer, controller stub)
sdk/            SDK template, MSVC property sheet, and consumer README
src/
  cif/          CIF protocol glue — CVN-C CIF interface and message server
  fi/           TLC-FI / GEN-FI facilities interface (consumer and controller apps)
  jsonrpc/      JSON-RPC 2.0 implementation (sessions, framing, dispatch)
  models/       Protocol type definitions for GEN-FI and TLC-FI
  runtime/      App container runtime — config loading, startup, main loop
  tcp/          TCP and TLS transport layer (asio-based)
tests/          Unit and integration tests (Google Test)
```

### Generated folders

These are created locally when building or running CMake. They are git-ignored and should not be committed.

```
build-vs/
  x64-Debug-1.1.0/     VS 2022 Debug build, TLC-FI 1.1.0  (preset vs2022-x64-debug-1.1.0)
  x64-Release-1.1.0/   VS 2022 Release build, TLC-FI 1.1.0
  x64-Debug-2.0.1/     VS 2022 Debug build, TLC-FI 2.0.1  (preset vs2022-x64-debug-2.0.1)
  x64-Release-2.0.1/   VS 2022 Release build, TLC-FI 2.0.1
build-sdk/              SDK payload build (produced by the SDK build steps above)
~/build/
  OpenItsContainer-linux-ninja-debug-1.1.0/    Linux/WSL Ninja Debug build
  OpenItsContainer-linux-ninja-release-1.1.0/  Linux/WSL Ninja Release build
CMakeFiles/             CMake internal bookkeeping — generated in the source root by some tools
compile_commands.json   Compilation database for clangd IntelliSense (copy from Ninja build output)
out/                    Visual Studio CMake output directory (used by VS's built-in CMake integration)
_deps/                  FetchContent dependency sources downloaded by CMake at configure time
Testing/                CTest runtime artefacts (last test log, cost data)
```
