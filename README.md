# fpj2me

**A bare-metal J2ME runtime (CLDC 1.1 + MIDP 2.0) that runs real Java-ME game
jars — as an [fpdoom](https://github.com/ilyakurdyukov/fpdoom) app on
Spreadtrum SC6531E feature phones.**

Verified hardware: **itel it2163 (i2163)** — Spreadtrum SC6531E,
ARM926EJ-S @ 208 MHz, ARMv5TE, no FPU (soft-float via libgcc), 4 MB PSRAM,
128x160 RGB565 LCD, launched by `sdboot`/`fpmenu` from a FAT32 microSD card.

What that means in practice: drop `fpj2me.bin` next to your Doom binaries,
drop 128x160 J2ME game jars into `games/fpj2me/`, add one line to
`fpbin/config.txt` — and Java games run on the phone, with a native
128x160 portrait screen, an upright D-pad, and *exactly* the same LCD
refresh flow stock fpdoom uses (that last part is deliberate and
hard-won; see [Screen handling](#screen-handling) for the full story).

Assassin's Creed (Gameloft, 128x160 build) boots end-to-end: splash,
loading, sound prompt, difficulty menu, story, gameplay, RMS saves.

---

## Table of contents

1. [What is inside](#what-is-inside)
2. [Status at a glance](#status-at-a-glance)
3. [Requirements](#requirements)
4. [Quick start (5 minutes)](#quick-start)
5. [Integrating with fpdoom, step by step](#integrating-with-fpdoom)
6. [Building fpj2me.bin — full reference](#building-reference)
7. [The fpdoom app contract (for maintainers)](#fpdoom-app-contract)
8. [Running Java games](#running-games)
9. [Screen handling — how fpj2me drives the LCD like fpdoom](#screen-handling)
10. [Keypad](#keypad)
11. [VM architecture and memory map](#vm-architecture)
12. [Testing without hardware](#testing)
13. [Troubleshooting (screen-color triage)](#troubleshooting)
14. [MIDP conformance notes](#midp-conformance)
15. [Known limitations](#known-limitations)
16. [Porting to other handsets](#porting)
17. [Milestones](#milestones)
18. [Condensed changelog](#changelog)
19. [External references](#references)
20. [License](#license)

The full engineering log — every bug hunt, root cause, and fix from every
hardware round — is in [`README.txt`](README.txt). This README is the
user/maintainer guide; README.txt is the lab notebook.

---

<a name="what-is-inside"></a>
## 1. What is inside

fpj2me is a complete, self-contained CLDC 1.1 + MIDP 2.0 virtual machine
written in freestanding C99 (no libc — it runs under `-nostdlib` on bare
metal), packaged as an **fpdoom app directory**:

| Piece | What it does |
| --- | --- |
| 202-opcode CLDC interpreter | full JVM bytecode set incl. float/double (soft-float), computed-goto dispatch, exception tables, `<clinit>` |
| Classfile parser | total (every read bounds-checked, every CP index validated), iterative — hierarchy depth cannot overflow the 36 KB app stack |
| Mark & sweep GC | 1 MiB Java heap, 32k handles, iterative marking (1024-entry worklist) |
| Native registry | 250+ methods: `java.lang`, `java.util`, `java.io`, `javax.microedition.{lcdui,game,rms,media,midlet}`, `com.nokia.mid` (Sound, DeviceControl, DirectGraphics) |
| JAR reader | ZIP central-directory walker + tinfl inflate (vendored miniz 3.0.2) |
| Image pipeline | stb_image 2.34 PNG decoder -> RGB565 blitter with all 8 `TRANS_*` constants |
| RMS | RecordStore on SD (log-structured flat files, per-jar suites) |
| Launcher menu | on-phone jar picker (`fp_menu.c`): scans `games/fpj2me/*.jar`, lists, scrolls, launches, returns |
| Portrait LCD scanout | native 128x160 portrait panel reprogramming + stock-fpdoom refresh flow |
| Host test rig | 172 checks, runs on a PC with plain gcc — no phone, no ARM toolchain |

```
~11,300 lines of C + vendored libs, zero fpdoom files modified.
```

---

<a name="status-at-a-glance"></a>
## 2. Status at a glance

- **Works on the phone** (itel it2163, SC6531E): upright native-portrait
  128x160 scanout, split-screen black bar gone, toroidal image wrap gone,
  D-pad upright (physical UP = Canvas UP).
- **172/172 host checks pass** (170 unit + 2 boot-in-36KB-stack-thread
  checks, `make check`).
- **Refresh flow = stock fpdoom**: the GRAM window is programmed once at
  boot; every frame is `LCDC end-of-frame wait` + `sys_start_refresh()`.
  No per-frame panel register access (this is what finally killed the
  image wrap — see [Screen handling](#screen-handling)).
- **AC1 end-to-end**: manifest -> class loading -> splash -> menu ->
  gameplay, RMS saves written, no missing natives, no panics.
- The interpreter sustains ~4.7x the original bytecode rate on the hot
  path (per-class constant-pool resolution cache).

---

<a name="requirements"></a>
## 3. Requirements

**To build the target binary (`fpj2me.bin`)**

- a checkout of [fpdoom](https://github.com/ilyakurdyukov/fpdoom) (fpj2me
  is one of its app directories — it includes fpdoom's
  `buildinc.make`, so it *must* live inside the fpdoom tree to build)
- an ARM cross compiler: any `arm-*-gcc` that can emit ARMv5 code
  (`arm-buildroot-linux-gnueabi-`, `arm-linux-gnueabi-`,
  `arm-none-eabi-` all work — the link is `-nostdlib`, so "linux" vs
  "none" flavors barely matter). Needs `gcc`, `objcopy`, `nm`.
- make, standard coreutils

**To build/run the host checks (`make check`)**

- gcc (or clang), python3, pthreads — no phone, no ARM toolchain

**To run on the phone**

- an sdboot-capable SC6531E handset with fpmenu (the fpdoom boot chain),
  a FAT32 microSD card (no exFAT), and 128x160 J2ME game jars
  (commercial jars ship *preverified* already — no preverify step needed)

---

<a name="quick-start"></a>
## 4. Quick start (5 minutes)

```sh
# 1. get fpdoom
git clone https://github.com/ilyakurdyukov/fpdoom
# 2. drop fpj2me in as an app directory
cp -r fpj2me /path/to/fpdoom/
# 3. build the target binary
cd /path/to/fpdoom/fpj2me
make LIBC_SDIO=3 CHIP=1 TOOLCHAIN=arm-buildroot-linux-gnueabi
# 4. sanity-check the runtime on your PC (no hardware needed)
make check
```

Copy `fpj2me.bin` to the SD card and add the menu line — full details in
the next two sections.

---

<a name="integrating-with-fpdoom"></a>
## 5. Integrating with fpdoom, step by step

### 5.1 What "an fpdoom app" means

fpdoom is a small bare-metal framework for SC6531E phones. Its boot chain
on these handsets is:

```
sdboot (ROM loader, FAT32 only)
  -> fpbin/fpmain.bin  (fpmenu, the launcher)
       reads fpbin/config.txt, shows the menu
  -> launches an app binary (fpbin/<app>.bin) with the menu line's args
       fpdoom startup (entry.c) parses its own options (--dir, --bright,
       --charge, --rotate, ...), points the FAT current directory at the
       game folder, then calls the app's main()
```

Everything an app sees — LCD, keypad, SD file I/O, timers — comes from
fpdoom's system objects (`syscode.h`), which are **statically linked into
the app binary**. Each app re-initializes the whole machine in its own
`sys_init()`, so one app cannot pollute another (fpj2me's portrait LCD
state dies with the process; fpmenu/Doom are unaffected).

fpj2me follows this contract exactly: it is a directory with a Makefile
that includes `../buildinc.make`, and **it modifies zero fpdoom files**.
All fpdoom interaction happens through documented hooks and primitives,
replicated 1:1 from fpdoom's `syscode.c` / `lcd_spi.h` into
`src/fp_platform.c` where the tree needed register-level recipes.

### 5.2 Step 1 — get fpdoom

```sh
git clone https://github.com/ilyakurdyukov/fpdoom
```

### 5.3 Step 2 — copy this directory in

```sh
cp -r fpj2me /path/to/fpdoom/
```

The resulting layout (same shape as fpdoom's own app dirs like retris):

```
fpdoom/
  buildinc.make           <- fpdoom's shared app build rules (included)
  fpj2me/
    Makefile              <- APP_SRCS + the two overrides (see 6.2)
    src/ vendor/ tests/ tools/ sdcard/ README.md README.txt
```

### 5.4 Step 3 — build the target binary

```sh
cd /path/to/fpdoom/fpj2me
make LIBC_SDIO=3 CHIP=1 TOOLCHAIN=arm-buildroot-linux-gnueabi
```

| Variable | Meaning |
| --- | --- |
| `LIBC_SDIO=3` | **Mandatory for sdboot phones**: file I/O goes through the SD/FAT32 driver. `LIBC_SDIO=0` builds a debug variant that talks to fpdoom's `libc_server` over USB instead (see 5.8). |
| `CHIP=1` | SoC select — `1` = SC6531E (itel it2163 family). |
| `TOOLCHAIN=` | The `arm-*` prefix of your cross compiler. Either put its `bin/` on `PATH` or give a full prefix path (`TOOLCHAIN=/opt/arm/bin/arm-buildroot-linux-gnueabi`). |

Output: **`fpj2me.bin`** — a two-stage fpdoom image (part2 stub loads and
relocates part1; soft-float `libgcc` linked in). Any ARMv5 toolchain
works because the link is `-nostdlib`; the Makefile already handles the
two classic link pitfalls (see [6.3](#toolchain-gotchas)).

### 5.5 Step 4 — SD card layout

Same pattern as every other fpdoom game (binaries in `fpbin/`, data in
`games/<name>/`):

```
<SD root>/
  fpbin/
    fpj2me.bin
  games/
    fpj2me/
      ac1j2me.jar          <- as many jars as you like (128x160 builds!)
      game.jar             <- the M1 smoke midlet (sdcard/ in this tree)
      rms/                 <- OPTIONAL: per-jar RecordStore saves.
                             Not required: the fpdoom libc has no mkdir,
                             so the runtime falls back to flat
                             rms_<jar>_<store>.rms files in the same folder
```

No `.jad` files are required — the MIDlet entry class is read from each
jar's own `META-INF/MANIFEST.MF` (`MIDlet-1`). An optional sibling
`<jar-name>.jad` is loaded if present and **overrides** the manifest
(MIDP-2.0 semantics; Gameloft jars often need their JAD for IGP config).

All paths are relative: fpmenu's `--dir` makes fpdoom's startup point the
FAT current directory at the game folder — exactly how `doom1.wad` is
found.

### 5.6 Step 5 — the fpmenu config.txt

Add to `fpbin/config.txt` (a fully commented ready-to-use file ships in
[`sdcard/config.txt`](sdcard/config.txt)):

```
$fpj2me = fpbin/fpj2me.bin $@
...
|Java Games| $fpj2me --dir games/fpj2me
```

**THE $@ RULE (verified against fpmenu's readconf.h) — read this before
editing config.txt:**

- The **first non-empty, non-comment line** of `config.txt` is *not* a
  variable definition. fpmenu stores it as the `$@` variable — the
  argument list prepended to **every** app it launches. Keep it as plain
  options:

  ```
  --bright 50 --charge 2
  ```

- Variables are defined **below** that first line. If your first line
  reads `$fpj2me = fpbin/fpj2me.bin $@` instead, fpmenu parses that text
  as `$@`'s *value*, `$fpj2me` never becomes a defined variable, and the
  menu entry **silently stops working** (fpmenu looks for a file
  literally named `$fpj2me` and finds nothing). This is the #1 config
  trap.
- fpdoom's stock line `$@ = $@ --rotate 3` (shared landscape LCD config
  for fpmenu/Doom) **no longer needs to be removed**: fpj2me reprograms
  the panel to native portrait at boot regardless of the rotate setting
  it inherits, and clears the keypad-rotation bits so the D-pad is
  upright. Keeping or removing the line are both fine.

**App-private flags and the `--` separator:**

`entry.c` consumes its own option tokens (`--bright`, `--charge`,
`--dir`, ...) and **exits on unknown `-` options** — so `--` is the only
channel for app-private arguments. Everything after `--` reaches
fpj2me's `main()`:

```
|Java Games| $fpj2me --dir games/fpj2me -- --gram-repin
```

The one flag today:

| Flag | Meaning |
| --- | --- |
| `--gram-repin` (also `--gram-repin=1`, `gram_repin=1`) | Opt-in (default **off**): re-enable the per-frame CASET/RASET/RAMWR window re-pin. Only for a parallel-LCM GC9106-class panel whose page counter genuinely drifts. The default flow — no per-frame panel commands, exactly like stock fpdoom — is correct for the it2163's SPI panel. See [9.4](#gram-repin). |

### 5.7 Step 6 — first launch / M1 smoke test

The `sdcard/` folder of this tree is a ready-made `games/fpj2me/`. Copy
its contents (`game.jar`, `game.jad`) to the card and launch the menu
entry. The visible cycle on the phone:

1. **BLUE** "scanning" flash — the app is alive, enumerating the SD folder.
2. The **jar menu** (black, white text, teal selection bar) — navigate
   UP/DOWN, launch with OK.
3. **GREEN** "loading" flash — the jar is opened, manifest parsed,
   classes loaded.
4. Black screen with a **GREEN box**, "fpj2me M1" + "press a key", white
   1px border — the smoke midlet's `paint()` ran through the interpreter.
5. Press any key — box turns **RED** (keypad -> `keyPressed` -> repaint).
6. Press RSOFT — the midlet calls `notifyDestroyed()`, the **menu
   returns** (the same exit path real Gameloft games use).
7. Exiting the menu itself reboots back to fpmenu (standard fpdoom app
   exit via watchdog).

### 5.8 Debug builds (USB logging)

With `LIBC_SDIO=3` the USB log channel is off (the SD driver owns the
link). To see `fp_dbg()` logs on a PC:

```sh
make LIBC_SDIO=0 CHIP=1 TOOLCHAIN=<prefix>   # rebuild
```

and run under fpdoom's `libc_server` over USB. Panics always show a red
screen regardless of build; boot telemetry is painted on the LCD (see
[Troubleshooting](#troubleshooting)).

---

<a name="building-reference"></a>
## 6. Building fpj2me.bin — full reference

### 6.1 The two build modes

```sh
# TARGET binary (what you copy to fpbin/)
make LIBC_SDIO=3 CHIP=1 TOOLCHAIN=arm-<prefix>

# HOST checks (PC, no hardware, no ARM toolchain)
make check          # = tests/run.sh
```

`make check` needs gcc + python3 only. It regenerates the test jars,
compiles the **whole runtime** natively against stubbed `fp_*` HAL
primitives, and runs 172 checks (see [Testing](#testing)).

### 6.2 What the app Makefile does

```make
APP_SRCS = src/target_main src/fp_platform src/fp_menu src/fp_font \
           src/jar src/heap src/interp src/natives \
           src/midp_image src/midp_graphics src/midp_input \
           src/midp_rms src/midp_media \
           vendor/miniz vendor/stb_impl
include ../buildinc.make
APP_CFLAGS += -std=c99 -I$(SYSDIR) -Isrc -Ivendor
APP_CFLAGS += -DFP_LCD_W=128 -DFP_LCD_H=160
EXTRA_CFLAGS += -fno-stack-protector
```

Plus two deliberate overrides of fpdoom's part-link rules and a miniz
define — each exists to fix a real toolchain failure:

| Override | Why |
| --- | --- |
| `-lgcc` linked **after** the objects, wrapped in `--start-group ... --end-group`, in **both** part rules | CLDC 1.1 float/double opcodes need the ARM soft-float helpers (`__aeabi_dadd`, `__aeabi_fmul`, ...). fpdoom links `-nostdlib` and its stock rule puts nothing after the objects, where an archive member would never be pulled in. |
| `-fno-stack-protector` on every TU | Buildroot GCC defaults to `-fstack-protector-strong` -> undefined `__stack_chk_guard`/`__stack_chk_fail` under `-nostdlib`. |
| `MINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS` on `miniz.o` | tinfl-only build: no stdio, no time, no zip archive APIs (jar.c walks the central directory itself). |

### <a name="toolchain-gotchas"></a>6.3 Toolchain gotchas (all already fixed in-tree — for when you re-vendor or reuse the patterns)

1. **`__aeabi_*` undefined at link**: your build is not using this
   Makefile's part rules (stale objects or an old tree copy).
   `make clean` and rebuild.
2. **`__stack_chk_*` undefined**: an old tree without
   `-fno-stack-protector`. Rebuild from this tree.
3. **Newlib bare-metal toolchains (`arm-none-eabi`, GCC 16+) default to
   emulated TLS**; stb_image's `thread_local` globals then pull libgcc's
   `emutls.o` -> undefined `malloc`/`abort`. The tree defines
   `STBI_NO_THREAD_LOCALS` in **both** stb-using TUs
   (`vendor/stb_impl.c` and `src/midp_image.c`) — single-threaded by
   design, so plain statics are correct. If you re-vendor stb, keep that
   define.

### 6.4 Anatomy of the output image

`fpj2me.bin` is a two-stage fpdoom image: a small part2 stub that loads
part1 and applies relocations, with libgcc soft-float members linked
after the app objects. It is a plain loadable file for fpmenu — nothing
gets reflashed; the phone's boot ROM chain stays stock.

Approximate binary budget on the it2163:

| Region | Size |
| --- | --- |
| fpdoom code+data | ~700 KB |
| fpj2me .text/.rodata (VM + bridge + tinfl + PNG decoder) | ~220 KB |
| Total PSRAM use at runtime (with all arenas, see [11](#vm-architecture)) | ~2.7 MB of 4 MB |

---

<a name="fpdoom-app-contract"></a>
## 7. The fpdoom app contract (for maintainers)

Which files touch fpdoom, and how:

| File | fpdoom contact |
| --- | --- |
| `src/platform.h` | the HAL contract — everything fpj2me needs from fpdoom, as plain C functions. Point each at your fpdoom primitives or wrap them in `fp_platform.c`. Everything else in fpj2me is HAL-agnostic and unit-testable on a PC. |
| `src/fp_platform.c` | the ONLY register-level implementation: LCM/SPI recipes replicated 1:1 from fpdoom `syscode.c`/`lcd_spi.h`, the portrait switch, the refresh flow, the keymap, file I/O, time, debug. Includes `syscode.h`. |
| `src/target_main.c` | the app `main()` + the `lcd_appinit()` hook. Includes `syscode.h`. |
| everything else | pure VM/bridge code; compiles on a PC against the stubbed HAL in `tests/hostcheck.c`. |

**HAL surface** (from `platform.h`): `fp_screen` (the RGB565 framebuffer
— Doom's front buffer reused), `fp_lcd_flush`/`fp_lcd_wait`,
`fp_time_ms`/`fp_sleep_ms`, `fp_dbg`, `fp_rawmap`/`fp_key_event`/
`fp_keys_held`, `fp_fopen`/`fp_fread`/`fp_fwrite`/`fp_fseek`/
`fp_ftell`/`fp_fclose`, `FPJ_SD_ROOT`.

**Hooks fpj2me provides into fpdoom's boot**: `lcd_appinit()` is called
by fpdoom's `sys_init()` *between* `lcd_init()` and `lcdc_init()` — the
app's one chance to size the framebuffer window (fpdoom never sets
`w2/h2` itself; a 0x0 window scans out nothing = black screen with
backlight) and, for fpj2me, to reprogram the panel while the bus is
still in 8-bit command mode.

**Weak telemetry hooks**: `fpj_stage(msg)` and `fpj_stage_class(name)`
are weak no-ops in `interp.c`; the target (`target_main.c`) overrides
them to paint boot milestones on the green loading screen, and the host
rig (`tests/boot_jar.c`) overrides them to print `[stage]`/`[class]`
lines. Both are gated on `fpj_telemetry`, which is turned off at
`startApp` entry so telemetry can never paint over a running game.

---

<a name="running-games"></a>
## 8. Running Java games

### 8.1 What kind of jars work

- **CLDC 1.1 / MIDP 2.0** midlets (JAR + internal `MANIFEST.MF`).
- **128x160 builds** for the 1:1 portrait path. Other resolutions
  letterbox (see [9.6](#non-128x160)).
- Size limit: **~2 MB** per jar (larger fails with boot error E-1).
- Commercial jars ship **preverified** — no preverify step. For your own
  test MIDlets, preverify with Oracle WTK 2.5.2.
- Audio is silent by design (no audio HW bring-up); the Nokia Sound /
  `javax.microedition.media` surfaces are modeled so games *run*.
- Nokia UI (`com.nokia.mid.ui` FullCanvas / DirectGraphics /
  DeviceControl) is modeled — Gameloft GLLib games depend on it.

### 8.2 The game loop contract (why Gameloft games work)

Gameloft midlets run their whole frame loop **inside `startApp()`** —
blocking `repaint(); serviceRepaints(); Thread.sleep()`. fpj2me handles
this: `serviceRepaints()` delivers `paint()` synchronously, sleeping
natives pump input + repaints in 10 ms slices, a cooperative
`Thread.start()` runs `Runnable.run()` inline, and the interpreter polls
input + delivers repaints every 2^18 dispatches so even a pure busy-poll
loop stays live. `notifyDestroyed()` fully resets the VM and returns to
the jar menu.

### 8.3 Save data (RMS)

Per-jar RecordStore suites under `games/fpj2me/rms/<jar-name>/` when the
directory exists, otherwise flat `rms_<jar>_<store>.rms` files next to
the jar (the fpdoom libc has no `mkdir` — the fallback is automatic).
Saves are flushed when the midlet exits.

### 8.4 JAD files (optional but recommended for Gameloft)

`<jar-name>.jad` sitting next to the jar is loaded into RAM at boot and
takes priority over the manifest (MIDP-2.0). The loader is RFC-822-aware:
case-insensitive keys, CRLF/LF endings, folded continuation lines. On
device this costs zero extra SD I/O after boot. Without a JAD, the
runtime supplies non-null fallbacks for the Gameloft IGP/URL property
keys so GLLib's unconditional `trim()/toUpperCase()` cannot NPE-loop
the splash (the "350-second invisible menu" bug class).

---

<a name="screen-handling"></a>
## 9. Screen handling — how fpj2me drives the LCD like fpdoom

This is the hardest-won part of the tree. The short version:

> **The panel is programmed ONCE at boot (native 128x160 portrait);
> every frame is just "wait for LCDC end-of-capture, then
> `sys_start_refresh()`" — no per-frame panel register access, exactly
> like stock fpdoom.**

### 9.1 The fpdoom LCD pipeline and the app's slot in it

```
sys_init()
  -> lcd_init()      panel table + config.txt options applied
                     (MADCTL possibly XORed by --rotate; w1/h1 set)
  -> lcd_appinit()   APP HOOK (fpj2me: the portrait switch lives here)
                     fpdoom NEVER sets w2/h2 — the app must size its
                     framebuffer window; 0x0 = black screen w/ backlight
  -> lcdc_init()     programs the LCDC DMA from w2/h2 (img.size_xy,
                     pitch), masks LCDC_BASE->irq.en bit 0 after the
                     boot refresh
```

The problem fpj2me inherits: fpdoom boots the panel through its shared
landscape configuration — most commonly fpmenu's stock
`$@ = $@ --rotate 3` (XORs MADCTL with `rtab[3]` = 0x60, sets MV, swaps
w/h) — leaving the panel scanning **160x128 landscape**. Pushing a
128x160 portrait framebuffer (pitch 128) into that grid shears every row
by 32 pixels, wraps the image around a wandering black bar, and renders
the whole picture sideways.

### 9.2 The portrait switch (once, at boot)

`src/fpj_lcd.h` is the pure decision half — `fpj_lcd_plan()`, a
host-unit-tested pure function:

- Input: `sys_data.mac` (MADCTL as actually programmed), `rotate`,
  `w1/h1`.
- Undo fpdoom's rotate XOR (`rtab = {0, 0xa0, 0xc0, 0x60}`) to recover
  the panel's **native portrait MADCTL** — the orientation the OEM
  firmware runs its own portrait UI in (RGB-order bit and mirror
  polarity preserved verbatim; landscape-native tables get MV cleared +
  MY/MX swapped).
- Emit **CASET (0x2A) = 0..127, RASET (0x2B) = 0..159** (plus optional
  GRAM offsets, see [9.6](#non-128x160)), then **RAMWR (0x2C)**.
- Patch `sys_data.display` **before `lcdc_init()` reads it**:
  `w1/h1/w2/h2 = 128x160`, so the LCDC DMA window and
  `spi_refresh_next()`'s pixel count are exactly
  **128 x 160 = 20,480 pixels = 40,960 bytes**, row-major, pitch 128.

Window == framebuffer == DMA == GRAM area: the write pointer wraps to
(0,0) at every frame end — the invariant fpdoom's own full-screen apps
maintain.

The register half is `fpj_lcd_apply_portrait()` (`fp_platform.c`),
called from `lcd_appinit()` — between `lcd_init()` and `lcdc_init()`,
while the bus is still in 8-bit command mode, exactly the slot where
fpdoom's own `lcd_init()` programs its `cmd_init2`. It sends the command
burst on both buses:

- **SPI**: the `lcd_spi.h` byte recipe (CD bit, channel-length
  save/restore).
- **parallel DBI (LCM)**: `lcm_set_mode`/`lcm_send` through the
  `0x60000000 | cs<<26` FIFO window.

If the plan does not apply (mono flag, `--mac` override, or a panel
whose portrait geometry is not exactly 128x160), the app falls back to
the letterbox clamp (see [9.6](#non-128x160)).

### 9.3 The per-frame flow — stock fpdoom, plus one real gate

```c
fp_lcd_flush()  ==  fpj_diag_overlay()        /* stamp the diag strip   */
                   fpj_lcdc_wait_frame()      /* REAL end-of-frame gate */
                   fpj_lcd_gram_reset()       /* default: NO-OP        */
                   sys_start_refresh()        /* 20480-pixel DMA        */
```

- `sys_start_refresh()` is fpdoom's per-frame refresh:
  `clean_dcache()` + `spi_refresh_next()` + LCDC start. The 20,480-word
  stream exactly fills the 128x160 GRAM window, so the panel's write
  pointer wraps modulo 20,480 back to (0,0) **on its own** — fpdoom
  itself resets nothing per frame on SPI panels (`lcm_init()` forces
  `page_reset = 0`; only LCM GC9106/09 get a bare 0x2C).
- **`fpj_lcdc_wait_frame()` is the one addition**, and it is needed
  because of an fpdoom quirk: `lcdc_init()` masks `LCDC_BASE->irq.en`
  bit 0, which makes `sys_wait_refresh()` a no-op — a new refresh could
  be started while the LCDC is mid-capture. The flush instead polls the
  **raw** end-of-capture bit `LCDC_BASE->irq.raw & 1` (raw latches
  regardless of the enable mask; `sys_start_refresh()` re-sets it every
  frame so every submitted frame latches it), clears it via
  `LCDC_BASE->irq.clr |= 1`, bounded by ~50 ms (>10x the worst-case
  20,480-word shift). An armed flag tracks "submitted, not yet retired"
  so the first flush never stalls. Result: the LCDC is never re-armed
  mid-capture, and `fp_screen` is only redrawn after the DMA finished
  reading it.

### <a name="gram-repin"></a>9.4 Why there is no per-frame re-pin (and what `--gram-repin` is for)

Rounds (4)-(6) of the hardware debugging re-issued CASET/RASET/RAMWR
before **every** DMA submit, with increasingly careful gates:

- (5) gated on SPI TX FIFO real-empty + not-busy -> constant
  `(dx=32, dy=4)` toroidal wrap — exactly **544 words** = 4 rows x 128
  px + 32 px = the LCDC capture FIFO (~512 words) + SPI TX FIFO (~32
  words), i.e. the stranded frame tail clocked out around the freshly
  latched RAMWR as uncounted data.
- (6) gated on the SPI TX_END transfer-completion IRQ (the count
  *closed*) -> the wrap survived, only its phase changed.

That was the proof: **the per-frame register banging itself causes the
pixel-count mismatch.** A command burst toggles `chnl_len` / CD /
`ctl8` / `ctl9` (and parks `HLD_EN` / `RGB565_EN`) on the SPI that the
LCDC capture stream is driving; `sys_wait_refresh()` cannot fence it
(the IRQ is masked), the burst lands mid-stream, and the submit starts a
new capture over the residue. Meanwhile stock fpdoom/fpmenu drive this
exact SPI panel with zero wrap precisely because they never touch the
panel per frame.

Round (7) therefore adopted the stock flow (program once at boot,
submit per frame — section 9.3). The full per-frame re-pin machinery
survives behind the **opt-in `gram_repin` flag** for a parallel-LCM
GC9106-class panel whose page counter genuinely drifts:

```
|Java Games| $fpj2me --dir games/fpj2me -- --gram-repin
```

Opt-in program (pinned by `fpj_lcd_flush_seq()` and host-unit-tested):
SPI `LCDC_WAIT -> TX_END -> BUS_IDLE -> CASET -> RASET -> RAMWR ->
SUBMIT`; LCM drops TX_END. Default program: `LCDC_WAIT -> SUBMIT`, on
both buses. Mono: a single SKIP.

### 9.5 The keypad story (same root, other half)

fpdoom packs two rotations into `sys_data.rotate` (`key << 4 |
screen`), and fpmenu's stock `--rotate 3` leaves `0x33`: screen rotation
(needed by fpmenu's landscape menu) *and* keypad rotation. fpdoom's
`sys_getkeymap()` remaps the D-pad through the KEYPAD bits before the
app sees the codes — a 90-degree-rotated D-pad (physical UP = J2ME
LEFT, ...). CENTER and the soft keys are not in the rotation domain,
which is why they were already correct.

`src/fpj_key.h` (pure, host-unit-tested): `fpj_key_rotate()` clears
**only** the keypad bits (0x33 -> 0x03) when the portrait plan applied;
`keytrn_init()` (`fp_platform.c`) applies it before `sys_getkeymap()`.
The full chain raw -> `num_turn` -> `FPJ_RAWMAP` -> `KP_*` is replicated
from `syscomm.c` so the host check pins it 1:1. Non-portrait fallback
panels keep fpdoom's rotation so keys match the sideways letterbox.

### <a name="non-128x160"></a>9.6 Non-128x160 / mono / offset-glass panels

- Color panels whose portrait geometry is not exactly 128x160 (e.g.
  240x320) **keep fpdoom's letterbox clamp**: `fp_screen` stays a packed
  128x160 buffer, wider/taller panels letterbox — intended for 128x160
  game builds. The menu footer prints the real panel size.
- Monochrome panels / `--mac` overrides (`sys_data.mac & 0x100`) keep
  fpdoom's mono refresh path — the plan returns 0, the flush emits a
  single SKIP.
- 132x162 "tab" glass (some ST7735 modules) letterboxes by 2/1 px in
  portrait: build with
  `-DFPJ_LCD_GRAM_XOFF=2 -DFPJ_LCD_GRAM_YOFF=1`
  (the itel-family tables in fpdoom's `lcd_config.h` are plain 128x160,
  so the default 0/0 is correct for the it2163).

---

<a name="keypad"></a>
## 10. Keypad

The mapping the game sees (MIDP keycodes, de-facto Nokia/SE values all
2007+ Gameloft builds use):

| Physical key | MIDP keycode |
| --- | --- |
| D-pad UP / DOWN / LEFT / RIGHT | -1 / -2 / -3 / -4 |
| OK / CENTER | -5 (FIRE) |
| Left soft key | -6 (SOFT1) |
| Right soft key | -7 (SOFT2) |
| digits 0-9, `*`, `#` | ASCII '0'-'9', '*', '#' |

- `Canvas.getGameAction()` returns the official action codes (UP=1,
  LEFT=2, RIGHT=5, DOWN=6, FIRE=8, GAME_A..D=9..12).
- `GameCanvas.getKeyStates()` bits are `1 << action code`, latched since
  the previous call; digits 2/4/6/8/5 count as the nav pad.
- Raw pad codes are fpdoom's `KEYPAD_ENUM` from `syscode.h`. If a key is
  dead on your handset, adjust the one `fp_rawmap` table in
  `src/fp_platform.c` (dump your raw codes once with `fp_dbg`).

---

<a name="vm-architecture"></a>
## 11. VM architecture and memory map

```
+--------------------------------------------------------------+
| fpdoom system (syscode: LCD, keypad, SD/FAT32, timers)       |
|   statically linked into fpj2me.bin                          |
+--------------------------------------------------------------+
| fpj2me                                                        |
|   target_main.c  app entry, banners, error screens            |
|   fp_menu.c      jar picker          fp_font.c  8x8 text     |
|   fp_platform.c  HAL impl (LCD/keys/IO/time)                  |
|   ------------------- VM core (host-testable) ---------------- |
|   jar.c          ZIP central dir + tinfl (miniz)              |
|   heap.c         objects, handles, mark&sweep GC, meta arena   |
|   interp.c       classfile parser + 202-opcode interpreter     |
|   natives.c      the native registry (250+ methods)           |
|   midp_image.c   PNG->RGB565 (stb_image, STBI_ONLY_PNG)       |
|   midp_graphics.c drawImage/drawRegion (8 TRANS)/clip/fill     |
|   midp_input.c   keypad -> MIDP keycodes, GameCanvas states   |
|   midp_rms.c     RecordStore flat files                       |
|   midp_media.c   silent Player/Manager + listener events      |
+--------------------------------------------------------------+
```

**Interpreter**: full CLDC 1.1 opcode set (202 opcodes, computed-goto
dispatch), exception tables with JVM unwind semantics (NPE, AIOOBE,
ClassCastException, ArithmeticException, NoClassDefFoundError, `athrow`,
catch-all finally handlers), lazy `<clinit>` on first active use
(JVMS 5.5), iterative class loading (super-chain parsed in a flat loop —
stack usage does not depend on hierarchy depth; the app stack is only
36 KB, `sc6531e_fdl.ld EXTRA_SIZE 0x9000`).

**Performance**: per-class constant-pool resolution cache — field ops
park a `JField*` (or MISS sentinels), invokes park a monomorphic
`InvokeSite` inline cache keyed on the receiver's runtime class.
Measured 22.2M -> 103.3M bytecodes/s on host x86 -O1 (4.7x); the
removed `strcmp` walks are relatively heavier on the slow-PSRAM ARM9.

**The on-phone diag strip**: every flush stamps a small overlay row
(gc=, c=, v=, hf=, k=, p=, lp=, `C:` current class, `M:` last
missing-native — full "Class.method", wrapped onto a 6th row). `v` =
bytecodes executed, `p` = paints, `k` = keys, `gc` = collections. It is
rendered *only* on flush, never mid-SD-read.

**Memory map (PSRAM, within fpdoom's allocation):**

| Region | Size |
| --- | --- |
| fpdoom code+data | ~700 KB |
| fpj2me .text/.rodata | ~220 KB |
| fpj_objects (32k handles) | 128 KB |
| fpj_heap (Java GC arena) | 1 MiB |
| fpj_stack (interpreter frame pool) | 64 KB |
| meta arena (class metadata, bump) | 512 KB |
| fp_screen (back buffer 128x160x2) | 40 KB |
| **Total** | **~2.7 MB of 4 MB** — same envelope fpdoom runs Doom in |

---

<a name="testing"></a>
## 12. Testing without hardware

### 12.1 `make check` — the host check suite

`tests/run.sh` regenerates the test jars (`tools/make_test_jar.py`,
`tools/make_deep_jar.py`), compiles the **whole runtime** natively
against stubbed `fp_*` primitives, and runs:

- **170 unit checks**: jar central-dir scan + real tinfl inflate; the
  blitter (all 8 `TRANS_*`, transparency, clip/fill/drawString);
  RecordStore log semantics across reopen; real stb PNG decode to
  RGB565 at 128x160; input semantics (`getGameAction`/`getKeyStates`);
  full manifest -> `MIDlet-1` -> `class_load` -> `startApp` boots;
  branches taken and not-taken (guards the spec-correct branch base);
  the MIDP 2.0 property chain (sibling `.jad` overrides manifest,
  RFC-822 unfolding, IGP/URL fallbacks); the diag strip rules; the
  Nokia Sound surface; the AC1 conformance fixes (setClip-REPLACE,
  opaque black, white mutable images, fillTriangle, getRecord-into);
  the full MIDP -1..-7 key contract; the per-class cp cache identity;
  **the LCD plans** (`fpj_lcd_plan`: the exact it2163 rotate-3 landscape
  state -> native portrait, GRAM offsets, mono/240x320 fallbacks,
  statelessness; `fpj_lcd_flush_seq`: default = LCDC_WAIT+SUBMIT with
  zero per-frame register steps — the (4)/(5)/(6) bug shape pinned as a
  regression guard; opt-in gram_repin program orders); **the keypad
  plans** (`fpj_key_rotate` bit surgery, the rotate-3 bug demo pinned,
  the fixed chain); the Gameloft-shaped blocking-startApp boot; GC.
- **2 boot-stack checks**: the smoke jar and a generated 24-deep
  super-chain jar booted inside a pthread with **exactly the phone's
  36 KB stack** (stack overflow segfaults on the host instead of only
  freezing the phone).

**STATUS: 172/172 PASS.**

### 12.2 `tests/boot_jar` — boot an arbitrary jar on the PC

```sh
./tests/boot_jar /path/to/game.jar [stack_kb]
```

Runs the real `fpj2me_boot()` path with the real clock
(`CLOCK_MONOTONIC` + real sleeping — Gameloft frame loops pace
themselves with `currentTimeMillis`/`sleep`, and a frozen fake clock
stalls them forever). Prints the `[stage]` / `[class]` telemetry the
phone paints on its green screen. Use it to reproduce on-target boot
hangs with real-world jars.

- `[stack_kb]` (or env `FPJ_STACK_KB`): run the boot in a pthread with
  that many KB of stack (pass `36` to emulate the phone).
- env `FPJ_PUMP_N`: how many frame pumps to run after boot (default 3)
  — first-frames paint-path crashes reproduce here too.
- Exit codes: `0` boot ok, `1` boot error (code printed), `124` hang
  (use `timeout`), `139` segfault (wild pointer in the parser).

---

<a name="troubleshooting"></a>
## 13. Troubleshooting (screen-color triage)

**The backlight contract**: fpmenu switches the backlight OFF the
instant it launches any app; only the app's own `sys_start()` switches
it back on — and fpj2me paints its blue banner *before* that. So the
color you see (or don't see) tells you exactly how far execution got:

| What you see | Meaning / what to do |
| --- | --- |
| **BLACK, backlight OFF** (nothing at all) | fpj2me.bin never executed a single instruction. In order of likelihood: (1) the `.bin` on the card is truncated — fpmenu's loader loops forever when the FAT cluster chain yields fewer bytes than the directory entry promises; compare the byte size on the PC with the built file, recopy, eject cleanly, reinsert. (2) wrong path/name — the config line must read `\|Java Games\| fpbin/fpj2me.bin $@ --dir games/fpj2me` with the binary at `fpbin/fpj2me.bin`. (3) a mislinked build — rebuild with this tree's Makefile. (4) card hygiene — FAT32 only (no exFAT), badly fragmented cards can break cluster chains; copy onto freshly FAT32-formatted card in one pass. |
| **Phone reboots straight back to fpmenu** | the app ran and failed *before* `sys_init` (e.g. `--dir` path not found — case- and spelling-exact) and the watchdog rebooted it: check the config line. |
| **BLACK, backlight ON** | fpj2me runs but the LCDC scans out nothing. This tree always assigns `w2/h2` — if you see it, you built from an older scaffold; rebuild. |
| **BLUE + "scanning"** | alive, enumerating the SD folder. Stuck here = SD/FAT problem (rebuild with `LIBC_SDIO=0` for USB logs). |
| **MENU** (black, white text, teal bar) | everything through FAT works. Navigate UP/DOWN, launch with OK. |
| **RED + "no .jar files"** | the app runs fine but the game folder is empty or wrong (config `$@` rule broken so `--dir` never reached us and the scan saw the SD root). The screen lists what to check; press a key -> back to fpmenu. |
| **GREEN + "loading"** | jar being opened, manifest parsed, classes loaded, `startApp` entered. **Live boot telemetry**: row 9 = yellow milestone (heap / open jar / fopen ok / eocd / cdir / manifest / entry hdr / inflate / entry ok / class / instance / ctor / startApp / run), row 11 = white class-name line, row 13 = the timer cross-check ("timer ok" or "timer NNNms!"). **If the screen freezes, the last visible text IS the diagnostic** — report it. Note: Gameloft midlets block inside `startApp` (their frame loop runs there); the yellow line may stay at "startApp" while the game is already painting. Stuck with no text and no paint = hang inside `fpj2me_boot` (SD I/O or classfile) — `LIBC_SDIO=0` time. |
| **RED + "fpj2me panic"** | only unrecoverable states (stack/call-depth overflow, OOM while throwing). NPE/cast/array-OOB/div0/athrow/ClassNotFound are catchable Java exceptions now — the game's own catch blocks handle them. |
| **RED + "boot failed" + E-code** | jar did not start. E-1 open failed (bad zip / >2 MB), E-2 no `MIDlet-1` in manifest, E-3 MIDlet class missing, E-4 out of memory, E-5 no `startApp` method, E-6 an exception escaped `<init>`/`startApp` (row 4 names the class). Press a key -> back to the menu. |
| **Game screen** | running. Exit via the game's own exit (`notifyDestroyed`) -> menu returns. |
| **Black bars top/bottom** | panel taller than 160: the 128x160 window letterboxes (intended for 128x160 game builds). The menu footer prints the real panel size. |
| **Image wraps toroidally** (text split at one edge, tail at the opposite edge, e.g. "MISSION FAILE\|D") | the pixel stream reached the panel phase-shifted. This tree refreshes exactly like stock fpdoom (no per-frame commands), which eliminates it. If it *ever* reappears after a rebuild, the flush program regressed — check the `fpj_lcd_flush_seq` host units. (If you are on a parallel-LCM GC9106-class panel with a genuinely drifting page counter, opt in with `-- --gram-repin`.) |
| **D-pad directions rotated** | the KEYPAD bits of `--rotate` are not being cleared — you are on a fallback (non-portrait) panel, or an old build. See [9.5](#screen-handling). |
| **Link error `__aeabi_*` undefined** | build not using this Makefile's part rules (stale objects). `make clean`, rebuild. |
| **Link error `__stack_chk_*` / emutls `malloc` undefined** | old tree / re-vendored stb without `STBI_NO_THREAD_LOCALS`. See [6.3](#toolchain-gotchas). |

---

<a name="midp-conformance"></a>
## 14. MIDP conformance notes

Verified against the JSR-118 javadoc (details in `README.txt`):

- `Sprite.TRANS_*` values 0..7 match the spec's constant-values page
  exactly (`TRANS_ROT180=3`, `TRANS_MIRROR_ROT270=4`, `TRANS_ROT90=5`,
  `TRANS_ROT270=6`, `TRANS_MIRROR_ROT90=7`). Games inline these.
- `Canvas.getGameAction()` returns the official action codes — not the
  negative device keycodes.
- `GameCanvas.getKeyStates()` bits are `1 << action code`, latched.
- `setClip()` **REPLACES** the clip (JSR-118); `clipRect()` intersects.
  (GLLib's glyph engine draws every glyph as `setClip(cell)` +
  `drawImage` — the intersecting bug rendered only the first glyph of
  every string.)
- Mutable images start **WHITE** per the spec; `setColor(0)` +
  `fillRect` produces opaque black, not the transparency color-key.
- MIDlet property chain: sibling `.jad` overrides the jar manifest,
  RFC-822 continuation lines, case-insensitive keys; unknown keys
  return null (standard "unsupported feature" semantics).
- `System.getProperty` spoofs platform `Nokia6300` / locale `en-US` (+
  profiles/config/encoding/screen size/jtwi/m3g/wma) — what GLLib
  expects.
- Nokia `DirectGraphics` constants (FLIP_H 0x2000, FLIP_V 0x4000,
  ROT90/180/270) map onto the same blitter via `DirectUtils`.
- Nokia Sound: `getState()` returns 1 (`SOUND_STOPPED`), never 0
  (`SOUND_PLAYING`) — 0 made GLLib's splash wait out a full audio
  timeout per transition.

---

<a name="known-limitations"></a>
## 15. Known limitations (the M2 list)

- **Media is silent** by design (no audio HW bring-up).
- `java/lang/String` has no classfile: strings ARE char-array objects;
  `new String(...)` is not supported (literals + `valueOf` are). To be
  fixed properly by the M2 boot.jar (real `java.*` classfiles).
- Exceptions are single-threaded: no `InterruptedException` on
  `Thread.sleep`, no exception across the cooperative thread boundary.
- `Thread.start()` is cooperative/inline: no preemption; no second
  `start()` while one `run()` is live. `Timer`/`TimerTask` are no-ops.
- Missing library natives (anything not in the registry) degrade to
  logged no-ops with zeroed returns — boot continues; implement the ones
  your game needs (the "native missing" diag line lists them).
- `RecordStore.enumerateRecords` returns null.
- GLLib IGP splash: jars without their JAD lack the IGP config
  properties; the fallbacks un-stick GLLib, but the full IGP content
  path needs the sibling `.jad` dropped next to the jar.

---

<a name="porting"></a>
## 16. Porting to other handsets

- **Other screen sizes**: build with `-DFP_LCD_W=<w> -DFP_LCD_H=<h>` and
  use jars built for that resolution. Note the 1:1 portrait reprogram
  path currently keys on exactly 128x160 panels — other sizes take the
  letterbox fallback by design.
- **132x162 offset glass**: `-DFPJ_LCD_GRAM_XOFF=2
  -DFPJ_LCD_GRAM_YOFF=1` (see [9.6](#non-128x160)).
- **A dead key**: adjust the `fp_rawmap` table in `src/fp_platform.c`
  (raw codes are fpdoom's `KEYPAD_ENUM`; dump yours once with `fp_dbg`).
- **Another SC6531E phone** (same SoC, different panel/keypad): usually
  just the keymap + GRAM offsets. fpdoom's `lcd_config.h` panel tables
  drive detection.
- **A different SoC entirely**: `fp_platform.c`'s register recipes are
  replicated from SC6531E fpdoom (`syscode.c`/`lcd_spi.h`). Audit them
  against your SoC's fpdoom sources, and pick the right `CHIP=` for the
  fpdoom part of the build. Everything above `platform.h` is
  HAL-agnostic by construction.

---

<a name="milestones"></a>
## 17. Milestones

| | |
| --- | --- |
| M0 | DONE — arena, libc shims, host build green; on-target PNG + blit. |
| M1 | DONE — parser, 202-opcode interpreter, GC, native registry, MIDlet bootstrap; HelloWorld-class runs on host. |
| M1.5 | DONE — launcher jar menu, manifest boot, per-suite RMS, midlet-exit -> VM reset -> menu return, error screens. |
| M2 | NEXT — exceptions across threads, cooperative scheduler, boot.jar (real `java.lang`/`java.util`), `RecordEnumeration`, first Gameloft menu boot. |
| M3 | boot Assassin's Creed (128x160) end-to-end. (Reached in practice during hardware bring-up.) |
| M4 | perf: dirty-rect flush, ARM asm blit, GC tuning. |

---

<a name="changelog"></a>
## 18. Condensed changelog

Full detail, root causes, and verification logs for every entry: see
`README.txt`. Most recent first:

- **(7) 2026-11-25 — screen handled EXACTLY like stock fpdoom.** After
  (4)-(6) kept phase-shifting a toroidal wrap, the per-frame
  CASET/RASET/RAMWR re-pin was proven to *be* the bug (command bursts
  land mid-capture on the SPI the LCDC stream drives). New flow: window
  installed once at boot, per-frame = `fpj_lcdc_wait_frame()` (real raw
  end-of-capture poll, since `sys_wait_refresh()` is a no-op behind the
  masked IRQ) + `sys_start_refresh()`. Old re-pin kept as opt-in
  `--gram-repin`.
- **(6) 2026-11-25 — constant 544-word wrap** (= 4x128+32: LCDC capture
  FIFO + SPI TX FIFO) root-caused to the FIFO-idle gate passing while
  the frame's word count was still open; TX_END count-closed gate added
  (later superseded by (7)'s removal of per-frame commands).
- **(5) 2026-11-25 — toroidal wrap + landscape D-pad fixed**: hardware
  bus-drain gates before command bursts; `fpj_key.h` clears the KEYPAD
  rotation bits of `--rotate` (physical UP = Canvas UP).
- **(4) 2026-11-25 — LCD scanout fixed**: native 128x160 portrait
  reprogramming (MADCTL/CASET/RASET/RAMWR) at `lcd_appinit`, sys_data
  patched to 128x160 so DMA == GRAM window.
- **(3) 2026-11-25 — AC1 end-to-end**: the "350-second blink" was an
  invisible menu — four JSR-118 conformance bugs (setClip semantics,
  opaque black, white mutable images, int-width keycodes) +
  `Thread.yield` pumping + last AC1 natives.
- **(2) 2026-11-25 — splash slow-motion fixed** (0.47 FPS -> healthy):
  Nokia `Sound.getState` returning PLAYING, per-class CP resolution
  cache (4.7x), diag overlay no longer double-flushing.
- **2026-11-25 — JAD + IGP property fallbacks**: the post-splash NPE
  loop (`getAppProperty` returning null for IGP/URL keys) killed.
- **2026-11-24 — stack overflow, `<clinit>`, lcmp, modified-UTF-8**,
  catchable exceptions, cooperative threads, liveness pump, GLLib
  hardening, iterative parser, total bounds checking, RMS, panic
  screens... (see README.txt).

---

<a name="references"></a>
## 19. External references

| | |
| --- | --- |
| fpdoom | https://github.com/ilyakurdyukov/fpdoom (Unlicense) — the framework this runs on |
| sdboot | fpdoom's `sdboot/` — FAT32 only, `LIBC_SDIO=3`, `CHIP=1` |
| miniz | https://github.com/richgel999/miniz — 3.0.2, tinfl only (MIT) |
| stb_image | https://github.com/nothings/stb — 2.34, `STBI_ONLY_PNG` (MIT/PD) |
| JSR-118 | docs.oracle.com/javame/config/cldc/ref-impl/midp2.0/jsr118 — the MIDP 2.0 spec |
| preverify | commercial jars ship preverified; for your own test MIDlets use Oracle WTK 2.5.2 |

Vendored third-party code keeps its own licenses: `vendor/miniz.LICENSE`,
`vendor/stb.LICENSE`, `vendor/font8x8.h` (public domain).

---

<a name="license"></a>
## 20. License

Your call — suggested **MIT** for the fpj2me code in this tree. Vendored
files keep their own licenses (miniz MIT, stb MIT/PD, font8x8 PD). No
fpdoom files are modified, so fpdoom's Unlicense applies only to the
unmodified framework you build against.
