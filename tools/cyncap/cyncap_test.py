#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
"""Self-test for cyncap.py and cyncap_summary.py; needs no hardware.

Run: python3 -I cyncap_test.py

It needs neither the analyzer nor python-libusb1: the USB side of
cyncap.main() runs against a fake usb1 module (FakeAnalyzer below). Only
the check that Wireshark reads the pcap needs tshark; without it, that
test is skipped.
"""

import contextlib
import io
import os
import signal
import struct
import sys
import tempfile
import types
import unittest
from unittest import mock

# -I ignores PYTHONDONTWRITEBYTECODE; keep __pycache__ out of the tree
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cyncap            # noqa: E402
import cyncap_summary    # noqa: E402


def pkt(data, delta):
    """One analyzer packet record: be16 length, be16 delta, data, pad."""
    rec = struct.pack(">HH", len(data), delta) + bytes(data)
    if len(data) % 2:
        rec += b"\x00"
    return rec


def evt(code, delta):
    return bytes([0xFF, code]) + struct.pack(">H", delta)


def token(pid, addr, ep):
    v = addr | (ep << 7)          # CRC5 left zero; the summary ignores it
    return bytes([pid]) + struct.pack("<H", v)


def sof(frame):
    return bytes([0xA5]) + struct.pack("<H", frame & 0x7FF)


def data(pid, payload):
    return bytes([pid]) + bytes(payload) + b"\x00\x00"


STREAM = (
    evt(4, 0)                         # CAPTURE_START_HIGH
    + pkt(sof(100), 30)
    + pkt(token(0x2D, 0, 0), 7500)    # SETUP addr 0 ep 0
    + pkt(data(0xC3, b"\x80\x06\x00\x01\x00\x00\x40\x00"), 60)
    + pkt(b"\xD2", 60)                # ACK (odd length: one pad byte)
    + evt(25, 3)                      # BUS_RESET
    + evt(0, 0xFFFF)                  # timestamp wrap marker (NONE)
    + pkt(sof(101), 0)
    + evt(1, 10)                      # CAPTURE_STOP_NORMAL
)


class ParserTest(unittest.TestCase):

    def parse(self, chunks):
        p = cyncap.StreamParser()
        out = []
        for c in chunks:
            out.extend(p.feed(c))
        return p, out

    def test_clk_to_ns_matches_packetry(self):
        self.assertEqual([cyncap.clk_to_ns(c) for c in range(7)],
                         [0, 16, 33, 50, 66, 83, 100])

    def test_whole_stream(self):
        p, recs = self.parse([STREAM])
        kinds = [(r[0], r[2] if r[0] == "event" else len(r[2])) for r in recs]
        self.assertEqual(kinds, [
            ("event", 4), ("packet", 3), ("packet", 3), ("packet", 11),
            ("packet", 1), ("event", 25), ("event", 0), ("packet", 3),
            ("event", 1)])
        clks = [0, 30, 7530, 7590, 7650, 7653, 7653 + 0xFFFF,
                7653 + 0xFFFF, 7653 + 0xFFFF + 10]
        self.assertEqual([r[1] for r in recs],
                         [cyncap.clk_to_ns(c) for c in clks])
        self.assertEqual(p.pending(), 0)

    def test_every_split_point(self):
        _, whole = self.parse([STREAM])
        for i in range(len(STREAM) + 1):
            _, split = self.parse([STREAM[:i], STREAM[i:]])
            self.assertEqual(split, whole, f"split at {i}")
        _, bytewise = self.parse([STREAM[i:i + 1] for i in range(len(STREAM))])
        self.assertEqual(bytewise, whole)

    def test_zero_length_packet(self):
        _, recs = self.parse([pkt(b"", 5) + pkt(b"\xD2", 1)])
        self.assertEqual([(r[0], r[2]) for r in recs],
                         [("packet", b""), ("packet", b"\xD2")])


