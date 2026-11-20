# OpenItsContainer SDK

Run **your** CVN-C traffic controller inside the OpenItsContainer TLC-FI
container. You supply your controller (your C glue + your vendor's prebuilt
`.lib` files); the SDK supplies everything else — the TLC-FI protocol, the
JSON-RPC transport, TLS, the control loop, and `main()`. The result is a single
executable that connects to a TLC-FI Facilities server and drives your
controller.

## Package layout

```
include/            public headers (the only headers you compile against)
  runtime/RuntimeApp.hpp
  cif/CifController.h
  cif/CIF_INC.h
lib/x64/            prebuilt static libs (single /MD set, Debug + Release)
cmake/              CMake package: find_package(OpenItsContainer) + add_cvnc_controller()
msvc/               plain-MSVC property sheet + sample solution
template/           controller.c stub + tlcfi.json
```

You compile only your own controller sources. Those include just
`cif/CIF_INC.h` and `cif/CifController.h` — **no asio, nlohmann, or OpenSSL
headers are needed to build.** OpenSSL is a link-time dependency only.

## The integration contract

Your controller is linked in purely through a small set of C symbols. Exactly
**one** translation unit on your side must provide them (see
`template/controller.c`):

1. **CIF buffers.** Define the buffer sizes, then allocate the buffers by
   including the CVN-C interface under `CIF_PUBLIC`:

   ```c
   #define CIF_AANT_US_FC  <number of signal groups>
   #define CIF_AANT_US_OV  <number of other outputs>
   #define CIF_AANT_IS_D   <number of detectors>
   #define CIF_AANT_IS_OV  <number of other inputs>
   /* ... the remaining CIF_AANT_* / CIF_MAX_* sizes ... */

   #define CIF_PUBLIC
   #include "cif/CIF_INC.h"
   ```

   The four object counts above are read by the container (as the
   `CIF_PB_AANT_*` constants) to drive the automatic object mapping, so they
   must equal your real object counts.

2. **`applicatieprogramma(s_int16)`** — your control logic. Called once with
   `CIF_INIT` at startup, then every ~100 ms tick. Read inputs/detectors from
   `CIF_IS[]`; write desired signal-group states to `CIF_GUS[]`.

3. **`getCifItemName(CifObjectType, int index)`** (from `cif/CifController.h`) —
   maps each CIF array position to its TLC-FI object id. The container calls it
   for every index `0..count-1` of each type and builds the id↔index mapping
   automatically; the mapping is then validated against the objects the TLC
   reports. Return `NULL` for an absent index.

That is the whole boundary. There is no configuration file for the object
mapping — it comes from `getCifItemName`.

### C++ controllers: the linkage shim

If your glue TU is compiled as **C++** (not C), you must force external linkage
on the count constants before including `CIF_INC.h`, because a namespace-scope
`const` defaults to *internal* linkage in C++ (it is external in C):

```cpp
extern "C" {
    extern const short CIF_PB_AANT_US_FC;
    extern const short CIF_PB_AANT_US_OV;
    extern const short CIF_PB_AANT_IS_D;
    extern const short CIF_PB_AANT_IS_OV;
    extern const short CIF_PB_AANT_PARM1;
    extern const short CIF_PB_MAX_UBER;
    extern const short CIF_PB_MAX_IBER;
    extern const short CIF_PB_MAX_MON_IBER;
    extern const short CIF_PB_MAX_MON_UBER;
    extern const short CIF_PB_MAX_FILE_UBER;
}
#define CIF_PUBLIC
#include "cif/CIF_INC.h"
```

A pure-C TU does not need this.

## ABI rules

The prebuilt libs and your vendor `.libs` are linked into one process, so the
ABIs must match:

- **x64** only.
- **MSVC toolchain** (or clang-cl). MinGW/GCC cannot link MSVC static `.libs`.
  Any MSVC version from 2015 onward is link-compatible (Universal CRT).
