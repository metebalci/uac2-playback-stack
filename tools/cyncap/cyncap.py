#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
"""Headless capture from a Cynthion running the "USB Analyzer" gateware.

Usage:
  cyncap.py --speed high|full|low|auto --out FILE.pcap
            [--duration SECONDS] [--max-packets N] [--events FILE]

Writes pcap (nanosecond timestamps, LINKTYPE_USB_2_0 = 288): one record per
USB packet as the analyzer received it from the PHY (PID, payload, CRC; no
SYNC/EOP). Prints a one-line summary on exit. The exit code says whether
the capture is whole: 0 yes, 2 no stop event (suspected overrun), 3 the
stream did not begin with a CAPTURE_START event, 4 unknown event codes
(EXIT_* below); 1 is a refused start.

Source citations below refer to these trees, at these commits:
  [gw]  https://github.com/greatscottgadgets/cynthion, commit 31929c2,
        cynthion/python/src/gateware/analyzer/
  [pk]  https://github.com/greatscottgadgets/packetry, commit 650b7d4, src/
The cynthion 0.2.5 package ships the same gateware sources
(cynthion/gateware/analyzer/, identical to [gw]).
"""

import argparse
import signal
import struct
import sys
import time

# --- Device identity -------------------------------------------------------
# VID:PID 1d50:615b              [pk] backend/cynthion.rs:42
# Interface class 0xff, subclass 0x10 (analyzer), protocol 1 = protocol
# major version              [pk] backend/cynthion.rs:43-45, 153-169;
#                             [gw] top.py:243-245, cynthion shared/usb.toml
# Bulk IN endpoint 0x81, wMaxPacketSize 512   [gw] top.py:56-58, 247-249
VID, PID = 0x1D50, 0x615B
IF_CLASS, IF_SUBCLASS, IF_PROTOCOL = 0xFF, 0x10, 0x01
EP_IN = 0x81

# --- Vendor requests (type vendor, recipient interface, wIndex = interface)
# [gw] top.py:78-83 (codes), 139-150 (vendor + interface recipient);
# [pk] backend/cynthion.rs:408-425 (write: value in wValue, no data stage),
# 445-464 (read: 1-byte data stage).
REQ_GET_STATE = 0
REQ_SET_STATE = 1
REQ_GET_SPEEDS = 2
REQ_SET_TEST_CONFIG = 3      # not used: built-in test device on AUX
REQ_GET_MINOR_VERSION = 4    # [gw] top.py:60-62, 200-202 (MINOR_VERSION = 1)

# --- State register bits   [gw] top.py:87-110; [pk] backend/cynthion.rs:50-60
# bit 0 enable capture; bits 2:1 speed; bits 3-6 VBUS routing; bit 7 power
# control enable. Writing the register (any value) also resets the event and
# speed detectors ([gw] top.py:284-287, 342).
STATE_ENABLE = 0x01
STATE_SPEED_SHIFT = 1
STATE_SPEED_MASK = 0x06
# Speed codes = LUNA USBSpeed (HIGH 0, FULL 1, LOW 2) and AUTO 3:
# [gw] speeds.py:11-15, luna gateware/usb/usb2/__init__.py USBSpeed;
# [pk] usb.rs:61-67. (The comment at [gw] top.py:91-92 saying LS = 0b11 is
# stale; the code uses the USBSpeed values.)
SPEED_CODES = {"high": 0, "full": 1, "low": 2, "auto": 3}
# GET_SPEEDS bitmap  [gw] top.py:113-117, 183-194 (auto only on r0.6+)
SPEED_MASKS = {"auto": 0x01, "low": 0x02, "full": 0x04, "high": 0x08}