class CaptureStatsTest(unittest.TestCase):

    def run_stream(self, stream, max_packets=None):
        buf = io.BytesIO()
        w = cyncap.PcapWriter(buf)
        st = cyncap.Capture(w, base_ns=1_700_000_000_123_456_789,
                            max_packets=max_packets)
        st.consume(stream)
        return buf.getvalue(), st

    def test_stop_event_seen(self):
        _, st = self.run_stream(STREAM)
        self.assertTrue(st.stop_seen)
        self.assertEqual(st.packets, 5)
        self.assertEqual(st.bytes, 3 + 3 + 11 + 1 + 3)
        self.assertEqual(st.events.get("BUS_RESET"), 1)
        self.assertNotIn("NONE", st.events)
        self.assertIn("overrun=no", st.summary_line(1.0))

    def test_missing_stop_event_reports_overrun(self):
        _, st = self.run_stream(STREAM[:-4])
        self.assertFalse(st.stop_seen)
        self.assertEqual(st.exit_code(), 2)
        self.assertIn("overrun=suspected", st.summary_line(1.0))

    def test_start_event_first(self):
        _, st = self.run_stream(STREAM)
        self.assertEqual(st.exit_code(), 0)
        self.assertIn("start=ok unknown_events=0 ", st.summary_line(1.0))

    def test_stream_not_starting_with_start_event(self):
        # The stream begins inside a record, or with another record
        for lead in (pkt(sof(99), 3), evt(25, 3), b""):
            with self.subTest(lead=lead):
                _, st = self.run_stream(lead + STREAM[4:])
                self.assertEqual(st.exit_code(), 3)
                self.assertIn("start=missing", st.summary_line(1.0))
        _, st = self.run_stream(b"")
        self.assertEqual(st.exit_code(), 3)
        self.assertIn("start=missing(no record)", st.summary_line(1.0))

    def test_unknown_event_codes(self):
        _, st = self.run_stream(STREAM[:-4] + evt(31, 2) + evt(0xC7, 2) + STREAM[-4:])
        self.assertEqual(st.exit_code(), 4)
        self.assertIn("unknown_events=2 ", st.summary_line(1.0))
        self.assertIn("EVENT_31=1 EVENT_199=1", st.summary_line(1.0))
        # A missing start outranks them, and both outrank a missing stop
        _, st = self.run_stream(STREAM[4:-4] + evt(31, 2))
        self.assertEqual(st.exit_code(), 3)
        _, st = self.run_stream(STREAM[:-4] + evt(31, 2))
        self.assertEqual(st.exit_code(), 4)

    def test_stop_full_and_error_events_reported(self):
        _, st = self.run_stream(evt(4, 0) + evt(2, 5))
        self.assertIn("CAPTURE_STOP_FULL=1", st.summary_line(1.0))

    def test_max_packets(self):
        raw, st = self.run_stream(STREAM, max_packets=2)
        self.assertEqual(st.packets, 2)
        recs = list(cyncap_summary.read_pcap(io.BytesIO(raw)))
        self.assertEqual(len(recs), 2)

    def test_pcap_header_and_timestamps(self):
        raw, _ = self.run_stream(STREAM)
        magic, vmaj, vmin, _, _, snap, link = struct.unpack("<IHHiIII", raw[:24])
        self.assertEqual((magic, vmaj, vmin, link), (0xA1B23C4D, 2, 4, 288))
        self.assertGreaterEqual(snap, 1027)
        recs = list(cyncap_summary.read_pcap(io.BytesIO(raw)))
        self.assertEqual(recs[0][0], 1_700_000_000_123_456_789 + cyncap.clk_to_ns(30))
        self.assertEqual(recs[1][0] - recs[0][0],
                         cyncap.clk_to_ns(7530) - cyncap.clk_to_ns(30))
        self.assertEqual(recs[2][1], data(0xC3, b"\x80\x06\x00\x01\x00\x00\x40\x00"))


