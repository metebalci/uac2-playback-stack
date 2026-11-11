<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Mete Balci -->
# uac2-playback-stack

u2ps is a USB Audio Class 2.0 (UAC2) playback stack for USB 2.0
High-Speed, written in C. It has two stacks: the device stack, a UAC2 sink,
for DACs, and the host stack, which plays to a UAC2 device, for players. It
is audio-specific: it carries only what one UAC2 audio function needs, with
no class table, no event queue and no generic endpoint API.

## Goals

- u2ps is a UAC2-compliant playback device stack and host stack for the
  Synopsys DWC2 USB controller (for example in ST's STM32
  microcontrollers) and the ChipIdea USB controller (for example in NXP's
  i.MX RT crossover MCUs).
- It is lean and fast, as published measurements of its code size,
  interrupt load and losses are to show against TinyUSB and, later, against
  the vendors' own UAC2 stacks. It never copies audio data.
- It has no external dependency. It needs only a C11 compiler and
  `memcpy`, `memmove`, `memset` and `memcmp`, which every freestanding
  toolchain provides.
- It follows USB 2.0 and UAC2 to the letter. Every deviation is optional,
  off by default and documented. It aims to pass USB-IF's USB3CV Chapter 9
  tests for USB 2.0 devices.
- It is observable: every packet comes with the microframe it arrived in,
  and every gap with its cause.
- It is written from the USB 2.0 and UAC2 specifications and the vendors'
  reference manuals, with no code taken from other USB stacks.
- It plays high-resolution audio: sampling frequencies up to 768 kHz,
  32-bit PCM, 32-bit float, and DSD as DoP.
- It is released under the MIT License.

Recording, UAC1, streaming at full speed, other USB classes, composite
devices and, at first, hubs on the host side are not goals. Neither are
1.5 MHz PCM and DSD512 as DoP: both need more than 1,024 bytes per
microframe, and so several transactions per microframe (high-bandwidth
isochronous endpoints, USB 2.0 5.6.3), while u2ps uses one transaction per
microframe.

## Status

This is release 0.1.0. There are two controller ports, DWC2 and ChipIdea;
one port is chosen at compile time. The targets are the STM32F723
(Cortex-M7, DWC2), the STM32U5A5 (Cortex-M33, DWC2), the STM32H7S3
(Cortex-M7, DWC2; host stack only, for now) and the i.MX RT1062
(Cortex-M7, ChipIdea).

| Part | State in 0.1.0 |
|---|---|
| Device stack, DWC2 device port | Runs on boards: the NUCLEO-U5A5ZJ-Q (below), and, in bit-perfect-checker's firmware, the STM32F723E-DISCO and the STM32U5G9J-DK2. |
| Host stack, DWC2 host port | Tested on a PC only, against a fake host port and, through a bus that steps microframes, against the device stack. Not yet run on a board. |
| STM32H7S3 target | The host stack only; its PHY start-up is tested on a PC against a register model. Not yet run on a board. |
| ChipIdea device port | Its decision logic only, tested on a PC against a simulated controller. It has no register code yet: the `imxrt1062` build compiles the libraries, but an app for the i.MX RT1062 does not link. |

The device stack and the DWC2 device port run on a NUCLEO-U5A5ZJ-Q with a
Linux host. These results came from images built with `arm-none-eabi-gcc`
14.2 (`-O2`) from the tree of their day; the board images have been built
with ATfE since 2026-11-09 (see "Optimisation: ATfE -Os, GCC -O2").

- On 2026-11-06 the example app enumerated at High-Speed, passed a replay
  of the standard requests and 100 re-enumerations, and fell back to the
  minimal full-speed configuration when forced to full speed.
- On 2026-11-07, in about 190 streams from 48 to 768 kHz with 16-, 24- and
  32-bit samples, captured with usbmon on the host, the device lost no
  packet up to 192 kHz, and the feedback endpoint answered all but one of
  2.27 million polls. At 768 kHz the USB interrupt took at most 1,419
  cycles at 160 MHz (`isr_max`) and 4.4 % of the core (`isr_pct`), as the
  example app's `stream_stats` lines report them.
- On 2026-11-07 USB-IF's USB3CV ("Chapter 9 Tests [USB 2 devices]", 36
  tests) passed on the example at High-Speed and, with the forced
  full-speed test build, at full speed, with no failure.

