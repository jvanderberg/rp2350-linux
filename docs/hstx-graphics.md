# High-Speed, Low-Memory HSTX Graphics Engine

Design notes for adding HDMI/DVI video output to `rp2350-linux` using the
RP2350's **HSTX** (High-Speed Transmit) peripheral: a zero-PSRAM-bandwidth
**glass terminal** by default, a **race-the-beam PPU** (tilemap, redefinable
fonts, sprites, and copper list) in Core 1 SRAM, and an **optional on-demand
320×240 PSRAM framebuffer**.

## Goals

- **Zero impact on Linux when idle or in terminal mode:** no PSRAM/QMI
  bandwidth consumption and no XIP cache thrashing.
- **Tiny SRAM footprint (~16–24 KB):** leave the bulk of the 520 KB SRAM
  available for the JIT code cache, soft-TLB, and Core 1 I/O services.
- **Glitch-free HDMI sync:** keep DVI timing alive even across flash
  erase/program operations (`rvemu-rs/rvemu-pico/src/flash.rs`).
- **60 fps from emulated Linux:** offload pixel pushing to native Hazard3
  code so `tcc` C programs, Lua, and MicroPython only update compact
  tile/sprite/scroll tables once per frame.
- **No monitor re-sync between modes:** every mode outputs the same
  640×480@60 Hz DVI timing, so switching between text, PPU, and 320×240
  bitmap (or split-screen) happens cleanly on VBLANK.

## Hardware

The RP2350's HSTX peripheral drives DVI/TMDS directly on **GP12–GP19** (4
differential pairs: clock + 3 data lanes), which any HDMI monitor accepts as
video-only DVI-over-HDMI. No active converter chip is needed.

### Adapter & Cable
- **22-pin FPC HSTX-to-DVI breakout** (e.g., Adafruit RP2350 22-pin FPC HSTX
  to DVI Adapter PID 6055, or SparkFun HSTX to DVI Breakout): provides the
  HDMI receptacle, termination/coupling, and DDC/HPD level shifting.
- **22-pin, 0.5 mm pitch FPC ribbon cable** (5–20 cm, matching the board's
  Type A / Type B connector orientation).

### Pin Conflicts in `rp2350-linux`
See `rvemu-rs/rvemu-pico/src/pins.rs` and `README.md`:
- **GP16–GP19 (HSTX lanes):** currently claimed by **SPI0** (`/dev/spidev0.0`).
  SPI0 must be disabled or moved in firmware so HSTX owns all 8 pins
  (`GP12–GP19`).