class SummaryTest(unittest.TestCase):

    def capture(self, pkts):
        buf = io.BytesIO()
        w = cyncap.PcapWriter(buf)
        for i, p in enumerate(pkts):
            w.write(1_000_000_000 + i * 125_000, p)
        buf.seek(0)
        return list(cyncap_summary.read_pcap(buf))

    def test_transactions(self):
        recs = self.capture([
            sof(5), token(0x2D, 0, 0), data(0xC3, b"\x00\x09\x01\x00\x00\x00\x00\x00"), b"\xD2",
            sof(5), token(0x69, 3, 0), b"\x5A",                 # IN NAK
            sof(5), token(0xE1, 3, 1), data(0xC3, bytes(768)),  # iso OUT, no handshake
            sof(6), token(0x69, 3, 1), data(0x4B, b"\x00\x00\x0c\x00"),  # iso IN feedback
        ])
        s = cyncap_summary.Summary()
        for ts, p in recs:
            s.add(ts, p)
        s.finish()
        self.assertEqual(s.pid_counts["SOF"], 4)
        self.assertEqual(s.frames_first_last(), (5, 6))
        t = s.transactions
        self.assertEqual(t[(0, 0, "SETUP")]["hs"]["ACK"], 1)
        self.assertEqual(t[(3, 0, "IN")]["hs"]["NAK"], 1)
        self.assertEqual(t[(3, 1, "OUT")]["len"][768], 1)
        self.assertEqual(t[(3, 1, "OUT")]["hs"]["none"], 1)
        self.assertEqual(t[(3, 1, "IN")]["len"][4], 1)
        self.assertEqual(s.setups[0][1:], (0, 0x00, 0x09, 1, 0, 0, "ACK"))
        text = s.report()
        self.assertIn("SET_CONFIGURATION", text)
        self.assertIn("addr 3 ep 1 OUT: 1 in", text)
        self.assertEqual(t[(3, 1, "OUT")]["sof_first"], 3)
        self.assertEqual(t[(3, 1, "IN")]["sof_first"], 4)

    def test_sof_gap(self):
        recs = self.capture([sof(1), sof(1), sof(1)])
        recs.append((recs[-1][0] + 1_000_000, sof(2)))   # 1 ms gap
        s = cyncap_summary.Summary()
        for ts, p in recs:
            s.add(ts, p)
        s.finish()
        self.assertEqual(s.sof_gaps, 1)
        self.assertEqual(s.sof_missing, 7)


# --- The USB side, against a fake usb1 module ------------------------------
# The fake has only what cyncap.main() calls: python-libusb1's names, with
# the analyzer behind them as cyncap.py's comments describe it.

class FakeUSBError(Exception):
    pass


class FakeUSBErrorTimeout(FakeUSBError):
    pass


class FakeUSBErrorInterrupted(FakeUSBError):
    pass


TRANSFER_COMPLETED, TRANSFER_TIMED_OUT, TRANSFER_CANCELLED = 0, 2, 3


class FakeAlt:

    def __init__(self, number, alt, cls, sub, proto):
        self.v = (number, alt, cls, sub, proto)

    def getNumber(self):
        return self.v[0]

    def getAlternateSetting(self):
        return self.v[1]

    def getClass(self):
        return self.v[2]

    def getSubClass(self):
        return self.v[3]

    def getProtocol(self):
        return self.v[4]


class FakeTransfer:

    def __init__(self, ana):
        self.ana = ana
        self.submitted = False
        self.cancelled = False
        self.status = None
        self.buf = b""

    def setBulk(self, ep, length, callback, timeout):
        self.ep, self.length, self.callback = ep, length, callback

    def submit(self):
        assert not self.submitted
        self.submitted = True

    def isSubmitted(self):
        return self.submitted

    def cancel(self):
        self.cancelled = True

    def getStatus(self):
        return self.status

    def getActualLength(self):
        return len(self.buf)

    def getBuffer(self):
        return self.buf

    def complete(self, status, data):
        self.submitted = False
        self.status, self.buf = status, data
        self.callback(self)


