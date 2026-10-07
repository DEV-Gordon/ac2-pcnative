# ac2native — Ace Combat 2 (PS1) as a native PC program

A native Linux port of **Ace Combat 2** (PlayStation, NTSC-U) built by **static recompilation**: the game's MIPS code
is translated to C and compiled together with a native runtime that replaces the console's hardware. There is **no
emulator** and no BIOS file needed — the program reads *your own* disc image and runs the game as ordinary PC code.

> **Status: playable, not finished (~80 %).** Intro, menus, briefing, hangar, missions, movies, music, saving and
> loading all work. See [Status](#status) for what is still open.

## Legal — please read

- This repository contains **no game data and no code taken from the game**: no disc image, no BIOS, no
  executables, no recompiled code.
- You need **your own copy** of *Ace Combat 2 (USA)* (serial SLUS-00404). The build script extracts the game's
  executables from your disc on your machine, translates them, and builds the program locally.
- The generated sources (`gen/`) and the resulting binary contain code derived from the game. **Do not share them.**
  Share this repository instead; each person builds their own copy.
- *Ace Combat* and *Namco* are trademarks of their owners. This is an unofficial fan project with no affiliation.

## Quick start

You need Linux (x86-64) with OpenGL 3.3, and **your own disc image** of the USA release: `Ace Combat 2 (USA).bin`
(raw MODE2/2352 sectors; a `.cue` next to it is fine, but the `.bin` is what you point the program at).

**1. Install the build tools** (once):

```sh
# Debian / Ubuntu / Kali
sudo apt install build-essential cmake ninja-build python3 libsdl2-dev libgl1-mesa-dev git
```

**2. Get the code and build it against your disc:**

```sh
git clone <this repository> ac2native && cd ac2native
./build.sh "/path/to/Ace Combat 2 (USA).bin"
```

`build.sh` does three things and takes a few minutes:

1. **Extract** — copies the game's three executables out of your disc into `work/`. It prints the MD5 of the loader:
   it must be `2622786de8d04eb398e52d311ea7ff95`. Any other value means the disc is not the USA release.
2. **Recompile** — translates those executables to C in `gen/` (about 9 MB, a second or two).
3. **Compile** — builds everything with CMake and Ninja into `build/ac2native`.

**3. Play:**

```sh
./build/ac2native
```

The first time (no settings yet) the launcher opens: check the disc path on the *Game* tab (it looks for the disc in
the usual folders by itself), adjust video and controls if you like, and press **Play**. The launcher
opens every time by default; untick *Show this launcher every time the game starts* (*Game* tab) to start playing
directly, and use `./build/ac2native --launcher` to bring the settings back. To skip it for one run:
`./build/ac2native "/path/to/Ace Combat 2 (USA).bin"`. `./install-desktop-shortcut.sh` adds a menu entry.

### Good to know

- **The disc is read every time you play** (data, music and movies come straight from the image), so keep it accessible.
- **Where your data lives:** settings, memory cards and debug states are in `~/.local/share/AC2Native/AceCombat2/`
  (`config.ini`, `card0/`, `states/`) — outside the repository. To move your progress to another PC, copy `card0/`.
- **`work/`, `gen/` and `build/` are generated on each PC** from its own disc. They contain material derived from the
  game: they are never committed and must not be shared.
- To update after pulling new changes, run `./build.sh` again (add the disc path as above).

### Troubleshooting

| Message / symptom | Cause and fix |
|---|---|
| `not a PlayStation disc image (MODE2/2352 .bin expected)` | You gave a `.cue`, an `.iso` or a cooked image. Use the raw `.bin`. |
| `SLUS_004.04 not found: is this Ace Combat 2 (USA)?` | Another game or another region. Only the USA release is supported. |
| CMake: `Could not find SDL2` / missing `GL/gl.h` | Install `libsdl2-dev` and `libgl1-mesa-dev` (see step 1). |
| *Cannot open the disc image* when starting | The saved disc path is wrong (disc moved): open `./build/ac2native --launcher` and pick it again. |
| Window stays black or fails to open | The GPU/driver must provide OpenGL 3.3 core. |
| A gamepad does nothing | Run `AC2_INPUTLOG=1 ./build/ac2native -v` and look for `pad:` lines; open an issue with that output. |

## Controls

Keyboard defaults (all rebindable in the launcher, *Controls* tab) and gamepad (any SDL game controller):

| Action | Keyboard | Gamepad |
|---|---|---|
| Pitch / roll | W S / A D | D-pad or left stick |
| Machine gun | F | A |
| Missile | Space | B |
| Ground map | M | X |
| Change target | R | Y |
| Brake / Accelerate | Left Ctrl / Left Shift | LB / RB |
| Rudder left / right | Q / E | LT / RT |
| Start / Select | Enter / Backspace | Menu / View |
| Pause menu | Esc | Xbox / Guide button |
| Fullscreen | F11 | — |

The game's own *Option → Key Config* screen still works (NOVICE / EXPERT modes).

## Options

In the launcher: window mode and size, VSync, aspect (4:3 / 16:9 / stretch), screen filter, **internal resolution**
(1x, 2x, 4x), **widescreen**, movies on/off, volume, key and pad bindings.

- 2x runs well on almost any PC. 4x is smooth with a quad-core CPU (the software renderer uses one thread per extra core).
- Settings live in `~/.local/share/AC2Native/AceCombat2/config.ini`; memory cards in `card0/` next to it.

### Debug states

Because the game is native code there are no emulator-style savestates. Instead the pause menu (Esc) has three
*Debug states* slots (**F5** saves slot 1, **F9** loads it). A state stores the memory card as it was at start plus
every button press since; loading restarts the game and replays them at full speed, so you skip the intro and
briefings. After a load the game sees the memory card as it was when that session started, so saving in-game from
there overwrites your real card with that older progress.

## Status

| Area | State |
|---|---|
| Boot, menus, briefing, hangar, flight, HUD | working |
| Movies (intro and endings), music (XA) | working |
| Sound effects (SPU) | mostly working — some silence reported in mission 2, under investigation |
| Save / load to memory card, gamepad, keyboard | working |
| Internal resolution 1x/2x/4x, widescreen | working |
| All missions and both endings played through | not yet verified |
| Windows build, installer, packaging | not started (see `PLAN.md`) |

## How it works

| Part | Where | What it does |
|---|---|---|
| Recompiler | `tools/recomp.py` | Translates the three game executables to C: delay slots, jump tables, `setjmp` resume points, virtual clock |
| Runtime | `runtime/` | High-level BIOS, interrupts, DMA, timers, CD drive reading your image, software GPU (multithreaded, scalable), SPU mixer, MDEC, GTE, SDL2 + OpenGL |
| Native movie player | `runtime/movie.c` | Decodes the game's STR videos (bit-exact with the game's own decoder) |
| Launcher and pause menu | `frontend/`, `runtime/platform.cpp` | Dear ImGui |
| Extraction | `tools/extract.py` | Pulls `SLUS_004.04` and the game modules out of the disc image |

Developer notes are in `NOTES.md` (findings, debugging switches, what was tried) and the roadmap is in `PLAN.md`
(both in Spanish). Useful switches: `-v`, `--frames N`, `--input SCRIPT`, `--shot-every N PREFIX`, `--scale N`,
`--load-state FILE`, `--play-movie N`, and environment variables such as `AC2_PERFLOG=1` and `AC2_INPUTLOG=1`.

## Project layout

```
runtime/    native runtime (hardware, BIOS, GPU, SPU, CD, movies, platform)
frontend/   launcher and configuration
tools/      recompiler, extractor, analysis and test helpers
assets/     UI fonts and icon
third_party Dear ImGui
gen/ work/  generated from YOUR disc by build.sh — never committed
```

## License

Copyright (C) 2026 the ac2native contributors. Licensed under the **GNU General Public License, version 2**
(see [`LICENSE`](LICENSE)).

Bundled third-party components keep their own licenses: Dear ImGui (MIT, `third_party/imgui/LICENSE.txt`),
Roboto fonts (Apache-2.0, `assets/fonts/LICENSE-Roboto.txt`). SDL2 (zlib license) is linked dynamically.
