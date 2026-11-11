<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Mete Balci -->
# AGENTS.md: integrating u2ps

This guide tells a coding agent how to add a USB Audio Class 2.0 (UAC2) playback stack to a firmware project: u2ps as a DAC (the device stack) or as a player (the host stack). It describes u2ps 0.1.0 (`CMakeLists.txt:18`, `README.md:43`).
Every fact carries its source as `path:line`, relative to the root of the u2ps tree.
If a fact here and the source disagree, the source wins; report the difference.
The app APIs and the port interfaces are unstable: they may change in any release before 1.0.0 (`device/u2ps_dev.h:4-5`, `host/u2ps_host.h:4-5`, `README.md:110-112`), so pin the release of u2ps you integrate.
`tests/check_agents_sync.cmake` checks this guide against the app headers, and `tests/check_agents_citations.cmake` that every `path:line` citation names a file of the tree and lies within it (section 6).

Terms: the *app* is your firmware. A *callback* is a `u2ps_app_*` function that you define. A *call* is a `u2ps_dev_*` or `u2ps_host_*` function that you call. *Class memory* is the buffers that the app lends to a device stream (`device/u2ps_dev.h:389-413`). *DMA memory* is memory the controller's DMA can reach. A *microframe* is 125 us; `u2ps_uframe_t` holds a microframe number modulo 16384 (`common/u2ps_uframe.h:6-15`, `common/u2ps_uframe.h:23`). A *hole* is microframes of a stream without a packet, with a reason `u2ps_miss_t` (`common/u2ps_types.h:30-57`).

## 1. What u2ps is, and what runs today

- u2ps is a playback-only UAC2 stack in C for USB 2.0 High-Speed (`README.md:5-9`). The device stack is a UAC2 sink for DACs; the host stack plays to a UAC2 device (`README.md:6-8`).
- It needs only a C11 compiler plus `memcpy`, `memmove`, `memset` and `memcmp` (`README.md:20-22`).
- It does not do recording, UAC1, streaming at full speed, other USB classes, composite devices, hubs on the host side, 1.5 MHz PCM or DSD512 as DoP (`README.md:34-39`). At full speed the device enumerates with a minimal configuration that cannot stream (`device/u2ps_dev.h:112-118`). The host stack identifies a full- or low-speed device and ends its session, and runs the USB-IF test device at any speed (section 4.4, `host/u2ps_host.h:53-59`).
- One port (controller driver) is chosen per build: DWC2 or ChipIdea (`README.md:43-47`, `CMakeLists.txt:35-38`).

| Target | Device stack | Host stack |
|---|---|---|
| STM32U5A5 (DWC2) | Runs on the NUCLEO-U5A5ZJ-Q with a Linux host: enumeration, 100 re-enumerations, forced full speed, streams up to 768 kHz, and USB3CV's Chapter 9 tests at High-Speed and at full speed (`README.md:56-72`). bit-perfect-checker's firmware runs it on the STM32U5G9J-DK2 too, built as `stm32u5a5` (`README.md:74-80`). | Library and DWC2 host port build, and an app links (section 4.1). The port has register code and its decision logic is tested on the PC, but it has not run on a board yet (`README.md:52`, `README.md:82-93`, `port/dwc2/hcd/u2ps_dwc2_hcd.c:4-6`). |
| STM32F723 (DWC2) | Runs on the STM32F723E-DISCO in bit-perfect-checker's firmware (`README.md:74-80`); u2ps's own checks of it are its builds and a link check (section 2.2). | As the STM32U5A5. |
| STM32H7S3 (DWC2) | Not available yet: `-DU2PS_DEVICE=ON` with `-DU2PS_CHIP=stm32h7s3` stops the configure step and names errata ES0596 2.21.1 and 2.21.3, which the device port has no workaround for; build `-DU2PS_HOST=ON -DU2PS_DEVICE=OFF` (`CMakeLists.txt:126-135`, `port/dwc2/common/u2ps_dwc2_chip.h:29-41`). | Library and DWC2 host port build, and an app links (section 4.1); the PHY start-up is tested on the PC against a register model (`tests/test_dwc2_phy.c`). Not run on a board yet (`README.md:53`). |
| i.MX RT1062 (ChipIdea) | Builds. The port has decision logic and no register code: it defines no `u2ps_dport_*` function, so an app does not link (`README.md:54`, `port/chipidea/dcd/u2ps_chipidea_dcd.c:5-6`). | No host port. The library builds with the ChipIdea host constants (`README.md:408-410`). |
| PC build | Runs against the test kit's fake device port (`testkit/fake_dport.c`); your DAC app runs there too (section 7). | Runs against the test kit's fake host port and a scripted device, and with the device stack in a loopback through a bus that steps microframes; your player app runs there too (section 7, `README.md:52`, `tests/CMakeLists.txt:127-188`). |

The host stack's DWC2 port has not run on a board yet: integrate it as section 4.1 says, and check your player app with the test kit first (section 7), against `host/u2ps_host.h`.

## 2. Build and link

1. Use CMake 3.22 or later and Ninja (`README.md:251`). For ARM builds use Arm Toolchain for Embedded (ATfE) 23.1.0, the project's toolchain, with `cmake/arm-none-eabi-clang.cmake`, and name its directory, the one that holds `bin/clang`, in `U2PS_ATFE_DIR` (`cmake/arm-none-eabi-clang.cmake:4-10,22-30`). `arm-none-eabi-gcc` on the PATH with `cmake/arm-none-eabi.cmake` builds u2ps too (`cmake/arm-none-eabi.cmake:4`). Both files give each chip the same CPU flags (`cmake/arm-none-eabi.cmake:34-41`, `cmake/arm-none-eabi-clang.cmake:51-62`), and every ARM build compiles without a warning with either (checked: ATfE 23.1.0 and GCC 14.2).
2. Configure inside the u2ps tree, or from your project with `add_subdirectory(<u2ps-root> u2ps)`. Pass the toolchain file, `U2PS_CHIP`, the stack you link and, for the device stack, `U2PS_VBUS` on the command line, because the toolchain file reads `U2PS_CHIP` before the project exists (`cmake/arm-none-eabi-clang.cmake:51-67`, `cmake/arm-none-eabi.cmake:28-46`). With ATfE (`U2PS_ATFE_DIR` may come from the environment instead):

   ```
   cmake -S <your-project> -B <build-dir> -G Ninja \
     -DCMAKE_TOOLCHAIN_FILE=<u2ps-root>/cmake/arm-none-eabi-clang.cmake -DU2PS_ATFE_DIR=<atfe-dir> \
     -DCMAKE_BUILD_TYPE=RelWithDebInfo -DU2PS_CHIP=stm32u5a5 -DU2PS_DEVICE=ON -DU2PS_VBUS=app
   ```

   With GCC, `-DCMAKE_TOOLCHAIN_FILE=<u2ps-root>/cmake/arm-none-eabi.cmake` and no `U2PS_ATFE_DIR`. Without `U2PS_ATFE_DIR`, or with a directory that has no `bin/clang`, the ATfE file stops the configure step and names both fixes (`cmake/arm-none-eabi-clang.cmake:22-30`, checked).

   Give the build type: added to your project, u2ps keeps an empty one, and its libraries then build without optimisation (section 2.1). With the tree's ATfE file `RelWithDebInfo` compiles with `-Os -g -DNDEBUG`, with the GCC file with `-O2 -g -DNDEBUG` (below the options table).

   Checked: a project with `add_subdirectory(<u2ps-root> u2ps)` and `target_link_libraries(app PRIVATE u2ps_device)` configures and compiles this way. The link then asks for startup code and a linker script, which u2ps does not bring (`README.md:344-345`).
   With your own toolchain file, and with `EXCLUDE_FROM_ALL`, see section 2.1.
3. Select the stacks: `-DU2PS_DEVICE=ON` for a DAC (the device stack, `u2ps_device`), `-DU2PS_HOST=ON` for a player (the host stack, `u2ps_host`), or both. Both are `OFF` by default, and a configure with neither stops with a message that names both. A build has only the libraries of the stacks it selects, and of their ports. Set `U2PS_VBUS` when `U2PS_DEVICE` is `ON`: the device library stops the configure step without it. A host-only build needs no `U2PS_VBUS`. Set `U2PS_HOST_IA_VBUS_RATED_MA` when `U2PS_HOST` is `ON`: the host library stops the configure step without it (`CMakeLists.txt:116-125`, `device/CMakeLists.txt:24-38`, `host/CMakeLists.txt:30-39`).
4. Link `u2ps_device` for the device stack. It links `u2ps_common` and the port that `U2PS_PORT` names, and it puts the headers and the port's `u2ps_dport_cfg.h` on the include path (`device/CMakeLists.txt:88-91`, `port/dwc2/dcd/CMakeLists.txt:26`, `port/chipidea/dcd/CMakeLists.txt:35`). Link `u2ps_host` for the host stack; on a DWC2 chip it links `u2ps_dwc2_hcd` and takes `u2ps_hport_cfg.h` from it (`host/CMakeLists.txt:84-93`). The reference app also names `u2ps_dwc2_dcd` (`examples/nucleo_u5a5zj_q/CMakeLists.txt:57`).
5. A DWC2 build defines `U2PS_CHIP_STM32F723`, `U2PS_CHIP_STM32U5A5` or `U2PS_CHIP_STM32H7S3` for you. Do not define it yourself: two of them, or none, stop the build at `#error` (`port/dwc2/common/CMakeLists.txt:20`, `port/dwc2/common/u2ps_dwc2_chip.h:48-50,130-132`). Each chip's facts (its IRQ number, base addresses, highest core clock, data cache, errata) are in `port/dwc2/common/u2ps_dwc2_chip.h`.
6. A successful ARM configure prints `u2ps: ARM build for <chip>, device port <port>` (with `port` instead of `device port` in a build without the device stack), then `u2ps: stacks: device host` with the stacks it builds (`CMakeLists.txt:209-217`). Every CMake `FATAL_ERROR` and every `#error` names the option, its valid values and what to set (`README.md:205-209`).
7. The reference integration is `examples/nucleo_u5a5zj_q/`, built in the tree with `-DU2PS_EXAMPLES=ON`. It sets `U2PS_DEVICE=ON` and `U2PS_VBUS=app` itself (`U2PS_DEVICE=OFF` stops the configure step) and fetches CMSIS files at configure time (`examples/nucleo_u5a5zj_q/example.cmake:8-25`, `README.md:749-751`). `examples/pc_testkit/` is the reference for testing an integration on the PC (section 7).

Options and constants. Each is a CMake cache variable set with `-D<name>=<value>`; a numeric one must be a decimal integer (`README.md:205-209`, `CMakeLists.txt:82-89`). The libraries' constants reach the app as public compile definitions; an app that does not see them stops at `#error` (`device/u2ps_dev.h:171-176`, `host/u2ps_host.h:211-218`).

| Name | Default | Valid | Effect |
|---|---|---|---|
| `U2PS_CHIP` | none | `stm32f723`, `stm32u5a5`, `stm32h7s3` (host stack only), `imxrt1062` | ARM build for that chip (`CMakeLists.txt:35,41-51`). |
| `U2PS_PORT` | the chip's | `dwc2`, `chipidea` | Device port; a value that does not match the chip stops the configure step (`CMakeLists.txt:52-58`). |
| `U2PS_DEVICE` | `OFF` | `ON`, `OFF` | `ON`: builds the device stack, for a DAC (`u2ps_device`). At least one of `U2PS_DEVICE` and `U2PS_HOST` must be `ON`, or the configure step stops (`CMakeLists.txt:116-125`). With `U2PS_CHIP=stm32h7s3`, `ON` stops the configure step: the device port has no workaround for ES0596 2.21.1 and 2.21.3 yet (`CMakeLists.txt:126-135`). |
| `U2PS_HOST` | `OFF` | `ON`, `OFF` | `ON`: builds the host stack, for a player (`u2ps_host`) (`CMakeLists.txt:116-125`). |
| `U2PS_VBUS` | none | `app`, `bus` | Device VBUS mode (section 3.5), needed only with `U2PS_DEVICE=ON`; `port` stops the configure step (`device/CMakeLists.txt:24-38`). |
| `U2PS_REFUSE_FS` | `OFF` | `ON`, `OFF` | `ON`: the device never enumerates at full speed (`device/u2ps_dev.h:112-118`). |
| `U2PS_CONTROL_HOOK` | `OFF` | `ON`, `OFF` | `ON`: the stack calls `u2ps_app_on_control()`, which the app must then define (`device/u2ps_dev.h:145-146`). |
| `U2PS_CONTRACT_CHECKS` | `OFF` | `ON`, `OFF` | `ON`: both stacks check the app's contracts at run time and report each violation through `u2ps_app_on_contract()`, which the app must then define; `OFF` compiles the checks to nothing (section 5.1, `CMakeLists.txt:107-115`, `device/u2ps_dev.h:166-170`). |
| `U2PS_LTO` | `OFF` | `ON`, `OFF` | `ON`: link-time optimisation of u2ps's libraries; your link must then run LTO with the same compiler (section 2.2, `CMakeLists.txt:155-181`). |
| `U2PS_ATFE_DIR` | none | ATfE's directory, with `bin/clang` | Read by `cmake/arm-none-eabi-clang.cmake`, from the command line or the environment; unset, that file stops the configure step (`cmake/arm-none-eabi-clang.cmake:22-30`). The PC build passes it to the test `test_build_type_arm` (`tests/CMakeLists.txt:279-292`). |
| `U2PS_EP0_BUF` | 512 | multiple of 32, 64 to 65504 | Device control buffer in bytes; every descriptor must fit it (`device/u2ps_dev.h:137-141`). |
| `U2PS_GAP_PAUSE` | 400 | 1 to 8191 microframes | More `NO_PACKET` microframes than this are a pause, not a hole (`device/u2ps_dev.h:147-152`). |
| `U2PS_CLOSE_MAX` | 10 | 1 to 65535 ms | A stream's close may take this long before `u2ps_dev_tick()` returns `U2PS_FAULT_CLOSE` (`device/u2ps_dev.h:153-154`). |
| `U2PS_START_MAX` | 30 | 1 to 65535 ms | SET_INTERFACE's status stage is held this long for a start (`device/u2ps_dev.h:155-157`). |
| `U2PS_ISO_OUT_MPS_MAX` | 1024 | 1 to 1024 | Largest wMaxPacketSize of a device stream; it sizes the port's receive FIFO (`device/u2ps_dev.h:158-160`). |
| `U2PS_ISO_OUT_COUNT_MAX` | 16 | 2 (DWC2) or 3 (ChipIdea) to 65535 | Most buffers of a device stream; the app holds at most `count` - `U2PS_DPORT_ISO_OUT_RESERVE` of them (`device/u2ps_dev.h:161-165`, `README.md:225`). |
| `U2PS_HOST_EP0_BUF` | 1024 | multiple of 64, 256 to 65472, at least 2 + 12 x `U2PS_HOST_SUB_RANGE_MAX` (448 with its default) | Host control buffer; a longer configuration descriptor ends the session with `U2PS_HOST_END_TOO_LARGE`. A multiple of 64, because a control IN data stage reserves whole packets of endpoint 0's size (`host/u2ps_host.h:175-181`, `README.md:226`). |
| `U2PS_HOST_ALT_MAX` | 16 | 1 to 255 | Most streaming alternate settings kept, the lowest-numbered (`host/u2ps_host.h:182-183`). |
| `U2PS_HOST_SUB_RANGE_MAX` | 32 | 1 to (`U2PS_HOST_EP0_BUF` - 2) / 12 (85 with its default) | Most sub-ranges of the clock's RANGE kept (`host/u2ps_host.h:184-186`). |
| `U2PS_HOST_QUEUE_MAX` | 16 | 1 to 255 | Most packets queued at once (`host/u2ps_host.h:187`). |
| `U2PS_HOST_DEBT_MAX` | 8 | 1 to 8000 | Microframes' worth of slots a hole may leave owed (`host/u2ps_host.h:188-190`). |
| `U2PS_HOST_CONTROL_MAX` | 5000 | 1 to 65535 ms | Time a host control transfer may take (`host/u2ps_host.h:191-192`). |
| `U2PS_HOST_ACCEPT_NO_IAD` | `OFF` | `ON`, `OFF` | `ON` accepts a function without an audio Interface Association Descriptor if it has exactly one AudioControl interface, a deviation from UAC2 4.6 (`host/u2ps_host.h:193-194`). |
| `U2PS_HOST_STRINGS` | `ON` | `ON`, `OFF` | `OFF`: the host reads no strings; they are `NULL` (`host/u2ps_host.h:195-196`). |
| `U2PS_HOST_IA_VBUS_RATED_MA` | none | 8 to 5000, mA | Required with `U2PS_HOST=ON`. The current the board's VBUS supply of the host port is rated for, IA_VBUS_RATED of the OTG and EH Compliance Plan: the stack selects no configuration whose bMaxPower x 2 mA exceeds it (section 4.4). Set it to your product's rating: the VBUS switch's current limit less what the board itself draws from it. Unset, the configure step stops; outside 8 to 5000 the build stops (`host/u2ps_host.h:197-201`, `host/CMakeLists.txt:30-39`, `host/u2ps_host.c:47-49`). A DAC alone leaves out `-DU2PS_HOST=ON` and does not set it. |
| `U2PS_HOST_REFUSE_FS` | `OFF` | `ON`, `OFF` | `ON` ends the session of a full- or low-speed device at once with `U2PS_HOST_END_SPEED`, and the test device then runs at high speed only: a deviation from USB 2.0 7.1.2.3 (`host/u2ps_host.h:202-205`). |
| `U2PS_DWC2_HSE_HZ` | 16000000 (STM32U5A5), 25000000 (STM32F723), 24000000 (STM32H7S3) | STM32U5A5 and STM32H7S3: 16, 19.2, 20, 24, 26, 32 MHz; STM32F723: 12, 12.5, 16, 24, 25 MHz, in Hz | HSE frequency the USB PHY takes as reference; set your board's. Another value is an `#error` (`port/dwc2/common/CMakeLists.txt:21-53`, `port/dwc2/common/u2ps_dwc2_phy.c:56-58,120,152-154`). |
| `U2PS_DWC2_CORE_HZ` | 160000000 (STM32U5A5), 216000000 (STM32F723), 600000000 (STM32H7S3) | above 30 MHz, at most 160 MHz (STM32U5A5), 216 MHz (STM32F723) or 600 MHz (STM32H7S3), in Hz | Highest core clock the app runs at; the port bounds its waits with it, at a lower clock they are longer (`port/dwc2/common/CMakeLists.txt:21-49,54-57`, `port/dwc2/common/u2ps_dwc2.h:42-49`). |
| `U2PS_CHIPIDEA_FN_UNIT` | `none` | `none`, `uframes`, `frames` | ChipIdea only; only `uframes` makes microframe numbers exact (`port/chipidea/dcd/CMakeLists.txt:17-28`, `port/chipidea/u2ps_dport_cfg.h:55-61`). |

