# Building from source

This is a native recompilation, so building from source is involved — most
people just want to **play**, in which case grab the binary release (see the
Releases page) and follow the package README. Building is for developers who
want to modify the code.

> You must provide files from your **own legally obtained** copy of Skate 3.
> No game code, assets, or dumps are included in this repository.

## Prerequisites

- **CMake** 3.20+ and **Ninja**
- A C++20 compiler — **Clang/LLVM** (the reference toolchain) with the
  **Visual Studio Build Tools** (MSVC) on Windows for the standard library and
  linker
- Git (with submodule support)
- Your own Skate 3 Xbox 360 dump / game data (see the base project's
  [README](README.md) — "How Do I Play?" — for how the game data is staged and
  the recompiled sources are generated; the transpiled `generated/` output is
  produced locally and is **not** committed)

## Clone (with submodules)

```bash
git clone --recursive https://github.com/DonaldDuck979/skate3recomp.git
cd skate3recomp
# if you forgot --recursive:
git submodule update --init --recursive
```

The runtime SDK is a submodule (`third_party/rexglue-sdk`, pointing at this
fork's `online-layer` branch). Its imgui is **vendored** (committed directly),
so a fresh clone doesn't depend on any unfetchable upstream — it just builds.

## Configure & build (Windows)

From a shell with the MSVC environment loaded (run `vcvars64.bat` first, or use
a "x64 Native Tools" prompt / Git Bash after sourcing it):

```bash
# configure once (generates the build tree; see CMakePresets.json for presets)
cmake --preset release   # or your configured preset

# build the game + the standalone relay
cmake --build out/build/release --target skate3 skate3_relay -j 8
```

Outputs land in `out/build/release/`:
- `skate3.exe` + `rexruntime.dll` — the game
- `skate3_relay.exe` — the standalone online relay

Deploy `skate3.exe` + `rexruntime.dll` over a working recomp install to run
your changes; run `skate3_relay.exe` (or point players at one) for online.

## Where the online code lives

See [ONLINE.md](ONLINE.md). In short: `src/net/` (protocol, session, relay,
game modes) plus additions in `src/skate3_native_scene.cpp`,
`src/skate3_native_debug_dialog.*`, `src/skate3_app_common.cpp`, and the SDK
overlay/kernel edits in the `rexglue-sdk` submodule.

## Good first contributions

- **Runtime auto-scan of the score/trick memory addresses** — they're currently
  correct for the reference game title-update but may shift on others; the
  `skate3_addr_autoscan` cvar exists but is off by default. Making it reliable
  would let S.K.A.T.E./scoring work on every install.
- Tightening remote-skater sync, name-tag rendering, more game modes.