[bit-perfect-checker](https://github.com/metebalci/bit-perfect-checker), a
project of the same author, integrated the device stack and the DWC2
device port into its firmware and ran its regression on the
NUCLEO-U5A5ZJ-Q, the STM32F723E-DISCO and the STM32U5G9J-DK2 (built as
`stm32u5a5`: the STM32U5G9 has the same OTG_HS register set) on
2026-11-07 and 2026-11-08: the same bit-perfect verdicts as its build with
its previous USB stack, and no packet lost.

The host stack enumerates a device, finds its audio function and streams
to it. It also meets the duties of an Embedded Host: it identifies full-
and low-speed devices and tells why it cannot play them, refuses hubs,
selects no configuration that needs more current than the board's VBUS
supply is rated for, runs the USB-IF test device (VID 0x1A0A) of the
Embedded Host electrical tests, and lets the app suspend and resume the
port. The DWC2 host port has its register code
(`port/dwc2/hcd/u2ps_dwc2_hcd.c`, interrupt handler `u2ps_dwc2_hcd_irq()`
in `u2ps_dwc2_hcd.h`) and builds for the STM32U5, the STM32F723 and the
STM32H7S3; its decision logic is tested on a PC, and so is the STM32H7S3's
PHY start-up, against a register model (`tests/test_dwc2_phy.c`). The
STM32H7S3 builds the host stack only: its errata sheet (ES0596 Rev 10)
lists four OTG_HS items. Two are faults of IN endpoints, 2.21.1 and
2.21.3, whose workarounds the DWC2 device port does not have yet, so
`-DU2PS_DEVICE=ON` with `-DU2PS_CHIP=stm32h7s3` stops the configure step
and names them. 2.21.2, a false `OTG_GINTSTS.CIDSCHG` after a reset, needs
nothing: neither port unmasks CIDSCHG. 2.21.4, a false chirp-K when the
port reset starts within 150 PHY clock cycles of a full-speed device's
connect, needs nothing either: the host stack starts the reset only after
its 100 ms connect debounce (TATTDB).

A PC test kit lets an integrator test its own app against either stack
without a board; see "Test your integration on the PC".

The pull-up follows VBUS as the app reports it (VBUS mode `app`), or the
device is bus-powered (mode `bus`); see "VBUS" below. A port that senses
VBUS itself (mode `port`) is not available yet.

The port interfaces and the app APIs of both stacks may change in any
release before 1.0.0; the device side at least until the ChipIdea port has
received a 768 kHz stream without a single changed sample.

## Deviations and known limits

Every deviation from USB 2.0 and UAC2 but one is a build option, off by
default:

- `U2PS_REFUSE_FS=ON`: the device never enumerates at full speed (USB 2.0
  requires a high-speed device to work at full speed too); see "Full
  speed".
- `U2PS_HOST_REFUSE_FS=ON`: the host ends the session of a full- or
  low-speed device at once (USB 2.0 7.1.2.3).
- `U2PS_HOST_ACCEPT_NO_IAD=ON`: the host accepts an audio function without
  an Interface Association Descriptor (UAC2 4.6).
- `U2PS_DWC2_FORCE_FS=ON`: the DWC2 device enumerates at full speed only,
  for bench checks.

The one on by default: CLEAR_FEATURE(ENDPOINT_HALT) to the isochronous
data or feedback endpoint succeeds and changes nothing. Those endpoints
have no Halt feature (USB 2.0 9.4.5 requires it for interrupt and bulk
endpoints only), and a request for a feature that does not exist is a
Request Error (9.4.1). It is tolerated for hosts that clear the halt of
every endpoint; USB3CV's Chapter 9 tests do not test it on isochronous
endpoints. It is to become a STALL in a later release.

Known limits of 0.1.0:

- DWC2 device port: nothing bounds the EP0 OUT DMA while SETUP packets
  come back to back. RM0456 73.14.55 allows any number of them before the
  IN or OUT token that ends the setup stage, and what the core's buffer DMA
  does past the third is not documented and not measured. If it advances 8
  bytes for each, a host that sends 17 or more without such a token writes
  past the 64-byte EP0 area and its 64-byte guard into the DMA memory after
  them; `ep0_overrun` (`port/dwc2/dcd/u2ps_dwc2_dcd.h`) counts it only
  afterwards. Only a faulty or hostile host does that.
- DWC2 host port: a run of 8,192 microframes (1.024 s) or more with the
  stream's queue dry counts modulo 16,384 microframes, and as none when
  that remainder is 8,192 or more: its NO_BUFFER hole, or its drained
  microframes after `u2ps_host_drain()`, are then reported short or not at
  all (`host/u2ps_hport.h`, `host/u2ps_host.h`).
- DWC2 host port: at port babble the port resets its core inside the
  OTG_HS handler, one pass of about 26 ms, at most 66 ms, in which thread
  code and every interrupt at or below the USB priority wait. Give audio
  DMA, watchdog feeds and safety polls a higher priority or a 66 ms margin.
- DWC2 host port: a feedback endpoint of bInterval 14 to 16 is refused
  (`U2PS_HOST_STOP_OPEN`). USB 2.0 allows up to 16, but the port counts
  microframes modulo 2^14 with signed distances; 1 to 13 work.

## Layout

| Directory | Contents |
|---|---|
| `common/` | Code that both stacks share: USB and UAC2 constants, microframe arithmetic. No controller code. |
| `device/` | The device stack. |
| `host/` | The host stack. |
| `port/dwc2/common/` | DWC2 code that the device and host ports share. |
| `port/dwc2/dcd/` | The DWC2 device port. |
| `port/dwc2/hcd/` | The DWC2 host port. |
| `port/chipidea/dcd/` | The ChipIdea device port. |
| `testkit/` | The PC test kit, for integrators and for the tests: the fake device and host ports, the scripted host and device, the trace of the app's callbacks; PC build only. |
| `tests/` | Unit tests for the PC build, and descriptor fixtures in `tests/fixtures/`. |
| `examples/` | Example apps for boards, whose vendor files are fetched at configure time, and the PC test kit's worked example, `examples/pc_testkit/`. |
| `cmake/` | The toolchain files for the ARM builds: `arm-none-eabi-clang.cmake` for Arm Toolchain for Embedded (ATfE), the project's toolchain, and `arm-none-eabi.cmake` for `arm-none-eabi-gcc`. |
| `tools/` | Tools for the bench: `tools/cyncap/`, a headless USB capture with a Cynthion; see "Tools". |

Each directory of a stack or a port is one static library: `u2ps_device`,
`u2ps_host`, `u2ps_dwc2_common`, `u2ps_dwc2_dcd`, `u2ps_dwc2_hcd` and
`u2ps_chipidea_dcd`. `common/` is `u2ps_common`, an interface library of
headers only, which every stack links. A build has the libraries of the
stacks it selects (`U2PS_DEVICE`, `U2PS_HOST`) and of their ports. The PC
build adds the test kit's libraries, `u2ps_testkit_device`,
`u2ps_testkit_host` and `u2ps_testkit_loopback`, each for the stacks it
needs. The PC build of u2ps as the top-level project, not as part of an
integration, adds five device libraries for the tests, whatever the
build's options: `u2ps_device_app` and
`u2ps_device_bus`, each in its VBUS mode, `u2ps_device_refuse_fs`, built
with `U2PS_REFUSE_FS=ON`, `u2ps_device_limits`, built with small stream
limits, and `u2ps_device_hook`, built with `U2PS_CONTROL_HOOK=ON`; all but
`u2ps_device_bus` in VBUS mode `app`. It also adds three host libraries:
`u2ps_host_plain` with the defaults, `u2ps_host_no_iad`, built with
`U2PS_HOST_ACCEPT_NO_IAD=ON`, and `u2ps_host_no_strings`, built with
`U2PS_HOST_STRINGS=OFF`. All eight have the contract checks
(`U2PS_CONTRACT_CHECKS=ON`). Two more, `u2ps_device_unchecked` and
`u2ps_host_unchecked`, are `u2ps_device_app` and `u2ps_host_plain` without
them.

The device library's headers are `device/u2ps_dport.h`, the interface
between the device stack and its port, and `device/u2ps_dev.h`, the app
API. The host library's headers are `host/u2ps_hport.h`, the interface
between the host stack and its port, and `host/u2ps_host.h`, the app API.

## Build constants and options

Each is a CMake cache variable or option, set with `-D<name>=<value>`.
The libraries' constants reach the app as public compile definitions, so
the library and the app see the same values. A numeric constant must be a
decimal integer, or the configure step stops; a value out of its range
stops the build with a message that names the constant and its range.

| Name | Default | Valid values | What it does |
|---|---|---|---|
| `U2PS_DEVICE` | `OFF` | `ON`, `OFF` | `ON`: builds the device stack, for a DAC (`u2ps_device`). A build selects at least one stack: with neither `U2PS_DEVICE` nor `U2PS_HOST` on, the configure step stops. |
| `U2PS_HOST` | `OFF` | `ON`, `OFF` | `ON`: builds the host stack, for a player (`u2ps_host`). |
| `U2PS_VBUS` | none: every build with `U2PS_DEVICE=ON` sets it | `app`, `bus` | The VBUS mode of the device stack; see "VBUS". A build without the device stack needs none. |
| `U2PS_REFUSE_FS` | `OFF` | `ON`, `OFF` | `ON`: the device never enumerates at full speed; see "Full speed". |
| `U2PS_CONTROL_HOOK` | `OFF` | `ON`, `OFF` | `ON`: the device stack calls `u2ps_app_on_control()` for every SETUP it answers. |
| `U2PS_CONTRACT_CHECKS` | `OFF` | `ON`, `OFF` | `ON`: both stacks check at run time the rules their headers state and report each violation through `u2ps_app_on_contract()`; see "Device app API". `OFF` compiles the checks to nothing. |
| `U2PS_LTO` | `OFF` | `ON`, `OFF` | `ON`: link-time optimisation of the libraries; see "Packet path: LTO and placement". |
| `U2PS_EP0_BUF` | 512 | a multiple of 32, 64 to 65504 | Bytes of the device's control buffer; every descriptor must fit it. |
| `U2PS_GAP_PAUSE` | 400 | 1 to 8191 microframes | More NO_PACKET microframes than this are a pause, not a hole. |
| `U2PS_CLOSE_MAX` | 10 | 1 to 65535 milliseconds | How long a stream's close may take before `u2ps_dev_tick()` reports `U2PS_FAULT_CLOSE`. |
| `U2PS_START_MAX` | 30 | 1 to 65535 milliseconds | How long SET_INTERFACE's status stage waits for a stream start before it completes and `u2ps_dev_tick()` reports `U2PS_FAULT_START`. |
| `U2PS_ISO_OUT_MPS_MAX` | 1024 | 1 to 1024 | The largest wMaxPacketSize of a device stream; it sizes the port's receive FIFO. |
| `U2PS_ISO_OUT_COUNT_MAX` | 16 | 2 (DWC2) or 3 (ChipIdea) to 65535 | The most buffers a device stream may have; the app holds at most that count less `U2PS_DPORT_ISO_OUT_RESERVE` (see "Device app API"). |
| `U2PS_HOST_EP0_BUF` | 1024 | a multiple of 64, 256 to 65472, at least 2 + 12 times `U2PS_HOST_SUB_RANGE_MAX` (so at least 448 with its default) | Bytes of the host's control buffer; a longer configuration descriptor ends the session. A multiple of 64, as a control IN data stage reserves whole packets of endpoint 0's size. |
| `U2PS_HOST_ALT_MAX` | 16 | 1 to 255 | The most streaming alternate settings the host keeps. |
| `U2PS_HOST_SUB_RANGE_MAX` | 32 | 1 to (`U2PS_HOST_EP0_BUF` - 2) / 12, so 85 with its default | The most sub-ranges of the clock's RANGE the host keeps. |
| `U2PS_HOST_QUEUE_MAX` | 16 | 1 to 255 | The most packets the host queues at once. |
| `U2PS_HOST_DEBT_MAX` | 8 | 1 to 8000 microframes | The most microframes' worth of audio slots a hole may leave owed. |
| `U2PS_HOST_CONTROL_MAX` | 5000 | 1 to 65535 milliseconds | How long a host control transfer may take. |
| `U2PS_HOST_ACCEPT_NO_IAD` | `OFF` | `ON`, `OFF` | `ON` accepts a device whose audio function has no Interface Association Descriptor if it has one AudioControl interface, a deviation from UAC2 4.6. |
| `U2PS_HOST_STRINGS` | `ON` | `ON`, `OFF` | `OFF`: the host reads no string descriptors. |
| `U2PS_HOST_IA_VBUS_RATED_MA` | none: every build with `U2PS_HOST=ON` sets it | 8 to 5000 mA | The current the board's VBUS supply of the host port is rated for (IA_VBUS_RATED); the host selects no configuration whose bMaxPower needs more. |
| `U2PS_HOST_REFUSE_FS` | `OFF` | `ON`, `OFF` | `ON`: the host ends the session of a full- or low-speed device at once, a deviation from USB 2.0 7.1.2.3; see "Full speed". |
| `U2PS_CHIP` | none (PC build) | `stm32f723`, `stm32u5a5`, `stm32h7s3` (host stack only), `imxrt1062` | The chip of an ARM build; see "ARM builds". |
| `U2PS_ATFE_DIR` | none | the directory of ATfE that holds `bin/clang` | Read by `cmake/arm-none-eabi-clang.cmake`, from the command line or the environment; see "ARM builds". The PC build passes it to `test_build_type_arm`. |
| `U2PS_PORT` | the chip's | `dwc2`, `chipidea` | The device port; it must match the chip. |
| `U2PS_EXAMPLES` | `OFF` | `ON` with `stm32u5a5` | Also builds the chip's example app: the NUCLEO-U5A5ZJ-Q test app. |
| `U2PS_DWC2_HSE_HZ` | 16000000 (STM32U5), 25000000 (STM32F723), 24000000 (STM32H7S3) | STM32U5 and STM32H7S3: 16, 19.2, 20, 24, 26 or 32 MHz; STM32F723: 12, 12.5, 16, 24 or 25 MHz, in Hz | The HSE frequency the USB PHY takes its reference from. |
| `U2PS_DWC2_CORE_HZ` | 160000000 (STM32U5), 216000000 (STM32F723), 600000000 (STM32H7S3) | above 30 MHz (OTG_HS needs an AHB clock above it), at most 160 MHz (STM32U5), 216 MHz (STM32F723) or 600 MHz (STM32H7S3), in Hz | The highest core clock the app runs at; the DWC2 port bounds its waits with it. |
| `U2PS_DWC2_FORCE_FS` | `OFF` | `ON`, `OFF` | `ON`: the DWC2 device enumerates at full speed only, for bench checks. |
| `U2PS_DWC2_OUT_FOLLOW` | `ON` | `ON`, `OFF` | `OFF`: the data endpoint's parity follow starts off, for bench comparisons. |
| `U2PS_TEST_INJECT` | `OFF` | `ON` in a test build only | The DWC2 port's injection hook; a `Release` or `MinSizeRel` build stops the configure step. |
| `U2PS_TEST_OLD_RULE` | `OFF` | `ON` with `U2PS_TEST_INJECT=ON` only | The re-arm rule before B-next, for the comparison. |
| `U2PS_CHIPIDEA_FN_UNIT` | `none` | `none`, `uframes`, `frames` | The unit of the ChipIdea dTD's Frame Number field; see "ARM builds". |


## Requirements

- CMake 3.22 or later and Ninja.
- For the PC build, GCC with AddressSanitizer and UndefinedBehaviorSanitizer.
- For the ARM builds, Arm Toolchain for Embedded (ATfE), Arm's LLVM
  toolchain (clang, the lld linker, llvm-ar and picolibc), version 23.1.0:
  the project's ARM toolchain, which builds its board images and its
  published measurements. The GNU Arm toolchain (`arm-none-eabi-gcc`,
  checked with 14.2) on the PATH builds them too; the checks build with
  both.

## Build and test on a PC

The PC build uses the host compiler. It builds the common, device and host
libraries of the stacks it selects, the test kit and the tests, all with
AddressSanitizer and UndefinedBehaviorSanitizer. The tests of u2ps need
both stacks; a build of one stack builds and runs that stack's tests only.
Of the ports it builds only the decision logic of the DWC2 and ChipIdea
device ports and of the DWC2 host port, which has no register access, and
the DWC2 chip facts and start-up values; the tests use the test kit's fake
ports. `test_dwc2_phy_*` runs the DWC2 start-up's register code
(`port/dwc2/common/u2ps_dwc2_phy.c`) against a register model, and
`test_dwc2_chip_unknown` and `test_dwc2_chip_two` check that a DWC2 build
with an unknown chip, or with two chips, stops at the chip header's
`#error`. The host library takes the fake host
port's configuration header, `testkit/u2ps_hport_cfg.h`.
Each test is an executable that CTest runs; a failed check or any sanitizer
report fails the test, and so does a contract violation a test does not
expect. Some tests are CMake scripts instead. `agents_sync` checks
`AGENTS.md` against the app headers both ways; its rule is at the top of
`tests/check_agents_sync.cmake`. It passes while the guide and the headers
agree, and fails, naming each symbol, when they do not. `test_agents_sync`
checks that check on scratch guides. `agents_citations` checks that every
`path:line` citation of `AGENTS.md` names a file of the tree and lies
within it (`tests/check_agents_citations.cmake`), and
`test_agents_citations` checks that check. `test_stack_selection` configures the
tree with each choice of stacks and checks which configures stop and which
libraries the others build. `test_build_type` checks the build type u2ps
sets, and `test_build_type_arm` the optimisation of the ARM toolchain
files (see "Optimisation: ATfE -Os, GCC -O2"); the latter needs ATfE,
named by `-DU2PS_ATFE_DIR=<atfe-dir>` or the environment, and
`arm-none-eabi-gcc`, and is skipped without them. The tests named
`pc_testkit_*` are the test kit's worked example (see "Test your
integration on the PC").

```
cmake -S . -B build/pc -G Ninja -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/pc
ctest --test-dir build/pc
```

The last command reports `100% tests passed`. The sync check also runs
alone, without a build: `cmake -DU2PS_ROOT=. -P tests/check_agents_sync.cmake`.

## Test your integration on the PC

The test kit in `testkit/` lets an integrator test its own app against
u2ps on the PC, without a board. The test links one kit library instead of
the stack's: `u2ps_testkit_device` for a DAC app, `u2ps_testkit_host` for
a player app against a scripted UAC2 device, or `u2ps_testkit_loopback`
for a player app against the u2ps device stack through a bus that steps
microframes. Each holds the stack built with the build's options and the
contract checks, and a fake port. The test plays the other side of the
bus through the kit: it enumerates the device, sets a sampling frequency
and an alternate setting, injects packets in chosen microframes and holes
of each reason, or plugs in a device and steps the time. The kit records
every callback of the app in order with its arguments, every contract
violation and every violation of the port interface, and
`u2ps_testkit_result()` gives the test's verdict. The headers are
`testkit/u2ps_testkit.h` (checks, the trace, the verdict, the time),
`testkit/u2ps_testkit_dev.h`, `testkit/u2ps_testkit_host.h` and
`testkit/u2ps_testkit_loopback.h`. The kit is part of the PC build only.

`examples/pc_testkit/` is a worked example to copy: a minimal DAC app with
its test, and a minimal player app with a test against the scripted
device and one through the loopback. It builds as an integration does,
as a project that adds u2ps with `add_subdirectory()`:

```
cmake -S examples/pc_testkit -B build/pc_testkit -G Ninja \
  -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/pc_testkit
ctest --test-dir build/pc_testkit --output-on-failure
```

`ctest` reports `100% tests passed, 0 tests failed out of 3`, and each test
ends with a line such as
`u2ps testkit: PASS: 29 checks, 0 failed, 0 contract violations, 0 port violations`.
`AGENTS.md` describes the kit's calls in its section "Test your
integration on the PC".

## ARM builds

An ARM build uses a toolchain file and needs `U2PS_CHIP`. It produces
static libraries only, with no startup code and no linker script. There
are two toolchain files, with the same CPU flags for each chip:

- `cmake/arm-none-eabi-clang.cmake`, Arm Toolchain for Embedded (ATfE),
  the project's toolchain. `U2PS_ATFE_DIR` names the directory of ATfE,
  the one that holds `bin/clang` (`<atfe-dir>`, such as a directory
  `ATfE-23.1.0-Linux-x86_64`), with `-DU2PS_ATFE_DIR=<atfe-dir>` or in
  the environment; without it the configure step stops and names it. The file
  takes clang, lld and the LLVM binutils from that directory and gives
  clang the chip's target triple; clang picks its C library from the CPU
  flags.
- `cmake/arm-none-eabi.cmake`, `arm-none-eabi-gcc` on the PATH.

| `U2PS_CHIP` | Chip | CPU flags | Target triple (ATfE) | Device port |
|---|---|---|---|---|
| `stm32f723` | STM32F723 | `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard` | `thumbv7em-none-eabihf` | `dwc2` |
| `stm32u5a5` | STM32U5A5 | `-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard` | `thumbv8m.main-none-eabihf` | `dwc2` |
| `stm32h7s3` | STM32H7S3 | `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard` | `thumbv7em-none-eabihf` | `dwc2` (host stack only) |
| `imxrt1062` | i.MX RT1062 | `-mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard` | `thumbv7em-none-eabihf` | `chipidea` |

With ATfE:

```
cmake -S . -B build/f723 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake \
  -DU2PS_ATFE_DIR=<atfe-dir> -DU2PS_CHIP=stm32f723 -DU2PS_DEVICE=ON -DU2PS_HOST=ON \
  -DU2PS_VBUS=app -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/f723

cmake -S . -B build/u5a5 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake \
  -DU2PS_ATFE_DIR=<atfe-dir> -DU2PS_CHIP=stm32u5a5 -DU2PS_DEVICE=ON -DU2PS_HOST=ON \
  -DU2PS_VBUS=app -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/u5a5

cmake -S . -B build/rt1062 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake \
  -DU2PS_ATFE_DIR=<atfe-dir> -DU2PS_CHIP=imxrt1062 -DU2PS_DEVICE=ON -DU2PS_HOST=ON \
  -DU2PS_VBUS=app -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/rt1062

cmake -S . -B build/h7s3 -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake \
  -DU2PS_ATFE_DIR=<atfe-dir> -DU2PS_CHIP=stm32h7s3 -DU2PS_HOST=ON \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/h7s3
```

The STM32H7S3 build has the host stack only: with `-DU2PS_DEVICE=ON` its
configure step stops, names the errata ES0596 2.21.1 and 2.21.3, and
gives the fix, `-DU2PS_HOST=ON -DU2PS_DEVICE=OFF`.

With GCC, the same commands with
`-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` and without
`U2PS_ATFE_DIR`:

```
cmake -S . -B build/u5a5-gcc -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
  -DU2PS_CHIP=stm32u5a5 -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/u5a5-gcc
```

`U2PS_PORT` (`dwc2` or `chipidea`) names the port that the device library
links. It defaults to the chip's controller, and the configure step stops if
it names another one. The host library builds for every chip; on a DWC2
chip it links the DWC2 host port and takes its configuration header,
`port/dwc2/u2ps_hport_cfg.h`. The i.MX RT1062 has no host port yet; its
host library takes the ChipIdea host port's constants from
`port/chipidea/u2ps_hport_cfg.h`.
A DWC2 build also defines the
chip's macro (`U2PS_CHIP_STM32F723`, `U2PS_CHIP_STM32U5A5` or
`U2PS_CHIP_STM32H7S3`; `port/dwc2/common/u2ps_dwc2_chip.h` holds each
chip's facts, and a build without one of them stops at its `#error`) and puts
the port's configuration header, `port/dwc2/u2ps_dport_cfg.h`, on the
include path of the device library and of the app that links it. The
i.MX RT1062 build puts `port/chipidea/u2ps_dport_cfg.h` there. Its option
`U2PS_CHIPIDEA_FN_UNIT` (`none`, `uframes` or `frames`; default `none`)
names the unit of the dTD's Frame Number field, which the reference manual
does not state. With `none` the port finds holes from FRINDEX alone, and
only with `uframes` are its microframe numbers exact.

Every build compiles with `-Wall -Wextra -Werror` and a few further warnings,
with either compiler.
u2ps as the top-level project builds `RelWithDebInfo` unless
`CMAKE_BUILD_TYPE` says otherwise. Added to another project, it keeps that
project's build type; when the type is empty, the configure step prints
that u2ps's libraries build without optimisation.

### Optimisation: ATfE -Os, GCC -O2

ATfE builds default to `-Os`, GCC builds to `-O2`. The ATfE toolchain
file sets `RelWithDebInfo` to `-Os -g -DNDEBUG` as a cache default;
`-DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG"` on the command line
overrides it. With the GCC toolchain file, and in the PC build,
`RelWithDebInfo` keeps CMake's `-O2 -g -DNDEBUG`. A given build type keeps
CMake's flags for it with either compiler: `Debug` `-g`, `Release` `-O3
-DNDEBUG`, `MinSizeRel` `-Os -DNDEBUG`. The injection build
(`U2PS_TEST_INJECT=ON`) is a `RelWithDebInfo` or `Debug` build, so with
ATfE it is `-Os` by default too. Published numbers show ATfE `-Os` and GCC
`-O2` side by side.

The handler's time: measured on 2026-11-09 on a NUCLEO-U5A5ZJ-Q
(STM32U5A5 at 160 MHz), with the example app built from the tree of that
day with `U2PS_LTO=OFF`. A Linux host played 12 s each of 192 kHz and
768 kHz 32-bit and 768 kHz 24-bit, then ten 1 s streams of 768 kHz 32-bit;
no stream had a hole. ATfE's `-O2` and `-Oz` came from
`CMAKE_C_FLAGS_RELWITHDEBINFO`. The cycles of the handler's pass for an OUT
packet (mean and maximum over all streams) come from the injection build's
pass profile; `isr_pct` is the handler's share of the core, from the
example's `stream_stats` lines:

| Compiler and option | OUT pass, mean / max cycles | `isr_pct` |
|---|---|---|
| ATfE 23.1.0, `-O2` | 671 / 798 | 3.86 % |
| ATfE 23.1.0, `-Os` (the default) | 690 / 722 | 4.01 % |
| ATfE 23.1.0, `-Oz` | 733 / 759 | 4.20 % |
| GCC 14.2, `-O2` (the default) | 624 / 638 | 3.60 % |

Code size: measured on 2026-11-11 from the 0.1.0 tree, no board involved.
The `stm32u5a5` build of the device stack alone (`-DU2PS_DEVICE=ON
-DU2PS_VBUS=app -DU2PS_EXAMPLES=ON`, `RelWithDebInfo`, `U2PS_LTO=OFF`, no
injection hook); the library text is the sum of the text of
`libu2ps_device.a`, `libu2ps_dwc2_dcd.a` and `libu2ps_dwc2_common.a`, as
the compiler's own `size -t` gives it for each archive (`llvm-size` for
ATfE, `arm-none-eabi-size` for GCC), and the example's text that of its
ELF:

| Compiler and option | Text of the three libraries | Text of the example app |
|---|---|---|
| ATfE 23.1.0, `-O2` | 20,880 bytes | 95,364 bytes |
| ATfE 23.1.0, `-Os` (the default) | 17,516 bytes | 33,148 bytes |
| ATfE 23.1.0, `-Oz` | 15,578 bytes | 25,452 bytes |
| GCC 14.2, `-O2` (the default) | 17,009 bytes | 35,148 bytes |

### DMA memory

The controller's DMA reads and writes some memory itself: the stacks'
control buffers and the ports' own memory, in section `.bss.u2ps_dma`
(`U2PS_DMA_SECTION`), the device stack's class memory and the host
stack's packets. The app's linker script places `*(.bss.u2ps_dma)` in RAM
the DMA reaches, ahead of its `*(.bss .bss.*)` rule; without a rule of its
own, that rule takes it.

| Chip | Memory the DMA reaches | Also checked |
|---|---|---|
| STM32U5A5 | SRAM1, SRAM2, SRAM3, SRAM5 from 0x2000_0000 (SRAM6 on the U5Fx/5Gx); not SRAM4, not the backup SRAM | each SRAM's clock enable in RCC and its power-down bit in `PWR_CR1` |
| STM32F723 | DTCM, SRAM1, SRAM2 from 0x2000_0000; not ITCM RAM | the DTCM clock enable, `RCC_AHB1ENR.DTCMRAMEN` |
| STM32H7S3 | AXI SRAM1 to SRAM4 from 0x2400_0000 as the option bytes map them, AHB SRAM1 and SRAM2 at 0x3000_0000; not DTCM, ITCM or the backup SRAM. AXI SRAM2, 0x2402_0000 to 0x2403_FFFF, is always mapped and has no ECC. | `FLASH_OBW2SR`: `ITCM_AXI_SHARE` maps AXI SRAM1, `DTCM_AXI_SHARE` AXI SRAM3 (an undefined value refuses that SRAM), `ECC_ON_SRAM` = 0 for AXI SRAM4; each AHB SRAM's clock enable in `RCC_AHB2ENR` (off at reset) |
| i.MX RT1062 | not stated in the reference manual | nothing: the ChipIdea port has no register code |

`u2ps_dev_init()` and `u2ps_host_init()` check every byte of the section's
memory against this rule before anything else, at every init, and return
`U2PS_DEV_ERR_DMA_MEMORY` or `U2PS_HOST_ERR_DMA_MEMORY` if one lies
elsewhere; nothing changes then. The DWC2 port applies the same rule to
class memory at each stream's open. With `-DU2PS_CONTRACT_CHECKS=ON` the
host stack also checks every byte of each queued packet against the
ranges the host port reports for the rule at init
(`u2ps_hport_dma_ranges()`), and reports a packet outside them as
`U2PS_CONTRACT_HOST_QUEUE_DATA`; on the STM32H7S3 that catches a packet in
DTCM, the STM32F723's usual DMA memory. On the STM32U5 every SRAM is clocked
and powered at reset; an app that switches one off must keep the DMA's
memory out of it. The stacks and the ports zero their memory in the
section at init, so the section may be `NOLOAD` and the startup code need
not zero it. A linker script may also check the placement at link time,
for example on the STM32U5A5, with the section in an output section of
its own:

```
.u2ps_dma (NOLOAD) : ALIGN(32) { *(.bss.u2ps_dma) } > RAM
ASSERT(ADDR(.u2ps_dma) >= 0x20000000 &&
       ADDR(.u2ps_dma) + SIZEOF(.u2ps_dma) <= 0x20270000,
       "u2ps: .bss.u2ps_dma must lie in SRAM1, SRAM2, SRAM3 or SRAM5")
```

### Packet path: LTO and placement

`-DU2PS_LTO=ON` compiles the libraries with the compiler's link-time
optimisation, through CMake's `INTERPROCEDURAL_OPTIMIZATION`. Their
objects then hold the compiler's intermediate code only, so the app's link
must run LTO with the same compiler; each library passes the LTO flag on
to the link of the target that links it.

| Toolchain | Compile | Archive | The link gets | Linker |
|---|---|---|---|---|
| ATfE | `-flto=thin` (ThinLTO) | `llvm-ar` | `-flto=thin` | lld, which runs the LTO |
| GCC | `-flto=auto -fno-fat-lto-objects` | `gcc-ar` | `-flto=auto` | GNU ld with GCC's LTO plugin |

A link's `--wrap` of a u2ps function does not reach the calls inside
the optimised libraries; the example app's timing of masked regions is off
in such a build (`mask_max=none`). Measured on 2026-11-08 on the
NUCLEO-U5A5ZJ-Q, with the example built with GCC 14.2 (`-O2`) from the tree
of that day, before the packet-path changes below, with the whole example
under LTO: the handler's pass for an OUT packet took 637 instead of 869
cycles on average (-27 %), from the injection build's pass profile.

The packet path is in two named sections, so that a linker script can put
it into tightly coupled memory with one rule each: `.text.u2ps_fast` holds
the device port's pass for an OUT packet and everything it calls, the
feedback endpoint's passes and the release (`U2PS_FAST_TEXT` in
`common/u2ps_types.h`); `.bss.u2ps_fast` holds the port's and the device
stack's state, zero-initialised (`U2PS_FAST_BSS`). Both survive LTO. A
linker script without a rule for them takes them with its
`*(.text .text.*)` and `*(.bss .bss.*)` rules, as the example's does: the
STM32U5 has no TCM for them to go to. On the STM32F723 they take, in a
link of the device stack from the 0.1.0 tree (`AGENTS.md` describes the
link; measured on 2026-11-11), built with ATfE (`-Os`), 4,284 bytes of ITCM
(4,124 bytes of code and 160 bytes of linker veneers for the calls to
flash) and 508 bytes of DTCM, 4,428 and 508 bytes with `U2PS_LTO=ON`;
built with GCC (`-O2`),
4,432 (128 of them veneers) and 472 bytes, 4,656 and 472 bytes with
`U2PS_LTO=ON`. `AGENTS.md` gives the rules.
Rare events (bus reset, EP0, the close, the feedback's recovery) stay out
of line and out of these sections.

A successful ARM build looks like this. The configure step prints
`-- u2ps: ARM build for stm32f723, device port dwc2` (with the chip and
port it builds for; the `imxrt1062` build first prints that its device port
has no register code yet; a build without the device stack prints
`port` instead of `device port`), then `-- u2ps: stacks: device host`,
and ends with
`-- Build files have been written to: build/f723`. The build prints no
warning, since every warning stops it, ends with a line such as
`[21/21] Linking C static library port/dwc2/dcd/libu2ps_dwc2_dcd.a` (the
count and the last library vary), and exits with status 0. A DWC2 build
writes `libu2ps_device.a`, `libu2ps_host.a`, `libu2ps_dwc2_common.a`,
`libu2ps_dwc2_dcd.a` and `libu2ps_dwc2_hcd.a`; the `imxrt1062` build
writes `libu2ps_device.a`, `libu2ps_host.a` and `libu2ps_chipidea_dcd.a`. A build of one stack
writes that stack's library and its port's only.

## Device app API

`device/u2ps_dev.h` is the app API of the device stack. The app describes
its audio function once in `u2ps_dev_description_t`: formats, the clock's
sampling frequencies, the initial one (`initial_sampling_frequency`, one
of them, or 0 for the first), terminal type and strings. The stack builds
every descriptor from it. Each format, one streaming alternate setting, is
Type I PCM or IEEE_FLOAT (`u2ps_audio_data_format_t`, UAC2 Audio Data
Formats 2.3.1.7); IEEE_FLOAT is 32 bits in 4-byte subslots. The app learns
the format of a stream when it starts.

Public names follow the specifications: a field that lands in a descriptor
field takes that field's name without its type prefix, in snake_case
(`bSubslotSize` is `subslot_size`, `idVendor` is `id_vendor`); without a
single field, the specification's term for the concept
(`audio_data_format`, `sampling_frequency`); a unit the stack adds stays
in the name (`max_power_ma`).

- `u2ps_dev_init()` checks the description, resets the stack and starts
  the port. It returns `U2PS_DEV_OK` or a reason of `u2ps_dev_err_t`:
  the description, or the port's reason (clock, power, security such as
  TrustZone, PHY, reset, controller mode, memory), which each port maps onto
  these. After a refused description, `u2ps_dev_description_error()`
  names the rule broken (`u2ps_dev_description_error_t`, each value with its
  fix). It may be called again while detached to change the description;
  once the port runs, such a re-init skips the hardware start-up. Called
  while attached, it detaches first. The clock keeps its sampling
  frequency across a re-init if the new description lists it.
- `u2ps_dev_connect()` allows and ends the attach. A detach ends a stream
  and the configuration as a bus reset does; a second one changes nothing.
- `u2ps_dev_speed()` gives the speed of the last bus reset since the
  attach.
- The stream's buffers: the app lends `count` buffers of class memory
  (`u2ps_stream_cfg_t`), and the port keeps `U2PS_DPORT_ISO_OUT_RESERVE`
  of them armed (1 on DWC2, 2 on ChipIdea), because neither controller
  receives an isochronous packet without one. So the app holds at most
  `count` less that many packets at a time, and a packet that comes while
  it does is a NO_BUFFER hole of its own microframe: every packet the
  host sends is delivered or counted, and a host pause during a hold
  stays NO_PACKET. To hold k packets, lend k +
  `U2PS_DPORT_ISO_OUT_RESERVE` buffers. The holes after the
  stream's last packet, up to the last packet the port received, come
  before `u2ps_app_on_stream_stop()`, and a hole longer than 65,535
  microframes comes in several calls. They do not wait for the stream's
  end: once no packet has been delivered for more than `U2PS_GAP_PAUSE`
  microframes' worth (50 ms), each `u2ps_dev_tick()` reports the holes
  held back so far, so that the holes of a hold reach the app while the
  host pauses; each hole is reported once.
- The app's callbacks run in the port's interrupt, except those that the
  table at the top of `device/u2ps_dev.h` lists, which also run in the
  app's thread with the USB vector masked; the table names the calls each
  callback may make. `u2ps_dev_init()`, `u2ps_dev_connect()`,
  `u2ps_dev_vbus()`, `u2ps_dev_reply()`, `u2ps_dev_tick()` (at least every
  10 ms) and the other thread calls all run in one thread.
  `u2ps_app_on_configured()` reports entering and leaving the Configured
  state, and why it left: a bus reset, SET_CONFIGURATION 0, a detach, a
  VBUS loss or a re-init. `u2ps_app_on_stream_start()` runs before the port
  enables the stream's endpoints, which it does only if the app accepts.
  `u2ps_app_on_sampling_frequency()` gets the sampling frequency the host
  asked for and the one the stack chose. Suspend is reported only after a
  bus reset since the pull-up went on.
- Optional: built with `-DU2PS_CONTROL_HOOK=ON`, the stack calls
  `u2ps_app_on_control()` with every SETUP it answers and the result
  (the data stage's length, or a request error), for logging. Without
  it, the app does not define that callback.
- Optional: built with `-DU2PS_CONTRACT_CHECKS=ON`, the device and the
  host stack check at run time the rules their headers state that they can
  see: thread calls inside callbacks or nested in each other, the tick's
  period while a bound is timed, a description changed after init,
  releases and queues the header forbids, replies and callback results it
  does not list, an attach before a successful init. Each violation is
  reported once through `u2ps_app_on_contract()` with a code of
  `u2ps_contract_t` (`common/u2ps_types.h`), which names the rule and its
  fix; the stack then goes on as without the checks. Use it while
  integrating and in tests; without it the checks compile to nothing and
  the app does not define the hook.

The header holds the details: contexts, the order of the callbacks, and
the counters of `u2ps_dev_stats()`.

## VBUS

USB 2.0 lets the pull-up have power only while VBUS is present
(7.1.5.1) and requires it off within 10 s of a VBUS loss (7.2.1). The
stack puts the pull-up on only while the app allows the attach and VBUS is
present. How it learns of VBUS is the VBUS mode, the CMake option
`U2PS_VBUS`, which has no default:

- `app`: the app reads VBUS, debounces it, and reports the level with
  `u2ps_dev_vbus()` on every poll, from the thread that calls the stack's
  other thread calls. VBUS counts as absent until the first report, which
  may come before `u2ps_dev_init()`. A loss ends a stream and the
  configuration as a detach does, then calls `u2ps_app_on_vbus(false)`;
  while VBUS is absent the stack ignores every event the port reports. A
  stalled poll leaves the pull-up as the last report set it.
- `bus`: a bus-powered device whose pull-up takes its supply from VBUS.
  VBUS is always present, and `u2ps_dev_init()` refuses a self-powered
  description.
- `port`: the port senses VBUS itself. Not available yet; the configure
  step stops.

A bus-powered device must signal attach within 100 ms of VBUS (TSIGATT,
7.1.7.3), so it calls `u2ps_dev_connect(true)`, and in mode `app` reports
VBUS, within 100 ms of power-up. It also keeps to one unit load until
configured (7.2.1) and to the suspend current (7.2.3); both are the app's
duties.

## Example app

`examples/nucleo_u5a5zj_q/` is a test app for the NUCLEO-U5A5ZJ-Q: a UAC2
sink with 2 channels, five formats (four PCM, then 32-bit float as
alternate setting 5) and the sampling frequencies 44.1 to 768 kHz, as VID
0x16C0 PID 0x27E0, the shared pair for audio class devices in Objective
Development's USB-IDs-for-free.txt. Its rules ask for a serial number that
starts with the distributor's domain or e-mail address: the example builds
it from the CMake option `U2PS_EXAMPLE_SERIAL_PREFIX` and the chip's unique
ID, so set your own prefix before you distribute a build. It runs in VBUS
mode `app`: it
reads VBUS on PC2 with the ADC, takes a new level once it has held for
50 ms, and reports it on every pass of its main loop; VBUS reads absent if
the ADC did not start. It counts packets, bytes, holes and pauses, releases
every packet at once, and reports on the board's ST-LINK virtual COM port
at 115200 8N1. Each line is one event in a stable form: the event's name,
then `key=value` pairs in a fixed order, for example
`configured speed=high` or `fault fault=open code=1 cause=stride_align`;
the header comment of `examples/nucleo_u5a5zj_q/main.c` lists every event
with its keys and units. The app writes a line at each stream's start and
stop, at each change of the configuration (with the reason it ended) and of
VBUS, a `stream_stats` line per second while streaming, and answers to the
commands `version`, `stats`
(the counters at any time), `vbus` (the level the stack knows and the
reading in mV), `diag` (the
port's feedback start record, IISOIXFR by microframe, close times and EP0
counters), `reconnect` (detach for 100 ms, then attach again),
`disconnect` and `connect`, `reinit a|b|bad` (initialise the stack again
while detached with the boot description, one with bcdDevice 0x0101 and
an initial sampling frequency of 48 kHz, or one it must refuse; the line
after it gives
the result, for a refused description the rule broken, and the call's time
in core cycles), `speed`, `clock`
(the core clock counted against the host's SOFs), `mode` (the port's
switches for comparisons), `fb` (a fixed feedback value), `defer <ms>|off`
(each stream start deferred and opened that many milliseconds later; by
default the app opens at once), `canary` (the class memory tail probe) and
`help`. Built with `-DU2PS_CONTRACT_CHECKS=ON`, it also writes a
`contract` line for each kind of contract violation the stack reported.
Each buffer of its class memory ends with a canary word, and its
stride stays a multiple of `U2PS_DPORT_ALIGN`, as the port requires
(`examples/nucleo_u5a5zj_q/app_class_mem.h`). The `stream_stats` line carries the
interrupt load of each second: `isr_max`, the longest pass of the USB
handler in core cycles, `isr_pct`, the handler's share of the second,
and `mask_max`, the longest time the device stack masked the USB vector
from thread. It builds with `-DU2PS_EXAMPLES=ON` in the `stm32u5a5` ARM
build, which sets `U2PS_DEVICE=ON` and `U2PS_VBUS=app` unless the command
line sets them (`U2PS_DEVICE=OFF` or another VBUS mode stops the
configure step); it may also build the host library with
`-DU2PS_HOST=ON -DU2PS_HOST_IA_VBUS_RATED_MA=<mA>`:

```
cmake -S . -B build/u5a5-example -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-clang.cmake \
  -DU2PS_ATFE_DIR=<atfe-dir> -DU2PS_CHIP=stm32u5a5 -DU2PS_EXAMPLES=ON
cmake --build build/u5a5-example
```

The build writes `u2ps-test-nucleo_u5a5zj_q.elf`, `.bin` (for flash at
0x08000000) and `.hex` in `build/u5a5-example/examples/nucleo_u5a5zj_q/`.
With ATfE it links with lld and picolibc, ATfE's default C library: the
app takes only `memcpy`, `memset` and `__libc_init_array` from it, and its
linker script gives picolibc's `__libc_init_array` the bounds it reads
(`__bothinit_array_start`, `__bothinit_array_end`). With
`-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` (and no
`U2PS_ATFE_DIR`) it builds with `arm-none-eabi-gcc`, GNU ld and
newlib-nano (`--specs=nano.specs`). Both use ST's startup file for GCC,
which clang assembles as it is, and the same linker script. The image's
text with each compiler and option is in the second table of
"Optimisation: ATfE -Os, GCC -O2"; ATfE at `-O2` gives almost three times
as much as at `-Os`, most of it in the app's own logging code in `main.c`,
which clang inlines more at `-O2`.
It downloads Arm's CMSIS core headers and ST's CMSIS device files for the
STM32U5 at configure time, pinned to fixed commits and checked by SHA-256;
no vendor code is kept in the tree. The image runs with TrustZone disabled
(option byte TZEN = 0); with TZEN = 1 the port refuses to start and the app
reports `init result=failed reason=security code=4 port_reason=tzen ...`.

The DWC2 device port arms a stream's data endpoint and its feedback
endpoint for microframes of one parity at a time. From the stream's open,
each endpoint's parity follows the bus at every EOPF, the data endpoint's
until its first packet and the feedback endpoint's until its first poll,
with no time limit: Linux polls the feedback endpoint up to about 240 ms
after SET_INTERFACE at high rates. The follow costs one EOPF pass of the
handler per microframe until then, and for the whole stream if the host
never polls the feedback endpoint. The cycles of that pass are still to
be measured (`eopf` in the `passes` line of an injection build, below).

Options of the DWC2 port for bench checks: `-DU2PS_DWC2_FORCE_FS=ON` makes
the device enumerate at full speed only; `-DU2PS_DWC2_OUT_FOLLOW=OFF`
starts with the data endpoint's parity follow off, so that the parity
guessed at a stream's open stays until the first packet, as before the
follow (the example's `mode out 0|1` switches it at run time, and `diag`
shows `out_follows` and `out_follow_late`, the follow writes that landed
too late); and `U2PS_DWC2_HSE_HZ` (default
16000000 for the STM32U5, 25000000 for the STM32F723, 24000000 for the
STM32H7S3) gives the HSE frequency that the USB PHY takes its reference
from. `U2PS_DWC2_CORE_HZ` (default 160000000 for the STM32U5, 216000000
for the STM32F723, 600000000 for the STM32H7S3) is the highest core clock
the app runs at; the port bounds its waits in microseconds with the
core's cycle counter at that clock. On a Cortex-M7 (STM32F723,
STM32H7S3) the port unlocks the DWT before it enables the counter, and
checks that it counts; if not, init returns `U2PS_DEV_ERR_TIMER` or
`U2PS_HOST_ERR_TIMER` and starts nothing.

Test builds, never a release: `-DU2PS_TEST_INJECT=ON` builds the
injection hook, with the example's commands `inject <every> <us>` (every
nth re-arm of the stream that would be in time is held until the next
microframe plus that many microseconds), `ack <every>` (a lost ACK on
every nth control read) and `passes` (the handler's time by kind of
pass). `-DU2PS_TEST_OLD_RULE=ON`, only with it, arms the stream by the
rule before B-next, for the comparison. `-DU2PS_EXAMPLE_STALL=ON` builds
the stall mode (`examples/nucleo_u5a5zj_q/app_stall.h`): in the last
second of every 3 s of a stream the app holds every packet it may hold,
count - `U2PS_DPORT_ISO_OUT_RESERVE`, and then releases them all, so each
packet the host sends meanwhile is a NO_BUFFER hole. After each
`stream_stop` line it writes a `stall` line with the stream's delivered
packets, its holes by reason, its tail (the holes after its last
packet) and its pauses, and in an injection build the OUT passes between
stalls and during them. The version line names the test options a build
has.

## Full speed

A high-speed device must also attach and answer at full speed. By default the
device stack enumerates at full speed with a minimal configuration that holds
only the audio function's AudioControl interface, so nothing can stream, and
the device_qualifier and other_speed_configuration descriptors describe the
other speed. Configured with `-DU2PS_REFUSE_FS=ON`, the device stack stalls
every GET_DESCRIPTOR at full speed, so the device never enumerates there, and
stalls device_qualifier and other_speed_configuration at high speed.

A host's port must support low, full and high speed (USB 2.0 7.1.2.3). The
host stack streams at high speed only, but by default it enumerates a full-
or low-speed device far enough to identify it: the device descriptor, with
endpoint 0's packet size of that speed, and the configurations; if none
of them holds a playable audio function, also the device_qualifier and, if
the device gives one, its other-speed configurations (USB 2.0 9.6.2,
9.6.4). It then ends the session with `U2PS_HOST_END_SPEED` if one of them
holds a playable audio function, and with `U2PS_HOST_END_NOT_PLAYABLE`
otherwise. So a high-speed DAC that, like the u2ps device stack, gives a
full-speed configuration without a streaming interface ends with
`U2PS_HOST_END_SPEED`. A device that does not answer ends with
`U2PS_HOST_END_TIMEOUT` or `U2PS_HOST_END_ERROR`. The USB-IF test device runs its test at full speed
too. Configured with `-DU2PS_HOST_REFUSE_FS=ON`, the host stack ends every
full- or low-speed session at once with `U2PS_HOST_END_SPEED`, before any
request: a documented deviation.

## Tools

`tools/cyncap/` captures the USB traffic between a host and a device with a
Great Scott Gadgets Cynthion that runs its analyzer gateware, without a GUI,
for coding agents and CI. It writes a pcap file that Packetry and Wireshark
read, and `cyncap_summary.py` prints a text summary of one. It needs Python
3 and, for the capture only, the pip package `libusb1`; its README gives
the setup, the usage and the limits. Its self-test needs no hardware; the PC
build runs it as the CTest test `cyncap_selftest` when CMake finds Python 3,
and skips it otherwise.

## License

u2ps is released under the MIT License; see `LICENSE`.

## How it was written

u2ps is written with [Claude Code](https://claude.com/claude-code). Claude
Opus 5.5 did the design, the code, the tests and the board work; Claude
Sonnet 5.5 did lookups and documents in the early days. Claude Fable 5.2
made an independent review before release 0.1.0, and OpenAI Codex a second
one.
