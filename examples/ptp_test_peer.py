#!/usr/bin/env python3
"""
PTP test peer — mimic a neighbouring VRI for local PTP koppelsignaal testing.

This tool speaks the Point-to-Point (PTP) wire protocol used between traffic
controllers (ASTRIN PTP standard, 2009). It connects to the container's
tlcfi_ptp TCP transport and lets you drive a hosted controller's incoming
koppelsignalen (IKS) and watch the koppelsignalen it sends out (UKS) — without a
second real controller.

The container is a transparent byte pipe, so the bytes on this TCP socket are
the raw PTP frames produced and consumed by the controller's PTP stack.

This is an independent implementation of the protocol. It contains no vendor
source code. The wire-level details (the exact CRC variant, byte stuffing and
the koppelsignaal packing) are pinned by the self-test vectors in this file;
run `--selftest` to check them.

The examples below are oriented on the TINT1 "talking traffic" example
controller.

------------------------------------------------------------------------------
Setup on the container side (tlcfi.json), matching the defaults below:
    "ptpLinks": [
        { "linkId": 1, "mode": "listen", "peerIp": "127.0.0.1", "tcpPort": 7100 }
    ]
  and set the controller's PTP PORTNR (e.g. PTP_TINT2.PORTNR in <TINT>ptp.c) to
  that same linkId (any value, here: 1). peerIp must be a literal IP
  ("127.0.0.1"), not "localhost". With mode "listen" the container waits for us
  to connect (this script's default). Use mode "connect" + this script's
  --listen for the reverse.

Examples:
    # container link is mode "listen" on 7100 -> we connect (default),
    # then drive incoming signals live from the keyboard:
    python ptp_test_peer.py --host 127.0.0.1 --port 7100

    # seed ten incoming koppelsignalen:
    python ptp_test_peer.py --ks 1,0,1,0,0,0,0,0,0,0

    # container link is mode "connect" -> we listen instead:
    python ptp_test_peer.py --listen --port 7100

    # verify the codec against the protocol test vectors, without a network:
    python ptp_test_peer.py --selftest

Interactive keys (while running):
    1 2 3 4 5 6 7 8 9 0   raise IKS signal 1..10 by 1  (IKS -> controller)
    q w e r t y u i o p   lower IKS signal 1..10 by 1  (the row right below)
    space                 reset all IKS signals to 0
    Esc / Ctrl+C          quit

A live panel shows the IKS we send, the UKS the controller sends back, the
incoming status byte, the OKE flag and a received-frame counter. The counter
also ticks on the controller's alive frames, so you can see the link is live
before any UKS value changes.

Addresses default to the TINT1 pairing (controller SRC=101, DEST=102): we send
with dest=101 (so the controller accepts us) and expect incoming dest=102.
Override with --send-addr / --expect-addr for other controllers.
"""

import argparse
import os
import socket
import sys
import threading
import time

# --- Frame constants ---
SYNC = 0x16
DATA = 0x01                 # "SOH": data frame, or alive frame when empty
ACK = 0x06
NAK = 0x15
CONTROL_TYPES = frozenset((DATA, ACK, NAK))

# --- Status bits ---
LINK_BIT = 0x01
DATA_BIT = 0x02
OKE_BIT = 0x04
AUX_BIT = 0x08

# --- CRC parameters ---
CRC_POLY = 0x8408           # reflected 0x1021


def crc16_byte(state, byte):
    """Advance the PTP CRC-16 state by one byte.

    This is CRC-16/Kermit (reflected polynomial 0x8408, initial value 0, no
    final XOR) with one variant: the 16-bit state is treated as a signed
    quantity, so the right shift is arithmetic and bit 15 is duplicated. A
    logical shift yields different values, which deployed controllers reject.
    """
    state ^= byte
    for _ in range(8):
        feedback = state & 1
        state = (state >> 1) | (state & 0x8000)     # arithmetic shift
        if feedback:
            state ^= CRC_POLY
    return state & 0xFFFF


def crc16(data):
    """CRC-16 over a byte sequence, starting from 0."""
    state = 0
    for byte in data:
        state = crc16_byte(state, byte)
    return state


def _escape(body):
    """Double every 0x16 in `body` (the bytes after the leading SYNC)."""
    out = bytearray()
    for byte in body:
        out.append(byte)
        if byte == SYNC:
            out.append(SYNC)
    return bytes(out)