# --- Event codes   [gw] events.py:10-52; [pk] event.rs:151-185
EVENT_NAMES = {
    0: "NONE",                    # timestamp wrap marker, [gw] analyzer.py:244-249
    1: "CAPTURE_STOP_NORMAL", 2: "CAPTURE_STOP_FULL", 3: "CAPTURE_STOP_ERROR",
    4: "CAPTURE_START_HIGH", 5: "CAPTURE_START_FULL", 6: "CAPTURE_START_LOW",
    7: "CAPTURE_START_AUTO",
    8: "SPEED_DETECT_HIGH", 9: "SPEED_DETECT_FULL", 10: "SPEED_DETECT_LOW",
    11: "SPEED_DETECT_AUTO",
    12: "LINESTATE_SE0", 13: "LINESTATE_CHIRP_J", 14: "LINESTATE_CHIRP_K",
    15: "LINESTATE_CHIRP_SE1", 16: "LINESTATE_LS_J", 17: "LINESTATE_LS_K",
    18: "LINESTATE_FS_J", 19: "LINESTATE_FS_K", 20: "LINESTATE_SE1",
    21: "VBUS_INVALID", 22: "VBUS_VALID", 23: "LS_ATTACH", 24: "FS_ATTACH",
    25: "BUS_RESET", 26: "DEVICE_CHIRP_VALID", 27: "HOST_CHIRP_VALID",
    28: "SUSPEND", 29: "RESUME", 30: "LS_KEEPALIVE",
}
EV_NONE, EV_STOP_NORMAL = 0, 1
EV_STARTS = (4, 5, 6, 7)          # CAPTURE_START_<speed>

# Exit codes: the capture is whole (0); a refused start, with a message
# (1, SystemExit); no stop event, a suspected overrun (2); the stream did not
# begin with a CAPTURE_START event (3); unknown event codes (4). With
# several, the lowest of 3, 4 and 2 in that order wins: 3 and 4 say the
# records cannot be trusted.
EXIT_OK, EXIT_OVERRUN, EXIT_NO_START, EXIT_UNKNOWN_EVENT = 0, 2, 3, 4

LINKTYPE_USB_2_0 = 288
PCAP_MAGIC_NS = 0xA1B23C4D        # nanosecond pcap, as Packetry saves ([pk] file.rs:185-191)
SNAPLEN = 0xFFFF                  # [pk] backend/cynthion.rs:210


def clk_to_ns(clk):
    """60 MHz analyzer clock cycles to ns, rounding down.

    The timestamp counter runs in the 60 MHz 'usb' domain ([gw]
    analyzer.py:180-182); conversion as [pk] backend/cynthion.rs:116-122.
    """
    q, r = divmod(clk, 3)
    return q * 50 + (0, 16, 33)[r]


class StreamParser:
    """Splits the bulk byte stream into packet and event records.

    Format ([gw] analyzer.py:57-63, 300-366; [pk] backend/cynthion.rs:491-565):
    the stream is 16-bit words sent MSB first ([gw] fifo.py:141-152, top.py:471).
    - Packet: be16 length, be16 timestamp delta, length bytes (PID..CRC as the
      PHY delivered them), one pad byte if length is odd (words are 16 bit,
      [gw] analyzer.py:300-324).
    - Event: 0xFF, event code, be16 timestamp delta ([gw] analyzer.py:334-346).
      A packet cannot start with 0xFF: lengths are at most 1027
      ([gw] analyzer.py:65-66).
    The delta counts 60 MHz cycles since the previous record: the counter is
    cleared at each packet start and set to 1 after each event ([gw]
    analyzer.py:222-243); a NONE event is inserted before it wraps ([gw]
    analyzer.py:244-249). The running sum of deltas is the capture time.
    Yields ("packet", ts_ns, bytes) and ("event", ts_ns, code).
    """

    def __init__(self):
        self.buf = bytearray()
        self.total_clk = 0

    def pending(self):
        return len(self.buf)

    def feed(self, data):
        self.buf += data
        buf = self.buf
        pos, n = 0, len(buf)
        out = []
        while n - pos >= 4:
            delta = (buf[pos + 2] << 8) | buf[pos + 3]
            if buf[pos] == 0xFF:
                self.total_clk += delta
                out.append(("event", clk_to_ns(self.total_clk), buf[pos + 1]))
                pos += 4
                continue
            length = (buf[pos] << 8) | buf[pos + 1]
            need = 4 + length + (length & 1)
            if n - pos < need:
                break
            self.total_clk += delta
            out.append(("packet", clk_to_ns(self.total_clk),
                        bytes(buf[pos + 4:pos + 4 + length])))
            pos += need
        del buf[:pos]
        return out


