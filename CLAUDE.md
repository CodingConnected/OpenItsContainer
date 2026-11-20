# AGENTS.md

## Build Commands

### Basic Setup
```bash
# Generate compile commands for IntelliSense
./generate_compile_commands.ps1

# Build with Debug configuration
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Build with Release configuration
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Testing

```bash
# Run all unit tests (default, excluding integration tests)
./run_tests.sh              # Linux/Mac
./run_tests.ps1             # PowerShell

# Run integration tests only
./run_tests.sh -i           # Linux/Mac
./run_tests.ps1 -Integration # PowerShell

# Run all tests including integration
./run_tests.sh -a           # Linux/Mac
./run_tests.ps1 -All        # PowerShell

# Run specific test executable
ctest -C Debug --verbose -R test_name

# Run tests on existing build
cd build
ctest -C Debug -V -L unit
ctest -C Debug -V -L integration
```

### Build Options
```bash
# Build with TLC-FI protocol version 1.1.0 (requires TLCFI_BUILD_TESTS)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTLCFI_PROTOCOL_VERSION=1.1.0

# Build with tests enabled
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DTLCFI_BUILD_TESTS=ON
```

## Code Style Guidelines

### Language and Compiler
- **Language**: C++20 (use features from C++20 standard)
- **Compiler**: MSVC (Visual Studio 2022) or compatible
- **Platform**: Windows only (targeting Windows API)
- **Standard**: CXX_STANDARD=20, CXX_EXTENSIONS=OFF

### File Organization
- **Headers**: Use `#pragma once` (no guard headers)
- **Implementation**: Separate `.cpp` files for each public header
- **Include Order**: Standard library → Third-party → Project headers
- **Namespaces**: Use `tlcfi::` for all project code, nested namespaces for modules

### Naming Conventions

**Classes and Structures**
- PascalCase (e.g., `JsonRpcSession`, `FiFacilitiesServer`, `ITransport`)
- Acronyms: Keep case (e.g., `Fi`, `TLC`, `CIF`, `RPC`)

**Functions and Methods**
- camelCase (e.g., `start()`, `stop()`, `asyncSend()`, `onConnected()`)

**Variables and Parameters**
- camelCase (e.g., `transport`, `sessionId`, `portNumber`)

**Type Names**
- PascalCase for classes and structs
- camelCase for functions, variables, parameters
- PascalCase for namespaces (e.g., `tlcfi::jsonrpc`)

**Constants and Enums**
- UPPERCASE_SNAKE_CASE (e.g., `MAX_SESSIONS`, `TLCFI_VERSION_1_1_0`)

### Header Structure

```cpp
#pragma once

// --- Standard Library Includes ---
#include <memory>
#include <mutex>
#include <string>

// --- Third-Party Includes ---
#include <asio/...>

// --- Project Includes ---
#include ".../header.hpp"

// --- Namespace Declaration ---
namespace tlcfi::module {

// --- Forward Declarations (if needed) ---
// class ClassName;

// --- Classes and Methods ---
class ClassName {
public:
    // Public interface
    ClassName(/*param*/, /*param*/);
    virtual ~ClassName(); // Always provide virtual destructor for base classes

    // Methods
    void methodName(/*param*/);
    std::uint8_t getConstant();
    static ClassName* create(/*param*/);

    // Callbacks
    std::function<void()> onEvent;

protected:
    // Protected interface
    void protectedMethod();

private:
    // Private implementation
    void helperMethod();
    std::unique_ptr<Implementation> impl_;
    static const std::size_t MAX_SIZE = 1024;
};

// --- Namespace Closing ---
} // namespace tlcfi::module
```

### Implementation Structure

