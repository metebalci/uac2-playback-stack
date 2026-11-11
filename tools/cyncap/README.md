<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Mete Balci -->
# cyncap

cyncap captures USB 2.0 traffic with a Great Scott Gadgets Cynthion that
runs the analyzer gateware ("USB Analyzer"), without Packetry's GUI. It is
meant for coding agents and CI: it runs headless, stops on a time or
packet limit or a signal, prints one summary line and reports a suspected
overrun in its exit code. It writes a pcap file that Packetry and
Wireshark read. `cyncap_summary.py` prints a text summary of such a file,
so that an agent can read a capture without Wireshark.

## Requirements

- Python 3. The tools are written for Python 3.8 or later and tested with
  Python 3.14.
- For `cyncap.py`, the pip package `libusb1` (python-libusb1; tested with
  3.4.0), which needs the libusb-1.0 library. The pip package `cynthion`
  (tested with 0.2.5, which pulls in `pyusb`) provides the `cynthion`
  command, which the setup below uses. `cyncap_summary.py` and the
  self-test need neither.
- Access to the Cynthion without root rights. `cynthion setup` installs
  the udev rule `54-cynthion.rules`, which grants it to the user logged in
  at the machine's seat (`TAG+="uaccess"`). A headless host, where agents
  and CI log in remotely, needs a rule that also sets a group or a mode,
  for example `MODE="0660", GROUP="plugdev"` for vendor ID 1d50 and
  product ID 615b.
- The analyzer gateware on the Cynthion: `cynthion info` shows the
  bitstream "USB Analyzer (Cynthion Project)". `cynthion run analyzer`
  loads it, `cynthion update` writes it to the configuration flash.
- The Cynthion's CONTROL port connected to the host that runs cyncap, and
  the bus to capture through its TARGET-C and TARGET-A ports.

```
python3 -m venv /path/to/venv
/path/to/venv/bin/pip install cynthion libusb1
```

## Usage

```
/path/to/venv/bin/python cyncap.py --speed high|full|low|auto --out FILE.pcap \
    [--duration SECONDS] [--max-packets N] [--events FILE.txt]
python3 cyncap_summary.py FILE.pcap [--setups N]
python3 -I cyncap_test.py        # self-test, no hardware
```

- `--speed` is the bus speed, and it must match it: at another speed the
  analyzer sees no packets. Use `high` for a high-speed device.
- The capture stops after `--duration` seconds, after `--max-packets`
  packets, or on SIGINT or SIGTERM. Without a limit it runs until a signal
  arrives.
- `--events` writes the analyzer's events (capture start and stop, bus
  reset, line states, VBUS) as text lines: the analyzer's time in seconds,
  the event code and its name.
- The exit code says whether the capture is whole. Only 0 means that it
  is:

  | Code | Meaning | Summary line |
  |---|---|---|
  | 0 | The stream began with the start event, had no unknown event code, and the analyzer's stop event arrived. | `overrun=no start=ok unknown_events=0` |
  | 1 | Refused start: no analyzer, another protocol version, an unsupported speed, no python-libusb1. The message starts with `cyncap:`; no summary line. | - |
  | 2 | The stop event did not arrive: the analyzer probably overran (see "Limits"). | `overrun=suspected(...)` |
  | 3 | The stream's first record was not a `CAPTURE_START_<speed>` event: the stream began inside a record, and the first records are not to be trusted (see "Limits"). | `start=missing(first record: ...)`, or `start=missing(no record)` |
  | 4 | Event codes that the protocol does not define came: records were misread. | `unknown_events=<n>`, and `EVENT_<code>=<count>` among the events |

  With several of them, 3 comes before 4, and 4 before 2.

cyncap prints one line on exit, here of a 68 s capture around a 60 s
stream at 768 kHz with 32-bit stereo samples. The capture was taken
before the fields `start=` and `unknown_events=` existed; they now follow
`overrun=`, as `start=ok unknown_events=0` in a whole capture:

```
cyncap: packets=1624041 bytes=373747501 duration=68.003s analyzer_time=67.976522s stream_bytes=381867712 ignored_after_max=0 leftover_bytes=0 overrun=no events: CAPTURE_START_HIGH=1 CAPTURE_STOP_NORMAL=1 stop=duration speed=high gateware=v1.1 control_errors=0 file=capture.pcap
```

