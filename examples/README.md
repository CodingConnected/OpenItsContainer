# TLC-FI Examples

Three example applications cover the main integration patterns. All are built as
part of the main project — configure and build from the project root first (see
the root `README.md`). The executables land in
`build-vs/<preset>/examples/<Config>/` on Windows.

---

## consumer_app

A read-only monitoring application built on `FiConsumerApp` and `FiAppHost`. It
connects to a TLC-FI Facilities server, reads meta data to discover available
objects, subscribes to all object types (SignalGroups, Outputs, Detectors,
Inputs), and prints state changes to the console. TLS is supported via
command-line flags. Runs until interrupted (Ctrl+C).

```
consumer_app [host] [port] [username] [password] [uri] [--tls] [--ca-file <file>] [--no-verify]
```

| Argument | Default |
|---|---|
| `host` | `localhost` |
| `port` | `11501` |
| `username` | `Consumer1` |
| `password` | `Password1` |
| `uri` | `app://Consumer1` |
| `--tls` | TLS disabled |

`--ca-file` must point to a PEM-encoded CA certificate file (`.pem` or `.crt`).
If omitted when TLS is enabled, the system's default CA store is used instead.
Use `--no-verify` to skip peer certificate verification entirely (not recommended
in production).

No configuration files — all settings are passed on the command line.

**Key classes:** `tlcfi::fi::FiConsumerApp`, `tlcfi::fi::FiAppHost`

---

## control_app

A controller application built on `FiControllerApp` and `FiAppHost`. It manages
the full control state machine (NotConfigured → Offline → StartControl →
InControl), subscribes to the Session object, and fires `onControlInControl` /
`onControlEndControl` callbacks when control of an intersection is granted or
ended. All connection retries and logging are handled by `FiAppHost`.

```
control_app [host] [port] [username] [password] [uri]
```

| Argument | Default |
|---|---|
| `host` | `localhost` |
| `port` | `11501` |
| `username` | `Controller1` |
| `password` | `Password1` |
| `uri` | `app://Controller1` |

No configuration files — all settings are passed on the command line.

**Key classes:** `tlcfi::fi::FiControllerApp`, `tlcfi::fi::FiAppHost`

---

## control_app_cif

The full runtime container hosting the bundled actuated controller. This is the
closest example to a real production deployment. It delegates the entire
lifecycle (connect, CIF bridge, control loop, retry, logging) to
`tlcfi::runtime::runContainer()`. The only application-specific code is loading
the intersection configuration before the container initialises.

```
control_app_cif [--config <controller-config-path>] [<runtime-options>]
```

Runtime options (host, port, TLS, serial port, logging) are read from
`tlcfi.json`. The intersection configuration is read from `controller_config.json`
(or the path given with `--config`). Both files are copied to the build output
directory automatically by CMake.

**Key classes:** `tlcfi::runtime::RuntimeApp`, `cif::ActuatedController`

---

## ptp_test_peer.py

A standalone Python tool (no build step, no dependencies beyond CPython) that
**mimics a neighbouring VRI** over the container's PTP transport, so you can
exercise a hosted controller's koppelsignaal coupling locally without a second
real controller. It speaks the raw PTP wire protocol (SYN framing, CCITT CRC,
the BIT0→BIT1→BIT2 status handshake, and multivalent koppelsignaal packing) that
the container passes through as a transparent byte pipe. The protocol is the
ASTRIN PTP standard (2009); the peer is an independent implementation and
contains no vendor source code.

```
python ptp_test_peer.py [--host 127.0.0.1] [--port 7100] [--listen]
                        [--send-addr 101] [--expect-addr 102]
                        [--ks v0,v1,...] [--status 0xNN] [--period S]
```