class PcapWriter:
    """Classic pcap, nanosecond resolution, LINKTYPE_USB_2_0."""

    def __init__(self, f):
        self.f = f
        f.write(struct.pack("<IHHiIII", PCAP_MAGIC_NS, 2, 4, 0, 0, SNAPLEN,
                            LINKTYPE_USB_2_0))

    def write(self, ts_ns, data):
        sec, ns = divmod(ts_ns, 1_000_000_000)
        self.f.write(struct.pack("<IIII", sec, ns, len(data), len(data)))
        self.f.write(data)


class Capture:
    """Turns stream bytes into pcap records and keeps the counts."""

    def __init__(self, writer, base_ns, max_packets=None, events_file=None):
        self.parser = StreamParser()
        self.writer = writer
        self.base_ns = base_ns
        self.max_packets = max_packets
        self.events_file = events_file
        self.packets = 0
        self.bytes = 0
        self.stream_bytes = 0
        self.ignored_packets = 0      # arrived after --max-packets was reached
        self.events = {}
        self.stop_seen = False
        self.first = None             # the stream's first record: "packet" or an event name
        self.unknown_events = 0       # events with a code outside EVENT_NAMES
        self.last_ts = 0

    def full(self):
        return self.max_packets is not None and self.packets >= self.max_packets

    def consume(self, data):
        self.stream_bytes += len(data)
        for kind, ts, val in self.parser.feed(data):
            self.last_ts = ts
            if self.first is None:
                self.first = (kind if kind == "packet" else
                              EVENT_NAMES.get(val, f"EVENT_{val}"))
            if kind == "packet":
                if self.full():
                    self.ignored_packets += 1
                    continue
                self.writer.write(self.base_ns + ts, val)
                self.packets += 1
                self.bytes += len(val)
            else:
                if val == EV_NONE:
                    continue
                name = EVENT_NAMES.get(val, f"EVENT_{val}")
                self.events[name] = self.events.get(name, 0) + 1
                if val not in EVENT_NAMES:
                    self.unknown_events += 1
                if val == EV_STOP_NORMAL:
                    self.stop_seen = True
                if self.events_file:
                    self.events_file.write(f"{ts / 1e9:.9f} {val} {name}\n")

    def start_seen(self):
        """The stream began with a CAPTURE_START event: the analyzer writes
        it first, when the enable reaches it ([gw] analyzer.py:196-209).
        Anything else first means the stream began inside a record."""
        return self.first in (EVENT_NAMES[c] for c in EV_STARTS)

    def exit_code(self):
        if not self.start_seen():
            return EXIT_NO_START
        if self.unknown_events:
            return EXIT_UNKNOWN_EVENT
        return EXIT_OK if self.stop_seen else EXIT_OVERRUN

    def summary_line(self, wall_s):
        ev = " ".join(f"{k}={v}" for k, v in self.events.items()) or "-"
        # A stop request in the OVERRUN state returns to AWAIT_START without
        # writing CAPTURE_STOP_NORMAL ([gw] analyzer.py:268-271, 289-294);
        # from every other state the stop event is written ([gw]
        # analyzer.py:213-221). The gateware emits no overrun event, so a
        # missing stop event is the only overrun sign the host gets.
        overrun = ("no" if self.stop_seen else
                   f"suspected(no stop event; data ends at {self.last_ts / 1e9:.6f}s)")
        start = ("ok" if self.start_seen() else
                 f"missing(first record: {self.first})" if self.first else
                 "missing(no record)")
        return (f"cyncap: packets={self.packets} bytes={self.bytes} "
                f"duration={wall_s:.3f}s analyzer_time={self.last_ts / 1e9:.6f}s "
                f"stream_bytes={self.stream_bytes} "
                f"ignored_after_max={self.ignored_packets} "
                f"leftover_bytes={self.parser.pending()} "
                f"overrun={overrun} start={start} "
                f"unknown_events={self.unknown_events} events: {ev}")