`cyncap_summary.py` prints the packet count per PID, the SOF frame
numbers, the SOF rate, the gaps between SOFs longer than 1.5 times the
median interval, and per address, endpoint and token the transactions:
their count, their rate over the time the endpoint was active, the number
of SOF intervals with at least one transaction, the data lengths and the
handshakes. It then lists the SETUP requests with the standard requests
decoded. A capture of the start of a 48 kHz stream, with `--setups 3`:

```
file capture.pcap
packets 64186, span 3.515881 s, malformed 0
PIDs: SOF=28136 DATA0=17995 OUT=16000 IN=2007 ACK=20 DATA1=13 SETUP=7 NAK=7 PING=1
SOF: 28136 SOFs, frame numbers 1227..648 (2048 distinct), 8002.3 SOF/s, interval median 124.966 us, gaps >1.5x median: 0 (about 0 SOFs missing, largest gap 125.0 us)
Transactions (addr ep token: count over active time, rate, SOF intervals with at least one; data lengths; handshakes):
  addr 1 ep 0 IN: 13 in 2.003925 s (6.0/s), in 13 of 16039 SOF intervals; len -x6, 0x3, 1x2, 4x2; ACK=7, NAK=6
  addr 1 ep 0 OUT: 6 in 0.002374 s (2106.3/s), in 6 of 20 SOF intervals; len 0x4, 4x2; ACK=5, NAK=1
  addr 1 ep 0 PING: 1 in 0.000000 s (0.0/s), in 1 of 1 SOF intervals; len -x1; ACK=1
  addr 1 ep 0 SETUP: 7 in 2.003800 s (3.0/s), in 7 of 16038 SOF intervals; len 8x7; ACK=7
  addr 1 ep 1 IN: 1994 in 1.992179 s (1000.4/s), in 1994 of 15945 SOF intervals; len 4x1994; none=1994
  addr 1 ep 1 OUT: 15994 in 1.998350 s (8003.1/s), in 15994 of 15994 SOF intervals; len 48x15994; none=15994
SETUP requests (7, first 3):
  +1.319231 s addr 1 CLASS_CUR bm=0xa1 IN intf wValue=0x0200 wIndex=0x0400 wLength=1 -> ACK
  +1.319981 s addr 1 CLASS_CUR bm=0xa1 IN intf wValue=0x0100 wIndex=0x0400 wLength=4 -> ACK
  +1.320482 s addr 1 CLASS_CUR bm=0x21 OUT intf wValue=0x0100 wIndex=0x0400 wLength=4 -> ACK
```

## Output

The pcap file has nanosecond timestamps and LINKTYPE_USB_2_0 (288), as
Packetry saves. Each record is one USB packet as the PHY delivered it:
PID, payload and CRC, without SYNC and EOP. The timestamp is the host's
wall-clock time at the capture's start plus the analyzer's 60 MHz
timestamp. Events are not in the pcap, because LINKTYPE_USB_2_0 has no
record type for them; `--events` writes them to a text file.

## Limits

- The speed must be given. With `--speed auto` the analyzer's speed
  detector chooses it, but in a test with a high-speed device,
  `--speed auto` captured no packet and no event, even across a port
  reset, after the start event CAPTURE_START_FULL. The cause is not known.
  SET_STATE does not reset the detector: its `reset` input
  (speed_detection.py:74) is connected in top.py:342 but not used inside
  it.
- In `--speed high` the analyzer reports no line-state or bus-reset
  events; a bus reset shows only as a gap in the SOFs (two 50.5 ms gaps
  for a port reset by a Linux host).
- The gateware reports no overrun. When its buffers fill, it stops writing
  records, and when the capture is stopped it writes no stop event
  (analyzer.py:268-271, 289-294). cyncap therefore reports
  `overrun=suspected` and exits with status 2 when the stop event is
  missing. The buffers are the board's 8 MiB HyperRAM (2^22 16-bit words,
  fifo.py:59) and 8 KiB in the FPGA (4,096 16-bit words, analyzer.py:68).
  The 68 s capture of a 768 kHz stream above had no overrun.