1. Add a PTP link to the container's `tlcfi.json` and point the controller's PTP
   `PORTNR` at the same `linkId` (any value — it's an engineer-chosen key):

   ```json
   "ptpLinks": [
       { "linkId": 1, "mode": "listen", "peerIp": "127.0.0.1", "tcpPort": 7100 }
   ]
   ```

2. Start the container, then run the peer (it connects as the TCP client):

   ```
   python ptp_test_peer.py --host 127.0.0.1 --port 7100 --ks 1,0,1,0,0,0,0,0,0,0
   ```

The peer sends the given koppelsignaal values as **incoming** signals (IKS) to the
controller and prints the controller's **outgoing** signals (UKS) plus the `OKE`
(good-connection) flag as they arrive. `--send-addr` must equal the controller's
PTP `SRC` (default 101); `--expect-addr` is the controller's `DEST` (default 102).
Use `--listen` when the container link is mode `"connect"` instead. Establishment
takes a few seconds (the controller gates its first send on its wait-timer). See
the module docstring and `--selftest` for the wire-format details, and
`sdk/README.md` for the container-side PTP configuration.

### Configuration files

Both JSON files are used **only** by `control_app_cif`. `consumer_app` and
`control_app` take all settings from the command line.

#### `tlcfi.json` — runtime configuration

Read by `RuntimeApp` at startup.

| Key | Description |
|---|---|
| `host`, `port` | Facilities server address |
| `username`, `password`, `uri` | Registration credentials |
| `tls.enabled` | Enable TLS (default `false`) |
| `tls.caFile` | PEM CA certificate file for server verification |
| `tls.verifyPeer` | Verify server certificate (default `true`) |
| `tls.expectedPeerName` | Expected TLS peer / SNI hostname |
| `cifSerialPort` | TCP port for the CIF IBER/UBER serial bridge (0 = disabled) |
| `cifSerialBindAddress` | Bind address for the CIF serial bridge (default: `127.0.0.1`) |
| `cifLoggingPort` | TCP port for the CIF MON IBER/UBER logging bridge (0 = disabled) |
| `cifLoggingBindAddress` | Bind address for the CIF logging bridge (default: `127.0.0.1`) |
| `cifFileLogDir` | Directory for CIF VLOG file output (`CIF_FILE_UBER`); empty = disabled |
| `cifFileLogSystem` | Filename prefix for CIF VLOG files; defaults to `username` if omitted |
| `jsonLog` | Path for raw JSON-RPC message tracing; empty = disabled |
| `parmStateDir` | Directory for the `CIF_PARM1` persistence file (default `.`; empty = disabled) |
| `parmStateIntervalMs` | Minimum interval between persistence writes in ms (default `1000`, min `50`) |
| `logFile` | Path for a plain-text copy of the application log (no ANSI colour codes); empty = disabled |
| `logLevel` | Minimum log level for **both** console and file output (`trace`\|`debug`\|`info`\|`warning`\|`error`\|`fatal`; default `info`) |
| `consoleLogLevel` | Minimum log level for console output only (overrides `logLevel` for console) |
| `fileLogLevel` | Minimum log level for file output only (overrides `logLevel` for file) |
| `logFileMaxSizeMb` | Max application log file size in MB before rotation (default `1`; `0` = unlimited) |
| `logFileMaxBackups` | Number of application log backup files to keep — `.1` (newest) … `.N` (oldest) (default `5`) |
| `jsonLogMaxSizeMb` | Max JSON-RPC trace file size in MB before rotation (default `1`; `0` = unlimited) |
| `jsonLogMaxBackups` | Number of JSON-RPC trace backup files to keep (default `5`) |
| `controllerOnlyItems` | Array of `{"name": "...", "type": "..."}` objects. Lists CIF items the controller exposes via `getCifItemName` that are absent from the TLC-FI meta data; the bridge skips them during mapping. `type` must be one of `signalGroup`, `output`, `detector`, `input`. |
| `manuallyCoupledItems` | Array of `{"tlcfiName": "...", "cifName": "...", "type": "..."}` objects. Explicitly couples a TLC-FI object to a CIF item **whose names differ** on the two sides (by default coupling is by exact, case-sensitive name identity). A manual couple wins over an identity match, and **both** names must be found on their side (cifName on the controller, tlcfiName in the TLC meta) or the container aborts at startup. If the manual `cifName` shadows a controller item that also carries the manual `tlcfiName` literally, declare that literal item under `controllerOnlyItems`. `type` must be one of `signalGroup`, `output`, `detector`, `input`. |

#### `controller_config.json` — intersection configuration

Read by the actuated controller before the container initialises.

| Key | Description |
|---|---|
| `signalGroups` | Signal group definitions: `id`, `index`, `minGreen`, `maxGreen`, `amber`, `minRed`, `direction`, `priorityMaxGreen` (all timing in tenths of a second), `detectors` (array of detector IDs). All timing fields are optional with built-in defaults. |
| `detectors` | Detector definitions (`id`, `index`) |
| `outputs` | Output definitions (`id`, `index`) |
| `inputs` | Input definitions (`id`, `index`) |
| `conflicts` | Intergreen conflict table (`from`, `to`, `intergreen`) |