- **Dynamic release CRT (`/MD`) in every configuration.** The SDK ships one
  `/MD` lib set; your app is built `/MD` even in Debug, so the container libs
  (and your vendor `/MD` libs) link in any configuration. Debug builds still
  debug your own sources (`/Zi`/`/Od`) — just on the release CRT. Do not use
  `/MDd` or `/MT`.
- **One controller per process** — the CVN-C globals are singletons.

Both consumption paths below pin x64 + `/MD` for you.

## Runtime configuration — `tlcfi.json`

The container starts with no command-line arguments; it reads its connection
and streaming settings from `tlcfi.json` next to the binary (override the path
with `--tlcfi <file>`):

```json
{
    "host": "localhost",
    "port": 11501,
    "username": "Controller1",
    "password": "Password1",
    "uri": "app://Controller1",
    "tls": {
        "enabled": false,
        "caFile": "",
        "verifyPeer": true,
        "expectedPeerName": ""
    },

    "cifSerialPort": 0,
    "cifSerialBindAddress": "127.0.0.1",
    "cifLoggingPort": 0,
    "cifLoggingBindAddress": "127.0.0.1",
    "cifFileLogDir": "",
    "cifFileLogSystem": "",
    "jsonLog": "",

    "parmStateDir": ".",
    "parmStateIntervalMs": 1000,

    "controllerOnlyItems": [],
    "manuallyCoupledItems": [],
    "ptpLinks": []
}
```

All keys are optional (absent keys keep built-in defaults; a missing file is a
warning, not an error). The diagnostic settings can also be given as flags
(`--cif-serial-port`, `--cif-serial-bind`, `--cif-logging-port`,
`--cif-logging-bind`, `--cif-file-log-dir`, `--cif-file-log-system`,
`--json-log`, `--parm-state-dir`, `--parm-state-interval-ms`), which override
the JSON. Precedence: defaults < `tlcfi.json` < flags.

TLS is disabled by default for compatibility. For production networks, set
`tls.enabled` to `true`. Peer certificate and hostname verification remains
enabled by default. Set `tls.caFile` to a PEM CA bundle when the system trust
store does not contain the issuing CA. When connecting by IP address to a
certificate issued for a DNS name, set `tls.expectedPeerName` to that DNS name.
Disabling `tls.verifyPeer` removes server authentication and should only be used
in isolated testing.

The CIF serial/logging TCP servers default to `127.0.0.1`. Set
`cifSerialBindAddress` / `cifLoggingBindAddress` (or `--cif-serial-bind` /
`--cif-logging-bind`) to `0.0.0.0` or another interface when remote CCOL
monitoring is required. Inbound lines are injected into the controller's
unauthenticated CIF command channel — expose only on a trusted network.

`cifFileLogDir` enables CIF VLOG file output (`CIF_FILE_UBER`); `cifFileLogSystem`
overrides the filename prefix (defaults to `username`). `jsonLog` traces raw
JSON-RPC messages to a file, useful for debugging protocol issues.

`parmStateDir` sets the directory for the `CIF_PARM1` persistence file (default
`.`; set to empty to disable). `parmStateIntervalMs` controls the minimum
interval between disk writes (default 1000 ms, floor 50 ms).

`controllerOnlyItems` lists CIF objects your controller exposes via
`getCifItemName` that are **absent from the TLC-FI meta data** — i.e. the
intersection does not report them. Without this, the container logs a warning for
every unmatched name. Each entry requires `"name"` and `"type"`, where `type` is
one of `signalGroup`, `output`, `detector`, `input`:

```json
"controllerOnlyItems": [
    { "name": "exclOutputA", "type": "output" },
    { "name": "fix",         "type": "input"  }
]
```