# --- USB side (python-libusb1) ---------------------------------------------

NUM_TRANSFERS = 16        # Packetry uses 4 x 16 KiB ([pk] backend/cynthion.rs:47-48)
TRANSFER_LEN = 0x4000


def find_interface(handle):
    dev = handle.getDevice()
    for cfg in dev.iterConfigurations():
        for intf in cfg:
            for alt in intf:
                if alt.getClass() == IF_CLASS and alt.getSubClass() == IF_SUBCLASS:
                    return alt.getNumber(), alt.getAlternateSetting(), alt.getProtocol()
    raise SystemExit("cyncap: no analyzer interface (class ff/10); is the "
                     "USB Analyzer bitstream running?")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--speed", required=True, choices=SPEED_CODES)
    ap.add_argument("--out", required=True)
    ap.add_argument("--duration", type=float)
    ap.add_argument("--max-packets", type=int)
    ap.add_argument("--events", help="also write analyzer events (text) here")
    ap.add_argument("--test-delay-read", type=float, default=0.0, metavar="SECONDS",
                    help="test only: start reading the stream this late, to "
                         "provoke an overrun")
    args = ap.parse_args(argv)

    try:
        import usb1
    except ImportError:
        raise SystemExit("cyncap: needs python-libusb1 (module usb1): "
                         "pip install libusb1") from None

    stop_req = []

    def on_signal(signum, _frame):
        stop_req.append(signal.Signals(signum).name)
    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    ctx = usb1.USBContext()
    ctx.open()
    handle = ctx.openByVendorIDAndProductID(VID, PID)
    if handle is None:
        raise SystemExit("cyncap: no Cynthion analyzer (1d50:615b) found")
    ifnum, altnum, proto = find_interface(handle)
    if proto != IF_PROTOCOL:
        raise SystemExit(f"cyncap: analyzer protocol v{proto}, this tool knows v{IF_PROTOCOL}")
    handle.setAutoDetachKernelDriver(True)
    handle.claimInterface(ifnum)
    if altnum:
        handle.setInterfaceAltSetting(ifnum, altnum)

    rtype_in = usb1.TYPE_VENDOR | usb1.RECIPIENT_INTERFACE | usb1.ENDPOINT_IN
    rtype_out = usb1.TYPE_VENDOR | usb1.RECIPIENT_INTERFACE | usb1.ENDPOINT_OUT

    def read_byte(req):
        d = handle.controlRead(rtype_in, req, 0, ifnum, 64, timeout=1000)
        if len(d) != 1:
            raise SystemExit(f"cyncap: request {req} returned {len(d)} bytes")
        return d[0]

    control_errors = []

    def write_state(v):
        # Under heavy bulk IN traffic the control transfer has been seen to
        # fail with LIBUSB_ERROR_IO although the gateware took the value
        # (GET_STATE read back the new value). Read back and retry.
        for _ in range(3):
            try:
                handle.controlWrite(rtype_out, REQ_SET_STATE, v, ifnum, b"", timeout=1000)
                return
            except usb1.USBError as e:
                control_errors.append(f"SET_STATE:{e.__class__.__name__}")
            try:
                if read_byte(REQ_GET_STATE) == v:
                    return
            except usb1.USBError as e:
                control_errors.append(f"GET_STATE:{e.__class__.__name__}")
        raise SystemExit(f"cyncap: cannot set state 0x{v:02x}: {control_errors}")

    state = read_byte(REQ_GET_STATE)
    speeds = read_byte(REQ_GET_SPEEDS)
    try:
        minor = read_byte(REQ_GET_MINOR_VERSION)
    except usb1.USBError:
        minor = 0                 # [pk] backend/cynthion.rs:196-198
    if not speeds & SPEED_MASKS[args.speed]:
        raise SystemExit(f"cyncap: speed {args.speed} not supported (GET_SPEEDS=0x{speeds:02x})")

    # Keep the VBUS bits (3-7) as found: with bit 7 clear the gateware passes
    # TARGET-C VBUS to TARGET-A ([gw] top.py:296-310), the power-on default.
    base_state = state & ~(STATE_ENABLE | STATE_SPEED_MASK) & 0xFF
    if state & STATE_ENABLE:
        # A previous run left capture enabled; stop it first so the next
        # enable passes AWAIT_START, which resets the buffers and makes the
        # endpoint discard old data ([gw] analyzer.py:189-209, top.py:466-485).
        write_state(base_state | (state & STATE_SPEED_MASK))
        time.sleep(0.05)
        while True:
            try:
                if not handle.bulkRead(EP_IN, 1 << 16, timeout=100):
                    break
            except usb1.USBErrorTimeout:
                break

    out = open(args.out, "wb")
    events_file = open(args.events, "w") if args.events else None
    writer = PcapWriter(out)
    run_state = base_state | (SPEED_CODES[args.speed] << STATE_SPEED_SHIFT) | STATE_ENABLE

    cap = None
    transfers = []
    stopping = [False]
    errors = []
    wall = 0.0

    def on_done(tr):
        st = tr.getStatus()
        if st == usb1.TRANSFER_CANCELLED:
            return
        if st in (usb1.TRANSFER_COMPLETED, usb1.TRANSFER_TIMED_OUT):
            n = tr.getActualLength()
            if n:
                cap.consume(tr.getBuffer()[:n])
        else:
            errors.append(st)
            return
        if not (stopping[0] and cap.stop_seen):
            tr.submit()

    def pump():
        # A signal interrupts libusb's poll(); that is not an error here.
        try:
            ctx.handleEventsTimeout(0.05)
        except usb1.USBErrorInterrupted:
            pass

    for _ in range(NUM_TRANSFERS):
        tr = handle.getTransfer()
        tr.setBulk(EP_IN, TRANSFER_LEN, callback=on_done, timeout=200)
        transfers.append(tr)

    # Analyzer time 0 is the CAPTURE_START event, written when the enable
    # reaches the gateware ([gw] analyzer.py:196-209); take host time there.
    base_ns = time.time_ns()
    cap = Capture(writer, base_ns, args.max_packets, events_file)
    write_state(run_state)
    t0 = time.monotonic()
    if args.test_delay_read:
        time.sleep(args.test_delay_read)
    for tr in transfers:
        tr.submit()

    reason = None
    try:
        while reason is None:
            pump()
            if stop_req:
                reason = stop_req[0]
            elif args.duration is not None and time.monotonic() - t0 >= args.duration:
                reason = "duration"
            elif cap.full():
                reason = "max-packets"
            elif errors:
                reason = f"transfer-error({errors[0]})"
        # Stop: clear enable, then drain until the stop event arrives; the
        # endpoint is flushed once the analyzer is idle and disabled
        # ([gw] top.py:481-482).
        stopping[0] = True
        wall = time.monotonic() - t0
        write_state(run_state & ~STATE_ENABLE)
        deadline = time.monotonic() + 3.0
        while not cap.stop_seen and time.monotonic() < deadline:
            pump()
    finally:
        for tr in transfers:
            if tr.isSubmitted():
                try:
                    tr.cancel()
                except usb1.USBError:
                    pass
        end = time.monotonic() + 1.0
        while any(tr.isSubmitted() for tr in transfers) and time.monotonic() < end:
            pump()
        out.close()
        if events_file:
            events_file.close()
    print(cap.summary_line(wall) + f" stop={reason} speed={args.speed} "
          f"gateware=v{proto}.{minor} control_errors={len(control_errors)} "
          f"file={args.out}")
    if control_errors:
        print("cyncap: control errors: " + " ".join(control_errors), file=sys.stderr)
    handle.releaseInterface(ifnum)
    handle.close()
    ctx.close()
    return cap.exit_code()


if __name__ == "__main__":
    sys.exit(main())
