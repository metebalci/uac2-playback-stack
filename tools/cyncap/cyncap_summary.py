#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mete Balci
"""Text summary of a LINKTYPE_USB_2_0 pcap (as written by cyncap.py or Packetry).

Usage: cyncap_summary.py FILE.pcap [--setups N]

Prints per-PID counts, SOF frame numbers and rate, SOF gaps, and per
endpoint and direction the transactions (token, data length, handshake),
then the SETUP requests in order. Packet layout: USB 2.0 section 8.4
(token: PID, 7-bit address, 4-bit endpoint, CRC5; SOF: PID, 11-bit frame
number, CRC5; data: PID, payload, CRC16; handshake: PID only).
"""

import argparse
import struct
import sys
from collections import Counter

LINKTYPE_USB_2_0 = 288

PIDS = {
    0xE1: "OUT", 0x69: "IN", 0xA5: "SOF", 0x2D: "SETUP",
    0xC3: "DATA0", 0x4B: "DATA1", 0x87: "DATA2", 0x0F: "MDATA",
    0xD2: "ACK", 0x5A: "NAK", 0x1E: "STALL", 0x96: "NYET",
    0x3C: "PRE/ERR", 0x78: "SPLIT", 0xB4: "PING",
}
TOKENS = {"OUT", "IN", "SETUP", "PING"}
DATA = {"DATA0", "DATA1", "DATA2", "MDATA"}
HANDSHAKES = {"ACK", "NAK", "STALL", "NYET"}

STD_REQ = {0: "GET_STATUS", 1: "CLEAR_FEATURE", 3: "SET_FEATURE",
           5: "SET_ADDRESS", 6: "GET_DESCRIPTOR", 7: "SET_DESCRIPTOR",
           8: "GET_CONFIGURATION", 9: "SET_CONFIGURATION",
           10: "GET_INTERFACE", 11: "SET_INTERFACE", 12: "SYNCH_FRAME"}
DESC = {1: "DEVICE", 2: "CONFIGURATION", 3: "STRING", 4: "INTERFACE",
        5: "ENDPOINT", 6: "DEVICE_QUALIFIER", 7: "OTHER_SPEED_CONFIGURATION",
        8: "INTERFACE_POWER", 15: "BOS"}
UAC2_REQ = {1: "CUR", 2: "RANGE", 3: "MEM"}     # UAC2 A.14


def read_pcap(f):
    """Yields (ts_ns, bytes) from a classic pcap file object."""
    hdr = f.read(24)
    if len(hdr) < 24:
        raise ValueError("not a pcap file (short header)")
    magic = struct.unpack("<I", hdr[:4])[0]
    endian = "<"
    if magic in (0xD4C3B2A1, 0x4D3CB2A1):
        endian = ">"
        magic = struct.unpack(">I", hdr[:4])[0]
    if magic == 0xA1B2C3D4:
        frac = 1000
    elif magic == 0xA1B23C4D:
        frac = 1
    else:
        raise ValueError(f"not a classic pcap file (magic 0x{magic:08x}; pcapng is not read)")
    link = struct.unpack(endian + "I", hdr[20:24])[0] & 0x0FFFFFFF
    if link != LINKTYPE_USB_2_0:
        raise ValueError(f"linktype {link}, expected {LINKTYPE_USB_2_0} (USB_2_0)")
    while True:
        rh = f.read(16)
        if len(rh) < 16:
            return
        sec, sub, incl, _orig = struct.unpack(endian + "IIII", rh)
        data = f.read(incl)
        if len(data) < incl:
            return
        yield sec * 1_000_000_000 + sub * frac, data


def pid_name(p):
    if not p:
        return "EMPTY"
    b = p[0]
    if (b >> 4) != (~b & 0x0F):
        return "BADPID"
    return PIDS.get(b, f"PID_{b:02x}")