def encode_frame(kind, destination, payload=b""):
    """Encode one PTP frame, including the leading SYNC and byte stuffing."""
    if len(payload) > 0xFF:
        raise ValueError("payload must be at most 255 bytes")
    header = bytes((kind & 0xFF, destination & 0xFF, len(payload)))
    body = header + crc16(header).to_bytes(2, "little")
    if payload:
        body += bytes(payload) + crc16(payload).to_bytes(2, "little")
    return bytes((SYNC,)) + _escape(body)


def encode_signals(status, values):
    """Pack a status byte and N multivalent koppelsignalen (2 bytes each, big-endian)."""
    out = bytearray((status & 0xFF,))
    for value in values:
        out.append((value >> 8) & 0xFF)
        out.append(value & 0xFF)
    return bytes(out)


def decode_signals(payload):
    """Unpack (status, values) from a koppelsignaal payload."""
    if not payload:
        return 0, []
    status = payload[0]
    values = [
        (payload[i] << 8) | payload[i + 1]
        for i in range(1, len(payload) - 1, 2)
    ]
    return status, values


class FrameReader:
    """Decode PTP frames from a blocking socket byte stream."""

    def __init__(self, sock, on_raw=None):
        self._sock = sock
        self._buffer = bytearray()
        self._offset = 0
        self._on_raw = on_raw       # optional callback(bytes) per raw recv chunk

    def _raw_byte(self):
        """Return the next raw byte, blocking; None on clean EOF."""
        while self._offset >= len(self._buffer):
            chunk = self._sock.recv(4096)
            if not chunk:
                return None
            if self._on_raw:
                self._on_raw(chunk)
            self._buffer = bytearray(chunk)
            self._offset = 0
        value = self._buffer[self._offset]
        self._offset += 1
        return value

    def _byte_after_sync(self):
        """Return one un-stuffed byte; None on clean EOF.

        The leading SYNC is sent once; every later 0x16 is sent doubled.
        """
        value = self._raw_byte()
        if value is None:
            return None
        if value == SYNC:
            if self._raw_byte() is None:     # consume the duplicate
                return None
        return value

    def read_frame(self):
        """Return (type, destination, payload) for the next valid frame, or None.

        Frames with a bad CRC are discarded and the reader resynchronises on
        the next SYNC.
        """
        while True:
            # --- find a SYNC followed by a control byte ---
            value = self._raw_byte()
            if value is None:
                return None
            if value != SYNC:
                continue
            kind = self._raw_byte()
            if kind is None:
                return None
            if kind == SYNC:            # doubled SYNC before a frame start
                continue
            if kind not in CONTROL_TYPES:
                continue

            # --- header remainder: dest, len, crc1 ---
            destination = self._byte_after_sync()
            length = self._byte_after_sync()
            crc_lo = self._byte_after_sync()
            crc_hi = self._byte_after_sync()
            if None in (destination, length, crc_lo, crc_hi):
                return None
            if crc16(bytes((kind, destination, length))) != (crc_lo | (crc_hi << 8)):
                continue                # bad header CRC -> resync

            if kind != DATA or length == 0:
                return kind, destination, b""   # ACK/NAK or alive DATA

            # --- payload and crc2 ---
            payload = bytearray()
            for _ in range(length):
                value = self._byte_after_sync()
                if value is None:
                    return None
                payload.append(value)
            crc_lo = self._byte_after_sync()
            crc_hi = self._byte_after_sync()
            if None in (crc_lo, crc_hi):
                return None
            if crc16(payload) != (crc_lo | (crc_hi << 8)):
                continue                # bad payload CRC -> resync
            return DATA, destination, bytes(payload)


def open_socket(host, port, listen):
    if listen:
        server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((host, port))
        server.listen(1)
        print(f"[peer] listening on {host}:{port} — waiting for the container to connect...")
        connection, address = server.accept()
        print(f"[peer] container connected from {address[0]}:{address[1]}")
        server.close()
        return connection
    connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print(f"[peer] connecting to {host}:{port} ...")
    connection.connect((host, port))
    print("[peer] connected")
    return connection


SIGNAL_COUNT = 10           # koppelsignalen per direction (TINT1 all-multivalent)
INC_KEYS = "1234567890"     # top number row: +1 to signals 1..10  (IKS -> controller)
DEC_KEYS = "qwertyuiop"     # row right below: -1 to signals 1..10


class MemoryStream:
    """A minimal recv() source, used by the self-test."""

    def __init__(self, data):
        self._data = data
        self._offset = 0

    def recv(self, size):
        chunk = self._data[self._offset:self._offset + size]
        self._offset += len(chunk)
        return chunk