Bench and test options (`U2PS_DWC2_FORCE_FS`, `U2PS_DWC2_OUT_FOLLOW`, `U2PS_TEST_INJECT`, `U2PS_TEST_OLD_RULE`, and the example's `U2PS_EXAMPLE_STALL`) are not for a release build; `U2PS_TEST_INJECT` stops a `Release` or `MinSizeRel` configure (`README.md:242-245`, `port/dwc2/dcd/CMakeLists.txt:44-47`).

Optimisation per compiler. ATfE builds default to `-Os`, GCC builds to `-O2`:
- The tree's ATfE file sets `CMAKE_C_FLAGS_RELWITHDEBINFO` to `-Os -g -DNDEBUG` as a cache default (`cmake/arm-none-eabi-clang.cmake:76-81`), so u2ps's default build type, `RelWithDebInfo`, is `-Os` with ATfE. `-DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -DNDEBUG"` on the command line wins and gives `-O2`.
- The GCC file sets no flags of a build type: `RelWithDebInfo` is CMake's `-O2 -g -DNDEBUG`. So is it with your own toolchain file, with either compiler (section 2.1).
- A build type you give keeps CMake's flags for it: `Debug` `-g` (no `-O`), `Release` `-O3 -DNDEBUG`, `MinSizeRel` `-Os -DNDEBUG`.
- The injection build (`-DU2PS_TEST_INJECT=ON`) with the default build type builds, and with ATfE at `-Os`.
- Checked by `tests/test_build_type.cmake` (CTest `test_build_type_arm`, which needs ATfE in `U2PS_ATFE_DIR` and `arm-none-eabi-gcc`, and is skipped without them): the last `-O` flag of the compile line of `device/u2ps_dev.c` and, for the injection build, of `port/dwc2/dcd/u2ps_dwc2_dcd.c`.

Cycles, measured on 2026-11-09 on a NUCLEO-U5A5ZJ-Q (STM32U5A5 at 160 MHz) with the example app built from the tree of that day with `U2PS_LTO=OFF`, a Linux host playing 192 kHz and 768 kHz with no hole: the handler's pass for an OUT packet, from the injection build's pass profile. Text, measured on 2026-11-11 from the 0.1.0 tree, no board: the sum of the text of `libu2ps_device.a`, `libu2ps_dwc2_dcd.a` and `libu2ps_dwc2_common.a` (each archive's `size -t`) in the device-only `stm32u5a5` build without the injection hook (`README.md`, "Optimisation: ATfE -Os, GCC -O2"):

| Compiler and option | OUT pass, mean / max cycles (2026-11-09) | `isr_pct` (2026-11-09) | Text of the three libraries (0.1.0) |
|---|---|---|---|
| ATfE 23.1.0, `-O2` | 671 / 798 | 3.86 % | 20,880 bytes |
| ATfE 23.1.0, `-Os` (the default) | 690 / 722 | 4.01 % | 17,516 bytes |
| ATfE 23.1.0, `-Oz` | 733 / 759 | 4.20 % | 15,578 bytes |
| GCC 14.2, `-O2` (the default) | 624 / 638 | 3.60 % | 17,009 bytes |

### 2.1 Your own toolchain file, and `EXCLUDE_FROM_ALL`

You may use your own toolchain file instead of the tree's, with either compiler, and add u2ps with `add_subdirectory(<u2ps-root> u2ps EXCLUDE_FROM_ALL)`. Checked with each of the toolchain files below, outside the tree, and this project: the configure printed `u2ps: ARM build for stm32u5a5, device port dwc2` and `u2ps: stacks: device`, and the build compiled the u2ps libraries (common, device and the DWC2 port) without a warning, with `U2PS_LTO` `OFF` and `ON` for ATfE. They are built because your target links `u2ps_device`; nothing of u2ps is in the `all` target by itself.

GCC, 4 lines:

```
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER arm-none-eabi-gcc)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
```

ATfE, 5 lines: clang also needs the target triple. Without `CMAKE_C_COMPILER_TARGET` clang compiles for the host, and CMake's compiler check stops the configure step with `clang: error: unsupported argument 'cortex-m33' to option '-mcpu='` (checked). CMake finds `llvm-ar` next to clang for an LTO build, and clang links with lld by default for these targets (checked).

```
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_C_COMPILER <atfe-dir>/bin/clang)
set(CMAKE_C_COMPILER_TARGET thumbv8m.main-none-eabihf)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard")
```

The triple of each chip: `thumbv7em-none-eabihf` for the STM32F723, the STM32H7S3 and the i.MX RT1062 (Cortex-M7; the STM32H7S3 and the i.MX RT1062 with `-mfpu=fpv5-d16`, a double-precision FPU), `thumbv8m.main-none-eabihf` for the STM32U5A5 (Cortex-M33) (`cmake/arm-none-eabi-clang.cmake:51-62`).

```cmake
cmake_minimum_required(VERSION 3.22)
# Before project(): after it, a cache entry without FORCE no longer sets it
set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
project(integrator C)
if(CMAKE_CROSSCOMPILING)
  set(U2PS_CHIP stm32u5a5 CACHE STRING "")
endif()
set(U2PS_DEVICE ON CACHE BOOL "")
set(U2PS_VBUS app CACHE STRING "")
add_subdirectory(<u2ps-root> u2ps EXCLUDE_FROM_ALL)
add_library(app STATIC app.c)
target_link_libraries(app PRIVATE u2ps_device)
```

What u2ps needs from your toolchain file, and what the tree's files do (`cmake/arm-none-eabi.cmake:17-49`, `cmake/arm-none-eabi-clang.cmake:19-81`):

| What | Needed? | Why and what happens without it |
|---|---|---|
| `CMAKE_SYSTEM_NAME` set (`Generic`) | Yes | CMake then sets `CMAKE_CROSSCOMPILING`, which alone selects the ARM build (`CMakeLists.txt:41,74,183`). Without it the configure is a PC build and stops, because `U2PS_CHIP` is set: `U2PS_CHIP and U2PS_PORT apply to ARM builds only` (`CMakeLists.txt:59-65`). Checked. |
| The CPU flags of the chip in `CMAKE_C_FLAGS` or `CMAKE_C_FLAGS_INIT` | Yes | u2ps adds no `-mcpu`, `-mthumb`, `-mfpu` or `-mfloat-abi` flag (`CMakeLists.txt:69-80`): its libraries take yours. The DWC2 port has Cortex-M instructions in inline assembly (`port/dwc2/common/u2ps_dwc2.c:149-150`, `port/dwc2/dcd/u2ps_dwc2_dcd.c:188`). Without the flags the assembler stops the compile with `selected processor does not support ... in ARM mode` (checked). The flags of each chip are in `cmake/arm-none-eabi.cmake:34-41` and `cmake/arm-none-eabi-clang.cmake:51-62`; use the same as your app, so that the hard-float ABI agrees. |
| For clang, the target triple in `CMAKE_C_COMPILER_TARGET` | Yes | Without it clang compiles for the host and rejects the CPU flags (checked, above). The ATfE file sets each chip's (`cmake/arm-none-eabi-clang.cmake:69-70`). |
| `CMAKE_C_COMPILER` with GNU inline assembly and attribute syntax | Yes | The tree names ATfE's clang and `arm-none-eabi-gcc` (`cmake/arm-none-eabi-clang.cmake:32`, `cmake/arm-none-eabi.cmake:20`); clang accepts the same syntax (checked: every ARM build). The ports use GNU `__asm volatile` and `__attribute__((section(...)))` (`port/dwc2/common/u2ps_dwc2.c:149`, `device/u2ps_dev.c:35-36`), and u2ps is compiled as `-std=c11` without extensions (`CMakeLists.txt:20-22`). |
| `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` | Your project's, not u2ps's | Without it CMake's compiler check links a program, which needs a startup file and a linker script, and the configure stops with `is not able to compile a simple test program` (checked; `cmake/arm-none-eabi.cmake:25-26`). |
| `U2PS_CHIP` read by the file, and `CMAKE_TRY_COMPILE_PLATFORM_VARIABLES` | No | Only the tree's toolchain files read `U2PS_CHIP`, to pick the CPU flags and, for clang, the triple (`cmake/arm-none-eabi.cmake:27-28,34-46`, `cmake/arm-none-eabi-clang.cmake:44-45,51-67`). u2ps reads it in `CMakeLists.txt` after the project exists, so you can set it there, as above. |
| `CMAKE_C_FLAGS_RELWITHDEBINFO` | No | The ATfE file sets it to `-Os -g -DNDEBUG` as a cache default (`cmake/arm-none-eabi-clang.cmake:76-81`), so that ATfE builds default to `-Os`. Without it `RelWithDebInfo` is CMake's `-O2 -g -DNDEBUG`, as with GCC (checked: your own clang file above gives `-O2`). For the project's default with your own ATfE file, set it there, or on the command line. |
| `CMAKE_SYSTEM_PROCESSOR`, `CMAKE_ASM_COMPILER`, `CMAKE_OBJCOPY`, `CMAKE_SIZE`, the find-root modes | No | No u2ps CMake file reads `CMAKE_SYSTEM_PROCESSOR`, and the libraries are C only (`CMakeLists.txt:18`). Only the reference app uses the assembler and the binary tools (`examples/nucleo_u5a5zj_q/CMakeLists.txt:79-82`); the ATfE file names `llvm-objcopy` and `llvm-size` (`cmake/arm-none-eabi-clang.cmake:39-40`). |

What u2ps does to your build:
- Its warnings (`-Wall -Wextra -Werror` and the rest) and `-ffunction-sections -fdata-sections` apply to u2ps's own targets only. Your targets keep your flags (checked: a file of the project with an unused variable compiled) (`CMakeLists.txt:69-80`).
- u2ps keeps your `CMAKE_BUILD_TYPE`. Only as the top-level project does it set an empty one to `RelWithDebInfo` (`CMakeLists.txt:25-33`). With an empty build type, u2ps's libraries build without optimisation, and the configure prints `u2ps: CMAKE_BUILD_TYPE is empty, so u2ps's libraries build without optimisation` (checked, `tests/test_build_type.cmake`). Set it on the command line (`-DCMAKE_BUILD_TYPE=RelWithDebInfo`), or in the cache before `project()`, as above (checked: `-O2 -g -DNDEBUG` with your own toolchain file, either compiler; `-Os -g -DNDEBUG` with the tree's ATfE file). The same line after `project()` does nothing: `project()` has already created the entry, empty.
- `U2PS_CHIP` and `U2PS_PORT` in a PC configure stop it (`CMakeLists.txt:59-65`). If one CMakeLists serves both builds, set them only when `CMAKE_CROSSCOMPILING`, as above.
- The PC test kit's libraries (section 7) are in the same `EXCLUDE_FROM_ALL` directory and are built when one of your tests links them (checked). The tests of u2ps itself are built only when u2ps is the top-level project (`CMakeLists.txt:183-195`).

### 2.2 Link-time optimisation and the packet path's sections

`-DU2PS_LTO=ON` compiles u2ps's libraries with the compiler's link-time optimisation, through CMake's `INTERPROCEDURAL_OPTIMIZATION` (`CMakeLists.txt:155-181`). The libraries' objects then hold the compiler's intermediate code and no machine code (`llvm-size` and `arm-none-eabi-size` show no text in them), so your link must run LTO with the same compiler as the linker driver. Each library passes the link's LTO flag on to the target that links it, so a CMake target that links `u2ps_device` gets it. What each compiler's link needs:

| Toolchain | u2ps compiles with | Archived with | Your link | Checked |
|---|---|---|---|---|
| ATfE (clang) | `-flto=thin` (ThinLTO, CMake's choice for clang) | `llvm-ar` | clang as the driver with lld, ATfE's linker, and `-flto=thin`, which clang turns into lld's LTO options for the CPU and the optimisation level (`-plugin-opt=mcpu=...`, `-plugin-opt=O2`, at `-Os` too: the objects carry `-Os` as their functions' `optsize` attribute; checked with `clang -###` and the bitcode). lld runs the LTO itself; no plugin is needed. | the reference app, the STM32F723 device link and the host links below link without a warning |
| GCC | `-flto=auto -fno-fat-lto-objects` | `gcc-ar` | `arm-none-eabi-gcc` as the driver with `-flto=auto`; GNU ld runs GCC's LTO through its plugin. | the same links |

With CMake the flag comes by itself. In a link you write yourself, give the flag of the table. Without it, both linkers still run the LTO (GNU ld through the plugin, lld by itself), and the STM32F723 device link without it gave the same binary as with it, with either compiler (checked); clang then passes lld no `-plugin-opt` option (checked with `clang -###`), so lld's LTO takes the CPU from the objects and its own optimisation level.

- Your own code stays as you compile it; only u2ps's code is optimised across its files.
- A `-Wl,--wrap` of a u2ps function does not reach the calls inside u2ps's code: LTO resolves them before the linker. The reference app wraps `u2ps_dport_irq_mask()` to time masked regions; with `U2PS_LTO=ON` it does not, and logs `mask_max=none` (checked: no call of the wrapper in the LTO image; `examples/nucleo_u5a5zj_q/CMakeLists.txt:69-77`).
- The stack's state variables, such as `u2ps_dev`, may become local symbols of the image, so a linker script cannot rely on their names: with GCC's LTO `nm` shows `u2ps_dev` as `b`, not `B`, and an `ASSERT` on it stops the link although it lies where it should (checked in the STM32F723 link below: `ASSERT(u2ps_dev >= ORIGIN(DTCM) && u2ps_dev < ORIGIN(DTCM) + LENGTH(DTCM), ...)` passes without LTO and stops the GCC link with LTO). With ATfE's ThinLTO `u2ps_dev` stayed global in that link and the `ASSERT` passed, which LTO need not keep. Assert on the output section instead (example below).

The packet path is in two named sections, which survive LTO with either compiler (checked: the LTO link's `.constprop` clones stay in them with GCC; lld's map of the ATfE LTO link shows `.text.u2ps_fast` and `.bss.u2ps_fast` input sections of the LTO objects) (`common/u2ps_types.h:18-28`):

| Section | Macro | Holds |
|---|---|---|
| `.text.u2ps_fast` | `U2PS_FAST_TEXT` | The DWC2 port's handler pass for an OUT packet and everything it calls, the feedback endpoint's passes, the release and the feedback value: the port's `u2ps_dwc2_dcd_irq()`, its stream logic, and the device stack's `u2ps_dev_on_packet()`, `u2ps_dev_on_missed()`, `u2ps_dev_release()` and `u2ps_dev_fb_set()`. A release from thread, outside an upcall, also calls `u2ps_dport_irq_mask()` and `u2ps_dwc2_irq_mask()`, which are not in it (below). |
| `.bss.u2ps_fast` | `U2PS_FAST_BSS` | The port's state, the device stack's state and VBUS level, and in an injection build its state and pass profile. Zero-initialised, without a load image. |

Without a rule, `*(.text .text.*)` and `*(.bss .bss.*)` take them, as in the reference script (`examples/nucleo_u5a5zj_q/stm32u5a5zj.ld:28-31,90-103`). The STM32U5A5 needs no rule: it has no TCM for them. On the STM32F723, place them in ITCM and DTCM with one rule each, in output sections that come before the ones whose catch-all rules would take them (the first matching rule in the script wins):

```
  .itcm : { *(.text.u2ps_fast) } > ITCM AT > FLASH   /* your startup code copies it, as .data */
  .dtcm (NOLOAD) : { *(.bss.u2ps_fast) } > DTCM      /* your startup code zeroes it, as .bss */
```

To check the placement at link time, assert on the output sections, which LTO leaves as they are (checked with GNU ld and lld: links with `U2PS_LTO` `OFF` and `ON`, and stops all of them when either rule takes nothing):

```
ASSERT(SIZEOF(.itcm) > 0 && ADDR(.itcm) + SIZEOF(.itcm) <= ORIGIN(ITCM) + LENGTH(ITCM),
       "u2ps: .text.u2ps_fast is not in ITCM")
ASSERT(SIZEOF(.dtcm) > 0 && ADDR(.dtcm) >= ORIGIN(DTCM) &&
       ADDR(.dtcm) + SIZEOF(.dtcm) <= ORIGIN(DTCM) + LENGTH(DTCM),
       "u2ps: .bss.u2ps_fast is not in DTCM")
```

The F723's flash has two addresses: 0x0800_0000 on the AXIM interface and 0x0020_0000 on the ITCM interface; ITCM RAM is at 0x0000_0000 (RM0431 3.3.1, Table 3, and 3.4.1). A Thumb `BL` reaches +-16 MB. Sizes, measured on 2026-11-11 from the 0.1.0 tree with a link of the STM32F723 device stack (the callback skeleton of section 3.10, a main loop and the OTG_HS vector), `RelWithDebInfo` (ATfE `-Os`, GCC `-O2`), ITCM at 0x0000_0000, DTCM at 0x2000_0000, and the image linked to run from either flash address:

| Toolchain | `U2PS_LTO` | Image runs from | `.text.u2ps_fast` in ITCM | of it, linker veneers | `.bss.u2ps_fast` in DTCM |
|---|---|---|---|---|---|
| ATfE | `OFF` | AXIM flash, 0x0800_0000 | 4,284 bytes | 160 bytes (16) | 508 bytes |
| ATfE | `ON` | AXIM flash, 0x0800_0000 | 4,428 bytes | 160 bytes (16) | 508 bytes |
| ATfE | `OFF` | ITCM flash, 0x0020_0000 | 4,124 bytes | none | 508 bytes |
| ATfE | `ON` | ITCM flash, 0x0020_0000 | 4,266 bytes | none | 508 bytes |
| GCC | `OFF` | AXIM flash, 0x0800_0000 | 4,432 bytes | 128 bytes (16) | 472 bytes |
| GCC | `ON` | AXIM flash, 0x0800_0000 | 4,656 bytes | 120 bytes (15) | 472 bytes |
| GCC | `OFF` | ITCM flash, 0x0020_0000 | 4,304 bytes | none | 472 bytes |
| GCC | `ON` | ITCM flash, 0x0020_0000 | 4,536 bytes | none | 472 bytes |

- The F723's ITCM RAM is 16 KB. Your `memcpy`, `memset` and interrupt code share it; the stack's packet path calls no library function.
- The packet path calls your callbacks (`u2ps_app_on_packet()`, `u2ps_app_on_hole()`, `u2ps_app_on_pause()`) and u2ps's code for rare events in flash. Run from the AXIM flash, that code lies beyond a branch's reach of ITCM, and the linker adds a veneer for each such call: 8 bytes with GNU ld, 10 with lld (which names them `__Thumbv7ABSLongThunk_*`); the calls the other way, from your vector function and from u2ps's code in flash into ITCM, take 10 (8 with LTO) more veneers in flash with GNU ld, 10 (9 with LTO) with lld. Run from the ITCM flash, 2 MB from ITCM RAM, every call is in reach and the link has no veneer. Either way, place your packet callback and your vector function in ITCM too, to save the flash wait states and, with the AXIM flash, their veneers.
- `u2ps_dev_release()` stays in the section only inside the packet upcall. From thread, outside an upcall, it masks the vectors through `u2ps_dport_irq_mask()` and `u2ps_dwc2_irq_mask()`, which are not in `.text.u2ps_fast` and run from flash (checked: so in the link without LTO; with LTO, GCC 14.2 inlined both into the release, and ATfE inlined `u2ps_dwc2_irq_mask()` into `u2ps_dport_irq_mask()`, which stays in flash; neither need do so).
- `.bss.u2ps_dma` is not part of this: it must lie in DMA memory (section 3.1). DTCM is DMA memory on the STM32F723 (`port/dwc2/common/u2ps_dwc2.h:174-188`).
- The port's state is cleared at init; the device stack's needs the zeroing (section 3.1, "Zeroing"). `.bss.u2ps_dma` needs none: the stacks and the ports zero it at init.

## 3. Device stack (a DAC)

### 3.1 Linker script, DMA memory and the port's constants

1. The control buffer and the port's memory are in section `.bss.u2ps_dma` (`U2PS_DMA_SECTION`). It has no load image, and the stack and the port zero their memory in it at init, so it may be `NOLOAD` and need not be zeroed by the startup code. Place `*(.bss.u2ps_dma)` in DMA memory ahead of `*(.bss .bss.*)` (`device/u2ps_dport.h:60-66`, `examples/nucleo_u5a5zj_q/stm32u5a5zj.ld:97-98`). `u2ps_dev_init()` checks every byte of it, through the port, before anything else, and returns `U2PS_DEV_ERR_DMA_MEMORY` if one lies where the port's DMA does not reach it; nothing changes then (`device/u2ps_dev.c:107-110`, `common/u2ps_types.h:143-160`).
2. Class memory must lie in DMA memory too. The DWC2 port refuses a stream whose memory is outside it, by the same rule (`U2PS_DWC2_OPEN_NOT_DMA`, `port/dwc2/dcd/u2ps_dwc2_iso.h:108-115`).
3. The port's `u2ps_dport_cfg.h` gives the constants for sizing class memory (`device/u2ps_dport.h:21-40`). Include it after `u2ps_dev.h`:

| Constant | STM32F723 | STM32U5A5 | i.MX RT1062 |
|---|---|---|---|
| `U2PS_DPORT_ALIGN`: alignment of class memory and of its stride, bytes | 32 | 4 | 32 |
| `U2PS_DPORT_STRIDE(mps)`: least stride for packets of `mps` bytes | `mps` rounded up to 4, then to 32 | `mps` rounded up to 4 | `mps` rounded up to 32 |
| `U2PS_DPORT_ISO_OUT_MIN`: least buffer count, `U2PS_DPORT_ISO_OUT_RESERVE` + 1 | 2 | 2 | 3 |
| `U2PS_DPORT_ISO_OUT_RESERVE`: buffers the port keeps armed; the app holds at most `count` minus this | 1 | 1 | 2 |
| `U2PS_DPORT_UFRAME_EXACT`: 1 if packet microframe numbers are exact | 1 | 1 | 1 only with `U2PS_CHIPIDEA_FN_UNIT=uframes`, else 0 |
| Memory the DMA reaches | DTCM (with `RCC_AHB1ENR.DTCMRAMEN` set), SRAM1, SRAM2; not ITCM RAM | SRAM1, SRAM2, SRAM3, SRAM5, SRAM6 at 0x2000_0000, each with its RCC clock enable set and not powered down (`PWR_CR1`); not SRAM4, not the backup SRAM | not stated in IMXRT1060RM Rev. 3; no register code, no check |
| Data cache | SRAM1 and SRAM2 are cached; the port cleans and invalidates whole lines; DTCM is not cached | none on the SRAM path | cached OCRAM is allowed; the port invalidates whole lines; DTCM is not cached |

Sources: `port/dwc2/u2ps_dport_cfg.h:15-56`, `port/chipidea/u2ps_dport_cfg.h:29-61`, `port/dwc2/common/u2ps_dwc2.h:174-188`, `port/dwc2/common/u2ps_dwc2_dma.h:45-71`, `port/chipidea/u2ps_dport_cfg.h:91-95`.

DMA memory on the STM32U5A5:
- Reach. The OTG_HS DMA reaches SRAM1, SRAM2, SRAM3 and SRAM5 (SRAM6 on the U5Fx and U5Gx) at their addresses from 0x2000_0000, and never SRAM4, which lies at 0x2800_0000, nor the backup SRAM (`examples/nucleo_u5a5zj_q/stm32u5a5zj.ld:3-7`, `port/dwc2/common/u2ps_dwc2_dma.h:45-56`). The port's rule takes the U5A5's SRAM layout (SRAM1 768 KB, SRAM2 64 KB, SRAM3 832 KB, SRAM5 832 KB, SRAM6 512 KB) and reads, for each SRAM a range touches, its clock enable (`RCC_AHB1ENR.SRAM1EN`, `RCC_AHB2ENR1.SRAM2EN` and `SRAM3EN`, `RCC_AHB2ENR2.SRAM5EN` and `SRAM6EN`) and its power-down bit (`PWR_CR1.SRAMxPD`). Every SRAM is clocked and powered at reset; keep the DMA's memory out of any SRAM your app switches off (`port/dwc2/common/u2ps_dwc2_dma.c:19-27`).
- What is checked. At every `u2ps_dev_init()`, before anything else: the control buffer, the EP0 area and the feedback word, through `u2ps_dport_dma_check()`, a port call; outside the rule, init returns `U2PS_DEV_ERR_DMA_MEMORY` (`port/dwc2/dcd/u2ps_dwc2_dcd.c:1374-1378`). At each stream open: class memory, refused with `U2PS_DWC2_OPEN_NOT_DMA` (`port/dwc2/dcd/u2ps_dwc2_dcd.c:990-991`). The check is an address and register test; it does not see GTZC privilege settings (section 3.2). A bus error that the controller's DMA raises is counted in `ahberr` of `u2ps_dwc2_dcd_diag()` (`port/dwc2/dcd/u2ps_dwc2_dcd.h:92`).
- Placement. The reference script has one RAM region, SRAM1, SRAM2, SRAM3 and SRAM5 as one block of 2496 KB, and puts `*(.bss.u2ps_dma)` first in `.bss`; its class memory is an ordinary array in `.bss` (`examples/nucleo_u5a5zj_q/stm32u5a5zj.ld:11-15,90-103`, `examples/nucleo_u5a5zj_q/main.c:230-236`). If all of your `.bss` lies in those SRAMs, you need no rule: without one, `*(.bss .bss.*)` takes the section (`device/u2ps_dport.h:60-66`). If your script has several RAM regions, for example SRAM4 for other data, put the section with an explicit rule into an output section in the DMA-reachable RAM, and put the class memory in DMA-reachable RAM in the same way.
- Zeroing. The section has no load image (checked: the object files mark it as allocated, without contents). `u2ps_dev_init()` zeroes the control buffer at every init (`device/u2ps_dev.c:120-122`), and the DWC2 port zeroes its EP0 area and feedback word in its start, after the core reset, and cleans their cache lines (`port/dwc2/dcd/u2ps_dwc2_dcd.c:1391-1397`). So the section may be `NOLOAD` and need not be zeroed by the startup code (tested on the PC: `tests/test_dev_dma.c`). The rest of the stack does need a zeroed `.bss`, as any C static, so keep the startup code's zeroing of it.
- A link-time check. A linker script may put the section in an output section of its own and assert its place, for example on the STM32U5A5:

  ```
  .u2ps_dma (NOLOAD) : ALIGN(32) { *(.bss.u2ps_dma) } > RAM
  ASSERT(ADDR(.u2ps_dma) >= 0x20000000 &&
         ADDR(.u2ps_dma) + SIZEOF(.u2ps_dma) <= 0x20270000,
         "u2ps: .bss.u2ps_dma must lie in SRAM1, SRAM2, SRAM3 or SRAM5")
  ```

  On the STM32F723 the bounds are 0x20000000 and 0x20040000 (DTCM, SRAM1, SRAM2). The init check stays: it also sees an SRAM switched off at run time.

### 3.2 Before `u2ps_dev_init()`

The DWC2 port starts the PHY and its clocks, but not the HSE. The app must have these in place (`port/dwc2/common/u2ps_dwc2.h:19-22`, `port/dwc2/common/u2ps_dwc2.h:77-82`):

- The HSE runs, at `U2PS_DWC2_HSE_HZ`. Otherwise init returns `U2PS_DEV_ERR_CLOCK`.
- STM32U5A5: the voltage range is range 2 or better (otherwise `U2PS_DEV_ERR_POWER`), and TrustZone is off, option byte TZEN = 0 (otherwise `U2PS_DEV_ERR_SECURITY`); the port is nonsecure only (`README.md:751-753`).
- STM32U5A5: the app has set the EPOD booster clock (PLL1MBOOST in RCC_PLL1CFGR) with its clock set-up; the port checks only the voltage range (`port/dwc2/common/u2ps_dwc2_phy.c:61-67`).
- STM32F723: the port enables the OTG_HS clocks itself, OTGHSEN and OTGHSULPIEN in RCC_AHB1ENR and OTGPHYCEN in RCC_APB2ENR; the embedded HS PHY clocks the core through OTGHSULPIEN (RM0431 5.3.10, Figure 14). The app provides only the HSE (`port/dwc2/common/u2ps_dwc2_phy.c:126-137`).
- The core clock does not exceed `U2PS_DWC2_CORE_HZ` (section 2).
- `.bss.u2ps_dma` lies in DMA memory, and its SRAM is clocked and powered (section 3.1). Otherwise init returns `U2PS_DEV_ERR_DMA_MEMORY`.
- STM32U5A5: no SRAM block of the DMA memory is made privileged in the GTZC (`GTZC1_MPCBBz_PRIVCFGRx`), which privileged code can do even with TZEN = 0 (RM0456 5.8.5). An unprivileged access to a privileged block reads zeros and its writes are dropped, without a bus error (RM0456 5.4.2); that the OTG_HS DMA's accesses are unprivileged unless `GTZC1_TZSC_PRIVCFGR3.OTGPRIV` is set is inferred from RM0456 3.5.4 (p. 159) and 3.6.3 (p. 172), which say a peripheral made privileged-only issues privileged transactions as a bus master. The port does not check these settings.

The reference board code is `examples/nucleo_u5a5zj_q/board.c:36-68` (HSE, PLL1, voltage range 1 with booster).

### 3.3 The description

The app describes its device and audio function once in a `u2ps_dev_description_t`; the stack builds every descriptor from it (`device/u2ps_dev.h:7-13`). The description and everything it points to stay valid and unchanged after `u2ps_dev_init()`: make them `static const`, never locals (`device/u2ps_dev.h:198-199`). To change it, init again with a second description and leave the first untouched (section 3.4, "Changing the description").

The function is an AudioControl interface 0 (clock, Input Terminal, Output Terminal) and an AudioStreaming interface 1 whose alternate settings 1 to `format_count` each carry one format, with an asynchronous isochronous OUT endpoint and an explicit feedback endpoint (`device/u2ps_dev.h:192-199`).

If `u2ps_dev_init()` returns `U2PS_DEV_ERR_DESCRIPTION`, nothing changed, and `u2ps_dev_description_error()` (any context) returns the rule broken as a `u2ps_dev_description_error_t`. It returns `U2PS_DESCRIPTION_OK` otherwise and before the first init (`device/u2ps_dev.h:515-519`). The stack checks the rules in the order of this table and names the first broken one (`device/u2ps_dev.h:238-242`):

| Field and rule | `u2ps_dev_description_error()` value |
|---|---|
| `desc` is not `NULL` | `U2PS_DESCRIPTION_NULL` |
| `formats` is not `NULL` | `U2PS_DESCRIPTION_FORMATS` |
| `format_count` at least 1 | `U2PS_DESCRIPTION_FORMAT_COUNT` |
| `sampling_frequencies` is not `NULL` | `U2PS_DESCRIPTION_SAMPLING_FREQUENCIES` |
| `sampling_frequency_count` from 1 to (`U2PS_EP0_BUF` - 2) / 12 (42 at the default) | `U2PS_DESCRIPTION_SAMPLING_FREQUENCY_COUNT` |
| `nr_channels` at least 1 | `U2PS_DESCRIPTION_NR_CHANNELS` |
| `max_power_ma` 0 to 500; an odd value is rounded up to a 2 mA unit (`device/u2ps_dev.h:211-213`) | `U2PS_DESCRIPTION_MAX_POWER_MA` |
| `self_powered` false in VBUS mode bus | `U2PS_DESCRIPTION_SELF_POWERED` |
| `manufacturer`, `product`, `serial_number`: printable ASCII (0x20 to 0x7E), at most 126 characters, descriptor within `U2PS_EP0_BUF`; `NULL` for none; language ID 0x0409 (`device/u2ps_dev.h:204-209`) | `U2PS_DESCRIPTION_MANUFACTURER`, `U2PS_DESCRIPTION_PRODUCT`, `U2PS_DESCRIPTION_SERIAL_NUMBER` |
| `channel_config` (`bmChannelConfig`, e.g. 0x3): no more bits set than `nr_channels`, bits 27 to 31 clear | `U2PS_DESCRIPTION_CHANNEL_CONFIG` |
| `terminal_type` (`wTerminalType` of the Output Terminal, e.g. 0x0301 speaker, 0x0602 digital audio interface): not 0x01xx, 0x02xx, 0x0703, 0x0708 to 0x0710 | `U2PS_DESCRIPTION_TERMINAL_TYPE` |
| `sampling_frequencies[]` in Hz, above 0, strictly ascending | `U2PS_DESCRIPTION_SAMPLING_FREQUENCY_ORDER` |
| `initial_sampling_frequency` is one of `sampling_frequencies[]`, or 0 for the first | `U2PS_DESCRIPTION_INITIAL_SAMPLING_FREQUENCY` |
| format `subslot_size` 1 to 4 bytes | `U2PS_DESCRIPTION_SUBSLOT_SIZE` |
| format `bit_resolution` 1 to 8 * `subslot_size` | `U2PS_DESCRIPTION_BIT_RESOLUTION` |
| format `audio_data_format` (`u2ps_audio_data_format_t`) is `U2PS_AUDIO_DATA_FORMAT_PCM` (0) or `U2PS_AUDIO_DATA_FORMAT_IEEE_FLOAT`; float needs `subslot_size` 4 and `bit_resolution` 32 (`common/u2ps_types.h:70-86`) | `U2PS_DESCRIPTION_AUDIO_DATA_FORMAT` |
| every format's wMaxPacketSize within `U2PS_ISO_OUT_MPS_MAX` | `U2PS_DESCRIPTION_MAX_PACKET_SIZE` |
| the high-speed configuration descriptor within `U2PS_EP0_BUF` (8 formats fit the default) | `U2PS_DESCRIPTION_CONFIGURATION_SIZE` |
| none broken | `U2PS_DESCRIPTION_OK` |

`id_vendor`, `id_product`, `bcd_device` and `category` (`bCategory`, e.g. 0x08) take any value (`device/u2ps_dev.h:201-203,214`). Format *n* (counting from 1) is alternate setting *n* (`device/u2ps_dev.h:222-223`).

Packet size. The highest sampling frequency sizes every data endpoint: wMaxPacketSize = (ceil(sampling frequency / 8000) + 1) * `nr_channels` * `subslot_size`, at most 1024 (`device/u2ps_dev.h:224-228`). Example: 768 kHz, 2 channels, 4-byte subslots gives (96 + 1) * 2 * 4 = 776 bytes. Eight channels at the same rate gives 3104 bytes, which init refuses with `U2PS_DESCRIPTION_MAX_PACKET_SIZE`.

A minimal valid description (checked: `u2ps_dev_init()` returns `U2PS_DEV_OK` with a fake port):

```c
#include <stddef.h>
#include "u2ps_dev.h"

static const u2ps_format_t formats[] = {
  {.subslot_size = 4, .bit_resolution = 24, .audio_data_format = U2PS_AUDIO_DATA_FORMAT_PCM},
};
static const uint32_t rates[] = {44100, 48000};
static const u2ps_dev_description_t desc = {
  .id_vendor = 0x1209, .id_product = 0x0001, .bcd_device = 0x0100,
  .manufacturer = "Example", .product = "Example DAC", .serial_number = NULL,
  .self_powered = false, .max_power_ma = 100, .category = 0x08, .terminal_type = 0x0301,
  .nr_channels = 2, .channel_config = 0x3,
  .formats = formats, .format_count = 1,
  .sampling_frequencies = rates, .sampling_frequency_count = 2,
  .initial_sampling_frequency = 0,
};
```

A description with five formats and ten sampling frequencies up to 768 kHz is `examples/nucleo_u5a5zj_q/main.c:188-225`.

### 3.4 Init, connect, the main loop

Order (`examples/nucleo_u5a5zj_q/main.c:1296-1342`):

1. Start the clocks (section 3.2).
2. `u2ps_dev_init(&desc)`. It checks the description, then the DMA memory, resets the stack, zeroes its control buffer and starts the port. The device stays detached.
3. If init returned `U2PS_DEV_OK`: set the USB vector's priority, enable the vector (section 3.6), then `u2ps_dev_connect(true)`. Calls before a successful init do nothing (`device/u2ps_dev.h:521-523`).
4. Loop in the thread: in mode app `u2ps_dev_vbus(level)` on every pass, and `u2ps_dev_tick(now_ms)` at least every 10 ms, the stack's one tick period (`device/u2ps_dev.h:604-606`).

The thread rule. `u2ps_dev_init()`, `u2ps_dev_connect()`, `u2ps_dev_vbus()`, `u2ps_dev_reply()`, `u2ps_dev_tick()`, `u2ps_dev_stream_open()`, `u2ps_dev_stats()` and `u2ps_dev_release()` outside a callback all run in one thread context, never from an interrupt and never from each other: each masks the port's vectors, and the mask does not nest (`device/u2ps_dev.h:23-27`).

`u2ps_dev_init()` returns a `u2ps_dev_err_t` (`common/u2ps_types.h:88-167`). On DWC2, `u2ps_dwc2_dcd_init_error()` (any context) gives the port's own `u2ps_dwc2_err_t`, `U2PS_DWC2_OK` if none (`port/dwc2/dcd/u2ps_dwc2_dcd.h:49-51`):

| `u2ps_dev_err_t` | Cause and fix | DWC2 `u2ps_dwc2_err_t` |
|---|---|---|
| `U2PS_DEV_OK` | Started. | `U2PS_DWC2_OK` |
| `U2PS_DEV_ERR_DESCRIPTION` | Description invalid; read `u2ps_dev_description_error()`. | none |
| `U2PS_DEV_ERR_CLOCK` | A clock the controller or PHY needs is not running; start the HSE before init. | `U2PS_DWC2_ERR_CLOCK` |
| `U2PS_DEV_ERR_POWER` | A supply or voltage setting is missing; STM32U5 voltage range 1 or 2. | `U2PS_DWC2_ERR_POWER` |
| `U2PS_DEV_ERR_SECURITY` | STM32U5 TrustZone is on; set option byte TZEN = 0. | `U2PS_DWC2_ERR_TZEN` |
| `U2PS_DEV_ERR_PHY` | PHY, its supply or PLL did not come up; check the board's USB supply and `U2PS_DWC2_HSE_HZ`. | `U2PS_DWC2_ERR_PHY` |
| `U2PS_DEV_ERR_RESET` | The controller's reset did not end; its bus clock or PHY clock is not running. | `U2PS_DWC2_ERR_RESET` |
| `U2PS_DEV_ERR_MODE` | The controller did not enter device mode; other code drives it. | `U2PS_DWC2_ERR_MODE` |
| `U2PS_DEV_ERR_MEMORY` | FIFO RAM does not fit or a flush timed out; lower `U2PS_ISO_OUT_MPS_MAX` to the largest wMaxPacketSize needed. | `U2PS_DWC2_ERR_FIFO` |
| `U2PS_DEV_ERR_PORT` | Another reason of the port. | |
| `U2PS_DEV_ERR_DMA_MEMORY` | `.bss.u2ps_dma` lies where the port's DMA does not reach it, or its SRAM's clock is off or it is powered down; place the section in DMA memory and keep that RAM on (section 3.1). Checked first, at every init; nothing changes. | none (the stack's check through `u2ps_dport_dma_check()`) |
| `U2PS_DEV_ERR_TIMER` | The cycle counter that times the port's waits does not count; nothing was started. Leave the DWT to the port: no code or debugger script may lock it or clear `DEMCR.TRCENA` or `DWT_CTRL.CYCCNTENA`; then init again. On a Cortex-M7 the port unlocks the DWT and checks that the counter counts; on the STM32U5A5 it does not check. | `U2PS_DWC2_ERR_CYCLES` |

Sources: `common/u2ps_types.h:94-167`, `port/dwc2/dcd/u2ps_dwc2_dcd.h:10-22`, `port/dwc2/common/u2ps_dwc2_start.h:19-51`. The stack calls the port's start-up on every init until it has succeeded once (`device/u2ps_dport.h:106-112`), so after a port error you may call init again.

Re-init. `u2ps_dev_init()` may be called again while detached, to change the description. Called while attached, it detaches first and gives `on_configured(false, U2PS_CONFIG_END_REINIT)`. The clock keeps its sampling frequency if the new description lists it. Stream numbers and request ids go on across a re-init (`device/u2ps_dev.h:501-512`).

Changing the description. The description in use stays valid and unchanged until a re-init has returned: the only way to a new description is `u2ps_dev_init()` with another one (`device/u2ps_dev.h:198-199,501-504`). So the app keeps two descriptions, and the arrays and strings that differ between them (`formats`, `sampling_frequencies`, the strings), and fills the one that is not in use (double-buffering):
1. Fill the second description. Do not write to the first, not even while the device is detached.
2. Call `u2ps_dev_init(&second)`. The stack validates the second description first; on `U2PS_DEV_ERR_DESCRIPTION` nothing changes and the first stays in use (`device/u2ps_dev.c:104-106`). Keep the first untouched then too, and read `u2ps_dev_description_error()`. A re-init while attached detaches first, and the stack's pointer moves to the second only after that (`device/u2ps_dev.c:119,127`).
3. When it returns `U2PS_DEV_OK`, the second is in use and the first is free: the next change fills the first.

Without double-buffering, with `U2PS_CONTRACT_CHECKS=ON`: `u2ps_dev_tick()` compares the description in use, with the formats, the rates and the strings it points to, against a checksum made at the init. A difference is reported once as `U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED`, also while detached (`device/u2ps_dev_audio.c:351-352`, `device/u2ps_dev.c:376-430`, `common/u2ps_types.h:242-247`). Checked on the PC with the test kit: a field written in place after `u2ps_dev_connect(false)` and before the re-init gave the violation at the next tick, although the re-init followed with the same pointer; the same change in the second description gave none; and after a refused re-init, a change to the first description gave the violation. A build without the checks reports nothing, and the stack answers the host from the changed description. The reference app's re-init command uses three descriptions, `desc`, `desc_b` and `desc_bad`, all made before the first init (`examples/nucleo_u5a5zj_q/main.c:207-228,1320-1324`).

`u2ps_dev_connect(on)` (`device/u2ps_dev.h:521-535`):
- `true` allows the attach. The pull-up is on while the attach is allowed and VBUS is present.
- `false` ends the attach. A stream and the configuration end as at a bus reset, with `U2PS_CONFIG_END_DETACH`; `u2ps_dev_speed()` returns `U2PS_SPEED_NONE`.
- `false` while already detached only ends the permission; no callback runs.

`u2ps_dev_speed()` (any context) returns a `u2ps_speed_t`: `U2PS_SPEED_NONE` while detached, after a VBUS loss, and after a bus reset until the port has reported the speed that reset ended at; then `U2PS_SPEED_FULL` or `U2PS_SPEED_HIGH` (`device/u2ps_dev.h:537-540`, `common/u2ps_types.h:59-68`). `U2PS_SPEED_LOW` comes only on the host side (section 4.4). Read it in `on_configured(true, ...)`: a full-speed configuration has no streaming interface (`device/u2ps_dev.h:680-682`).

### 3.5 VBUS

Choose the mode with `U2PS_VBUS` (`device/u2ps_dev.h:89-110,178-184`, `README.md:651-676`):

| Mode | Use | App duties |
|---|---|---|
| `app` (`U2PS_VBUS_APP`) | The app reads VBUS. | Report the debounced level with `u2ps_dev_vbus(present)` on every poll, from the thread of section 3.4, never from an interrupt. Define `u2ps_app_on_vbus()`. VBUS counts as absent until the first report; a report before init counts. A device that never reports VBUS never attaches. |
| `bus` (`U2PS_VBUS_BUS`) | Bus-powered; the pull-up takes its supply from VBUS. | None. VBUS is always present. `u2ps_dev_vbus()` and `u2ps_app_on_vbus()` are not declared. `self_powered` must be false. |
| `port` (`U2PS_VBUS_PORT`) | The port senses VBUS. | Not available: the configure step stops. |

- A VBUS loss (mode app) ends a stream and the configuration as a detach does, with `U2PS_CONFIG_END_VBUS`, then calls `u2ps_app_on_vbus(false)`. While VBUS is absent the stack ignores every event the port reports (`device/u2ps_dev.h:549-557`).
- The pull-up follows the last report. A poll that stalls with VBUS present leaves the pull-up on after the cable is pulled (`device/u2ps_dev.h:560-562`). Keep the poll running.
- A bus-powered device must signal attach within 100 ms of VBUS: call `u2ps_dev_connect(true)` and, in mode app, report VBUS within 100 ms of power-up. Keeping to 100 mA until configured and to the suspend current (0.5 mA averaged over 1 s) is the app's duty (`device/u2ps_dev.h:104-110`).
- `u2ps_dev_vbus_present()` (any context) returns the level the stack knows; in mode bus always true (`device/u2ps_dev.h:566-569`).
- Debounce is the app's. The reference app takes a new level once it has held for 50 ms and takes the first reading at once (`examples/nucleo_u5a5zj_q/main.c:1096-1116,1341`).

### 3.6 The interrupt handler (DWC2)

1. Define your chip's OTG_HS vector function from its startup file, and call `u2ps_dwc2_dcd_irq()` from it (include `u2ps_dwc2_dcd.h`). The reference app's function is `OTG_HS_IRQHandler` on the STM32U5 (`examples/nucleo_u5a5zj_q/main.c:567-575`).
2. `U2PS_DWC2_IRQ` is the IRQ number the NVIC calls take, not a function name: 73 on the STM32U5A5, 77 on the STM32F723 (`port/dwc2/common/u2ps_dwc2.h:35-38`).
3. The port has one vector; the NVIC priority is the app's. No other interrupt that runs port or stack code may share or exceed it (`port/dwc2/dcd/u2ps_dwc2_dcd.h:41-46`).
4. Set the priority and enable the vector after a successful init and before `u2ps_dev_connect(true)` (`examples/nucleo_u5a5zj_q/main.c:1328-1333`, `device/u2ps_dport.h:106`).
5. The stack masks and unmasks this vector itself around its thread calls, with `u2ps_dport_irq_mask()` (`device/u2ps_dport.h:133-141`). Do not mask or unmask it for the stack.

ChipIdea has no handler yet (section 1).

### 3.7 Callbacks the app defines

The stack has no weak defaults, so a missing callback is a link error (checked: removing `u2ps_app_on_pause` leaves an undefined reference). All run in the port's interrupt except the cases in section 3.11.

| Callback | Required | Purpose |
|---|---|---|
| `u2ps_app_on_request(req)` | always | A vendor request to an interface. Return the bytes to send (>= 0), `U2PS_REQ_STALL` or `U2PS_REQ_DEFER`. The stack answers every class request itself (`device/u2ps_dev.h:655-663`). |
| `u2ps_app_on_sampling_frequency(requested, chosen, id)` | always | The host sets the clock. `chosen` is the listed rate closest to `requested` (the lower on a tie). Return >= 0 to accept, `U2PS_REQ_STALL` to refuse, `U2PS_REQ_DEFER` to answer later with `u2ps_dev_reply(id, NULL, result)` (`device/u2ps_dev.h:665-672`). |
| `u2ps_app_on_configured(configured, why)` | always | The Configured state is entered (`why` = `U2PS_CONFIG_END_NONE`) or left (`why` in the table below). Calls alternate (`device/u2ps_dev.h:674-683`). |
| `u2ps_app_on_stream_start(info, cfg)` | always | A stream starts; lend class memory (section 3.8). |
| `u2ps_app_on_packet(data, len, uframe)` | always | One packet in class memory, held until `u2ps_dev_release()`. `len` 0 is a zero-length packet; the stream goes on. `uframe` may be `U2PS_UFRAME_UNKNOWN` (0xFFFF) (`device/u2ps_dev.h:712-716`, `common/u2ps_types.h:15-16`). |
| `u2ps_app_on_hole(uframes, why)` | always | `uframes` microframes without a packet, for `why`. One call per reason, `U2PS_MISS_NO_PACKET` first; a hole longer than 65,535 microframes in several calls. The stream's tail comes before `u2ps_app_on_stream_stop()`; in a long gap, the holes held back come at each tick (`device/u2ps_dev.h:718-737`). |
| `u2ps_app_on_pause(uframes)` | always | A `U2PS_MISS_NO_PACKET` gap of more than `U2PS_GAP_PAUSE` microframes. `uframes` is `U2PS_PAUSE_LONG` (0xFFFF) when the stack cannot tell the length (`device/u2ps_dev.h:594-596,739-751`). |
| `u2ps_app_on_stream_stop()` | always | The stream has stopped or its pending start was cancelled; the holes of its tail have come before it. Class memory is the app's again; drop every reference to the stream's packets (`device/u2ps_dev.h:753-761`). |
| `u2ps_app_on_fault(fault)` | always | Only `U2PS_FAULT_OPEN`, followed by `u2ps_app_on_stream_stop()` (`device/u2ps_dev.h:763-768`). |
| `u2ps_app_on_reset()` | always | Bus reset; the stream has stopped and the configuration has ended (`device/u2ps_dev.h:770-772`). |
| `u2ps_app_on_suspend()`, `u2ps_app_on_resume()` | always | Suspend is reported only after a bus reset since the pull-up went on. A resume comes only after a reported suspend, and none after a detach or a VBUS loss (`device/u2ps_dev.h:774-780`). |
| `u2ps_app_on_vbus(present)` | only with `U2PS_VBUS=app` | VBUS went or came (`device/u2ps_dev.h:782-795`). |
| `u2ps_app_on_control(setup, result)` | only with `U2PS_CONTROL_HOOK=1` | Every SETUP the stack answers, for logging; it changes nothing. Do not define it in a build without the hook (`device/u2ps_dev.h:797-812`). |
| `u2ps_app_on_contract(violation)` | only with `U2PS_CONTRACT_CHECKS=1` | A contract violation, reported once (section 5.1). Do not define it in a build without the checks (`device/u2ps_dev.h:814-826`). |

Why a configuration ended, `u2ps_config_end_t` (`device/u2ps_dev.h:301-313`):

| Value | Cause |
|---|---|
| `U2PS_CONFIG_END_NONE` | None: the device entered the Configured state. |
| `U2PS_CONFIG_END_BUS_RESET` | A bus reset; the host re-enumerates, or reset a device that answered too late. |
| `U2PS_CONFIG_END_SET_CONFIGURATION` | SET_CONFIGURATION 0, the host's choice. |
| `U2PS_CONFIG_END_DETACH` | The app's `u2ps_dev_connect(false)`. |
| `U2PS_CONFIG_END_VBUS` | VBUS loss; if the cable was not pulled, check the app's VBUS reading and debounce. |
| `U2PS_CONFIG_END_REINIT` | The app's `u2ps_dev_init()` while attached. |

Order of callbacks when a configuration ends at a bus reset, a detach, a VBUS loss or a re-init: the holes of the stream's tail (`on_hole`, `on_pause`), `on_stream_stop` (if a stream had started or its start was pending), then `on_configured(false, why)`, then at a bus reset `on_reset`, at a VBUS loss `on_vbus(false)`. At SET_CONFIGURATION `on_configured(false)` comes at once and `on_stream_stop` when the close has ended (`device/u2ps_dev.h:54-62`).

Vendor requests. A `u2ps_request_t` holds the `setup` (`u2ps_setup_t`), `buf`, `size`, `len` and `id`. `req->buf` is lent for the callback only, also when the callback defers. A device-to-host request may write its answer into `buf` (`size` bytes, `U2PS_EP0_BUF`) and return the length; the stack sends at most `wLength`. Host-to-device data is in `buf`, `len` bytes, always `wLength`: a shorter data stage is a request error that never reaches the app (USB 2.0 9.3.5). A negative result other than `U2PS_REQ_STALL` and `U2PS_REQ_DEFER` is a request error too, and a contract violation with the checks. To answer later, return `U2PS_REQ_DEFER`, keep `req->id`, and call `u2ps_dev_reply(id, data, result)` from the thread; `data` must be in the app's own memory, never `req->buf`. It returns false if the request is no longer pending. EP0 NAKs meanwhile; the host allows 50 ms for a request without data and 500 ms for the first data packet (`device/u2ps_dev.h:15-21,317-346,571-584`).

### 3.8 The stream

Start. The host selects a streaming alternate setting and the stack calls `u2ps_app_on_stream_start(info, cfg)`. `info` is a `u2ps_stream_info_t`: `stream` (a number from 1, never 0), `alternate_setting`, `format`, `sampling_frequency` and `max_packet_size` (`device/u2ps_dev.h:350-361`). The port enables the stream's endpoints only after the app has lent the memory, so no packet comes before (`device/u2ps_dev.h:685-692`).

Fill `*cfg`, a `u2ps_stream_cfg_t`. The port refuses the memory (`U2PS_FAULT_OPEN`) unless all of these hold (`device/u2ps_dev.h:389-419`):

| Field | Rule |
|---|---|
| `mem` | Not `NULL`; on `U2PS_DPORT_ALIGN`; declare it `_Alignas(U2PS_DPORT_ALIGN)`; in DMA memory. |
| `stride` | At least `U2PS_DPORT_STRIDE(info->max_packet_size)` and a multiple of `U2PS_DPORT_ALIGN`. Extra bytes per buffer (a canary, a header) keep it a multiple: round the sum up to `U2PS_DPORT_ALIGN`. |
| `count` | From `U2PS_DPORT_ISO_OUT_MIN` to `U2PS_ISO_OUT_COUNT_MAX`. The port keeps `U2PS_DPORT_ISO_OUT_RESERVE` of the buffers armed (1 on DWC2, 2 on ChipIdea), so you hold at most `count` - `U2PS_DPORT_ISO_OUT_RESERVE` packets: to hold k, lend k + `U2PS_DPORT_ISO_OUT_RESERVE`. A packet that comes while you hold that many is a `U2PS_MISS_NO_BUFFER` hole of its own microframe. |
| all `count` * `stride` bytes | In memory the port's DMA reaches (section 3.1). |
| `fb_value` | Already the nominal value (sampling frequency / 8000 in 16.16 samples per microframe, rounded to the nearest). Change it only for another first value. |

The reference app's stride: the port's least stride plus a 4-byte canary word, rounded up to `U2PS_DPORT_ALIGN` (`examples/nucleo_u5a5zj_q/app_class_mem.h:15-26`).

| Return of `on_stream_start` | Effect |
|---|---|
| `U2PS_STREAM_OPEN` | The port opens the stream now, with `*cfg` (`device/u2ps_dev.h:690-692`). |
| `U2PS_STREAM_DEFER` | The app opens it later from the thread with `u2ps_dev_stream_open(info->stream, cfg)`. SET_INTERFACE's status stage waits for it, at most `U2PS_START_MAX` ms (`device/u2ps_dev.h:693-697`). |
| `U2PS_STREAM_REFUSE` | Nothing opens. SET_INTERFACE stalls and the interface keeps alternate setting 0. No `on_stream_stop` follows (`device/u2ps_dev.h:698-699`). |

Every start that is not refused gets exactly one `on_stream_stop`, also when a bus reset, a detach, a VBUS loss, a re-init, SET_CONFIGURATION or a new SET_INTERFACE cancels it, and when the port refuses the memory (after `on_fault`) (`device/u2ps_dev.h:700-705`). The next stream's start waits for the last stream's close to end; the last stream's memory may be lent again at once, even in the same interrupt (`device/u2ps_dev.h:685-688,753-760`).

Deferring. `u2ps_dev_stream_open(stream, cfg)` is thread only. It returns a `u2ps_open_result_t` (`device/u2ps_dev.h:467-483,627-648`):

| Value | Meaning and fix |
|---|---|
| `U2PS_OPEN_DONE` | The stream is open. |
| `U2PS_OPEN_NOT_PENDING` | No start of that number is pending (cancelled, never deferred, or another stream's number). Nothing happened. Pass `info->stream` of an `on_stream_start` that returned `U2PS_STREAM_DEFER`, before its `on_stream_stop`. |
| `U2PS_OPEN_REFUSED` | `cfg` was `NULL`; the start is refused as with `U2PS_STREAM_REFUSE`. |
| `U2PS_OPEN_FAULT` | The port refused the memory. `on_fault(U2PS_FAULT_OPEN)` and `on_stream_stop` have come, in this thread. |

If the host changes the sampling frequency while the start is pending, the stream runs at the new one, the feedback starts at its nominal value instead of `cfg->fb_value`, and `info->sampling_frequency` no longer holds; `u2ps_app_on_sampling_frequency()` told the app (`device/u2ps_dev.h:639-643`). Keep the deferred `cfg` valid until you open or the start is cancelled. Linux selects every streaming alternate setting in turn while it enumerates, so with `U2PS_STREAM_DEFER` each start holds EP0 for the app's delay (`device/u2ps_dev.h:84-87`).

If you can lend memory at once but are not ready to consume, return `U2PS_STREAM_OPEN` and release nothing until ready. Each packet the host sends meanwhile is one `U2PS_MISS_NO_BUFFER` microframe; microframes without one stay `U2PS_MISS_NO_PACKET` (a pause if long), and EP0 is not held (`device/u2ps_dev.h:79-83`).

Packets and release. Every packet, zero-length ones included, is held until `u2ps_dev_release(stream, n)` gives the `n` oldest back to the port, in arrival order. Call it inside a callback, or from the thread. A release for a stopped stream, or of more packets than held, is ignored and counted in `release_errors` (`device/u2ps_dev.h:586-592`). A release writes no register of the port: a buffer is always armed, and a freed one is armed at the next completion (`device/u2ps_dport.h:242-248`). Audio data is never copied: `data` points into your class memory. Keep the buffer unchanged until you release it. The reference app releases one packet per `on_packet` (`examples/nucleo_u5a5zj_q/main.c:473-488`).

Holes and pauses. `on_hole(uframes, why)` reports microframes without a packet. With `U2PS_DPORT_UFRAME_EXACT` it comes immediately before the packet that ends the hole; otherwise before the first packet after the port's report (`device/u2ps_dev.h:718-737`). A hole longer than 65,535 microframes comes in several calls of the same reason. A `U2PS_MISS_NO_PACKET` gap longer than `U2PS_GAP_PAUSE` microframes is an `on_pause()`, never an `on_hole()`; holes of other reasons stay holes however long, and a gap of other reasons alone, such as a hold of every buffer you may hold while the host sends, is no pause, however long the ticks saw no packet. Call `u2ps_dev_tick()` regularly, or a gap of 1.024 s or more is decided by the port's reports alone (`device/u2ps_dev.h:147-152,604-616`).

The stream's tail: the holes after its last delivered packet, up to the last packet the port received (the packets that came while you held all you may, as `U2PS_MISS_NO_BUFFER`, and the microframes without a packet between them), come before `u2ps_app_on_stream_stop()`, at the close, a bus reset, a detach, a re-init and a VBUS loss, in the context of that `on_stream_stop`. The port counts them, so `NO_PACKET` there beyond `U2PS_GAP_PAUSE` is an `on_pause()`. Nothing after the last packet the port received is reported (`device/u2ps_dport.h:300-329`).

Holes at the long gap. When no packet has been delivered for more than `U2PS_GAP_PAUSE` microframes' worth (50 ms by default), each `u2ps_dev_tick()` asks the port for the holes it holds back (`u2ps_dport_report_holes()`) and reports them, with those the stack has summed, as at the stream's end: in your thread, with the vectors masked, before any `on_pause()`. So when you hold every buffer you may while the host streams, and the host then pauses, the `U2PS_MISS_NO_BUFFER` holes of that hold reach you about 50 ms after your last packet, not only with the next packet or at the stream's end. Each hole is reported once: the next packet brings only what came after, so a host pause after the hold is an `on_pause()` (or a `U2PS_MISS_NO_PACKET` hole) of the pause's own microframes, and the tail holds only what came after the last tick. Without tick calls the holes wait for the next packet or the tail (`device/u2ps_dev.h:604-625,718-737`).

`U2PS_UFRAME_UNKNOWN` (0xFFFF) is the `uframe` of a packet whose microframe the port does not know. It lies outside the numbers a microframe has (0 to `U2PS_UFRAME_MASK`, 0x3FFF) (`common/u2ps_types.h:15-16`, `common/u2ps_uframe.h:27`). When it occurs:

| Port | Value of `uframe` in `u2ps_app_on_packet()` |
|---|---|
| DWC2 | Never. The port computes every packet's microframe from the frame number it reads at the packet's completion, and `U2PS_DPORT_UFRAME_EXACT` is 1 (`port/dwc2/dcd/u2ps_dwc2_iso.c:65-69,160-167`, `port/dwc2/dcd/u2ps_dwc2_dcd.c:782`, `port/dwc2/u2ps_dport_cfg.h:53-56`). |
| ChipIdea | Never, in the port's logic. With `U2PS_CHIPIDEA_FN_UNIT=uframes` (`U2PS_DPORT_UFRAME_EXACT` 1) each number is exact. With `none` or `frames` (`U2PS_DPORT_UFRAME_EXACT` 0) it is an estimate: the last packet's number plus one plus the holes reported before the packet. The logic holds `U2PS_UFRAME_UNKNOWN` only as the start value of a retired transfer that delivers no packet (`port/chipidea/dcd/u2ps_chipidea_iso.c:304,400-403`, `port/chipidea/dcd/u2ps_chipidea_iso.h:75-79`, `port/chipidea/u2ps_dport_cfg.h:55-61`). The port has no register code yet, so no packet reaches an app (section 1). |
| Test kit | Only if your test injects it: `u2ps_testkit_dev_packet(len, U2PS_UFRAME_UNKNOWN)` (`testkit/fake_dport.c:707-729`). |

So `U2PS_DPORT_UFRAME_EXACT` 0 does not mean unknown, it means estimated: such a number is good for ordering and for the gap to the next packet, not for a hardware timestamp. The port interface still allows the value (`device/u2ps_dev.h:712-716`), so the app accepts it and does not use `uframe` as an index or a time. The stack then takes no microframe distance for that packet and for the next one, and decides the holes from the port's reports alone (`device/u2ps_dev_audio.c:497-505,519`; `tests/test_dev_stream.c:358-376`). Checked with the test kit: two packets with it, then packet 5 and packet 100, gave `on_packet` with `uframe=65535` twice, no hole before packet 5, and a hole of 94 before packet 100.

Hole reasons, `u2ps_miss_t` (`common/u2ps_types.h:30-57`):

| Value | Cause and fix |
|---|---|
| `U2PS_MISS_NO_PACKET` | No valid packet; cause unknown. The host sent nothing (a pause if long) or the packet was lost on the bus. Nothing to change in the app; if it recurs while the host plays, check cable, hubs and host. |
| `U2PS_MISS_LATE` | The port armed too late; the USB interrupt ran late. Give the vector the highest priority that runs code; keep other handlers and masked regions short. |
| `U2PS_MISS_NO_BUFFER` | The host sent a packet and no buffer was free: you held `count` - `U2PS_DPORT_ISO_OUT_RESERVE` packets. Release sooner (`u2ps_dev_release()`) or lend more buffers (`u2ps_stream_cfg_t.count`). |
| `U2PS_MISS_CORRUPTED` | A packet that did not cross the bus intact (CRC error, dropped). Check the signal path: cable, connector, hub, board. |

Feedback. The stream starts with `cfg->fb_value`. `u2ps_dev_fb_set(value)` (any context) sets the 16.16 samples per microframe that the feedback endpoint sends from now on, within two polls. A change of the sampling frequency sets the new rate's nominal value (`device/u2ps_dev.h:598-602`).

### 3.9 Faults, causes and counters

`u2ps_fault_t` (`device/u2ps_dev.h:437-458`):

| Fault | Reported by | Cause and fix |
|---|---|---|
| `U2PS_FAULT_NONE` | return of `u2ps_dev_tick()` | No fault. |
| `U2PS_FAULT_OPEN` | `u2ps_app_on_fault()`, then `u2ps_app_on_stream_stop()` | The port refused the class memory: a rule of `u2ps_stream_cfg_t` is broken. The stream did not start; SET_INTERFACE stalls and the interface keeps alternate setting 0. |
| `U2PS_FAULT_CLOSE` | return of `u2ps_dev_tick()`, once per close | A close took longer than `U2PS_CLOSE_MAX` ms. Call tick at least every 10 ms; if it persists, the port's close is stuck. The next stream still starts when the close ends. |
| `U2PS_FAULT_START` | return of `u2ps_dev_tick()`, once per start | A start held SET_INTERFACE's status stage for `U2PS_START_MAX` ms: the app deferred and did not open in time, or the last close was slow. The status then completes; the start stays pending and may still open. |

`u2ps_dev_tick(now_ms)` returns one fault per call; `U2PS_FAULT_CLOSE` and `U2PS_FAULT_START` for one SETUP come on separate calls. Without tick calls there is neither bound (`device/u2ps_dev.h:604-625`).

The cause of `U2PS_FAULT_OPEN` on DWC2. Read `u2ps_dwc2_dcd_open_error()` (any context) in `u2ps_app_on_fault(U2PS_FAULT_OPEN)` or after it. It returns a `u2ps_dwc2_open_err_t`, `U2PS_DWC2_OPEN_OK` after an open that succeeded (`port/dwc2/dcd/u2ps_dwc2_dcd.h:53-58`, `port/dwc2/dcd/u2ps_dwc2_iso.h:90-119`). The checks run in this order:

| Value | Rule broken and fix |
|---|---|
| `U2PS_DWC2_OPEN_COUNT` | `count` outside `U2PS_DPORT_ISO_OUT_MIN` to `U2PS_ISO_OUT_COUNT_MAX`. |
| `U2PS_DWC2_OPEN_MEM_NULL` | `mem` is `NULL`. |
| `U2PS_DWC2_OPEN_STRIDE_SHORT` | `stride` below the transfer size; use at least `U2PS_DPORT_STRIDE(info->max_packet_size)`. |
| `U2PS_DWC2_OPEN_STRIDE_ALIGN` | `stride` not a multiple of `U2PS_DPORT_ALIGN`; round it up. |
| `U2PS_DWC2_OPEN_MEM_ALIGN` | `mem` not on `U2PS_DPORT_ALIGN`; declare it `_Alignas(U2PS_DPORT_ALIGN)`. |
| `U2PS_DWC2_OPEN_NOT_DMA` | `count` * `stride` bytes not all in DMA memory (section 3.1). |
| `U2PS_DWC2_OPEN_STREAM` | The stream's other fields, or a stream still open or closing: the stack's, not the app's; report it. |

Counters. `u2ps_dev_stats(&stats)` (thread) copies a `u2ps_dev_stats_t`: `late_completions`, `late`, `no_buffer`, `corrupted`, `fb_recoveries`, `errors`, `fallback_over`, `release_errors`, `uframe_back` (`device/u2ps_dev.h:421-435,650-651`). The DWC2 port adds bench counters: `u2ps_dwc2_dcd_diag()` fills a `u2ps_dwc2_dcd_diag_t` (thread), `u2ps_dwc2_dcd_fb_start()` the feedback start record of the last open, and `u2ps_dwc2_dcd_mode()` with `u2ps_dwc2_dcd_mode_get()` set and read the switches for bench comparisons (`port/dwc2/dcd/u2ps_dwc2_dcd.h:60-137`).

### 3.10 A compilable skeleton of the callbacks

Checked: it compiles with `-Wall -Wextra -Werror` and the warnings of `CMakeLists.txt:69-72`, for the STM32U5A5, STM32F723 and i.MX RT1062 port headers, and links with the PC fake port. It consumes nothing; add your sink where marked.

```c
#include <stdbool.h>
#include <stdint.h>
#include "u2ps_dev.h"
#include "u2ps_dport_cfg.h"

#define BUFFERS 8u
/* Place class_mem in DMA memory (section 3.1). */
static _Alignas(U2PS_DPORT_ALIGN) uint8_t class_mem[BUFFERS * U2PS_DPORT_STRIDE(U2PS_ISO_OUT_MPS_MAX)];
static uint32_t stream_no;

int u2ps_app_on_request(const u2ps_request_t *req) { (void)req; return U2PS_REQ_STALL; }
int u2ps_app_on_sampling_frequency(uint32_t requested, uint32_t chosen, uint32_t id)
{
  (void)requested; (void)chosen; (void)id;
  return 0; /* accept; chosen applies */
}
void u2ps_app_on_configured(bool configured, u2ps_config_end_t why) { (void)configured; (void)why; }
u2ps_stream_start_t u2ps_app_on_stream_start(const u2ps_stream_info_t *info, u2ps_stream_cfg_t *cfg)
{
  uint16_t stride = U2PS_DPORT_STRIDE(info->max_packet_size);
  if (BUFFERS * (uint32_t)stride > sizeof class_mem)
    return U2PS_STREAM_REFUSE;
  stream_no = info->stream;
  cfg->mem = class_mem;
  cfg->stride = stride;
  cfg->count = BUFFERS;
  return U2PS_STREAM_OPEN;
}
void u2ps_app_on_packet(const uint8_t *data, uint16_t len, u2ps_uframe_t uframe)
{
  (void)data; (void)len; (void)uframe;
  /* consume data[0..len) here, then give the buffer back */
  u2ps_dev_release(stream_no, 1);
}
void u2ps_app_on_hole(uint16_t uframes, u2ps_miss_t why) { (void)uframes; (void)why; }
void u2ps_app_on_pause(uint16_t uframes) { (void)uframes; }
void u2ps_app_on_stream_stop(void) {}
void u2ps_app_on_fault(u2ps_fault_t fault) { (void)fault; }
void u2ps_app_on_reset(void) {}
void u2ps_app_on_suspend(void) {}
void u2ps_app_on_resume(void) {}
void u2ps_app_on_vbus(bool present) { (void)present; } /* U2PS_VBUS=app only */
```

The skeleton's stride is valid on every chip, because `U2PS_DPORT_STRIDE()` is already a multiple of `U2PS_DPORT_ALIGN`.

### 3.11 Context rules

Contexts: *interrupt* is the port's interrupt (section 3.6). *Thread* is the app's non-interrupt code, the one thread of section 3.4. *Masked thread* is that thread's code while the stack has the port's vectors masked (`device/u2ps_dev.h:15-27`).

| Call | Context |
|---|---|
| `u2ps_dev_init()`, `u2ps_dev_connect()`, `u2ps_dev_tick()`, `u2ps_dev_reply()`, `u2ps_dev_stream_open()`, `u2ps_dev_stats()` | Thread only, never from an interrupt (`device/u2ps_dev.h:23-27`). |
| `u2ps_dev_vbus()` (mode app) | Thread only (`device/u2ps_dev.h:543-545`). |
| `u2ps_dev_release()` | Inside any callback, or from the thread. |
| `u2ps_dev_fb_set()`, `u2ps_dev_speed()`, `u2ps_dev_vbus_present()`, `u2ps_dev_description_error()` | Any context. |

| Callback | Context |
|---|---|
| `on_request`, `on_sampling_frequency`, `on_configured(true)`, `on_stream_start`, `on_packet`, `on_reset`, `on_suspend`, `on_resume` | Interrupt only. |
| `on_hole`, `on_pause` | Interrupt; masked thread for the stream's tail when `u2ps_dev_connect(false)`, `u2ps_dev_init()` (a re-init) or `u2ps_dev_vbus()` (a loss) ends the stream, right before its `on_stream_stop`, and from `u2ps_dev_tick()` at each tick of a long gap (the holes held back). |
| `on_stream_stop` | Interrupt; masked thread when `u2ps_dev_connect(false)`, `u2ps_dev_init()` (a re-init) or `u2ps_dev_vbus()` (a loss) ends the stream, or `u2ps_dev_stream_open()` finds the port refusing. |
| `on_configured(false)` | Interrupt; masked thread under `u2ps_dev_connect(false)`, `u2ps_dev_init()` and `u2ps_dev_vbus()` (a loss). |
| `on_fault(U2PS_FAULT_OPEN)` | Interrupt; the thread of `u2ps_dev_stream_open()` for a deferred start. |
| `on_vbus` | Masked thread, from `u2ps_dev_vbus()`; unmasked before the first successful init. |
| `on_control` (hook build) | Interrupt; masked thread for a deferred request, a deferred start or a start past `U2PS_START_MAX`. |

No callback runs while another does. Inside any callback, in the interrupt or in thread, the app may call only `u2ps_dev_speed()`, `u2ps_dev_vbus_present()`, `u2ps_dev_fb_set()` and `u2ps_dev_release()`; it must defer `u2ps_dev_init()`, `u2ps_dev_connect()`, `u2ps_dev_vbus()`, `u2ps_dev_reply()`, `u2ps_dev_tick()`, `u2ps_dev_stream_open()` and `u2ps_dev_stats()` to its thread (`device/u2ps_dev.h:29-52`).

## 4. Host stack (a player)

### 4.1 State today

The host stack enumerates the one device on the root port, finds its audio function, configures it, reads its clock and streams to it (`host/u2ps_host.h:7-11`). It runs on the PC against the fake host port, and with the device stack in a loopback (`tests/test_host_loopback.c:1-41`); the test kit runs your player app the same way (section 7). The host library builds for every chip (`host/CMakeLists.txt:84-93`); on a DWC2 chip it links the DWC2 host port, which has register code but has not run on a board yet. The ChipIdea host port does not exist (section 1).

The DWC2 host port, as the device port's section 3.6 describes it for the device stack:
1. Its header is `port/dwc2/hcd/u2ps_dwc2_hcd.h`. Define your chip's OTG_HS vector function and call `u2ps_dwc2_hcd_irq()` from it; `U2PS_DWC2_IRQ` is the IRQ number, 73 on the STM32U5A5, 77 on the STM32F723, 91 on the STM32H7S3 (`port/dwc2/common/u2ps_dwc2_chip.h:54,77,97`). Set its priority and enable it after a successful `u2ps_host_init()`; no other interrupt that runs port or stack code may share or exceed that priority (`port/dwc2/hcd/u2ps_dwc2_hcd.h:67-74`). The stack masks and unmasks the vector itself (`u2ps_hport_irq_mask()`).
2. Before `u2ps_host_init()` the app starts the HSE, and on the STM32U5A5 sets voltage range 1 or 2 with TrustZone off, as for the device port (section 3.2). The port's init checks the DMA memory first (section 4.2), then starts the PHY at its first call and resets the core at each call (`host/u2ps_hport.h:128-138`). On a Cortex-M7 (STM32F723, STM32H7S3) the PHY's start first unlocks the DWT, enables its cycle counter and checks that it counts, else `U2PS_HOST_ERR_TIMER` (`port/dwc2/common/u2ps_dwc2_phy.c:21-38`).
   STM32H7S3, before `u2ps_host_init()` (`port/dwc2/common/u2ps_dwc2_phy.c:156-202`): the app has written the supply configuration, the lower byte of `PWR_CSR2`, which is write-once after power-on; it has set `PWR_CSR2.USB33DEN` and waited for `USB33RDY` before it configures PM0, PM1, PM8 or PM9 (RM0477 6.8.4; ES0596 2.2.19); and the HSE runs (else `U2PS_HOST_ERR_CLOCK`). The port then writes only the upper half-word of `PWR_CSR2` (`USB33DEN` and `USBHSREGEN` set, `USBREGEN` as the board left it), waits up to 10 ms for `USB33RDY` (else `U2PS_HOST_ERR_PHY`), selects the HSE as the USB PHY's kernel clock with `RCC_CCIPR1.USBREFCKSEL` for `U2PS_DWC2_HSE_HZ` while `RCC_AHB1ENR.USBPHYCEN` is 0, and enables `USBPHYCEN` and `OTGHSEN`. Set `U2PS_DWC2_CORE_HZ` to your highest core clock.
3. Why its init failed: `u2ps_dwc2_hcd_init_error()` (any context) gives the port's own `u2ps_dwc2_err_t`; the port maps it onto `u2ps_host_err_t` as the table of section 4.3 shows (`port/dwc2/hcd/u2ps_dwc2_hcd.h:10-20`, `port/dwc2/hcd/u2ps_dwc2_hcd.h:76-78`).
4. Diagnostics: `u2ps_dwc2_hcd_diag()` (thread) copies a `u2ps_dwc2_hcd_diag_t` of bench counters, among them `ahberr` for a bus error of the DMA, `babble`, `overcurrent`, `soft_resets` and `timeouts`; they restart at each `u2ps_host_init()` (`port/dwc2/hcd/u2ps_dwc2_hcd.h:80-102`). An injection build (`U2PS_TEST_INJECT=ON`, never a release) adds `u2ps_dwc2_hinject()` and the handler's pass profile `u2ps_dwc2_hpasses_get()` (`port/dwc2/hcd/u2ps_dwc2_hcd.h:105-140`).
5. Limits: a feedback endpoint of bInterval 1 to 13 only. bInterval 14 to 16, which USB 2.0 allows, ends the stream start with `U2PS_HOST_STOP_OPEN`: the port counts microframes modulo 2^14 with signed distances, which a poll period of 2^13 microframes or more exceeds, so 2^12 (512 ms) is the longest (`port/dwc2/hcd/u2ps_dwc2_hcd.h:27-34`). Port babble holds the handler for about 26 ms, at most 66 ms, for the core soft reset that RM0456 asks for; thread code and every interrupt at or below the USB priority wait meanwhile, so give audio DMA, watchdog feeds and safety polls a higher priority or a 66 ms margin. A disconnect flushes the channels without a wait (`port/dwc2/hcd/u2ps_dwc2_hcd.h:44-52`, `host/u2ps_hport.h:270-276`). A run of 8,192 microframes (1.024 s) or more with the stream's queue dry counts modulo 16,384 microframes, and as none when that remainder is 8,192 or more: its NO_BUFFER hole or its drained microframes are then reported short or not at all, a known limit of 0.1.0 (`host/u2ps_hport.h:288-297`).
6. Checked: the host skeleton of section 4.9 with a main loop and an OTG_HS vector that calls `u2ps_dwc2_hcd_irq()` links for the STM32U5A5, the STM32F723 and the STM32H7S3 (all RAM in AXI SRAM2), with and without `U2PS_LTO`, with ATfE and GCC, without an undefined symbol. That is all: it has not run on a board. Host streaming plays at high speed only; a full- or low-speed device is identified and its session ends (`host/u2ps_host.h:53-59`).

The host port interface is `host/u2ps_hport.h`; its configuration header `u2ps_hport_cfg.h` comes with each port (`host/u2ps_hport.h:34-43`). An app sees these constants:

| Constant | Meaning | DWC2 | ChipIdea |
|---|---|---|---|
| `U2PS_HPORT_ALIGN` | Alignment in bytes of every queued packet. | 4 | 1 |
| `U2PS_HPORT_LEAD` | Microframes ahead the port arms a packet. | 1 | 2 |
| `U2PS_HPORT_UFRAME_EXACT` | 1: `on_sent` microframe numbers are exact. | 1 | 1 |

Sources: `port/dwc2/u2ps_hport_cfg.h:13-33`, `port/chipidea/u2ps_hport_cfg.h:11-24`. The values are inferred from the manuals, not yet measured on a board.

### 4.2 Memory

Packets lie in DMA memory, start on `U2PS_HPORT_ALIGN` and stay untouched until their `on_sent`. A contiguous stream stays aligned only if every packet's length is a multiple of `U2PS_HPORT_ALIGN` (`host/u2ps_host.h:624-631`). The host stack's SETUP packet and control buffer, and the DWC2 host port's feedback buffers and status stage's buffer, are in section `.bss.u2ps_dma` (`U2PS_DMA_SECTION`); place it as in section 3.1 (`host/u2ps_hport.h:57-64`). `u2ps_host_init()` checks every byte of it through the port (`u2ps_hport_dma_check()`, by the same rule as the device port's) before anything else, at every init, and returns `U2PS_HOST_ERR_DMA_MEMORY` if one lies where the port's DMA does not reach it; nothing changes then, not even at a re-init. The stack and the port zero that memory at each init, so the section may be `NOLOAD` (`host/u2ps_host.c:810-814`, `port/dwc2/hcd/u2ps_dwc2_hcd.c:1160-1165`).

DMA memory on the STM32H7S3 (`port/dwc2/common/u2ps_dwc2_dma.h:77-99`): the OTG_HS DMA reaches the AXI SRAMs and the AHB SRAMs, never DTCM, ITCM or the backup SRAM (RM0477 Table 2). DTCM, the STM32F723's usual DMA memory, is not reached. The port reads the option bytes in `FLASH_OBW2SR`: `ITCM_AXI_SHARE` maps AXI SRAM1 (000 or 011: 0x2400_0000-0x2401_FFFF; 001: 0x2401_0000-0x2401_FFFF; 010: none), `DTCM_AXI_SHARE` maps AXI SRAM3 (000 or 011: 0x2404_0000-0x2405_FFFF; 001: 0x2404_0000-0x2404_FFFF; 010: none), an undefined value (bit 2 set) refuses that SRAM, and AXI SRAM4 (0x2406_0000-0x2407_1FFF) only with `ECC_ON_SRAM` = 0, whose factory value is 1. AXI SRAM2, 0x2402_0000-0x2403_FFFF, is always mapped and has no ECC: place `*(.bss.u2ps_dma)` and your packets there. AHB SRAM1 (0x3000_0000) and SRAM2 (0x3000_4000) count only with their clocks, `RCC_AHB2ENR.SRAM1EN` and `SRAM2EN`, off at reset. All of this RAM is cached by default: the port cleans each queued packet's lines before its DMA reads them, and cleans and invalidates its own buffers (`port/dwc2/common/u2ps_dwc2.h:110-172`). With `U2PS_CONTRACT_CHECKS=ON` the stack checks each queued packet against this rule (section 5.1, `U2PS_CONTRACT_HOST_QUEUE_DATA`).

### 4.3 Init, connect, VBUS, tick

The thread rule. `u2ps_host_init()`, `u2ps_host_connect()`, `u2ps_host_vbus()`, `u2ps_host_tick()`, `u2ps_host_test_mode()`, `u2ps_host_suspend()`, `u2ps_host_resume()`, `u2ps_host_stream_start()`, `u2ps_host_stream_stop()` and `u2ps_host_stats()` all run in one thread context, never from an interrupt and never from each other: each masks the port's vectors, and the mask does not nest (`host/u2ps_host.h:21-26`).

| Call | Behaviour |
|---|---|
| `u2ps_host_init()` | Checks the DMA memory (section 4.2), resets the stack and starts the port. Returns `U2PS_HOST_OK` or a `u2ps_host_err_t` (table below). Called again it re-inits: a session ends with `U2PS_HOST_END_REINIT`, the controller resets, the session permission and test mode end; the VBUS level is kept (`host/u2ps_host.h:512-521`). |
| `u2ps_host_connect(on)` | `true` allows sessions; `false` ends a session with `U2PS_HOST_END_DETACH` and disables the port. A session starts once a device is connected, VBUS is present and sessions are allowed. Refused, with no effect, in test mode and before a successful init (`host/u2ps_host.h:523-528`). |
| `u2ps_host_vbus(present)` | Report the level you sense on every poll; the stack acts on changes. A loss ends a session with `U2PS_HOST_END_VBUS`. Until the first call VBUS is absent; calls before init count. The board supplies VBUS with its over-current protection; the controller cannot (`host/u2ps_host.h:152-156,530-534`). |
| `u2ps_host_tick(now_ms)` | Call at least every 10 ms. It runs the enumeration's waits, the control timeouts, the test device's waits and the steps of a resume, each measured from the first tick after the wait began. Each wait is a lower bound: it ends at the first tick that reads more than its value in ms since its start, so it lasts more than its value at any phase of your millisecond counter, and a longer period lengthens it by up to one period (`host/u2ps_host.h:536-544`). |
| `u2ps_host_test_mode(selector)` | Puts the root port into USB test mode 1 to 5; a session ends with `U2PS_HOST_END_TEST_MODE`. Returns false for another selector, in test mode, or before init. Only init or a power cycle ends it (`host/u2ps_host.h:546-552`). |
| `u2ps_host_suspend()` | Suspends the root port: the SOFs stop, and the device suspends. Returns false, changing nothing, for one of five causes (section 4.10). Not while a stream starts, runs or stops (`host/u2ps_host.h:554-568`). |
| `u2ps_host_resume()` | Resumes the port that `u2ps_host_suspend()` suspended; `u2ps_app_host_on_resumed(false)` follows. Returns false when the port is not suspended or a resume runs (section 4.10, `host/u2ps_host.h:570-578`). |
| `u2ps_host_stats(&stats)` | Copies a `u2ps_host_stats_t` (thread). |

`u2ps_host_err_t` from init (`common/u2ps_types.h:169-211`):

| Value | Cause and fix |
|---|---|
| `U2PS_HOST_OK` | Started. |
| `U2PS_HOST_ERR_CLOCK` | A clock the controller or PHY needs is not running; start it (DWC2: the HSE at `U2PS_DWC2_HSE_HZ`). |
| `U2PS_HOST_ERR_POWER` | A supply or voltage setting is missing (STM32U5 voltage range 1 or 2). |
| `U2PS_HOST_ERR_SECURITY` | STM32U5 TrustZone is on; set TZEN = 0. |
| `U2PS_HOST_ERR_PHY` | PHY, supply or PLL did not come up; check the board's USB supply and `U2PS_DWC2_HSE_HZ`. |
| `U2PS_HOST_ERR_RESET` | The controller's reset did not end; its bus clock or PHY clock is not running. |
| `U2PS_HOST_ERR_MODE` | The controller did not enter host mode; other code drives it. |
| `U2PS_HOST_ERR_MEMORY` | FIFO RAM or port memory does not fit, or a flush timed out. |
| `U2PS_HOST_ERR_PORT` | Another reason of the port. |
| `U2PS_HOST_ERR_DMA_MEMORY` | `.bss.u2ps_dma` lies where the port's DMA does not reach it, or its SRAM's clock is off or it is powered down; place it as in section 3.1, on the STM32H7S3 as in section 4.2. Checked first, at every init; nothing changes. |
| `U2PS_HOST_ERR_TIMER` | The cycle counter that times the port's waits does not count; nothing was started. Leave the DWT to the port, as for `U2PS_DEV_ERR_TIMER` (section 3.4), and init again. DWC2: `U2PS_DWC2_ERR_CYCLES`. |

### 4.4 Sessions, enumeration and why a session ended

A session begins when the stack starts the root port's reset and ends with exactly one `u2ps_app_host_on_end(why, refusal)`. A disconnect before the reset begins ends no session. After a failure the stack leaves the device alone (the port is disabled): a new connect of the device, or `u2ps_host_connect(false)` then `(true)`, tries again (`host/u2ps_host.h:40-47`).

Enumeration, driven by `u2ps_host_tick()` (`host/u2ps_host.h:49-89`): 100 ms after the connect; reset for 50 ms; 10 ms reset recovery; GET_DESCRIPTOR(device) at address 0, 64 bytes at high speed and the first 8 at full and low speed, whose bMaxPacketSize0 must be 64 at high speed, 8, 16, 32 or 64 at full speed and 8 at low speed (else `U2PS_HOST_END_EP0_MPS`); SET_ADDRESS(1) and 2 ms; the device descriptor (a hub ends the session with `U2PS_HOST_END_HUB`; the test device goes on as below), then each configuration in turn until one holds a playable audio function within the power budget (at full and low speed, then the device qualifier and the other-speed configurations, below), then the strings in the first LANGID; SET_CONFIGURATION; RANGE and CUR of the clock's Sampling Frequency Control and, if readable, CUR of its Clock Validity Control; then `u2ps_app_host_on_device()`. Every control transfer may take `U2PS_HOST_CONTROL_MAX` ms, then the session ends with `U2PS_HOST_END_TIMEOUT`. A STALL of a string request leaves that string `NULL`, and a STALL of the Clock Validity Control leaves the validity unknown; any other STALL ends the session. The stack does not retry.

Full and low speed (`host/u2ps_host.h:53-59,65-79`). A host's port must support all three speeds (USB 2.0 7.1.2.3), but u2ps streams at high speed only. A full- or low-speed device is enumerated as far as its configurations, to identify it, and its session then ends: with `U2PS_HOST_END_SPEED` at the first configuration that holds a playable audio function. With none, the stack reads the device qualifier (USB 2.0 9.6.2), which a high-speed-capable device gives: a STALL, the answer of a full-speed-only device, or a malformed qualifier ends the session with `U2PS_HOST_END_NOT_PLAYABLE`; a valid one, its other-speed configurations (9.6.4) in turn, read as configurations are, and the first that holds a playable audio function ends it with `U2PS_HOST_END_SPEED`, none with `U2PS_HOST_END_NOT_PLAYABLE`. That refusal is always the first full-speed configuration's. So a high-speed DAC whose full-speed configuration has no streaming interface ends with `U2PS_HOST_END_SPEED`. The device is never configured, and no string is read. A device that answers nothing ends with `U2PS_HOST_END_TIMEOUT` or `U2PS_HOST_END_ERROR`. Built with `U2PS_HOST_REFUSE_FS=ON`, the stack ends every full- or low-speed session at once with `U2PS_HOST_END_SPEED`, before any request: a documented deviation (`host/u2ps_host.h:202-205`).

Power (`host/u2ps_host.h:91-96`). The stack selects no configuration whose bMaxPower x 2 mA exceeds `U2PS_HOST_IA_VBUS_RATED_MA` (USB 2.0 9.2.5.1); bMaxPower counts bus current whatever bmAttributes D6 says. When no configuration is playable within the budget but one was playable apart from its power, the session ends with `U2PS_HOST_END_POWER`, so a device that draws too much gets the power message, not `U2PS_HOST_REFUSAL_NOT_AUDIO` of a first configuration without audio. The USB-IF test device 0x1A0A/0x0200 declares the smaller of your rating and 500 mA, so it always fits.

The test device (`host/u2ps_host.h:98-126`). USB-IF's Embedded Host tests use a test device of VID 0x1A0A whose PID selects the test; a product must recognise it (OTG and EH Compliance Plan Rev. 1.2, 8.1.8). The stack handles it always, at any speed, for these PIDs only; every other PID of 0x1A0A takes the normal path. It reads configuration 0 whole, checks its power (over the budget: `U2PS_HOST_END_POWER` and no test), sends SET_CONFIGURATION with its bConfigurationValue, calls `u2ps_app_host_on_test_device(id_product, bcd_device)` in the interrupt, and runs the test:

| PID | Test | What the stack does, and how it ends |
|---|---|---|
| 0x0101 | TEST_SE0_NAK | `on_end(U2PS_HOST_END_TEST_MODE)`, the port disabled, test mode 3. Only `u2ps_host_init()` or a power cycle ends it. |
| 0x0102 | TEST_J | As 0x0101, test mode 1. |
| 0x0103 | TEST_K | As 0x0101, test mode 2. |
| 0x0104 | TEST_PACKET | As 0x0101, test mode 4. |
| 0x0106 | HS_HOST_PORT_SUSPEND_RESUME | SOFs for 15 s; suspend for 15 s; resume, the K for at least 20 ms; SOFs. No GET_STATUS. The session stays. |
| 0x0107 | SINGLE_STEP_GET_DEV_DESC | SOFs for 15 s; one whole GET_DESCRIPTOR(device, 18). The session stays. |
| 0x0108 | SINGLE_STEP_SET_FEATURE | The SETUP of GET_DESCRIPTOR(device, 18) at once; SOFs for 15 s; its data and status stages. The session stays. |
| 0x0200 | TTST_CONFIG | Stays configured; SOFs go on. |

Each 15 s wait lasts more than 15,000 ms and less than 15,002 ms plus one tick period, from the completion of SET_CONFIGURATION, or of the SETUP stage for 0x0108. While the session stays, `u2ps_host_stream_start()` returns 0 and `u2ps_host_suspend()` returns false; a disconnect, `u2ps_host_connect(false)`, a VBUS loss, test mode or a re-init end it, and a failed request ends it with the usual reason. Your app's duties:
- Log or show the test; for 0x0101 to 0x0104 show "USB test mode" and offer a re-init: a test mode holds the port until `u2ps_host_init()` or a power cycle (`host/u2ps_host.h:324-327`).
- A product that supports sessions (OTG): with PID 0x0200 and `bcd_device` bit 0 set (otg_vbus_off), turn VBUS off within 5 s after a disconnect from that session (`host/u2ps_host.h:674-681`). An Embedded Host without sessions has no such duty.

The audio function. An Interface Association Descriptor of the audio class bounds it; in a configuration with several, the first playable one counts. Without one the configuration is refused unless the library is built with `U2PS_HOST_ACCEPT_NO_IAD=1` and it has exactly one AudioControl interface. The stack plays to the USB streaming Input Terminal through the AudioStreaming interface linked to it, on that terminal's clock: a Clock Source, or a Clock Selector with one input that is one (`host/u2ps_host.h:140-150`).

`u2ps_host_end_t`, the `why` of `u2ps_app_host_on_end()` (`host/u2ps_host.h:308-376`):

| Value | Cause and fix |
|---|---|
| `U2PS_HOST_END_DISCONNECT` | The device disconnected. |
| `U2PS_HOST_END_DETACH` | The app's `u2ps_host_connect(false)`. |
| `U2PS_HOST_END_VBUS` | The app's `u2ps_host_vbus(false)`; if unintended, check the board's VBUS switch, its over-current flag and the app's reading. |
| `U2PS_HOST_END_DISABLED` | The port disabled itself (babble); check the cable, reconnect. |
| `U2PS_HOST_END_TEST_MODE` | The app's `u2ps_host_test_mode()`, or the test device's PID 0x0101 to 0x0104 after `on_test_device`; the port stays in test mode until a re-init or a power cycle. |
| `U2PS_HOST_END_REINIT` | The app's `u2ps_host_init()`. |
| `U2PS_HOST_END_SPEED` | The device enabled at full or low speed and one of its configurations, or one of the other-speed configurations its device qualifier announces, holds a playable audio function (with `U2PS_HOST_REFUSE_FS=ON`: at once); u2ps streams at high speed only. Connect a high-speed device directly with a high-speed cable. |
| `U2PS_HOST_END_EP0_MPS` | bMaxPacketSize0 is not one the speed allows (64 at high speed; 8, 16, 32 or 64 at full speed; 8 at low speed): the device is not compliant. |
| `U2PS_HOST_END_DESCRIPTOR` | The device descriptor or a configuration header is malformed: the device is not compliant. |
| `U2PS_HOST_END_TOO_LARGE` | A configuration descriptor is longer than `U2PS_HOST_EP0_BUF`; build with a larger `-DU2PS_HOST_EP0_BUF` (a multiple of 64, at least its wTotalLength). |
| `U2PS_HOST_END_NOT_PLAYABLE` | No configuration holds a playable audio function; the `refusal` argument says why. At full or low speed no other-speed configuration does either, or the device has no device qualifier (it is full-speed-only), and the refusal is the first full-speed configuration's. |
| `U2PS_HOST_END_CLOCK` | The clock's RANGE or CUR stalled or is malformed: the device's clock is not UAC2-compliant. |
| `U2PS_HOST_END_STALL` | The device stalled another request it must answer; reconnect it. |
| `U2PS_HOST_END_ERROR` | Transaction errors beyond the port's retries; check cable or hub, reconnect. |
| `U2PS_HOST_END_TIMEOUT` | A control transfer took longer than `U2PS_HOST_CONTROL_MAX`; a slow device needs a larger `-DU2PS_HOST_CONTROL_MAX`. |
| `U2PS_HOST_END_HUB` | The device is a hub (bDeviceClass 0x09); the host stack drives no hubs. Connect the device directly. |
| `U2PS_HOST_END_POWER` | Every playable configuration, or the test device's, needs more than `U2PS_HOST_IA_VBUS_RATED_MA`; power the device on its own, or build with the board's higher rating. |

Messages for the user. The OTG and EH Compliance Plan wants a clear message for every error a user meets (7.2.2), and a product that shows one maps the end reasons so:

| Message | End reasons and stop reasons |
|---|---|
| "Unsupported device" | `U2PS_HOST_END_NOT_PLAYABLE` (with its refusal), `U2PS_HOST_END_SPEED`, `U2PS_HOST_END_EP0_MPS`, `U2PS_HOST_END_DESCRIPTOR`, `U2PS_HOST_END_TOO_LARGE`, `U2PS_HOST_END_CLOCK` |
| "Device not responding" | `U2PS_HOST_END_ERROR`, `U2PS_HOST_END_TIMEOUT`, `U2PS_HOST_END_STALL`, `U2PS_HOST_END_DISABLED` |
| "Hub not supported" | `U2PS_HOST_END_HUB` |
| "Device needs more power" | `U2PS_HOST_END_POWER`, and your board's VBUS over-current flag (`u2ps_host_vbus(false)` gives `U2PS_HOST_END_VBUS`) |
| "USB test mode" (offer a re-init) | `U2PS_HOST_END_TEST_MODE` after `on_test_device` |
| "The device refuses this format or rate" | the stop reason `U2PS_HOST_STOP_REFUSED`, with its request |
| "This format does not fit the host" | the stop reason `U2PS_HOST_STOP_OPEN` |
| none: the user's or the app's own action | `U2PS_HOST_END_DISCONNECT`, `U2PS_HOST_END_DETACH`, `U2PS_HOST_END_REINIT` |

A responsive device that cannot be played ends its session within milliseconds of its device descriptor (`tests/test_host_messages.c:53-72`). One that stops answering ends `U2PS_HOST_CONTROL_MAX` after its last request began; one that answers each request just within it may take (3 + 2 x bNumConfigurations) x `U2PS_HOST_CONTROL_MAX`. The Plan allows 30 s for the message (6.7.23).

`u2ps_host_refusal_t`, the `refusal` argument: `U2PS_HOST_REFUSAL_NONE` with every end but `U2PS_HOST_END_NOT_PLAYABLE`. Otherwise it is the reason of the first configuration and, in it, the first audio function. The app cannot change the device's descriptors, so the device cannot be played, except for `U2PS_HOST_REFUSAL_NO_IAD`, which `U2PS_HOST_ACCEPT_NO_IAD` changes (`host/u2ps_host.h:378-412`):

| Value | What the descriptors lack or break |
|---|---|
| `U2PS_HOST_REFUSAL_NONE` | Nothing: a playable audio function. |
| `U2PS_HOST_REFUSAL_CUT` | It ends early: wTotalLength beyond the bytes read, or a descriptor beyond wTotalLength. |
| `U2PS_HOST_REFUSAL_LENGTH` | A descriptor with bLength below 2, or too short for its type. |
| `U2PS_HOST_REFUSAL_NOT_CONFIG` | Not a configuration descriptor. |
| `U2PS_HOST_REFUSAL_NOT_AUDIO` | No AudioControl interface (in the function). |
| `U2PS_HOST_REFUSAL_NO_IAD` | AudioControl interfaces but no audio IAD (UAC2 4.6), and `U2PS_HOST_ACCEPT_NO_IAD` off or more than one of them; with one, `-DU2PS_HOST_ACCEPT_NO_IAD=ON` accepts it. |
| `U2PS_HOST_REFUSAL_UAC1` | An audio function of USB Audio Class 1.0. |
| `U2PS_HOST_REFUSAL_VERSION` | No class-specific AC header, or bcdADC other than 2.00. |
| `U2PS_HOST_REFUSAL_NO_TERMINAL` | No USB streaming Input Terminal. |
| `U2PS_HOST_REFUSAL_NO_STREAMING` | No AudioStreaming interface linked to it, or none with alternate setting 0 without endpoints. |
| `U2PS_HOST_REFUSAL_CLOCK` | The terminal's clock is no Clock Source, nor a Clock Selector with one input that is one. |
| `U2PS_HOST_REFUSAL_NO_ALT` | No streaming alternate setting besides 0. |
| `U2PS_HOST_REFUSAL_ALT_REPEATED` | One alternate setting twice. |
| `U2PS_HOST_REFUSAL_FORMAT` | An alternate setting without its AS general or format type descriptor, or a Type I format whose subslot and resolution do not fit. |
| `U2PS_HOST_REFUSAL_ENDPOINT` | Endpoints other than one isochronous OUT data endpoint and, for explicit feedback, one isochronous IN feedback endpoint of at least 4 bytes. |
| `U2PS_HOST_REFUSAL_ALL_SKIPPED` | Every kept alternate setting is skipped (`u2ps_host_skip_t`). |

### 4.5 The device the stack gives you

`u2ps_app_host_on_device(dev)` gives a `u2ps_host_device_t`. It and everything it points to are the stack's, valid until `u2ps_app_host_on_end()` (`host/u2ps_host.h:276-306`):

- `id_vendor`, `id_product`, `bcd_device`; `manufacturer`, `product`, `serial_number` as ASCII (each UTF-16 code unit outside 0x20 to 0x7E becomes `?`), `NULL` if absent, stalled, or `U2PS_HOST_STRINGS` is 0.
- `terminal_type` of the Output Terminal the streaming Input Terminal feeds, 0 if none.
- `sampling_frequency_host_programmable`: the clock's frequency Control is host programmable. `sampling_frequency`: the clock's GET CUR after SET_CONFIGURATION.
- `clock_validity`, a `u2ps_clock_validity_t`, read after `sampling_frequency` and again at each `u2ps_host_stream_start()`:

| Value | Meaning |
|---|---|
| `U2PS_CLOCK_VALIDITY_UNKNOWN` | The Clock Validity Control is not readable, or it stalled. |
| `U2PS_CLOCK_VALIDITY_VALID` | CUR true: the clock is stable and usable. |
| `U2PS_CLOCK_VALIDITY_INVALID` | CUR false. |

  The stack still starts a stream when the validity is invalid (`tests/test_host_stream.c:171`); whether to start is the app's decision.
- `num_sub_ranges` and `sub_ranges`: the clock's RANGE as `u2ps_host_sub_range_t` (`min`, `max`, `res` in Hz; a discrete frequency has `min` = `max` and `res` 0). Start only at a frequency inside a sub-range.
- `alternate_setting_count` and `alternate_settings`: the kept streaming alternate settings (at most `U2PS_HOST_ALT_MAX`, ascending), each a `u2ps_host_alternate_setting_t`: `alternate_setting`, `format`, `nr_channels`, `channel_config`, `max_packet_size`, `synchronization_type`, `explicit_feedback`, `skip`. At least one is playable. Start a stream only on one whose `skip` is `U2PS_HOST_SKIP_NONE`.

Synchronization types, `u2ps_synchronization_type_t`, are bmAttributes D3..2 of the data endpoint as declared (`host/u2ps_host.h:229-235`): `U2PS_SYNCHRONIZATION_TYPE_NONE` (played at the nominal rate), `U2PS_SYNCHRONIZATION_TYPE_ASYNCHRONOUS`, `U2PS_SYNCHRONIZATION_TYPE_ADAPTIVE`, `U2PS_SYNCHRONIZATION_TYPE_SYNCHRONOUS`. `explicit_feedback` is true when the endpoint is asynchronous with an explicit feedback endpoint, and the stack follows that feedback. False: it plays at the nominal rate and ignores a feedback endpoint the setting declares (`host/u2ps_host.h:267-272`).

Why an alternate setting is skipped, `u2ps_host_skip_t` (`host/u2ps_host.h:237-251`). A skip only says why another setting is left out; the app cannot change the device:

| Value | Reason |
|---|---|
| `U2PS_HOST_SKIP_NONE` | Playable. |
| `U2PS_HOST_SKIP_FORMAT` | Not Type I PCM or IEEE_FLOAT (e.g. native DSD), IEEE_FLOAT other than 32 bits in 4-byte subslots, or no channel. |
| `U2PS_HOST_SKIP_HIGH_BANDWIDTH` | More than one transaction per microframe. |
| `U2PS_HOST_SKIP_INTERVAL` | A data endpoint not served every microframe (bInterval not 1). |
| `U2PS_HOST_SKIP_IMPLICIT_FEEDBACK` | Asynchronous without an explicit feedback endpoint. |
| `U2PS_HOST_SKIP_MAX_PACKETS_ONLY` | EP_GENERAL bmAttributes D7 set. |
| `U2PS_HOST_SKIP_PACKET_SIZE` | wMaxPacketSize over 1024 bytes. |

### 4.6 Starting and stopping a stream

`u2ps_host_stream_start(alternate_setting, sampling_frequency)` (thread) returns the new stream's number, the one `on_stream_start` will give, so that the start can be stopped while it runs. It returns 0, changing nothing, without a configured device (wait for `on_device`; also while suspended or resuming, and in the test device's session), while a stream starts, runs or stops (stop it and wait for its `on_stream_stop`), or for an alternate setting not kept or not playable. The requests are SET CUR of the clock's frequency (only if host programmable), GET CUR to read it back, CUR of the Clock Validity Control once if readable, SET_INTERFACE, then the port opens the stream and `u2ps_app_host_on_stream_start()` follows. The frequency read back becomes the device's `sampling_frequency`, and the stream's (`host/u2ps_host.h:580-600`).

Every accepted start gets exactly one `u2ps_app_host_on_stream_stop(why, request)`, with or without an `on_stream_start` before it. A STALL of a request of the start gives `U2PS_HOST_STOP_REFUSED` and leaves the device configured; a transaction error or a timeout ends the session.

`u2ps_host_stream_stop(stream)` (thread) stops the stream that `u2ps_host_stream_start()` returned. A running stream: no packet may be queued from now on, the port closes (packets not yet reported sent return at its end), SET_INTERFACE 0, then `on_stream_stop(U2PS_HOST_STOP_APP, ...)`. A start in progress is cancelled: the request in flight completes, no further request of the start follows, the port's stream never opens, no `on_stream_start` comes; SET_INTERFACE 0 follows only if the device accepted the start's SET_INTERFACE; then exactly one `on_stream_stop(U2PS_HOST_STOP_APP, ...)`. If the request in flight fails with a transaction error or a timeout, the session ends as without the stop (`U2PS_HOST_STOP_END`). Nothing happens for another number or a stream already stopping (`host/u2ps_host.h:648-664`).

`u2ps_host_stop_t`, the `why` of `u2ps_app_host_on_stream_stop(why, request)` (`host/u2ps_host.h:436-456`):

| Value | Cause and fix |
|---|---|
| `U2PS_HOST_STOP_APP` | The app's `u2ps_host_stream_stop()`, of a stream or of its start. |
| `U2PS_HOST_STOP_END` | The session ended; `u2ps_app_host_on_end()` follows with why. |
| `U2PS_HOST_STOP_REFUSED` | The device stalled a request of the start, or read back a sampling frequency of 0 or not 4 bytes; `request` names which. Start again with a frequency of the device's `sub_ranges[]` and an alternate setting with `U2PS_HOST_SKIP_NONE`; if those were used, the device refuses the pair. |
| `U2PS_HOST_STOP_OPEN` | The port refused the stream (it does not fit the port's limits); start another alternate setting. On the DWC2 host port also a feedback endpoint with bInterval 14 to 16 (it supports 1 to 13, section 4.1): that device cannot be played there. |

`u2ps_host_stop_request_t`, the `request` argument: `U2PS_HOST_STOP_REQUEST_NONE` with every reason but `U2PS_HOST_STOP_REFUSED` (`host/u2ps_host.h:458-480`):

| Value | Request the device refused and fix |
|---|---|
| `U2PS_HOST_STOP_REQUEST_NONE` | Not `U2PS_HOST_STOP_REFUSED`. |
| `U2PS_HOST_STOP_REQUEST_SET_SAMPLING_FREQUENCY` | SET CUR of the clock's frequency stalled: the device refuses that frequency; start with one of its `sub_ranges[]`. |
| `U2PS_HOST_STOP_REQUEST_GET_SAMPLING_FREQUENCY` | GET CUR of it stalled: the device does not report its clock's frequency (UAC2 5.2.5.1.1 requires it); it cannot be played. |
| `U2PS_HOST_STOP_REQUEST_SAMPLING_FREQUENCY_VALUE` | GET CUR read back 0 Hz or not 4 bytes; start again, or with another frequency. |
| `U2PS_HOST_STOP_REQUEST_SET_INTERFACE` | SET_INTERFACE to the alternate setting stalled: the device refuses that alternate setting, at least at this frequency; start another one or another frequency. |

A stalled Clock Validity read refuses nothing: the start goes on with the validity unknown (`host/u2ps_host.h:461-463`).

`u2ps_host_stream_info_t`, given at `u2ps_app_host_on_stream_start(info)` (`host/u2ps_host.h:416-434`): `stream`, `alternate_setting`, `format`, `nr_channels`, `sampling_frequency` (the clock's, read back), `max_packet_size`, `synchronization_type`, `explicit_feedback`, `nominal_rate` (slots per microframe, 16.16, rounded, for logs) and `clock_validity`. From `on_stream_start` on the app queues packets.

### 4.7 Sending audio: slots, queue, drain

A *slot* is one sample frame: `nr_channels` * `subslot_size` bytes. The packet size changes from microframe to microframe, so ask the stack for it:

1. `u2ps_host_slots_next(stream)` (any context) returns the number of slots the next packet carries. It is latched until that packet is queued. It returns 0 for a stream that is not running (`host/u2ps_host.h:602-622`).
2. Put exactly that many slots at `data` (DMA memory, on `U2PS_HPORT_ALIGN`) and call `u2ps_host_queue(stream, data)` (inside a callback, or from the thread). It returns false, and counts in `queue_errors`, for one of three causes (`host/u2ps_host.h:624-636`):
   - the stream is not running (`u2ps_host_slots_next()` returned 0): queue only between `on_stream_start` and `on_stream_stop`, with their number;
   - the queue is full (`U2PS_HOST_QUEUE_MAX` packets not yet reported by `on_sent`): wait for the next `on_sent`;
   - `data` is `NULL` or not on `U2PS_HPORT_ALIGN`: place the packet on it.
3. The packet's memory is yours again at its `u2ps_app_host_on_sent(uframe)`, which tells the microframe it went out in. Refill from there.

The accumulator as the app sees it (`host/u2ps_host.h:602-621`):
- With explicit feedback the rate is the last valid feedback value: 4 bytes, top 4 bits zero, within 12.5 % of nominal, in 16.16 slots per microframe. Until the first, and without explicit feedback, it is the nominal rate, `sampling_frequency` / 8000, exactly.
- From the stream's first packet on, every microframe on the bus adds the rate to an accumulator: each packet's own, and each `U2PS_MISS_LATE` or `U2PS_MISS_NO_BUFFER` one. Drained microframes add nothing.
- A packet takes the integer part of the accumulator, at most INT(rate) + 1 slots (Formats 2.3.1.1). So a hole's debt is repaid by packets of INT(rate) + 1 slots.
- The debt stays within `U2PS_HOST_DEBT_MAX` microframes' worth. The whole slots beyond it are dropped and counted in `slots_forgiven`.
- A packet of more slots than `max_packet_size` carries is cut to fit; the cut slots are dropped and counted in `slots_clamped`.
- A known limit of the DWC2 host port: a run of 8,192 microframes (1.024 s) or more with the queue dry counts modulo 16,384 microframes, and as none when that remainder is 8,192 or more, so its `U2PS_MISS_NO_BUFFER` microframes add less, or nothing (`host/u2ps_host.h:617-621`).

Keep the queue ahead of the bus: the port arms a packet `U2PS_HPORT_LEAD` microframes ahead, and an empty queue is a `U2PS_MISS_NO_BUFFER` hole (`host/u2ps_hport.h:39-41`, `common/u2ps_types.h:44-51`).

`u2ps_host_drain(stream)` (any context) says that the app stops queueing on purpose, for a gap. The queued packets go out; the empty microframes until the next `u2ps_host_queue()` are drained microframes, counted in `drained`, not holes, and the rate does not accumulate in them. If the queue had already run empty before the drain, that run of empty microframes is drained too (`host/u2ps_host.h:638-646`).

`u2ps_app_host_on_hole(uframes, why)` reports `uframes` microframes without a packet for `U2PS_MISS_LATE` or `U2PS_MISS_NO_BUFFER`, or a `U2PS_MISS_CORRUPTED` packet, immediately before the `on_sent` that ends it (`host/u2ps_host.h:700-703`). Reasons and fixes are in section 3.8.

`u2ps_host_stats_t` (`host/u2ps_host.h:482-497`): `sent`, `late_completions`, `late`, `no_buffer`, `corrupted`, `fb_polls`, `fb_missed`, `control_retries`, `errors`, and the stack's own `drained`, `fb_zero_length`, `fb_rejected`, `slots_clamped`, `slots_forgiven`, `control_timeouts`, `queue_errors`, `uframe_back`, `wakeups` (remote wakeups the port reported, section 4.10).

### 4.8 Host callbacks and contexts

| Callback | Purpose | Context |
|---|---|---|
| `u2ps_app_host_on_device(dev)` | The device is configured and its audio function can play. | Interrupt. Start the stream from the thread. |
| `u2ps_app_host_on_test_device(id_product, bcd_device)` | The USB-IF test device is configured; its test follows (section 4.4). | Interrupt. |
| `u2ps_app_host_on_resumed(remote_wakeup)` | The port is out of suspend and the device answered GET_STATUS; `remote_wakeup` says the device's wakeup began the resume (section 4.10). | Interrupt. |
| `u2ps_app_host_on_end(why, refusal)` | The session ended (section 4.4). | Interrupt; masked thread under `u2ps_host_connect(false)`, `u2ps_host_vbus(false)`, `u2ps_host_test_mode()`, a re-init, and `u2ps_host_tick()` when a resume does not end (`U2PS_HOST_END_ERROR`). |
| `u2ps_app_host_on_stream_start(info)` | The stream has started; queue packets. | Interrupt. |
| `u2ps_app_host_on_sent(uframe)` | The oldest queued packet went out in `uframe`; its memory returns. | Interrupt. |
| `u2ps_app_host_on_hole(uframes, why)` | A hole. | Interrupt. |
| `u2ps_app_host_on_stream_stop(why, request)` | The stream stopped or its start failed; every queued packet's memory is yours again. | Interrupt; masked thread with `U2PS_HOST_STOP_END` under the calls that end a session. |
| `u2ps_app_on_contract(violation)` | Only with `U2PS_CONTRACT_CHECKS=1`: a contract violation, reported once (section 5.1). | Where the stack found it: the interrupt, a callback, or the thread, possibly masked (`host/u2ps_host.h:712-724`). |

Sources: `host/u2ps_host.h:13-38,669-710`. Inside any callback the app may call `u2ps_host_slots_next()`, `u2ps_host_queue()` and `u2ps_host_drain()`; it must not call the thread calls of section 4.3 and defers them to its thread. `u2ps_host_tick()` makes one callback itself, `on_end` of a resume that does not end; every other wait it ends continues with a port call whose completion comes in the interrupt, or with the port's suspend or resume, which have none.

### 4.9 A compilable skeleton of the host callbacks

Checked: it compiles with the warnings of `CMakeLists.txt:69-72` for the STM32U5A5 and ChipIdea port headers, and links with the PC fake host port, where `u2ps_host_init()` returns `U2PS_HOST_OK`. It plays silence from the first playable alternate setting at the clock's current frequency; put your audio source where marked.

```c
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "u2ps_host.h"
#include "u2ps_hport_cfg.h"

#define PACKETS U2PS_HOST_QUEUE_MAX
/* Place packet in DMA memory (section 4.2). */
static _Alignas(U2PS_HPORT_ALIGN) uint8_t packet[PACKETS][1024];
static unsigned head, tail;  /* next to fill, oldest queued */
static unsigned queued;      /* packets queued and not yet sent */
static uint32_t stream_no;   /* 0: no stream running */
static unsigned slot_bytes;  /* nr_channels * subslot_size */

/* Set by on_device, read by the thread, which starts the stream */
static bool can_play;
static uint8_t play_alt;
static uint32_t play_rate;

static void fill(void) /* interrupt (callbacks) or thread */
{
  while (stream_no != 0 && queued < PACKETS) {
    uint16_t slots = u2ps_host_slots_next(stream_no);
    if (slots == 0)
      break;
    memset(packet[head], 0, slots * slot_bytes); /* your audio goes here */
    if (!u2ps_host_queue(stream_no, packet[head]))
      break;
    head = (head + 1u) % PACKETS;
    queued++;
  }
}

void u2ps_app_host_on_device(const u2ps_host_device_t *dev)
{
  can_play = false;
  for (uint8_t i = 0; i < dev->alternate_setting_count; i++) {
    const u2ps_host_alternate_setting_t *a = &dev->alternate_settings[i];
    if (a->skip == U2PS_HOST_SKIP_NONE) {
      play_alt = a->alternate_setting;
      play_rate = dev->sampling_frequency; /* the clock's current frequency */
      can_play = true;
      break;
    }
  }
}
void u2ps_app_host_on_end(u2ps_host_end_t why, u2ps_host_refusal_t refusal)
{
  (void)why; (void)refusal; /* show the message of section 4.4 */
  can_play = false;
}
void u2ps_app_host_on_test_device(uint16_t id_product, uint16_t bcd_device)
{
  (void)id_product; (void)bcd_device; /* log it; the stack runs the test */
}
void u2ps_app_host_on_resumed(bool remote_wakeup) { (void)remote_wakeup; }
void u2ps_app_host_on_stream_start(const u2ps_host_stream_info_t *info)
{
  stream_no = info->stream;
  slot_bytes = info->nr_channels * info->format.subslot_size;
  head = tail = queued = 0;
  fill();
}
void u2ps_app_host_on_sent(u2ps_uframe_t uframe)
{
  (void)uframe;
  tail = (tail + 1u) % PACKETS;
  queued--;
  fill();
}
void u2ps_app_host_on_hole(uint16_t uframes, u2ps_miss_t why) { (void)uframes; (void)why; }
void u2ps_app_host_on_stream_stop(u2ps_host_stop_t why, u2ps_host_stop_request_t request)
{
  (void)why; (void)request;
  stream_no = 0; /* every queued packet's memory is yours again */
  queued = 0;
}

/* The thread's loop body: call it every 10 ms or less */
void app_poll(uint32_t now_ms, bool vbus);
void app_poll(uint32_t now_ms, bool vbus)
{
  u2ps_host_vbus(vbus);
  u2ps_host_tick(now_ms);
  if (can_play && stream_no == 0) {
    can_play = false;
    (void)u2ps_host_stream_start(play_alt, play_rate);
  }
}
```

The thread also calls `u2ps_host_init()` once, then `u2ps_host_connect(true)`, before the first `app_poll()`. The host streaming tests are the working reference: `tests/test_host_stream.c` (start, stop, feedback, holes, queue errors) and `tests/test_host_loopback.c` (the exact bytes and microframes at 44.1 to 768 kHz).

### 4.10 Suspend and resume

Suspend and resume are optional for an Embedded Host, but one that suspends must let its user force a suspend (OTG and EH Compliance Plan 4.4.3). The stack offers both calls to the app and runs the test device's 0x0106 sequence itself (`host/u2ps_host.h:128-138`).

1. `u2ps_host_suspend()` (thread), with a configured device and no stream: the port stops its SOFs and the device suspends (USB 2.0 7.1.7.6). It returns false, changing nothing, for these causes, each with its fix (`host/u2ps_host.h:554-568`):
   - before a successful `u2ps_host_init()`: init first;
   - without a configured device, before `on_device` or in the test device's session: wait for `on_device`;
   - while a stream starts, runs or stops: stop it and suspend after its `on_stream_stop`; contract `U2PS_CONTRACT_HOST_SUSPEND_STREAM`;
   - while already suspended or resuming: wait for `on_resumed`; contract `U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED`;
   - in test mode: re-init.
2. While suspended, `u2ps_host_stream_start()` returns 0. A disconnect, a VBUS loss, `u2ps_host_connect(false)`, test mode and a re-init end the session as at any time, without a GET_STATUS.
3. `u2ps_host_resume()` (thread): the port drives the resume K for at least 20 ms (TDRSMDN), the stack asks the port on every tick to end it until the port is out of resume, waits 10 ms (TRSMRCY) from then, and reads GET_STATUS(device); `u2ps_app_host_on_resumed(false)` follows its completion, and streams may start again. It returns false when the port is not suspended or a resume runs; contract `U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED` unless the session ended or the device's wakeup began that resume (`host/u2ps_host.h:570-578`).
4. A STALL, an error or a timeout of that GET_STATUS ends the session with `U2PS_HOST_END_STALL`, `U2PS_HOST_END_ERROR` or `U2PS_HOST_END_TIMEOUT`. A port still in resume 50 ms after the stack first asked it to end the resume ends the session with `U2PS_HOST_END_ERROR`, from `u2ps_host_tick()`.
5. Remote wakeup: the stack never enables it on the device, but a device's K while suspended is handled. The port reports it with `u2ps_host_on_wakeup()` and drives the K itself; the stack resumes as in step 3 without the app's call, and `on_resumed(true)` follows. A wakeup at any other time, a resume in progress included, is only counted in `wakeups` (`host/u2ps_hport.h:262-268`).

The tests are the reference: `tests/test_host_suspend.c` runs each case against the fake port in its DWC2 model, where the port ends a resume at once, and its ChipIdea model, where the controller times the K itself.

## 5. Common mistakes and what they look like

| Mistake | What it looks like | Source |
|---|---|---|
| Neither `U2PS_DEVICE` nor `U2PS_HOST` set | The configure step stops with "No stack selected", naming both options. | `CMakeLists.txt:116-125` |
| `U2PS_VBUS` not set with `U2PS_DEVICE=ON` | The configure step stops with "U2PS_VBUS is not set". | `device/CMakeLists.txt:26-29` |
| `U2PS_HOST_IA_VBUS_RATED_MA` not set with `U2PS_HOST=ON`, or outside 8 to 5000 | The configure step stops with "U2PS_HOST_IA_VBUS_RATED_MA is not set", and says that a device-only build leaves out `-DU2PS_HOST=ON`; a value outside the range stops the build at a static assert that names the constant and its range. | `host/CMakeLists.txt:32-38`, `host/u2ps_host.c:47-49` |
| A host app without `u2ps_app_host_on_test_device()` or `u2ps_app_host_on_resumed()` | Link error: undefined reference. Every host app defines all `u2ps_app_host_*` callbacks. | `host/u2ps_host.h:669-710` |
| Linking `u2ps_device` and a kit library, or the kit in an ARM build | Duplicate symbols; the kit exists in the PC build only. Link the kit library alone. | `testkit/u2ps_testkit.h:8-19` |
| `U2PS_VBUS=port` | The configure step stops: "not available yet". | `device/CMakeLists.txt:30-33` |
| `U2PS_PORT` does not match `U2PS_CHIP`, or either set in a PC build | The configure step stops. | `CMakeLists.txt:52-65` |
| Toolchain file without `-DU2PS_CHIP` on the command line | The configure step stops: "U2PS_CHIP must be ...". | `cmake/arm-none-eabi.cmake:42-46`, `cmake/arm-none-eabi-clang.cmake:63-67` |
| ATfE's toolchain file without `U2PS_ATFE_DIR`, or with a directory that has no `bin/clang` | The configure step stops: "U2PS_ATFE_DIR must name the directory of Arm Toolchain for Embedded that holds bin/clang ...", naming `-DU2PS_ATFE_DIR`, the environment variable and the GCC file. | `cmake/arm-none-eabi-clang.cmake:22-30` |
| App compiled without the library's build constants | `#error "u2ps_dev.h: the device library's build constants ... are missing: link the CMake target u2ps_device ..."`; the host header says the same for `u2ps_host`. | `device/u2ps_dev.h:171-176`, `host/u2ps_host.h:211-218` |
| `self_powered = true` with `U2PS_VBUS=bus` | Init returns `U2PS_DEV_ERR_DESCRIPTION`; `u2ps_dev_description_error()` gives `U2PS_DESCRIPTION_SELF_POWERED`. | `device/u2ps_dev.h:256-258` |
| Description changed or freed after init | The stack keeps the pointer. Keep it `static const`. | `device/u2ps_dev.h:198-199` |
| `u2ps_app_on_control()` missing in a `U2PS_CONTROL_HOOK=1` build | Link error: undefined reference. | `device/u2ps_dev.h:797-812` |
| `u2ps_app_on_control()` defined in a build without the hook | The header does not declare it there. | `device/u2ps_dev.h:797-812` |
| A thread call from an interrupt, or from a callback (except those allowed in section 3.11) | The port's mask does not nest. | `device/u2ps_dev.h:23-27,46-52` |
| Calling `u2ps_dev_init()`, `u2ps_dev_connect()` and `u2ps_dev_tick()` from different threads | Same cause. They are one thread context. | `device/u2ps_dev.h:23-27` |
| In mode app, never calling `u2ps_dev_vbus()` | The device never attaches. | `device/u2ps_dev.h:560-562` |
| Stalling the VBUS poll with VBUS present | The pull-up stays on after the cable is pulled. | `device/u2ps_dev.h:560-562` |
| Not calling `u2ps_dev_tick()` or `u2ps_host_tick()` every 10 ms | The device: no `U2PS_FAULT_CLOSE` or `U2PS_FAULT_START`, long pauses decided by the port alone, the holes of a hold of every buffer reported only with the next packet or at the stream's end, the host's 50 ms limit for a request at risk. The host: every wait and timeout lengthens. | `device/u2ps_dev.h:73-78,604-625`, `host/u2ps_host.h:536-544` |
| `U2PS_STREAM_DEFER` and not opening in time | The status stage completes after `U2PS_START_MAX` ms; tick returns `U2PS_FAULT_START`; the start stays pending. | `device/u2ps_dev.h:73-78,452-457` |
| Class memory outside DMA memory, misaligned, stride too small or not a multiple of `U2PS_DPORT_ALIGN`, or `count` outside the limits | `on_fault(U2PS_FAULT_OPEN)`, then `on_stream_stop`; SET_INTERFACE stalls. On DWC2 `u2ps_dwc2_dcd_open_error()` names the rule. | `device/u2ps_dev.h:389-413,440-446` |
| A stride of the least stride plus a few bytes | Refused with `U2PS_DWC2_OPEN_STRIDE_ALIGN` where `U2PS_DPORT_ALIGN` is 32; round the sum up. | `device/u2ps_dev.h:396-398` |
| Not releasing packets | Once you hold `count` - `U2PS_DPORT_ISO_OUT_RESERVE`, each packet the host sends is a `U2PS_MISS_NO_BUFFER` microframe, not delivered. | `device/u2ps_dev.h:79-83,586-592` |
| Releasing a stopped stream's packets, or more than held | The call is ignored; `release_errors` counts it. | `device/u2ps_dev.h:590-592` |
| Using packet data or `req->buf` after the callback or `on_stream_stop` | The memory belongs to the stack or the port again. | `device/u2ps_dev.h:329-332,758-760` |
| `req->buf` as the data of `u2ps_dev_reply()` | Not allowed; the data must be in the app's own memory. | `device/u2ps_dev.h:571-584` |
| `u2ps_dev_connect(true)` before a successful init | Nothing happens. | `device/u2ps_dev.h:521-523` |
| HSE not running before init (DWC2) | `U2PS_DEV_ERR_CLOCK` (host: `U2PS_HOST_ERR_CLOCK`). | `port/dwc2/common/u2ps_dwc2_start.h:23-25` |
| `.bss.u2ps_dma` in memory the DMA does not reach (STM32U5 SRAM4 or backup SRAM, STM32F723 ITCM RAM, STM32H7S3 DTCM, ITCM or backup SRAM), or in an SRAM whose clock or power the app switched off | `U2PS_DEV_ERR_DMA_MEMORY` (host: `U2PS_HOST_ERR_DMA_MEMORY`), and nothing changes. | `common/u2ps_types.h:143-160` |
| A Cortex-M7 DWT whose cycle counter does not count (locked, or `TRCENA` cleared by other code) | `U2PS_DEV_ERR_TIMER` (host: `U2PS_HOST_ERR_TIMER`), and nothing starts. | `common/u2ps_types.h:161-166` |
| A host packet with a byte the DMA does not reach (STM32H7S3 DTCM) | With the contract checks: `U2PS_CONTRACT_HOST_QUEUE_DATA`, and the packet goes out as without them. | `host/u2ps_host_stream.c:241-244` |
| STM32U5 with TrustZone enabled | `U2PS_DEV_ERR_SECURITY` (host: `U2PS_HOST_ERR_SECURITY`). | `README.md:751-753`, `port/dwc2/common/u2ps_dwc2_start.h:29-31` |
| Another interrupt that runs port or stack code at the USB vector's priority or above | Forbidden: callbacks must never nest. | `port/dwc2/dcd/u2ps_dwc2_dcd.h:44-46` |
| Expecting a ChipIdea integration on hardware to work, or the DWC2 host port to be proven on one | The ChipIdea port has no register code; the DWC2 host port has register code but has not run on a board yet. | `port/chipidea/dcd/u2ps_chipidea_dcd.c:5-6`, `port/dwc2/hcd/u2ps_dwc2_hcd.c:4-6` |
| `u2ps_host_stream_start()` from `on_device` | A thread call inside a callback; defer it to the thread. | `host/u2ps_host.h:35-38` |
| `u2ps_host_stream_start()` returns 0 | No configured device (also while suspended or resuming, and in the test device's session), a stream already starting, running or stopping, or an alternate setting not kept or not playable. | `host/u2ps_host.h:589-594` |
| `u2ps_host_suspend()` with a stream, or `u2ps_host_resume()` without a suspend | Refused with false; with the contract checks, `U2PS_CONTRACT_HOST_SUSPEND_STREAM` or `U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED`. | `host/u2ps_host.h:554-578` |
| A full-speed cable or hub in front of a high-speed DAC | `U2PS_HOST_END_SPEED`, also when the device's full-speed configuration has no streaming interface (the stack finds the audio function in its other-speed configuration); `U2PS_HOST_END_HUB` for the hub itself. | `host/u2ps_host.h:53-59,65-79` |
| `u2ps_host_queue()` returns false | Not running, queue full, or `data` `NULL` or not on `U2PS_HPORT_ALIGN`; `queue_errors` counts it. | `host/u2ps_host.h:631-635` |
| Queueing a packet of another size than `u2ps_host_slots_next()` | The call takes exactly that many slots; the cap and the debt rules of section 4.7 apply. | `host/u2ps_host.h:624-626` |
| A gap on purpose without `u2ps_host_drain()` | Reported as `U2PS_MISS_NO_BUFFER` holes. | `host/u2ps_host.h:638-646`, `common/u2ps_types.h:44-51` |

### 5.1 Contract checks: let the stack find the mistakes

Use this while integrating. Configure u2ps with `-DU2PS_CONTRACT_CHECKS=ON` (it sets the device and the host library alike), define `u2ps_app_on_contract()`, and run your integration: on the PC with the test kit, whose stacks always have the checks (section 7), and on the board. Each call of the hook is a bug of your app; its code names the rule broken and the fix. Leave the option `OFF`, the default, in a release build: the checks then compile to nothing (`CMakeLists.txt:107-115`, `device/u2ps_dev_priv.h:221-252`, `host/u2ps_host_priv.h:90-124`).

- The hook is `void u2ps_app_on_contract(u2ps_contract_t violation)`, declared in both app headers only in a build with the checks (`device/u2ps_dev.h:814-826`, `host/u2ps_host.h:712-724`). It runs where the stack found the violation: in the port's interrupt, inside a callback, or in the thread, possibly with the vectors masked. Keep it short and call nothing of u2ps from it: count, log to memory, or stop at a breakpoint.
- Each occurrence is reported once. The stack then goes on exactly as a build without the checks does: a check adds no failure of its own (`common/u2ps_types.h:212-219`).
- The reference app counts each code with the interrupts off and logs a `contract violation=<name> code=<n> count=<n>` line from its main loop (`examples/nucleo_u5a5zj_q/main.c:79-82,549-565,1201-1229,1375`). The tests' hook fails any test that does not expect a violation (`tests/contract_log.c`, `tests/CMakeLists.txt:24`).

A minimal hook:

```c
#include "u2ps_dev.h" /* or u2ps_host.h */

#if U2PS_CONTRACT_CHECKS
volatile uint32_t contract_count[U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED + 1];

void u2ps_app_on_contract(u2ps_contract_t violation)
{
  if ((unsigned)violation <= U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED)
    contract_count[violation]++; /* read and log it from the main loop */
}
#endif
```

The codes, `u2ps_contract_t` (`common/u2ps_types.h:212-336`). A *thread call* is one that the context rules list for the app's thread (sections 3.11 and 4.3).

| Code | Rule broken | Fix |
|---|---|---|
| `U2PS_CONTRACT_NONE` | None; never reported. | |
| `U2PS_CONTRACT_DEV_CALL_IN_CALLBACK` | A device thread call (`u2ps_dev_init`, `connect`, `vbus`, `reply`, `tick`, `stream_open`, `stats`) inside a callback, in the interrupt or in thread with the vectors masked. | Set a flag in the callback; make the call from the thread. |
| `U2PS_CONTRACT_DEV_CALL_NESTED` | A device thread call, or `u2ps_dev_release()` outside a callback, while another thread call held the mask: from an interrupt or a second thread. | Make every thread call from one thread, never from an interrupt. |
| `U2PS_CONTRACT_DEV_CONNECT_BEFORE_INIT` | `u2ps_dev_connect(true)` before a successful `u2ps_dev_init()`; it does nothing. | Connect after init returned `U2PS_DEV_OK`. |
| `U2PS_CONTRACT_DEV_TICK_LATE` | `u2ps_dev_tick()` more than 10 ms after the last one while a close or a held SET_INTERFACE is timed. | Tick at least every 10 ms. |
| `U2PS_CONTRACT_DEV_DESCRIPTION_CHANGED` | The description, or what it points to, changed after init (seen at the next tick). | Keep it `static const`; for another, detach and init again. |
| `U2PS_CONTRACT_DEV_RELEASE_COUNT` | A release of more packets than the app holds of the stream; ignored. | Release each packet once, after its `on_packet`. |
| `U2PS_CONTRACT_DEV_RELEASE_STREAM` | A release for a number no `on_stream_start` gave, or, inside a callback, for a stream that has stopped; ignored. | Name the stream by `info->stream`; drop its packets at `on_stream_stop` without releasing them. |
| `U2PS_CONTRACT_DEV_REPLY_RESULT` | `u2ps_dev_reply()` with a result below 0 other than `U2PS_REQ_STALL`; taken as a stall. | `U2PS_REQ_STALL` or a result >= 0. |
| `U2PS_CONTRACT_DEV_REPLY_DATA` | A device-to-host reply with data to send but `data` `NULL` (it stalls), or `data` in `req->buf`. | Pass the answer in the app's own memory. |
| `U2PS_CONTRACT_DEV_CALLBACK_RESULT` | `on_request` or `on_sampling_frequency` returned a value below 0 other than `U2PS_REQ_STALL` and `U2PS_REQ_DEFER`, or `on_stream_start` one outside `u2ps_stream_start_t` (taken as `U2PS_STREAM_REFUSE`). | Return a listed value. |
| `U2PS_CONTRACT_HOST_CALL_IN_CALLBACK` | A host thread call (`u2ps_host_init`, `connect`, `vbus`, `tick`, `test_mode`, `suspend`, `resume`, `stream_start`, `stream_stop`, `stats`) inside a callback. | Set a flag in the callback; make the call from the thread. |
| `U2PS_CONTRACT_HOST_CALL_NESTED` | A host thread call, or `u2ps_host_queue()` outside a callback, while another thread call held the mask. | Make every thread call from one thread, never from an interrupt. |
| `U2PS_CONTRACT_HOST_CONNECT_BEFORE_INIT` | `u2ps_host_connect(true)` before a successful `u2ps_host_init()`; it does nothing. | Connect after init returned `U2PS_HOST_OK`. |
| `U2PS_CONTRACT_HOST_TICK_LATE` | `u2ps_host_tick()` more than 10 ms after the last one while a wait of the enumeration, of the test device or of a resume, or a control transfer, is timed; the wait grows by the excess. | Tick at least every 10 ms. |
| `U2PS_CONTRACT_HOST_QUEUE_STREAM` | A queue for a number `u2ps_host_stream_start()` never returned, or, inside a callback, for a stream that does not run; refused. | Queue between `on_stream_start` and the stop, with the stream's number. |
| `U2PS_CONTRACT_HOST_QUEUE_DATA` | A queue with `data` `NULL` or not on `U2PS_HPORT_ALIGN`; refused. Or a packet with a byte outside the memory the port's DMA reaches, by the port's reach rule as the port reported it at the last `u2ps_host_init()` (`u2ps_hport_dma_ranges()`; on the STM32H7S3 never DTCM, ITCM or the backup SRAM, section 4.2); reported, and sent as without the checks, so the device gets whatever the DMA reads there. On the PC the test kit's port reaches all of memory, so only the board build finds this one. | Place each packet on `U2PS_HPORT_ALIGN` in DMA memory, all of its bytes. |
| `U2PS_CONTRACT_HOST_QUEUE_FULL` | A queue with `U2PS_HOST_QUEUE_MAX` packets not yet reported by `on_sent`; refused. | Count the packets in flight; queue the next after an `on_sent`. |
| `U2PS_CONTRACT_HOST_QUEUE_UNSIZED` | A queue without `u2ps_host_slots_next()` since the last queued packet; the stack sends the size it computes now. | Call `u2ps_host_slots_next()` for each packet and fill exactly that many slots. |
| `U2PS_CONTRACT_HOST_SUSPEND_STREAM` | `u2ps_host_suspend()` while a stream starts, runs or stops; refused, since a stream needs SOFs. | Stop the stream and suspend after its `on_stream_stop`. |
| `U2PS_CONTRACT_HOST_SUSPEND_SUSPENDED` | `u2ps_host_suspend()` while the port is already suspended or resuming; refused. Not reported during a resume that the device's wakeup began. | Suspend once; after `u2ps_host_resume()`, suspend again only after `on_resumed`. |
| `U2PS_CONTRACT_HOST_RESUME_NOT_SUSPENDED` | `u2ps_host_resume()` while the port is not suspended, or a second time while the app's resume runs; refused. Not reported after the session ended, nor for a resume that the device's wakeup began. | Resume once after a `u2ps_host_suspend()` that returned true; wait for `on_resumed`. |

What the checks do not see, and why:
- A thread call from an interrupt other than the port's while no thread call runs: the stack cannot tell an interrupt from the thread without the CPU's state, which the port interface does not give it. Only a call that meets another thread call's mask is seen (`..._CALL_NESTED`).
- A release or a queue from the thread for a stream that the interrupt has just stopped: the app cannot avoid that race, so it is not reported; it is still ignored and counted (`release_errors`, `queue_errors`).
- Memory rules: class memory and host packets in DMA memory, a buffer left alone until it is released or sent, `req->buf` used after its callback, a description that was freed. They need memory protection. The port checks class memory's alignment, stride and reach itself (`U2PS_FAULT_OPEN`).
- How many bytes the app wrote into a host packet: `u2ps_host_queue()` takes no length, so the stack sees only whether the app asked `u2ps_host_slots_next()`.
- A callback's time against the port's interrupt budget, the attach within 100 ms of power-up (TSIGATT) and a VBUS report on every poll: they need a clock the stack does not have.
- The tick period while nothing is timed: then no bound rests on it.
- Mistakes that already report themselves: a description that breaks a rule (`U2PS_DEV_ERR_DESCRIPTION`), memory the port refuses (`U2PS_FAULT_OPEN`), and the results of `u2ps_dev_stream_open()`, `u2ps_host_stream_start()` and `u2ps_host_test_mode()`.

## 6. The done check

Sections 6.1 and 6.2 run from the root of the u2ps tree. The first needs no hardware; run it before you build your own firmware, and again after any change to your copy of u2ps. Section 6.3 lists what your firmware must show, and section 6.4 the commands for your own project.

### 6.1 u2ps itself: the PC build and the tests

```
cmake -S . -B build/pc -G Ninja -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/pc
ctest --test-dir build/pc
cmake -DU2PS_ROOT=. -P tests/check_agents_sync.cmake
```

Success is all of these (`README.md:294-301`, `tests/CMakeLists.txt:238-260`):
- `ctest` ends with `100% tests passed, 0 tests failed out of <N>`.
- The list shows `agents_sync` as `Passed`, not `Skipped`. It is skipped only while the tree has no `AGENTS.md` (`tests/CMakeLists.txt:244`). It fails when this guide and the app headers disagree.
- `agents_citations` is `Passed`: every `path:line` citation of this guide names a file of the tree and lies within it (`tests/check_agents_citations.cmake:1-20`).
- `test_agents_sync`, `test_agents_citations`, `test_stack_selection`, `test_host_budget`, `test_host_ep0_buf`, `test_host_waits`, `test_host_loopback`, `test_host_stream`, `test_host_contract`, `test_host_testdev`, `test_host_suspend`, `test_host_messages`, `test_host_speed`, the `test_dev_*` and `test_testkit_*` tests and the worked example's `pc_testkit_*` tests are `Passed`.
- The last command, the sync check alone without a build, prints `agents sync: AGENTS.md is in sync with the app and test kit headers:` with the counts, and exits with status 0 (`README.md:301-302`).

A failed check or any sanitizer report fails a test; the PC build compiles with ASan and UBSan (`tests/CMakeLists.txt:3-6`, `CMakeLists.txt:76-79`). The sync check also runs alone: `ctest --test-dir build/pc -R agents_sync --output-on-failure`. It prints `agents sync: AGENTS.md is in sync with the app and test kit headers:` and the counts. It reads the test kit's headers as well as the app headers. When it fails, it names each symbol that is missing from this guide, or that this guide names and the tree does not have (`tests/check_agents_sync.cmake:12-33`).

### 6.2 u2ps on the ARM chips

With ATfE, the project's toolchain (section 2):

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
```

With GCC, the same with `-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake` and without `U2PS_ATFE_DIR`.

Success: each configure prints `-- u2ps: ARM build for <chip>, device port <port>` and `-- u2ps: stacks: device host`; the `imxrt1062` configure also says the device port has no register code. Each build prints no warning (every warning stops it), ends with a line such as `[21/21] Linking C static library ...`, and exits with status 0 (`README.md:342-570`, `CMakeLists.txt:69-72`).

### 6.3 Your own firmware

1. Before the board: test your app on the PC with the test kit (section 7). Every test passes, each verdict with 0 contract violations and 0 port violations.
2. Configure with the toolchain file, `U2PS_CHIP`, your stack (`U2PS_DEVICE` or `U2PS_HOST`) and, for the device stack, `U2PS_VBUS` (section 2). The configure lines above must appear. A stop names the option to fix.
3. Compile your app with at least `-Wall -Wextra -Werror`, as u2ps does (`CMakeLists.txt:69-72`). The u2ps headers compile warning-free under those flags with the skeletons of sections 3.10 and 4.9.
4. The link has no undefined `u2ps_app_*` symbol, and for a device on the STM32U5A5 or STM32F723 no undefined `u2ps_dport_*` symbol. The ChipIdea port defines none (section 1).
5. After flashing, check from your own log or a counter (the reference app logs each of these as `key=value` lines, `examples/nucleo_u5a5zj_q/main.c:4-163`):
   - `u2ps_dev_init()` returned `U2PS_DEV_OK` (the reference app's `init result=ok`).
   - `u2ps_app_on_configured(true, U2PS_CONFIG_END_NONE)` runs when the host configures the device, and `u2ps_dev_speed()` is `U2PS_SPEED_HIGH` there (`configured speed=high`).
   - `u2ps_app_on_stream_start()` runs when the host selects a streaming alternate setting, and `u2ps_app_on_packet()` runs afterwards (`stream_start`, then `stream_stats` once a second).
   - `u2ps_dev_stats()` shows no `no_buffer`, `late`, `corrupted` or `release_errors` growth while the host plays. The reference app on the NUCLEO-U5A5ZJ-Q lost no packet up to 192 kHz with a Linux host (`README.md:64-69`).
6. The host stack has no hardware check yet. Its check is your tests with the test kit (section 7): against the scripted device and through the loopback to the u2ps device stack.
7. Build your firmware once with `-DU2PS_CONTRACT_CHECKS=ON` and a `u2ps_app_on_contract()` (section 5.1), and run a whole session: enumeration, a stream start, playing, a stop, a detach or an unplug. The hook is never called.

### 6.4 The done check in your own project

Sections 6.1 and 6.2 run in the u2ps tree. These run in your project, which adds u2ps with `add_subdirectory()` (sections 2.1 and 7.1), from your project's root. Replace `<...>`. Checked with a project of three files, the worked example's `dac_app.c` and `test_dac.c` (section 7.6) and a `main()` that calls `dac_app_init()` and `dac_app_poll()`, with the toolchain file of section 2.1.

1. Your PC tests with the test kit:

   ```
   cmake -S . -B build/pc -G Ninja -DU2PS_DEVICE=ON -DU2PS_VBUS=app
   cmake --build build/pc
   ctest --test-dir build/pc --output-on-failure
   ```

   Add `-DU2PS_HOST=ON -DU2PS_HOST_IA_VBUS_RATED_MA=<mA>` for a player (section 2). Do not pass `U2PS_CHIP` or a toolchain file. Success is all of these:
   - The configure step prints `-- u2ps: PC build with ASan and UBSan, fake ports` and `-- u2ps: stacks: device` (`device host` with both).
   - The build prints no warning and ends with a line `Linking C executable <your test>`; your test targets have `-Wall -Wextra -Werror`.
   - `ctest` ends with `100% tests passed, 0 tests failed out of <N>`, where N is your number of `add_test` calls.
   - Each test's verdict is `u2ps testkit: PASS: <n> checks, 0 failed, 0 contract violations, 0 port violations`. `ctest` shows it only for a failed test, or with `ctest --test-dir build/pc -V`; a test run by hand prints it as its last line and exits with status 0 (section 7.2).
2. Your firmware, with `-Werror`:

   ```
   cmake -S . -B build/fw -G Ninja -DCMAKE_TOOLCHAIN_FILE=<your-toolchain-file> \
     -DU2PS_CHIP=<chip> -DU2PS_DEVICE=ON -DU2PS_VBUS=app
   cmake --build build/fw
   ```

   Leave out `-DU2PS_CHIP` if your `CMakeLists.txt` sets it (section 2.1). Your firmware target has `target_compile_options(<firmware> PRIVATE -Wall -Wextra -Werror)`. Success is all of these:
   - The configure step prints `-- u2ps: ARM build for <chip>, device port <port>` and `-- u2ps: stacks: device`.
   - The compiler prints no `warning:` and no `error:`. The build ends with `Linking C executable <your firmware>` and exits with status 0, and the link has no `undefined reference to` (GNU ld) or `undefined symbol:` (lld) a `u2ps_app_*` symbol (section 3.7). A link without a startup file and a linker script also prints `warning: cannot find entry symbol _start` with either linker (checked); yours has both.
   - A warning in your own source stops the build: an unused variable in `dac_app.c` gave `error: unused variable 'x' [-Werror=unused-variable]` and a failed build (checked; clang words it `[-Werror,-Wunused-variable]`, checked).
3. The same firmware build with `-DU2PS_CONTRACT_CHECKS=ON` and your `u2ps_app_on_contract()` (section 5.1) configures and builds as in step 2 (checked); run it on the board as in section 6.3, step 7.

If a step fails, sections 2, 3.7 and 5 list the common causes. The PC build reproduces your app's logic, not the chip's timing, DMA reach or caches (section 7.7), so step 2 and the board check of section 6.3 stay.

## 7. Test your integration on the PC

The test kit runs your app against u2ps on the PC, without a board: your own `u2ps_app_*` callbacks against the stack built with your options, a fake port, and the other side of the bus played by the kit. It records every callback in order with its arguments, every contract violation and every violation of the port interface, and gives the test's verdict. It is part of the PC build only; an ARM build never builds it (`CMakeLists.txt:183-185`, `testkit/u2ps_testkit.h:1-42`).

### 7.1 Build

Configure your project for the PC, without the toolchain file, with the stacks you test and the options your firmware uses. Your project adds u2ps as for the board (section 2):

```
cmake -S <your-project> -B <build-dir> -G Ninja -DU2PS_DEVICE=ON -DU2PS_VBUS=app
```

Build one executable per test: your app's sources (the firmware's files, without the board code), the test's `main()`, and one kit library instead of the stack's library. Never link `u2ps_device` or `u2ps_host` as well: the kit library holds the stack (`testkit/u2ps_testkit.h:8-19`, `testkit/CMakeLists.txt:46-70`).

| Library | Needs | Your app | The kit plays | Header |
|---|---|---|---|---|
| `u2ps_testkit_device` | `U2PS_DEVICE=ON` | a DAC: the device callbacks | the device port and the host | `testkit/u2ps_testkit_dev.h` |
| `u2ps_testkit_host` | `U2PS_HOST=ON` | a player: the host callbacks | the host port and a scripted UAC2 device | `testkit/u2ps_testkit_host.h` |
| `u2ps_testkit_loopback` | both | a player: the host callbacks | the bus to the u2ps device stack, and its DAC app | `testkit/u2ps_testkit_loopback.h` |

```cmake
add_subdirectory(<u2ps-root> u2ps)
enable_testing()
add_executable(test_dac test_dac.c dac_app.c)
target_link_libraries(test_dac PRIVATE u2ps_testkit_device)
add_test(NAME test_dac COMMAND test_dac)
```

The kit library brings what the stack's library brings (the build constants as public compile definitions, the headers), with `U2PS_CONTRACT_CHECKS` set to 1 whatever the option says, the fake port's `u2ps_dport_cfg.h` or `u2ps_hport_cfg.h`, and the AddressSanitizer and UndefinedBehaviorSanitizer options for compiling and linking your test (`testkit/u2ps_testkit.h:20-26`, `testkit/CMakeLists.txt:22-31`). So your app defines `u2ps_app_on_contract()` (section 5.1); the kit records each violation first and then calls it.

The fake ports' constants differ from a chip's (`testkit/u2ps_dport_cfg.h:14-23`, `testkit/u2ps_hport_cfg.h:10-12`): `U2PS_DPORT_ALIGN` 4, `U2PS_DPORT_STRIDE(mps)` `mps` rounded up to 4, `U2PS_DPORT_ISO_OUT_MIN` 2, `U2PS_DPORT_ISO_OUT_RESERVE` 1, `U2PS_DPORT_UFRAME_EXACT` 1, `U2PS_HPORT_ALIGN` 4. Size class memory and host packets with these macros, not with numbers, so that the same code holds on the chip (section 3.1).

u2ps's own tests are built only when u2ps is the top-level project; your build builds the kit and the stacks' libraries (`CMakeLists.txt:183-195`).

### 7.2 What the kit runs, records and decides

- Your callbacks run as on the board: inside the kit's injections, which stand for the port's interrupt, or inside your own thread calls. Your test's `main()` is your app's thread: it calls your init and your stack calls as your `main()` does on the board.
- Time: `u2ps_testkit_set_poll(poll)` gives the kit your main loop's pass, which it calls once per millisecond of its time with that time; your pass calls `u2ps_dev_tick()` or `u2ps_host_tick()`. Without a poll, the kit calls the tick itself. `u2ps_testkit_now_ms()` is the kit's time since the side's reset (`testkit/u2ps_testkit.h:159-167`).
- The trace: every callback since `u2ps_testkit_trace_clear()` (a side's reset clears it too), each a `u2ps_testkit_call_t` with its `u2ps_testkit_callback_t`, up to four arguments as `uint32_t` and its result. `u2ps_testkit_trace_count()`, `u2ps_testkit_trace_get(i)` (NULL beyond the kept), `u2ps_testkit_calls(callback)` (count since the clear), `u2ps_testkit_callback_name()`, and `u2ps_testkit_trace_print()`, which prints one line per callback such as `u2ps testkit: trace 3: on_packet len=192 uframe=17`. The trace keeps `U2PS_TESTKIT_TRACE_MAX` (4096) calls and counts the rest; `U2PS_TESTKIT_CALLBACKS` is the number of callback kinds (`testkit/u2ps_testkit.h:66-140`).
- `u2ps_testkit_trace_match(expected)` compares the trace since the clear with a list of names separated by spaces, each optionally followed by `*n` for n calls in a row: `"on_configured on_stream_start on_packet*3 on_hole on_packet"`. On a mismatch it prints both and returns false (`testkit/u2ps_testkit.h:131-135`).
- Contract violations: each one a stack reports is printed as `u2ps testkit: contract violation U2PS_CONTRACT_DEV_RELEASE_COUNT (device stack)`, recorded, added to the trace as `on_contract`, and then passed to your `u2ps_app_on_contract()`. `u2ps_testkit_contract_count()` counts those in the record; `u2ps_testkit_contract_take(code)` takes one code's out of it, for a test that breaks a contract on purpose; `u2ps_testkit_contract_name(code)` gives its name (`testkit/u2ps_testkit.h:142-153`).
- Port violations: a rule of the port interface broken, or a mistake of your test's script, such as a packet injected without a stream. The fake port prints each, as `fake_dport: port violation: <reason>`. `u2ps_testkit_port_violations()` counts them; `u2ps_testkit_port_violations_take()` takes them all (`testkit/u2ps_testkit.h:154-157`). Both records survive a side's reset.
- The verdict: `U2PS_TESTKIT_CHECK(cond)` counts a check and prints `u2ps testkit: CHECK FAILED at <file>:<line>: <cond>` when it fails (the function behind it is `u2ps_testkit_check()`). `u2ps_testkit_result()` prints `u2ps testkit: PASS: <n> checks, 0 failed, 0 contract violations, 0 port violations`, or the same with `FAIL` and the counts, and returns 0 or 1: end `main()` with `return u2ps_testkit_result();` (`testkit/u2ps_testkit.h:52-64`). Any sanitizer report also ends the test with a failing exit code.

The callbacks in the trace: the name is the callback's name without `u2ps_app_`, and the arguments are in this order (`testkit/u2ps_testkit.h:66-107`).

| Value | Name in the trace | Arguments; result |
|---|---|---|
| `U2PS_TESTKIT_ON_REQUEST` | `on_request` | bmRequestType, bRequest, wValue, wIndex; its result |
| `U2PS_TESTKIT_ON_SAMPLING_FREQUENCY` | `on_sampling_frequency` | requested, chosen, id; its result |
| `U2PS_TESTKIT_ON_CONFIGURED` | `on_configured` | configured, why |
| `U2PS_TESTKIT_ON_STREAM_START` | `on_stream_start` | stream, alternate_setting, sampling_frequency, max_packet_size; the `u2ps_stream_start_t` |
| `U2PS_TESTKIT_ON_PACKET` | `on_packet` | len, uframe |
| `U2PS_TESTKIT_ON_HOLE` | `on_hole` | uframes, why |
| `U2PS_TESTKIT_ON_PAUSE` | `on_pause` | uframes |
| `U2PS_TESTKIT_ON_STREAM_STOP` | `on_stream_stop` | none |
| `U2PS_TESTKIT_ON_FAULT` | `on_fault` | fault |
| `U2PS_TESTKIT_ON_RESET` | `on_reset` | none |
| `U2PS_TESTKIT_ON_SUSPEND` | `on_suspend` | none |
| `U2PS_TESTKIT_ON_RESUME` | `on_resume` | none |
| `U2PS_TESTKIT_ON_VBUS` | `on_vbus` | present |
| `U2PS_TESTKIT_ON_CONTROL` | `on_control` | bmRequestType, bRequest, wValue, wIndex; the result argument |
| `U2PS_TESTKIT_HOST_ON_DEVICE` | `host_on_device` | id_vendor, id_product, alternate_setting_count, sampling_frequency |
| `U2PS_TESTKIT_HOST_ON_END` | `host_on_end` | why, refusal |
| `U2PS_TESTKIT_HOST_ON_STREAM_START` | `host_on_stream_start` | stream, alternate_setting, sampling_frequency, max_packet_size |
| `U2PS_TESTKIT_HOST_ON_SENT` | `host_on_sent` | uframe |
| `U2PS_TESTKIT_HOST_ON_HOLE` | `host_on_hole` | uframes, why |
| `U2PS_TESTKIT_HOST_ON_STREAM_STOP` | `host_on_stream_stop` | why, request |
| `U2PS_TESTKIT_HOST_ON_TEST_DEVICE` | `host_on_test_device` | id_product, bcd_device |
| `U2PS_TESTKIT_HOST_ON_RESUMED` | `host_on_resumed` | remote_wakeup |
| `U2PS_TESTKIT_ON_CONTRACT` | `on_contract` | violation, stack (0 device, 1 host) |

### 7.3 A DAC app: `u2ps_testkit_device`

Start every scenario with `u2ps_testkit_dev_reset()`; it resets the fake port, the trace and the time, not the stack, which your init resets (`testkit/u2ps_testkit_dev.h:63-68`). Then call your init, report VBUS, and drive the host's side:

| Call | What the host or the bus does |
|---|---|
| `u2ps_testkit_dev_vbus(present)` | VBUS comes or goes; in mode `app` this is also your board's VBUS poll: the kit calls `u2ps_dev_vbus()` from the thread. |
| `u2ps_testkit_dev_attached()` | The pull-up is on. |
| `u2ps_testkit_dev_enumerate(speed)` | A bus reset at `speed`, the device and configuration descriptors, SET_ADDRESS 1, the strings, SET_CONFIGURATION 1. It stops at the first transfer that does not end OK and prints which. |
| `u2ps_testkit_dev_configuration(&len)` | The configuration descriptor the enumeration read. |
| `u2ps_testkit_dev_set_sampling_frequency(hz)`, `u2ps_testkit_dev_get_sampling_frequency(&hz)` | SET CUR and GET CUR of the clock's Sampling Frequency Control. |
| `u2ps_testkit_dev_set_interface(alternate_setting)` | SET_INTERFACE of the AudioStreaming interface: 0 stops the stream, 1 to `format_count` starts one. |
| `u2ps_testkit_dev_control(&setup, data, &actual)` | Any control transfer, packet by packet; `u2ps_testkit_dev_control_resume(data, &actual)` finishes one that ended NAK. |
| `u2ps_testkit_dev_packet(len, uframe)`, `u2ps_testkit_dev_packet_bytes(data, len, uframe)` | A packet received in microframe `uframe`, into the next buffer of your class memory: a running counter's bytes, or yours. |
| `u2ps_testkit_dev_hole(uframes, why)` | The port reports a hole of any `u2ps_miss_t`. |
| `u2ps_testkit_dev_bus_reset(speed)`, `u2ps_testkit_dev_suspend()`, `u2ps_testkit_dev_resume()` | Bus events. |
| `u2ps_testkit_dev_run_ms(ms)` | The kit's time: at each millisecond your poll, or `u2ps_dev_tick()`. |
| `u2ps_testkit_dev_feedback()`, `u2ps_testkit_dev_held()`, `u2ps_testkit_dev_streaming()` | The feedback value the endpoint sends next, the packets your app holds, whether a stream is open. |

A control transfer ends as a `u2ps_testkit_control_t`: `U2PS_TESTKIT_CONTROL_OK`, `U2PS_TESTKIT_CONTROL_STALL`, `U2PS_TESTKIT_CONTROL_NAK` (the answer is pending: your `U2PS_REQ_DEFER` or `U2PS_STREAM_DEFER`; call `u2ps_dev_reply()` or `u2ps_dev_stream_open()`, then `u2ps_testkit_dev_control_resume()`) or `U2PS_TESTKIT_CONTROL_ERROR` (a protocol fault, or the kit used out of order; the kit prints why) (`testkit/u2ps_testkit_dev.h:50-61`).

Rules of the fake device port (`testkit/u2ps_testkit_dev.h:25-38`, `device/u2ps_dev.h:718-737`):
- A hole comes to your `on_hole` (or `on_pause`, NO_PACKET beyond `U2PS_GAP_PAUSE`) immediately before the packet that ends it. Inject the hole, then the packet whose microframe lies `uframes` after the one following the last packet. A gap in the microframes without a report is a NO_PACKET hole.
- The ring: each packet goes into the next buffer you lent and stays held until you release it. The fake keeps one buffer armed (`U2PS_DPORT_ISO_OUT_RESERVE` 1), as DWC2 does: a packet that comes while you hold `count` - 1 is not delivered and becomes a NO_BUFFER hole of 1, before the next packet; after the stream's last packet, such holes and the microframes without a packet between them come before `on_stream_stop`, or earlier, at each tick of `u2ps_testkit_dev_run_ms()` once no packet has been delivered for more than `U2PS_GAP_PAUSE` microframes' worth (the long gap; the poll's `u2ps_dev_tick()` reports them).
- A stream's close ends at the kit's next step: right after the control transfer that began it, or before the next injection.

### 7.4 A player app: `u2ps_testkit_host`

Start every scenario with `u2ps_testkit_host_reset()`; it resets the fake port, the trace, the time and the scripted device, not the stack (`testkit/u2ps_testkit_host.h:63-67`). Then call your init (`u2ps_host_init()`, `u2ps_host_vbus(true)`, `u2ps_host_connect(true)`) and plug in the device with `u2ps_testkit_host_connect()`; `u2ps_testkit_host_disconnect()` pulls it.

The kit's time advances only in `u2ps_testkit_host_run_ms(ms)`, `u2ps_testkit_host_run_uframes(n)` and `u2ps_testkit_host_run_until(done, max_ms)`. Each microframe the port sends one queued packet; every eighth a millisecond passes, with the port's completions and then your poll or the kit's `u2ps_host_tick()` (`testkit/u2ps_testkit_host.h:21-24`). `u2ps_testkit_host_queued()` counts the packets in the port's queue.

Holes: `u2ps_testkit_host_inject_late(uframes)` sends the next packet that many microframes late, a LATE hole before its `host_on_sent`; `u2ps_testkit_host_inject_corrupted()` damages the next packet, a CORRUPTED hole of 1 before its `host_on_sent`. A NO_BUFFER hole comes by itself when your queue runs empty while the stream runs (`testkit/u2ps_testkit_host.h:89-95`).

The scripted device (`testkit/u2ps_testkit_host.h:33-42,99-131`): VID 0x1234, PID 0x5678, a UAC2 sink of 2 channels whose alternate settings 1 to 4 carry 16/2, 24/4, 32/4 and 24/3 (bits/bytes) with explicit feedback, at 44.1 to 768 kHz. Change it with `u2ps_testkit_host_configuration(desc, len)`, `u2ps_testkit_host_sampling_frequencies(rates, n)` and `u2ps_testkit_host_feedback(value)` (16.16 slots per microframe; 0, the default, is the nominal rate); make it refuse with `u2ps_testkit_host_refuse_alternate_setting(alt)` and `u2ps_testkit_host_refuse_sampling_frequency(true)`. Read what the host did with `u2ps_testkit_host_sampling_frequency()` and `u2ps_testkit_host_alternate_setting()`, and what arrived with `u2ps_testkit_host_received()`, a `u2ps_testkit_received_t` (packets, bytes, corrupted, the last packet's length and microframe). `u2ps_testkit_host_on_packet(fn)` hands your test each packet's bytes.

Speed, the test device, suspend (`testkit/u2ps_testkit_host.h:73-81,101-107`): `u2ps_testkit_host_speed(speed)` makes the device's next reset end at `U2PS_SPEED_FULL` or `U2PS_SPEED_LOW` instead of `U2PS_SPEED_HIGH`, to see your app's `host_on_end` for a full- or low-speed device. `u2ps_testkit_host_device_ids(id_vendor, id_product, bcd_device)` changes its IDs; VID 0x1A0A with a test PID makes it the test device, and `host_on_test_device` follows the enumeration (section 4.4). `u2ps_testkit_host_test_mode()` gives the test mode selector the port is in, 0 for none. `u2ps_testkit_host_suspended()` tells whether the port is suspended, and `u2ps_testkit_host_wakeup()` makes the device signal a remote wakeup: while suspended, the stack resumes and `host_on_resumed(1)` follows (section 4.10). The fake port ends a resume at once, as DWC2 does. It also opens no stream whose feedback endpoint has a bInterval above 13, as the DWC2 host port (section 4.1, item 5): with a configuration of bInterval 14 to 16 your app gets `host_on_stream_stop` with `U2PS_HOST_STOP_OPEN` (`testkit/u2ps_testkit_host.h:25-31`, `testkit/fake_hport.h:226-230`).

### 7.5 A player against the u2ps device stack: `u2ps_testkit_loopback`

`u2ps_testkit_loopback_reset(desc)` replaces the host side's reset: the scripted device is replaced by the u2ps device stack with the kit's own DAC app, described by `desc` or, with NULL, a default description of 2 channels, 16/2, 24/4 and 32/4 at 44.1 to 768 kHz. It returns false, and prints why, if the device stack's `u2ps_dev_init()` refuses the description. Every packet your app sends arrives at the device stack with its bytes in the microframe of its `host_on_sent`, and every feedback poll reads the device stack's value. The device side's callbacks appear in the same trace (`on_stream_start`, `on_packet`, `on_hole`), and `u2ps_testkit_host_received()` and `u2ps_testkit_host_on_packet()` report what the kit's DAC app received (`testkit/u2ps_testkit_loopback.h:1-51`).

### 7.6 The worked example and its expected output

`examples/pc_testkit/` is an integration to copy: `dac_app.c` (a DAC app written as firmware) with `test_dac.c`, and `player_app.c` (a player app) with `test_player.c` (against the scripted device) and `test_player_loopback.c` (against the u2ps device stack). Its `CMakeLists.txt` is a project that adds u2ps as an integration does. From the root of the u2ps tree:

```
cmake -S examples/pc_testkit -B build/pc_testkit -G Ninja \
  -DU2PS_DEVICE=ON -DU2PS_HOST=ON -DU2PS_VBUS=app \
  -DU2PS_HOST_IA_VBUS_RATED_MA=500
cmake --build build/pc_testkit
ctest --test-dir build/pc_testkit --output-on-failure
```

Success is all of these (`examples/pc_testkit/CMakeLists.txt:1-44`):
- The configure step prints `-- u2ps: PC build with ASan and UBSan, fake ports` and `-- u2ps: stacks: device host`; the build prints no warning. Without `-DCMAKE_BUILD_TYPE` it also prints `-- u2ps: CMAKE_BUILD_TYPE is empty, so u2ps's libraries build without optimisation: ...`; the tests on the PC do not depend on it (section 2.1).
- `ctest` ends with `100% tests passed, 0 tests failed out of 3`.
- Each test's output ends with its verdict: `u2ps testkit: PASS: 29 checks, 0 failed, 0 contract violations, 0 port violations` for `test_dac`, with its own count of checks for the others.

Your own tests pass the same way: every test `Passed`, each verdict `PASS` with 0 failed checks, 0 contract violations and 0 port violations. u2ps's own PC build (section 6.1) runs the example's tests too, as `pc_testkit_test_dac`, `pc_testkit_test_player` and `pc_testkit_test_player_loopback`.

### 7.7 What the PC cannot show

The PC run checks the order and the arguments of your callbacks, your use of the calls, the contracts the stacks check, and your memory use under the sanitizers. It does not check what needs the chip: the time your callbacks take against the interrupt budget, DMA reach and cache maintenance, the alignment of a port whose `U2PS_DPORT_ALIGN` is 32, your board's VBUS reading, and the port's real timing. The board check of section 6.3 stays.