class Summary:

    def __init__(self):
        self.pid_counts = Counter()
        self.n = 0
        self.first_ts = None
        self.last_ts = None
        self.frames = []            # (ts, frame) of every SOF
        self.malformed = 0
        self.transactions = {}      # (addr, ep, token) -> {"count","len","hs"}
        self.setups = []            # (ts, addr, bmRequestType, bRequest, wValue, wIndex, wLength, hs)
        self.cur = None
        self.sof_gaps = 0
        self.sof_missing = 0
        self.sof_max_gap = 0
        self._finished = False
        self.gap_list = []          # (ts before gap, gap ns, frame before, frame after)

    def _close(self):
        t = self.cur
        if t is None:
            return
        self.cur = None
        key = (t["addr"], t["ep"], t["tok"])
        e = self.transactions.setdefault(key, {"count": 0, "len": Counter(), "hs": Counter(),
                                               "first": t["ts"], "last": t["ts"],
                                               "sof_first": t["sof"], "sof_last": t["sof"],
                                               "sofs": set()})
        e["count"] += 1
        e["last"] = t["ts"]
        e["sof_last"] = t["sof"]
        e["sofs"].add(t["sof"])
        e["len"]["-" if t["len"] is None else t["len"]] += 1
        e["hs"][t["hs"] or "none"] += 1
        if t["tok"] == "SETUP" and t["setup"] is not None:
            self.setups[-1] = self.setups[-1][:7] + (t["hs"] or "none",)

    def add(self, ts, p):
        self.n += 1
        if self.first_ts is None:
            self.first_ts = ts
        self.last_ts = ts
        name = pid_name(p)
        self.pid_counts[name] += 1
        if name in TOKENS:
            self._close()
            if len(p) != 3:
                self.malformed += 1
                return
            v = p[1] | (p[2] << 8)
            self.cur = {"tok": name, "addr": v & 0x7F, "ep": (v >> 7) & 0x0F,
                        "len": None, "hs": None, "setup": None, "ts": ts,
                        "sof": len(self.frames)}
        elif name == "SOF":
            self._close()
            if len(p) != 3:
                self.malformed += 1
                return
            self.frames.append((ts, (p[1] | (p[2] << 8)) & 0x7FF))
        elif name in DATA:
            if self.cur is None or self.cur["len"] is not None:
                self.malformed += 1     # data without a token
                return
            self.cur["len"] = len(p) - 3
            if self.cur["tok"] == "SETUP" and len(p) == 11:
                bm, req, wv, wi, wl = struct.unpack("<BBHHH", p[1:9])
                self.cur["setup"] = True
                self.setups.append((self.cur["ts"], self.cur["addr"], bm, req, wv, wi, wl, None))
        elif name in HANDSHAKES:
            if self.cur is None:
                self.malformed += 1
                return
            self.cur["hs"] = name
            self._close()
        else:
            self._close()

    def finish(self):
        """Closes the last transaction and computes the SOF gaps; idempotent."""
        if self._finished:
            return
        self._finished = True
        self._close()
        iv = [b[0] - a[0] for a, b in zip(self.frames, self.frames[1:])]
        if iv:
            nominal = sorted(iv)[len(iv) // 2]
            self.sof_nominal = nominal
            for i, d in enumerate(iv):
                if d > 1.5 * nominal:
                    self.sof_gaps += 1
                    self.gap_list.append((self.frames[i][0], d, self.frames[i][1],
                                          self.frames[i + 1][1]))
                    self.sof_missing += round(d / nominal) - 1
            self.sof_max_gap = max(iv)
        else:
            self.sof_nominal = 0

    def frames_first_last(self):
        if not self.frames:
            return None
        return self.frames[0][1], self.frames[-1][1]

    def report(self, max_setups=60):
        self.finish()
        span = ((self.last_ts or 0) - (self.first_ts or 0)) / 1e9
        L = [f"packets {self.n}, span {span:.6f} s, malformed {self.malformed}"]
        L.append("PIDs: " + " ".join(f"{k}={v}" for k, v in sorted(
            self.pid_counts.items(), key=lambda kv: -kv[1])))
        if self.frames:
            fset = {f for _, f in self.frames}
            fs = (self.frames[-1][0] - self.frames[0][0]) / 1e9
            rate = (len(self.frames) - 1) / fs if fs > 0 else 0
            L.append(f"SOF: {len(self.frames)} SOFs, frame numbers {self.frames[0][1]}..{self.frames[-1][1]} "
                     f"({len(fset)} distinct), {rate:.1f} SOF/s, interval median "
                     f"{self.sof_nominal / 1e3:.3f} us, gaps >1.5x median: {self.sof_gaps} "
                     f"(about {self.sof_missing} SOFs missing, largest gap {self.sof_max_gap / 1e3:.1f} us)")
            for ts, d, fa, fb in self.gap_list[:10]:
                L.append(f"  SOF gap at +{(ts - self.first_ts) / 1e9:.6f} s: {d / 1e3:.1f} us, "
                         f"frame {fa} -> {fb}")
            if len(self.gap_list) > 10:
                L.append(f"  ... {len(self.gap_list) - 10} more gaps")
        else:
            L.append("SOF: none")
        L.append("Transactions (addr ep token: count over active time, rate, SOF intervals "
                 "with at least one; data lengths; handshakes):")
        for (a, ep, tok), e in sorted(self.transactions.items()):
            lens = ", ".join(f"{k}x{v}" for k, v in e["len"].most_common(6))
            if len(e["len"]) > 6:
                lens += f", ... ({len(e['len'])} distinct)"
            hs = ", ".join(f"{k}={v}" for k, v in e["hs"].most_common())
            act = (e["last"] - e["first"]) / 1e9
            rate = (e["count"] - 1) / act if act > 0 else 0
            nsof = e["sof_last"] - e["sof_first"] + 1
            L.append(f"  addr {a} ep {ep} {tok}: {e['count']} in {act:.6f} s ({rate:.1f}/s), "
                     f"in {len(e['sofs'])} of {nsof} SOF intervals; len {lens}; {hs}")
        L.append(f"SETUP requests ({len(self.setups)}, first {min(len(self.setups), max_setups)}):")
        for s in self.setups[:max_setups]:
            ts, a, bm, req, wv, wi, wl, hs = s
            L.append(f"  +{(ts - self.first_ts) / 1e9:.6f} s addr {a} "
                     f"{describe_setup(bm, req, wv, wi, wl)} -> {hs}")
        return "\n".join(L)


def describe_setup(bm, req, wv, wi, wl):
    d = "IN" if bm & 0x80 else "OUT"
    typ = (bm >> 5) & 3
    rcp = {0: "dev", 1: "intf", 2: "ep", 3: "other"}.get(bm & 0x1F, "rsvd")
    if typ == 0:
        name = STD_REQ.get(req, f"STD_{req}")
        if req == 6:
            name += f"({DESC.get(wv >> 8, wv >> 8)} {wv & 0xFF})"
    elif typ == 1:
        name = f"CLASS_{UAC2_REQ.get(req, req)}"
    elif typ == 2:
        name = f"VENDOR_{req}"
    else:
        name = f"RSVD_{req}"
    return (f"{name} bm=0x{bm:02x} {d} {rcp} wValue=0x{wv:04x} wIndex=0x{wi:04x} "
            f"wLength={wl}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Summarise a USB 2.0 pcap.")
    ap.add_argument("pcap")
    ap.add_argument("--setups", type=int, default=60,
                    help="SETUP requests to list (default 60)")
    a = ap.parse_args(argv)
    s = Summary()
    with open(a.pcap, "rb") as f:
        for ts, p in read_pcap(f):
            s.add(ts, p)
    print(f"file {a.pcap}")
    print(s.report(a.setups))
    return 0


if __name__ == "__main__":
    sys.exit(main())