- The stream can begin inside a record. In one of four 2 s test captures
  during heavy bus traffic, the start event was missing, the first 70 ms
  gave malformed packets and unknown event codes (`EVENT_<code>` in the
  summary line), and about 0.3 s of analyzer time was missing; the parser
  found the records again after that. The cause is not known. cyncap
  exits with status 3 then (status 4 if only unknown event codes appear),
  and keeps the pcap file, whose first records are not to be trusted.
- SET_STATE has failed with LIBUSB_ERROR_IO while a 768 kHz stream ran,
  although the gateware took the value. cyncap reads the state back and
  retries, and counts such errors in `control_errors`.
- `cyncap_summary.py` reads classic pcap only, not pcapng.

## Protocol

cyncap reimplements the analyzer's host protocol, in Python, from two
sources, read at these commits:

- [gw] The analyzer gateware in
  https://github.com/greatscottgadgets/cynthion at commit
  [31929c2](https://github.com/greatscottgadgets/cynthion/tree/31929c20e81ad2d0726999756102f32bbf83172a),
  directory `cynthion/python/src/gateware/analyzer/`, BSD 3-Clause
  License. The pip package `cynthion` 0.2.5 ships the same gateware
  sources.
- [pk] Packetry's Cynthion backend in
  https://github.com/greatscottgadgets/packetry at commit
  [650b7d4](https://github.com/greatscottgadgets/packetry/tree/650b7d451736110635432e499797b3612481e757),
  `src/backend/cynthion.rs`, BSD 3-Clause License.

No code is taken from them: the gateware is a hardware description in
Amaranth (Python), and Packetry is Rust. cyncap uses their constants, which are the protocol: the identifiers,
the request and event codes, the bit positions and the record format.
`cyncap.py` cites the file and line of each fact. The facts:

- The analyzer is vendor ID 1d50, product ID 615b, with an interface of
  class 0xff, subclass 0x10 and protocol 1 (the protocol's major version),
  and a bulk IN endpoint 0x81 of 512 bytes ([pk] cynthion.rs:42-45, [gw]
  top.py:243-249).
- Vendor requests to the interface: 0 GET_STATE, 1 SET_STATE (the value in
  wValue, no data stage), 2 GET_SPEEDS, 3 SET_TEST_CONFIG and 4
  GET_MINOR_VERSION (1 at this commit) ([gw] top.py:60-62, 78-83; [pk]
  cynthion.rs:408-425, 445-464).
- The state register holds the capture enable (bit 0), the speed
  (bits 2:1: high 0, full 1, low 2, auto 3) and the VBUS routing (bits 3
  to 7) ([gw] top.py:87-110). cyncap keeps the VBUS bits as it finds them.
- The stream is a sequence of records in 16-bit big-endian words. A packet
  record is a 16-bit length, a 16-bit timestamp delta, the packet's bytes
  and one pad byte if the length is odd. An event record is 0xFF, the
  event code and a 16-bit timestamp delta. The deltas count 60 MHz cycles
  since the previous record; an event with code 0 marks a wrap of the
  counter ([gw] analyzer.py:57-66, 222-249, 300-366; [pk]
  cynthion.rs:491-565).
- The event codes are in [gw] events.py:10-52.

## Self-test

`cyncap_test.py` needs neither the analyzer nor python-libusb1. It checks
the stream parser at every split point of a chunk, the pcap file, the
summary, and `cyncap.main()` end to end against a fake of the analyzer
behind a fake `usb1` module: the state register's writes, the drain of a
capture left running, the read-back after a failed SET_STATE, the exit
codes without the stop event, without the start event first and with
unknown event codes, and the refusals. A test that Wireshark's
`tshark` reads the pcap is skipped when `tshark` is not installed. The
PC build registers it as the CTest test `cyncap_selftest` when CMake
finds Python 3.

## Files

| File | Contents |
|---|---|
| `cyncap.py` | The capture tool. |
| `cyncap_summary.py` | The text summary of a pcap file. |
| `cyncap_test.py` | The self-test. |

## How it was written

cyncap is written with [Claude Code](https://claude.com/claude-code), using
the Anthropic model Claude Opus 5.5.