def selftest():
    """Check the codec against the wire vectors used to validate this implementation."""
    # CRC vectors.
    assert crc16(bytes((0x01, 0x66, 0x15))) == 0x4860
    assert crc16(bytes([0x08] + [0] * 20)) == 0xBC06
    assert crc16(b"") == 0

    # Exact wire bytes.
    assert encode_frame(ACK, 101).hex() == "160665005643"
    assert encode_frame(DATA, 101).hex() == "160165000691"
    assert encode_frame(
        DATA, 101, encode_signals(0x0B, [1] + [0] * 9)
    ).hex() == "160165152a500b000100000000000000000000000000000000000053a2"
    assert encode_frame(
        DATA, SYNC, bytes((SYNC, SYNC, 0x00))
    ).hex() == "1601161603b3cc161616160002d2"

    # Round-trip through the decoder, including a frame that exercises stuffing.
    examples = [
        (ACK, 101, b""),
        (DATA, 101, b""),
        (DATA, 101, encode_signals(0x0B, [1] + [0] * 9)),
        (DATA, SYNC, bytes((SYNC, SYNC, 0x00))),
    ]
    for kind, destination, payload in examples:
        encoded = encode_frame(kind, destination, payload)
        reader = FrameReader(MemoryStream(encoded))
        assert reader.read_frame() == (kind, destination, payload)
        assert reader.read_frame() is None

    print("selftest: OK")


def make_getch():
    """Return a blocking, single-keystroke reader (Windows msvcrt or POSIX termios).

    Returns "" for function/arrow keys (their scancode is swallowed) so the
    caller only ever sees printable keys plus Esc (0x1B) and Ctrl+C (0x03).
    """
    try:
        import msvcrt

        def getch():
            char = msvcrt.getch()
            if char in (b"\x00", b"\xe0"):   # arrow/function key prefix: drop scancode
                msvcrt.getch()
                return ""
            return char.decode("latin-1")
        return getch
    except ImportError:  # POSIX fallback (project is Windows-only, but keep it portable)
        import termios
        import tty

        def getch():
            fd = sys.stdin.fileno()
            saved = termios.tcgetattr(fd)
            try:
                tty.setraw(fd)
                return sys.stdin.read(1)
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, saved)
        return getch


class PeerPanel:
    """A terminal panel redrawn in place: IKS sent, UKS received, link state.

    All mutable state is guarded by `lock`. render() rewrites the panel where
    it stands; log() prints a scrolling message above the panel.
    """

    def __init__(self, count):
        self.count = count
        self.lock = threading.Lock()
        self.iks = [0] * count       # what we send to the controller
        self.uks = [0] * count       # what the controller last sent us
        self.status_in = 0           # last incoming status byte
        self.oke = False             # controller reports a good link (bit 2)
        self.rx = 0                  # total DATA frames received (incl. alive)
        self._drawn = 0              # panel height last drawn (for cursor-up)

    def _lines(self):
        header = " ".join(f"{i + 1:>3}" for i in range(self.count))
        iks = " ".join(f"{v:>3}" for v in self.iks)
        uks = " ".join(f"{v:>3}" for v in self.uks)
        link = "OKE" if self.oke else "..."
        return [
            f"PTP peer   link={link}   in-status=0x{self.status_in:02X}   rx-frames={self.rx}",
            f"  signal   {header}",
            f"  IKS  ->  {iks}   (we send to controller)",
            f"  UKS  <-  {uks}   (controller sends back)",
            f"  keys: {INC_KEYS} +1  |  {DEC_KEYS} -1  |  space reset  |  Esc quit",
        ]

    def render(self):
        with self.lock:
            lines = self._lines()
            out = f"\x1b[{self._drawn}A" if self._drawn else ""  # up to panel top
            out += "\x1b[J"                                       # clear to end of screen
            out += "\n".join(lines) + "\n"
            sys.stdout.write(out)
            sys.stdout.flush()
            self._drawn = len(lines)

    def log(self, message):
        with self.lock:
            out = f"\x1b[{self._drawn}A\x1b[J" if self._drawn else ""  # wipe the panel
            out += message + "\n"
            sys.stdout.write(out)
            sys.stdout.flush()
            self._drawn = 0
        self.render()  # redraw the panel below the message


def _fit(values, count):
    """Pad/trim a value list to exactly `count` entries."""
    return (list(values) + [0] * count)[:count]