`manuallyCoupledItems` couples a TLC-FI object to a CIF item **whose names
differ** on the two sides. By default the container couples objects by exact,
case-sensitive **name identity** (a controller item named `US01` is coupled to
the TLC-FI object `US01`). When the names diverge, declare the couple explicitly.
Each entry requires `"tlcfiName"`, `"cifName"` and `"type"` (one of `signalGroup`,
`output`, `detector`, `input`):

```json
"manuallyCoupledItems": [
    { "tlcfiName": "us01", "cifName": "US001", "type": "signalGroup" }
]
```

Rules (all enforced at startup — a violation aborts the container):

- A manual couple **wins** over an identity match, regardless of the order the
  controller reports its items in.
- **Both** names must exist on their side: `cifName` among the controller's
  `getCifItemName` items, `tlcfiName` in the TLC-FI meta.
- Coupling stays **complete and exact**. If the manual `cifName` (e.g. `US001`)
  shadows a controller item that *also* exists literally under the manual
  `tlcfiName` (e.g. a controller item named `us01`), that literal item is now
  coupled to nothing and the container aborts — declare it under
  `controllerOnlyItems` to keep the deviation explicit.
- A name cannot appear in both `manuallyCoupledItems` and `controllerOnlyItems`
  for the same type, nor be coupled twice within a type.

## PTP (Point-to-Point) koppelsignaal links

If your controller includes the PTP module (the CVN-C serial coupling protocol
for exchanging koppelsignalen with neighbouring VRIs), the container provides the
transport for it. PTP is **optional and automatic**: the container ships the PTP
transport in `tlcfi_ptp`, but static-library linkage pulls it into your binary
**only if your controller references the PTP driver symbols** (`lees_char_COM_IBER`,
`bezet_COM_IBER`, `schrijf_char_COM_UBER`, `init_COM_IBER`, `init_COM_UBER`). A
controller built without PTP links exactly as before and the transport is dropped
entirely — there is no build-time switch to set.

The container implements **only the byte transport** (a transparent pipe). All
PTP protocol content — framing, CRCs, koppelsignaal encoding, timers — lives in
your controller's PTP module; the container just moves characters between that
module and a socket.

### Which CCOL PTP source files to compile

CCOL ships the PTP application layer in **two mutually-exclusive variants** — each
defines `ptp_init`, `ptp_control`, and `ptp_application_ks`, so you compile one
set or the other, never both:

- **`ptpwin.c`** — the *testomgeving / simulator* variant. It loops koppelsignalen
  back through the CCOLKS shared buffer (`get_ks` / `set_ks`, from `CCOLKS.dll`)
  between two ports **inside one process**. It never touches the COM byte
  transport, so it does **not** talk to a real peer over this container. Do **not**
  compile it for a live controller.
- **The real protocol stack** — `ptpfunc.c`, `ptpksfun.c`, `ptpkps.c`,
  `ptpccitt.c`. These frame/CRC koppelsignalen and move them over
  `lees_char_COM_IBER` / `schrijf_char_COM_UBER` — the symbols this container
  implements. **This is the set you want.**

  > Do **not** also compile `ptpcom.c`: `ptpfunc.c` does `#include "ptpcom.c"`
  > (CCOL unity-build style), so listing it separately double-defines
  > `WriteCharCom` and friends. The `.c` files pulled in via `#include`
  > (`ptpcom.c`, `ptpvar.c`, `sysdef.c`, `ptpdef.c`, …) are compiled through
  > their includer, not added to the project.

