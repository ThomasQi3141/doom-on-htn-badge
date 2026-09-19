# 2026 Hacker Badge — hardware reference

Extracted from the KiCad project in `Archive 2.zip` (`badge.kicad_sch`, prod-3) by
resolving the schematic netlist geometrically. Every pin below is traced from the
schematic, not guessed from a datasheet.

## Core

| Item | Value |
|---|---|
| Module | **ESP32-C3-MINI-1-N4** (U9) |
| Core | single RISC-V RV32IMC @ 160 MHz |
| SRAM | **400 KB total** (~320 KB usable DRAM once ROM + IDF take their cut) |
| Flash | **4 MB**, in-package, XIP/memory-mappable |
| PSRAM | **none** — the `-N4` variant has no PSRAM die and the module exposes no pins for one |
| Audio | **none** — no speaker, piezo, amp or DAC anywhere on the board |

GPIO11–17 are consumed by the in-package flash. Every remaining pin (0–10, 18–21)
is used; there are no spare GPIOs.

## Pin map

| GPIO | Net | Function |
|---|---|---|
| 0 | `DISP_RS` | display D/C |
| 1 | `DISP_SCL` | display SPI clock |
| 2 | `DISP_CS` | display chip select (10k pull-up) |
| 3 | `LED_DIN_3V3` | WS2812 data → SN74LVC1T45 level shifter → 5 V |
| 4 | `DISP_RST` | display reset (10k pull-up) |
| 5 | `I2C_SDA_BUS` | I²C data (4k7 pull-up) |
| 6 | `I2C_SCL_BUS` | I²C clock (4k7 pull-up) |
| 7 | `SR_QH` | 74HC165 serial out → MCU in |
| 8 | — | unconnected, 10k pull-up (strapping) |
| 9 | `ESP32_BOOT` | **START button** (SW10) + boot strap |
| 10 | `DISP_SDA` | display SPI MOSI |
| 18 | USB D− | via 33 R |
| 19 | USB D+ | via 33 R |
| 20 (RXD0) | `SR_SHLD` | 74HC165 SH/LD |
| 21 (TXD0) | `SR_CLK` | 74HC165 clock |

**UART0 is not available as a console** — GPIO20/21 drive the button shift
register. All logging and flashing go over the built-in USB Serial/JTAG on
GPIO18/19.

## Display

2.0" panel **HS20HS072RX** (U10) on a 12-pin FPC (FPC1), 4-wire SPI, **320×240**
(the badge firmware guide states `screen_width/height = 320×240`).

**Controller: ST7789** — confirmed on hardware. With no MISO there is no ID to
read, so both candidates were driven in turn and judged by eye: ST7789 (with
`INVON`, MADCTL `0x60`) renders correct upright non-inverted colour bars;
ILI9341 does not.

- Write-only: **there is no MISO**, so the controller cannot be read back. The
  driver must be identified by behaviour, not by RDDID.
- Backlight is **hard-wired on**: FPC pin 3 (A) → +3V3, pin 2 (K) → 10 R → GND.
  No PWM, no enable line, no software brightness control.
- Panel logic and I/O both run at 3V3 (FPC pins 4 and 5).

### SPI clock ceiling
The display sits on GPIO1/10/2, which are **not** the SPI2 IO_MUX pins
(IO_MUX for FSPI on C3 is CLK=6, D=7, Q=2, CS0=10, WP=5, HD=4). Signals route
through the GPIO matrix, which limits reliable operation to **40 MHz**.

At 40 MHz, 16 bpp:

| Region | Bytes | Time | Ceiling |
|---|---|---|---|
| Full 320×240 | 153,600 | 30.7 ms | 33 fps |
| Doom 320×200 | 128,000 | 25.6 ms | 39 fps |
| Doom 3D view only, 320×168 | 107,520 | 21.5 ms | 46 fps |

If the controller is an ST7789, switching to 12 bpp (`COLMOD` 4-4-4) cuts these
by 25 % and is worth doing — Doom's 256-colour palette loses little.

## Buttons

Eight inputs on a single **74HC165** (U8), read by bit-banging GPIO21/20/7.
`SER` and `CLK_INH` are grounded, so it is one 8-bit stage, not a chain.
All inputs carry 10k pull-ups, so a pressed button reads **0**.

| 165 input | Pin | Net | Switch |
|---|---|---|---|
| A | 11 | `BTN_1` | SW11 — MSK12C02 slide switch |
| B | 12 | `BTN_2` | SW2 tactile |
| C | 13 | `BTN_3` | SW3 tactile |
| D | 14 | `BTN_4` | SW4 tactile |
| E | 3 | `BTN_5` | SW8 tactile |
| F | 4 | `BTN_6` | SW7 tactile |
| G | 5 | `BTN_7` | SW5 tactile |
| H | 6 | `SW_HPM` | SW6 tactile |

The ninth button, **START, is GPIO9** — not on the shift register.

### Confirmed mapping
Established by pressing each button on hardware in a known order. The schematic
cannot give this — it names nets, not switches.

| Button | Net | 165 input |
|---|---|---|
| UP | `BTN_2` | B |
| DOWN | `BTN_5` | E |
| LEFT | `BTN_4` | D |
| RIGHT | `BTN_3` | C |
| A | `SW_HPM` | H |
| B | `BTN_7` | G |
| HOME | `BTN_6` | F |
| AUX1 | `BTN_1` | A (slide switch, a position not a press) |
| START | — | GPIO9 |