def main():
    parser = argparse.ArgumentParser(
        description="PTP test peer for OpenItsContainer local koppelsignaal testing.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--host", default="127.0.0.1",
                        help="container IP to connect to (or bind IP with --listen)")
    parser.add_argument("--port", type=int, default=7100, help="TCP port (tcpPort in ptpLinks)")
    parser.add_argument("--listen", action="store_true",
                        help="listen instead of connect (use when the container link is mode 'connect')")
    parser.add_argument("--send-addr", type=int, default=101,
                        help="dest byte we put in outgoing frames (must equal the controller's SRC)")
    parser.add_argument("--expect-addr", type=int, default=102,
                        help="dest byte we expect in incoming frames (the controller's DEST); informational")
    parser.add_argument("--ks", default="1,0,0,0,0,0,0,0,0,0",
                        help="initial IKS values; adjustable live from the keyboard")
    parser.add_argument("--status", type=lambda s: int(s, 0), default=LINK_BIT | DATA_BIT | AUX_BIT,
                        help="handshake status byte (default 0x0B = LINK_BIT|DATA_BIT|AUX_BIT)")
    parser.add_argument("--period", type=float, default=1.0,
                        help="seconds between outgoing data frames")
    parser.add_argument("--max", type=int, default=15,
                        help="ceiling an IKS signal can be raised to interactively")
    parser.add_argument("--debug", action="store_true",
                        help="hex-dump every raw byte sent/received (diagnose a silent link)")
    parser.add_argument("--selftest", action="store_true",
                        help="run the protocol test vectors and exit")
    args = parser.parse_args()

    if args.selftest:
        selftest()
        return 0

    if os.name == "nt":
        os.system("")  # enable ANSI/VT escape processing on the Windows console

    sock = open_socket(args.host, args.port, args.listen)
    panel_ref = {}  # late-bound so the raw tap can log through the panel
    on_raw = None
    if args.debug:
        on_raw = lambda chunk: panel_ref["panel"].log(f"[raw <- {len(chunk):>3}B] {chunk.hex(' ')}")
    reader = FrameReader(sock, on_raw=on_raw)
    panel = PeerPanel(SIGNAL_COUNT)
    panel_ref["panel"] = panel
    panel.iks = _fit([int(x, 0) for x in args.ks.split(",") if x.strip() != ""], SIGNAL_COUNT)

    send_lock = threading.Lock()
    stop = threading.Event()

    def send(frame):
        with send_lock:
            try:
                sock.sendall(frame)
                if args.debug:
                    panel.log(f"[raw -> {len(frame):>3}B] {frame.hex(' ')}")
            except OSError:
                stop.set()

    def send_iks():
        with panel.lock:
            snapshot = list(panel.iks)
        send(encode_frame(DATA, args.send_addr, encode_signals(args.status, snapshot)))

    def rx_loop():
        while not stop.is_set():
            try:
                frame = reader.read_frame()
            except OSError:
                break
            if frame is None:
                panel.log("[peer] connection closed by container")
                stop.set()
                break
            kind, destination, payload = frame
            if kind == DATA:
                # ACK every data/alive frame so the controller keeps/gains the link.
                send(encode_frame(ACK, args.send_addr))
                with panel.lock:
                    panel.rx += 1
                    if payload:
                        status, values = decode_signals(payload)
                        panel.status_in = status
                        panel.oke = bool(status & OKE_BIT)
                        if values:
                            panel.uks = _fit(values, SIGNAL_COUNT)
                panel.render()
            elif kind == NAK:
                panel.log(f"[peer] <- NAK dest={destination}")

    def kb_loop():
        getch = make_getch()
        while not stop.is_set():
            try:
                char = getch()
            except Exception:
                break
            if char == "":
                continue
            if char in ("\x1b", "\x03"):      # Esc / Ctrl+C
                stop.set()
                break
            changed = False
            with panel.lock:
                if char in INC_KEYS:
                    index = INC_KEYS.index(char)
                    panel.iks[index] = min(args.max, panel.iks[index] + 1)
                    changed = True
                elif char.lower() in DEC_KEYS:
                    index = DEC_KEYS.index(char.lower())
                    panel.iks[index] = max(0, panel.iks[index] - 1)
                    changed = True
                elif char == " ":
                    panel.iks = [0] * SIGNAL_COUNT
                    changed = True
            if changed:
                send_iks()   # push the new IKS immediately, don't wait for the period
                panel.render()

    threading.Thread(target=rx_loop, daemon=True).start()
    threading.Thread(target=kb_loop, daemon=True).start()

    panel.log(f"[peer] connected. status=0x{args.status:02X}, send-addr={args.send_addr}, "
              f"resend every {args.period}s. Adjust signals with the keys below.")
    try:
        while not stop.is_set():
            send_iks()
            stop.wait(args.period)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        try:
            sock.close()
        except OSError:
            pass
        print("\n[peer] stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