- **GP10–GP11 (SD card SCK/MOSI):** some HSTX adapters (including Adafruit's)
  wire **CEC** and **HPD** to GP10/GP11 by default. Cut the `CEC` and `HPD`
  solder jumpers on the adapter when using the SPI1 SD card.

## Why Race-the-Beam PPU Instead of an Always-On Framebuffer

The board's 8 MB APS6404L PSRAM runs on QMI CS1 at **116 MHz** in QPI mode
(**58 MB/s** theoretical peak, ~40–45 MB/s sustained), and shares both the
**QMI bus** and the **single 16 KB XIP cache** with the 16 MB flash holding
the Linux kernel's `.text` and `.rodata`.

| Mode (640×480@60 Hz DVI) | Buffer Size | Avg Bandwidth | Active Peak | QMI Impact |
|---|---|---|---|---|
| 640×480 RGB565 (PSRAM) | 600 KB | 36.9 MB/s | ~50 MB/s | Saturates QMI; stalls Linux |
| 320×240 RGB565 (PSRAM, 2×2) | 150 KB | 9.2 MB/s | ~6.3 MB/s* | ~15–20% of QMI |
| 320×240 8-bit (PSRAM, 2×2) | 75 KB | 4.6 MB/s | ~3.2 MB/s* | ~8–10% of QMI |
| 640×480 1-bit (SRAM/PSRAM) | 38.4 KB | 2.3 MB/s | ~3.2 MB/s | ~5% (or 0% in SRAM) |
| **SRAM Glass Terminal / PPU** | **~16–24 KB** | **0 MB/s** | **0 MB/s** | **0% QMI** |

*\*With 2× vertical pixel doubling, each source line is fetched once into a
small SRAM line buffer and streamed to HSTX twice.*

Two additional constraints apply to any PSRAM-backed scanout:
1. **XIP cache thrashing:** DMA reading through the cached PSRAM window
   (`0x11000000`) evicts hot guest RAM and kernel code from the 16 KB XIP
   cache. PSRAM scanout must read via the **uncached alias (`0x15000000`)**,
   with guest framebuffer writes kept coherent (uncached or flushed).
2. **Flash erase/program stalls (`flash.rs`):** writing `/home` or flash
   swap disables XIP for ~30–45 ms per sector erase (2–3 video frames) and
   pauses XIP DMA. A PSRAM-only scanout would drop DVI sync unless DMA is
   temporarily pointed at an SRAM fallback line. An SRAM-only PPU/terminal
   never touches QMI at all.

## Clocking & DVI Timing

- **Target mode:** 640×480@60 Hz (800×525 total, 25.175 MHz pixel clock).
- **HSTX DDR clock:** 5× pixel clock ≈ **125.875 MHz** (~126 MHz is within
  spec for virtually all HDMI monitors).
- **Hardware TMDS encoder:** HSTX encodes raw RGB332 (8-bit) or RGB565
  (16-bit) pixels to 10-bit TMDS in hardware, so scanline buffers store raw
  pixels rather than expanded TMDS words.
- **System clock interaction:** `rp2350-linux` currently runs `clk_sys` at
  **348 MHz** (`rp2350/board`), where `348 / 3 = 116 MHz` (~55 Hz, rejected
  by many monitors). Clean options:
  - Set `clk_sys` to a clean multiple of 126 MHz (**252 MHz** `/2` or
    **378 MHz** `/3`, if stable at voltage), or
  - Clock `clk_hstx` from a dedicated PLL / auxiliary clock source targeting
    ~126 MHz while keeping `clk_sys` independent.

## Core 1 SRAM PPU Architecture

Core 1 already runs entirely out of SRAM (`rvemu-rs/rvemu-pico/src/core1.rs`)
for USB serial and Wi-Fi. The PPU slots directly into this model:

- **Ping-pong scanline buffers in SRAM:** two buffers of 640 bytes (RGB332)
  or 1,280 bytes (RGB565).
- **Scanline budget:** 1 horizontal line = **31.78 µs** (~8,000–12,000
  Hazard3 cycles at 252–378 MHz).
- **DMA + IRQ pipeline:**
  1. DMA Channel A streams scanline `N` from Buffer A into the HSTX FIFO.
  2. When Channel A finishes, it chains automatically to Channel B (streaming
     scanline `N+1`) and raises a high-priority SRAM IRQ on Core 1.
  3. The IRQ handler renders scanline `N+2` into Buffer A (~1,500–2,500
     cycles for 80-column text; headroom left for sprites/copper) and returns.
- **Surviving flash writes:** when Core 0 erases/programs flash, `core1::park()`
  holds Core 1 in an SRAM spin loop off XIP. Because the PPU's code, font,
  tile/sprite tables, and line buffers all live in SRAM, the HSTX DMA IRQ can
  remain enabled during `park()` and keep driving HDMI with zero dropped
  frames. (If Core 1 itself initiates a flash op, swap DMA to a static SRAM
  blank line for the duration.)

## Engine Modes & Memory Budget

All modes share the same 640×480@60 Hz scanout pipeline and can be switched
on VBLANK (or mid-frame via the copper list).

### 1. Glass Terminal (Default)
Live-swappable font heights give higher effective text density without
changing HDMI timing:

| Font | Grid | Cell Buffer (2 B/cell: glyph + fg/bg) | Font Table (256 glyphs) |
|---|---|---|---|
| 8×16 | 80×30 | 4.8 KB | 4.0 KB |
| 8×8 | 80×60 | 9.6 KB | 2.0 KB |
| 6×8 | 106×60 | 12.7 KB | 2.0 KB |

### 2. Redefinable Glyphs / Tile Graphics
Extend the font table to **512 glyphs** (256 standard ASCII/box-drawing +
256 writable custom tiles = +2–4 KB SRAM):
- Lets terminal apps draw **native 640×480 1bpp** graphics with per-cell
  foreground/background color (e.g., a 128×256 or 256×128 inline canvas,
  plots, Sixel-like images, or crisp UI widgets) at 0% PSRAM bandwidth.
- Updated on the fly by writing 8 or 16 bytes per glyph directly to SRAM.

### 3. Retro PPU Mode (Gaming Engine)
Designed for ~16–24 KB total SRAM:
- **Background tilemap:** 8×8 or 8×16 tiles with fine X/Y pixel scroll
  registers (can double as the text grid).
- **Sprites:** 32–64 hardware-style sprites `(x, y, tile_id, palette, flags)`
  with horizontal/vertical flip and transparent color 0, composited per
  scanline in the IRQ handler.
- **Palette:** 256-entry RGB565 LUT (**512 bytes** in SRAM).
- **Copper list (optional):** compact per-scanline command list (e.g., change
  scroll X/Y, swap palette entries, switch font bank, or switch between
  320×240 graphics and 640×480 text for a crisp HUD/status bar).
- **VBLANK sync:** hardware frame counter + wake event so Linux user space
  can block on VBLANK at a clean 60 Hz.

### 4. On-Demand 320×240 PSRAM Framebuffer
- Disabled by default (0% QMI bandwidth).
- Enabled when Linux opens `/dev/fb0` or selects bitmap mode for a screen
  band.
- Allocates **75 KB** (8-bit indexed/RGB332) or **150 KB** (RGB565) in PSRAM
  and streams via the uncached alias (`0x15000000`) into the SRAM line buffer
  with 2×2 pixel doubling.
- During `flash.rs` erase/program, automatically falls back to a static SRAM
  blank/repeat line so DVI sync is preserved.

## Linux Guest Interface

Follows the same host-offload pattern as `/dev/rvemu-crypto` and `/dev/pio*`:

1. **Console / Glass Terminal:**
   - Mirror the existing console byte stream (or expose a dedicated TTY) to a
     small VT100/ANSI parser on Core 1 writing into the SRAM cell grid.
   - Support private OSC/DCS escape sequences (or a small control device) to
     switch 8×16 ↔ 8×8 font height and upload custom glyph bitmaps.
2. **PPU Device (`/dev/ppu`):**
   - `mmap()`able shared register/tile/sprite/palette/copper region (mirrored
     into Core 1 SRAM on VBLANK or double-buffered).
   - `poll()` / `ioctl(PPU_WAIT_VBLANK)` for tear-free 60 fps loops from C
     (`tcc`), Lua, or MicroPython.
3. **Optional Framebuffer (`/dev/fb0`):**
   - Lightweight `rvemu-fb` / `simplefb` driver backed by the reserved PSRAM
     region, enabling `fbcon` or raw pixel drawing when needed.

## Incremental Implementation Plan

1. **Bring-up (HSTX DVI sync):** Free GP16–GP19 from SPI0, resolve `clk_hstx`
   (~126 MHz), and output a static 640×480@60 Hz test pattern from SRAM via
   HSTX + ping-pong DMA on Core 1.
2. **SRAM Glass Terminal:** Add the 8×16 / 8×8 font table, 80×30 cell grid,
   scanline renderer IRQ, and Core 1 ANSI parser mirrored from the console;
   verify zero glitches during `flash.rs` writes.
3. **Dynamic Fonts & PPU:** Add writable glyph RAM, sprite table, 256-color
   RGB565 palette, copper list, and `/dev/ppu` with VBLANK `poll()`. Port
   `extras/demos/asteroids` or `stackfall` as a 60 fps reference demo.
4. **Optional 320×240 PSRAM `/dev/fb0`:** Add on-demand uncached PSRAM
   scanline fetch (`0x15000000`) with 2×2 doubling and SRAM blank-line
   fallback during flash operations.