### START is the boot strap
GPIO9 is the ESP32-C3 boot-mode strap. Holding **START** while powering on drops
the chip into ROM download mode — which is both how we flash and, more
importantly, the unbrickable recovery path. The stock badge guide's "turn it on
*without* holding Start" is the same mechanism seen from the other side.

## Other peripherals

- **6× WS2812B-2020** (LED1–LED6) chained on GPIO3, shifted to 5 V.
- **SC7A20HTR** accelerometer (U2) on I²C, INT1 unconnected to the MCU.
- **MFRC522B** NFC reader (U7) strapped for I²C on the same bus.
- Power: 2×AA → MT3608 boost to 5 V, OR'd with VBUS by an LM66200, then an
  XC6220B331MR LDO to 3V3. SW1 (MSK12C02) is the power slide switch via an
  AO3401A PMOS.

## What this means for Doom

~320 KB of RAM and 4 MB of flash puts this badge in the **RP2040 class**, not the
"ESP32 with PSRAM" class that every existing ESP32 Doom port assumes:

- Vanilla `doomgeneric` / `chocolate-doom` want a 1.5–6 MB zone heap. **They
  cannot fit**, with or without tuning.
- `rp2040-doom` runs the full shareware game in **264 KB** of RAM and 2 MB of
  flash, by recompressing the WAD into a custom flash-resident format and
  streaming everything. That is the only proven design at this budget, and we
  have *more* RAM and *twice* the flash than it needs.
- A 320×200 8 bpp framebuffer is 64,000 bytes — affordable — with palette
  conversion done into small DMA bounce buffers on the way out to SPI.
- No sound hardware means Doom's audio can be cut entirely, which reclaims
  both flash and RAM that the RP2040 port had to budget for.

## Measured, not estimated

From a minimal ESP-IDF v5.5.1 build for this board (no WiFi, no Bluetooth,
USB Serial/JTAG console) — `firmware/bringup`:

| | bytes |
|---|---|
| **Total DRAM available to the application** | **321,296** |
| Used by the bring-up firmware | 63,194 |
| **Free** | **258,102** |

Measured again at runtime on the badge itself:

```
total free DRAM:     322,404 bytes
largest free block:  188,416 bytes
  region 1: 194 KiB  RAM
  region 2: 113 KiB  Retention RAM
  region 3:  10 KiB  Retention RAM
```

**The heap is three disjoint regions, not one pool.** 322 KB total, but the
largest single allocation that can ever succeed is **188 KB**. Any Doom port has
to be designed around that split — a 64,000-byte framebuffer is fine, one large
contiguous arena is not.

### Display throughput, measured
A 320×200 full-frame push at 40 MHz takes **28,450 µs — a 35.1 fps ceiling**,
before any rendering. Arithmetic predicted 39 fps; the difference is per-
transaction overhead.

### Stock flash layout (from the backup)

| label | type | offset | size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 16,384 |
| phy_init | data/phy | 0xd000 | 4,096 |
| factory | app | 0x10000 | 2,752,512 |
| storage | data/littlefs | 0x2b0000 | 1,310,720 |

Only 176 KB of the 4 MB is erased. Replacing this with a ~700 KB app leaves
roughly 3 MB for WAD data — comfortably more than rp2040-doom needs.

### eFuses
Secure Boot **disabled**, Flash Encryption **disabled**, `SPI_BOOT_CRYPT_CNT` 0.
Custom firmware runs. Chip is ESP32-C3 rev v0.4, MAC `e8:3d:c1:20:cf:cc`.

## The flash budget, and why no WAD recompression is needed

The raw shareware `DOOM1.WAD` is 4,196,020 bytes — larger than the badge's
entire 4 MB flash. But the board has **no speaker, amp or DAC**, so every sound
and every piece of music is data for code that can never run:

| category | bytes | share |
|---|---|---|
| levels | 848,149 | 20.2% |
| sprites | 825,576 | 19.7% |
| wall patches | 763,612 | 18.2% |
| UI graphics | 698,180 | 16.6% |
| **sound effects** | **535,127** | **12.8%** |
| **music** | **245,179** | **5.8%** |
| flats | 221,184 | 5.3% |
| core tables | 31,494 | 0.8% |
| ENDOOM | 4,000 | 0.1% |
| **PC speaker** | **3,055** | **0.1%** |

Dropping all audio, ENDOOM and the attract-mode demos leaves **3,362,373 bytes**
across 1137 lumps. With the app partition at 640 KB, the WAD partition is
3,473,408 bytes — so **the entire shareware episode fits uncompressed, with
111 KB to spare.**

This removes the single largest piece of work rp2040-doom had to do. That port
needed a bespoke recompressed WAD format because it was squeezing into 2 MB
total. We have twice the flash and a silent board, so the data problem is simply
gone. Verified on hardware: all 9 levels present, 848,149 bytes of level data.

**The remaining hard problem is RAM, not flash.**

## Escape hatch
If the Doom binary outgrows 640 KB, dropping `HELP1` and `CREDIT` (68,168 bytes
each) frees 136 KB of WAD, paying for a 768 KB app partition.