```cpp
#include "header.hpp"
#include <iostream>

// Include necessary implementation files

// Define static constants
namespace {
    const std::size_t INTERNAL_BUFFER_SIZE = 8192;
}

namespace tlcfi::module {

ClassName::ClassName(/*param*/)
    : impl_(std::make_unique<Implementation>())
{
    // Constructor implementation
    std::cout << "Constructing " << __FUNCTION__ << std::endl;
}

ClassName::~ClassName()
{
    // Virtual destructor implementation
}

void ClassName::methodName(/*param*/)
{
    // Method implementation
}

// Inline implementations for small, frequently called methods
inline void ClassName::smallMethod(/*param*/)
{
    // Small inline implementation
}

} // namespace tlcfi::module
```

### Code Comments

1. **Include block comments** for complex logic
2. **Function documentation** using Doxygen-style:
   - `/**` block for functions
   - `@brief` for summary
   - `@param` for parameters
   - `@return` for return values
   - `@note` for important notes
   - `@warning` for warnings
   - `@throws` for exceptions
3. **Line comments** for inline explanations
4. **No comments** for obvious code
5. **Multi-line comments** use `/* ... */`

### Error Handling

1. **Custom exceptions**: Create namespace-specific exception classes
2. **Error codes**: Use custom error codes where strings aren't sufficient
3. **Return types**: Use `std::future` for async operations with error handling
4. **Error propagation**: Throw exceptions for unrecoverable errors
5. **Error responses**: Convert protocol errors to JSON-RPC error responses
6. **Logging**: Use appropriate logging for debug information

### Exception Classes

```cpp
namespace tlcfi::module {

class CustomException : public std::runtime_error {
public:
    CustomException(const std::string& message)
        : std::runtime_error(message)
    {}

    // Optional: Error code member
    int errorCode() const;
    void setErrorCode(int code);
};

} // namespace tlcfi::module
```

### Resource Management

1. **Smart pointers**: Use `std::shared_ptr` for shared ownership, `std::unique_ptr` for exclusive ownership
2. **RAII**: All resources must be managed in constructors/destructors
3. **Virtual destructors**: Required for polymorphic base classes
4. **No manual new/delete**: Use smart pointers exclusively
5. **Move semantics**: Prefer passing by value or move when appropriate

### Threading and Synchronization

1. **Thread safety**: All shared data must be protected by mutexes
2. **Lock guard**: Use `std::lock_guard` and `std::unique_lock` for RAII locking
3. **Async operations**: Use `asio::...` for asynchronous I/O
4. **Future/promise**: Use for async result handling
5. **Static**: Use `std::atomic` for single-variable thread safety

### Message Framing

JSON-RPC messages can be framed using different methods. All implement `IMessageFramer`:

- `BalancedJsonFramer`: Balanced JSON (prevents framing ambiguity). **This is the framer
  currently used by the TLC-FI client (`FiConsumerApp`) and server (`FiFacilitiesServer`).**
- `ContentLengthFramer`: HTTP-style Content-Length header. Provided as an alternative;
  not currently used by any application code.
- `NdjsonFramer`: Newline-delimited JSON (simple, but no batching). Provided as an
  alternative; not currently used by any application code.

### Protocol Versioning

Configure at build time using `TLCFI_PROTOCOL_VERSION`:
- `1.1.0`: Uses `TLCFI_VERSION_1_1_0` definition
- `2.0.1`: Uses `TLCFI_VERSION_2_0_1` definition

### TLS Configuration

- Use `tcp::TlsConfig` for TLS settings
- If no TLS config provided, uses plain TCP
- Set `OPENSSL_ROOT_DIR` CMake variable for manual OpenSSL installation

### Code Quality Checks

```bash
# Static analysis (clangd)
# Ensure clangd is installed and clangd: Restart language server

# Test execution
./run_tests.sh             # Run unit tests
./run_tests.sh -i          # Run integration tests

# Build verification
cmake --build build -j     # Parallel build
```

### Common Patterns

**Factory Pattern**: Preferred for creation of objects requiring dependencies

**Observer Pattern**: Use callbacks (`std::function`) for event notifications