class FakeAnalyzer:
    """The analyzer behind the fake usb1 module.

    The state register; an enable writes CAPTURE_START of the speed and then
    `packets` to the stream, a disable writes CAPTURE_STOP_NORMAL unless
    `overrun` is set (the gateware writes no stop event after an overrun).
    `stale` is stream data left by an earlier run, `fail_writes` the number
    of SET_STATEs that take the value and still fail. `start` replaces the
    start event the enable writes (b"" for none), and `events` are event
    codes written after the packets.
    """

    def __init__(self, state=0x00, speeds=0x0F, protocol=1, packets=(),
                 overrun=False, stale=b"", fail_writes=0, start=None, events=()):
        self.state = state
        self.speeds = speeds
        self.protocol = protocol
        self.packets = list(packets)
        self.overrun = overrun
        self.stream = bytearray(stale)
        self.fail_writes = fail_writes
        self.start = start
        self.events = list(events)
        self.writes = []
        self.transfers = []
        self.claimed = None

    # python-libusb1's module namespace
    def module(self):
        m = types.ModuleType("usb1")
        m.TYPE_VENDOR, m.RECIPIENT_INTERFACE = 0x40, 0x01
        m.ENDPOINT_IN, m.ENDPOINT_OUT = 0x80, 0x00
        m.TRANSFER_COMPLETED = TRANSFER_COMPLETED
        m.TRANSFER_TIMED_OUT = TRANSFER_TIMED_OUT
        m.TRANSFER_CANCELLED = TRANSFER_CANCELLED
        m.USBError = FakeUSBError
        m.USBErrorTimeout = FakeUSBErrorTimeout
        m.USBErrorInterrupted = FakeUSBErrorInterrupted
        ana = self

        class USBContext:
            def open(self):
                pass

            def close(self):
                pass

            def openByVendorIDAndProductID(self, vid, pid):
                return ana if (vid, pid) == (0x1D50, 0x615B) else None

            def handleEventsTimeout(self, _t):
                ana.pump()
        m.USBContext = USBContext
        return m

    # the device handle
    def getDevice(self):
        dev = types.SimpleNamespace()
        alts = [FakeAlt(0, 0, 0xFF, 0x10, self.protocol)]
        dev.iterConfigurations = lambda: [[alts]]
        return dev

    def setAutoDetachKernelDriver(self, _on):
        pass

    def claimInterface(self, n):
        self.claimed = n

    def releaseInterface(self, n):
        assert self.claimed == n
        self.claimed = None

    def setInterfaceAltSetting(self, _n, _alt):
        raise AssertionError("alternate setting 0 needs no SET_INTERFACE")

    def close(self):
        pass

    def controlRead(self, rtype, req, value, index, length, timeout):
        assert (rtype, value, index) == (0xC1, 0, 0), (rtype, value, index)
        return bytes([{cyncap.REQ_GET_STATE: self.state,
                       cyncap.REQ_GET_SPEEDS: self.speeds,
                       cyncap.REQ_GET_MINOR_VERSION: 1}[req]])

    def controlWrite(self, rtype, req, value, index, data, timeout):
        assert (rtype, req, index, data) == (0x41, cyncap.REQ_SET_STATE, 0, b"")
        was, self.state = self.state, value
        self.writes.append(value)
        if value & 1 and not was & 1:
            speed = (value >> 1) & 3
            if self.start is None:
                self.stream += evt(4 + speed, 0)      # CAPTURE_START_<speed>
            else:
                self.stream += self.start
            for p in self.packets:
                self.stream += pkt(p, 7500)
            for code in self.events:
                self.stream += evt(code, 9)
        elif was & 1 and not value & 1 and not self.overrun:
            self.stream += evt(1, 5)                  # CAPTURE_STOP_NORMAL
        if self.fail_writes:
            self.fail_writes -= 1
            raise FakeUSBError("LIBUSB_ERROR_IO")

    def bulkRead(self, ep, length, timeout):
        assert ep == 0x81
        if not self.stream:
            raise FakeUSBErrorTimeout()
        d = bytes(self.stream[:length])
        del self.stream[:length]
        return d

    def getTransfer(self):
        t = FakeTransfer(self)
        self.transfers.append(t)
        return t

    def pump(self):
        for t in [t for t in self.transfers if t.submitted]:
            if t.cancelled:
                t.complete(TRANSFER_CANCELLED, b"")
            elif self.stream:
                d = bytes(self.stream[:t.length])
                del self.stream[:t.length]
                t.complete(TRANSFER_COMPLETED, d)
            else:
                t.complete(TRANSFER_TIMED_OUT, b"")


SOFS = [sof(n) for n in range(10, 15)]