When built for the target controller (`AUTOMAAT` defined), `ptpcom.c` includes
`cominter.h` — the declaration of the five COM-transport symbols. The container
ships that header at **`<sdk>/include/cominter.h`**, on the default include root,
so the bare `#include "cominter.h"` resolves with no extra include-path setup
(both the CMake package and the MSVC property sheet already put `<sdk>/include`
on the compiler's include path). (Without `AUTOMAAT`, `ptpcom.c` instead uses
CCOL's own `wincom.h` for a local serial port, bypassing this container.)

Finally, the `PORTNR` your controller passes to the driver (e.g.
`PTP_TINT2.PORTNR` in the generated `*ptp.c`) must equal the **`linkId`** of the
matching `ptpLinks` entry below — it is an engineer-chosen link key, *not* a TCP
port number. Any integer works (the value is yours to pick); a `PORTNR` with no
matching `linkId` reads as permanently empty (no data flows) and logs a one-time
warning. The two peers' `linkId` values need not match each other.

Declare each link in `tlcfi.json` under `ptpLinks`. Each entry needs `linkId`
(= the controller's `PTP->PORTNR`), `mode`, and `peerIp`; `tcpPort` defaults to
`7100`:

```json
"ptpLinks": [
    { "linkId": 1, "mode": "connect", "peerIp": "10.0.0.2", "tcpPort": 7100 },
    { "linkId": 2, "mode": "listen",  "peerIp": "10.0.0.5" }
]
```

`mode` names what **this** container does for the link:

- `"connect"` — the container dials `peerIp:tcpPort` and, if the connection
  fails or drops, retries every 60 s.
- `"listen"` — the container accepts on `tcpPort`. Because several listen links
  can share one port, an incoming connection is routed to the link whose
  `peerIp` matches the remote address; a connection from an unconfigured address
  is refused. `peerIp` is therefore required for `listen` links too.

Each link is one full-duplex TCP connection carrying both directions. `mode` is
per-link, so one container can dial some peers while accepting from others.

Rules (enforced at startup — a violation aborts the container): at most 4 links;
each `linkId` unique (its value is otherwise unconstrained); every link has a
non-empty `peerIp`; and no two `listen` links share the same `peerIp` + `tcpPort`
(their connections could not be told apart). Absent or empty `ptpLinks` means no
PTP links are brought up.

## Building your controller

### Option A — CMake

```cmake
find_package(OpenItsContainer REQUIRED)   # point CMAKE_PREFIX_PATH at <sdk>/cmake

add_cvnc_controller(my_controller
    C_SOURCES       controller.c          # your controller glue
    CONTROLLER_LIBS vendor/cvnc_core.lib) # your vendor .libs
```

`add_cvnc_controller()` links the container + your libs, supplies `main()`, and
pins the ABI (x64, `/MD`, delay-loaded OpenSSL). Configure with
`-DCMAKE_PREFIX_PATH=<sdk>/cmake` and build.

### Option B — plain Visual Studio (no CMake)

1. Open `msvc/sample/my_controller.sln` (or your own x64 project).
2. In the Property Manager, add `msvc/OpenItsContainer.props` to the project.
3. Replace the sample `controller.c` with yours and add your vendor `.libs`
   under **Linker > Additional Dependencies**.
4. Build x64 / Release or Debug.

The property sheet wires the include dir, lib dirs, dependencies, `/MD`,
C++20, the Console subsystem, and delay-loaded OpenSSL. Override `OpenSSLDir` if
your OpenSSL is not at `C:\Program Files\OpenSSL-Win64`.

## OpenSSL and deployment

OpenSSL is **delay-loaded**: a deployment that uses plain TCP (no TLS) runs
without the OpenSSL DLLs present at all. If you use TLS, place the OpenSSL DLLs
(e.g. `libssl-3-x64.dll`, `libcrypto-3-x64.dll`) next to your binary. If your
OpenSSL build uses different DLL names, override them — `OPENITS_OPENSSL_DLLS`
in CMake, or the `DelayLoadDLLs` linker setting in MSVC.

## Quick start

1. Copy `template/controller.c` and `template/tlcfi.json` into your project.
2. Edit the sizes, object name tables, and `applicatieprogramma()` in
   `controller.c`; edit the connection in `tlcfi.json`.
3. Build with Option A or B above.
4. Run the resulting `.exe` (with `tlcfi.json` alongside it).
```