**RAII**: All resources freed in destructor, no manual cleanup needed

**Async API**: Operations return `std::future<T>` for async operation handling

**Protocol Implementation**: Follow GEN-FI and TLC-FI specs when implementing protocol methods

## Additional Notes

- This is a Windows-only project using MSVC compiler
- Use Ninja as the build system generator for best performance
- Regenerate `compile_commands.json` after modifying CMake or adding files
- Follow existing project structure and conventions in each module:
  - `cif/` - CIF protocol implementation
  - `fi/` - TLC-FI and RIS-FI facilities interfaces
  - `jsonrpc/` - JSON-RPC 2.0 implementation
  - `models/` - Protocol type definitions (generated and specification)
  - `tcp/` - Transport layer (TCP/TLS)
  - `examples/` - Usage examples
  - `tests/` - Unit and integration tests

### External / vendored source

- `src/cif/CIF_INC.h` is an **external source file** (CVN Commissie C, "CIF.INC")
  provided by a third party. It is the authoritative CVN-C interface definition and
  **must not be edited** — not even to fix warnings, remove unused defines, or
  reformat. We currently use only a subset of the interface (not all buffers/defines),
  but the file must remain byte-for-byte as delivered so it stays in sync with the
  upstream specification. Pre-existing compiler warnings from this file are expected
  and should be left alone.

  ## Docs MCP

When questions arise about protocols (VLOG, IVERA, TLC-FI, GEN-FI, etc.), check the Docs MCP server first.

## Knowledge Notes (MCP)

This repo uses the `knowledge-notes` MCP server for durable, searchable notes
across sessions. **Always pass `project: "OpenItsContainer"` on every call.**

### When to record a note

Record when you learn something that a fresh session could not re-derive from
`Grep`/`Glob` over the tree:

- **gotcha** — a non-obvious pitfall just bit you (or nearly did).
- **decision** — you made a design call whose *why* isn't visible in the
  resulting code ("we chose X over Y because Z"). Includes design choices.
- **convention** — a code-style or pattern norm adopted for this repo.
- **port-detail** — when porting (e.g. WPF→React), how a specific construct
  mapped, what worked, what didn't.
- **note** — general knowledge capture, default for anything that doesn't
  fit the four above.

Record in the moment, not at end-of-session — context fades fast.

```
record_note(
    project="{{PROJECT_NAME}}",
    kind="gotcha",
    topic="datagrid-flicker",
    body="Setting ItemsSource before binding fires OnCollectionChanged twice; ...",
    related_paths=["src/views/CustomerGrid.xaml"],
    tags=["wpf","datagrid"]
)
```

### When to recall

**First thing in a non-trivial task**, before diving into code:

```
recall_notes(project="{{PROJECT_NAME}}", query="<a concept phrase from the task>")
```

Then also try with any distinctive identifiers (class names, method names, error
strings). If nothing fires, browse with:

```
list_notes(project="{{PROJECT_NAME}}", kind="gotcha")
list_notes(project="{{PROJECT_NAME}}", kind="decision")
```

Default `mode='hybrid'` is almost always right. Switch to `mode='keyword'` only
for exact-identifier lookups where semantic paraphrase would add noise.

### Correcting or retiring notes

- Note is wrong but you have a correction → `supersede_note(old_id, new_body)`.
  The old note stays reachable by id but vanishes from recall.
- Note is wrong and you don't yet have a fix → `flag_stale(id, reason)`.

Do **not** delete notes. Supersede chains preserve history.

### What NOT to put in notes

- Transient task state (use the task tool).
- Code explanations a grep can produce.
- Project documentation (use the Docs MCP server).
- Anything already in this CLAUDE.md.

### Cross-project search

Rarely needed, but when a past port or pattern in a *different* project is
relevant:

```
recall_notes_global(query="<concept>")
```

Use sparingly — single-project recall is the default for a reason.