class MainTest(unittest.TestCase):
    """cyncap.main() end to end against FakeAnalyzer. The disable keeps the
    speed bits; the VBUS bits (3 to 7) stay as found."""

    def setUp(self):
        self.signals = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
        self.dir = tempfile.TemporaryDirectory()
        self.out = os.path.join(self.dir.name, "cap.pcap")

    def tearDown(self):
        for s, h in self.signals.items():
            signal.signal(s, h)
        self.dir.cleanup()

    def run_main(self, ana, *args, usb1="fake"):
        # A capture that never meets its stop condition would run forever:
        # bound every run, unless the test gives --duration itself
        if "--duration" not in args:
            args += ("--duration", "2")
        mod = ana.module() if usb1 == "fake" else None
        text = io.StringIO()
        with mock.patch.dict(sys.modules, {"usb1": mod}), \
                contextlib.redirect_stdout(text), \
                contextlib.redirect_stderr(io.StringIO()):
            rc = cyncap.main(["--out", self.out, *args])
        return rc, text.getvalue()

    def packets(self):
        with open(self.out, "rb") as f:
            return [p for _, p in cyncap_summary.read_pcap(f)]

    def test_capture_stops_with_stop_event(self):
        ana = FakeAnalyzer(state=0x48, packets=SOFS)    # VBUS bits 3 and 6 set
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "3")
        self.assertEqual(rc, 0)
        self.assertEqual(ana.writes, [0x49, 0x48])      # enable at high, then disable
        self.assertEqual(self.packets(), SOFS[:3])
        self.assertIn("packets=3 ", out)
        self.assertIn("overrun=no ", out)
        self.assertIn("CAPTURE_START_HIGH=1 CAPTURE_STOP_NORMAL=1", out)
        self.assertIn("stop=max-packets speed=high gateware=v1.1 control_errors=0", out)
        self.assertTrue(all(not t.submitted for t in ana.transfers))
        self.assertIsNone(ana.claimed)

    def test_speed_bits(self):
        for speed, code in (("high", 0), ("full", 1), ("low", 2), ("auto", 3)):
            with self.subTest(speed=speed):
                ana = FakeAnalyzer(state=0x80, packets=SOFS)
                rc, _ = self.run_main(ana, "--speed", speed, "--duration", "0")
                self.assertEqual(rc, 0)
                self.assertEqual(ana.writes, [0x81 | code << 1, 0x80 | code << 1])

    def test_missing_stop_event_exits_2(self):
        ana = FakeAnalyzer(packets=SOFS, overrun=True)
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "5")
        self.assertEqual(rc, 2)
        self.assertIn("overrun=suspected", out)
        self.assertEqual(self.packets(), SOFS)

    def test_missing_start_event_exits_3(self):
        # The stream begins inside a record: its first bytes are a packet's
        ana = FakeAnalyzer(packets=SOFS, start=pkt(sof(9), 3))
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "6")
        self.assertEqual(rc, 3)
        self.assertIn("start=missing(first record: packet) ", out)
        self.assertIn("overrun=no ", out)

    def test_unknown_event_code_exits_4(self):
        ana = FakeAnalyzer(packets=SOFS, events=[31])
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "5")
        self.assertEqual(rc, 4)
        self.assertIn("start=ok unknown_events=1 ", out)
        self.assertIn("EVENT_31=1", out)

    def test_enabled_state_is_stopped_and_drained_first(self):
        stale = pkt(sof(1), 1) + evt(1, 2)
        ana = FakeAnalyzer(state=0x4B, packets=SOFS, stale=stale)  # enabled at full
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "5")
        self.assertEqual(rc, 0)
        self.assertEqual(ana.writes, [0x4A, 0x49, 0x48])
        self.assertEqual(self.packets(), SOFS)

    def test_failed_set_state_is_read_back(self):
        ana = FakeAnalyzer(packets=SOFS, fail_writes=1)
        rc, out = self.run_main(ana, "--speed", "high", "--max-packets", "5")
        self.assertEqual(rc, 0)
        self.assertEqual(ana.writes, [0x01, 0x00])
        self.assertIn("control_errors=1 ", out)

    def test_refusals(self):
        cases = [
            (FakeAnalyzer(speeds=0x0E), "auto", "speed auto not supported"),
            (FakeAnalyzer(protocol=2), "high", "protocol v2"),
        ]
        for ana, speed, msg in cases:
            with self.subTest(msg=msg):
                with self.assertRaises(SystemExit) as e:
                    self.run_main(ana, "--speed", speed)
                self.assertIn(msg, str(e.exception.code))
                self.assertEqual(ana.writes, [])

    def test_no_analyzer(self):
        ana = FakeAnalyzer()
        mod = ana.module()
        mod.USBContext.openByVendorIDAndProductID = lambda self, vid, pid: None
        with mock.patch.object(FakeAnalyzer, "module", lambda self: mod):
            with self.assertRaises(SystemExit) as e:
                self.run_main(ana, "--speed", "high")
        self.assertIn("no Cynthion analyzer", str(e.exception.code))

    def test_without_libusb1(self):
        with self.assertRaises(SystemExit) as e:
            self.run_main(FakeAnalyzer(), "--speed", "high", usb1=None)
        self.assertIn("pip install libusb1", str(e.exception.code))


class TsharkTest(unittest.TestCase):

    @unittest.skipUnless(any(os.access(os.path.join(d, "tshark"), os.X_OK)
                             for d in os.environ.get("PATH", "").split(":")),
                         "tshark not installed")
    def test_tshark_reads(self):
        import subprocess
        with tempfile.NamedTemporaryFile(suffix=".pcap") as f:
            w = cyncap.PcapWriter(f)
            w.write(1, sof(3))
            f.flush()
            out = subprocess.run(["tshark", "-r", f.name], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr)
            self.assertIn("SOF", out.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
