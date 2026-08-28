# Skate 3 Recomp — Online Play (fan-made add-on)

This is an **online-play layer** built on top of the [Skate 3 native
recompilation](README.md). It is an unofficial, fan-made networking add-on and
is **not affiliated with EA**. No EA or Xbox Live servers are contacted or
reimplemented — all netcode is written from first principles and the relay is
self-hosted.

> **This repository contains NO Skate 3 game code, assets, dumps, or Title
> Update data.** To build or run, you must provide files from your own legally
> obtained copy of Skate 3, exactly as the base recompilation requires.

## What this adds

- **Real remote skaters** — each player renders with their own model + animation.
- **Spot Battle** (1–6 rounds) and full **S.K.A.T.E.** — turn-based, exact-trick
  matching including rotation, letters, played-list, trick exclusions.
- **Party system** with private parties.
- **In-game menu** — Online / Accessibility / Username / Game Modes / Party tabs,
  so modes and settings work without the console.
- **Standalone relay** (`skate3_relay`) so friends connect over the internet
  with no port-forwarding.

## Where the online code lives

- `src/net/` — wire protocol, serialization, session, relay, game-mode logic.
- Additions in `src/skate3_native_scene.cpp` (render bridge, trick detection),
  `src/skate3_native_debug_dialog.*` (HUD), `src/skate3_app_common.cpp`
  (wiring), and the SDK overlay/kernel edits (menu, sign-in, XAM).

## Building

Same as the base recompilation (see [README.md](README.md) and
[`skate3-build-toolchain`] notes). In short, on Windows with the VS build tools:

```
cmake --build out/build/release --target skate3 skate3_relay -j 8
```

You supply your own Skate 3 dump; the recompiled game code is generated locally
and is **git-ignored** (never committed).

## Credits & licensing

- **Base recompilation:** the upstream Skate 3 recomp project (this repo is a
  fork of it — see its README/history for authorship).
- **Runtime:** [`third_party/rexglue-sdk`](https://github.com/mchughalex/rexglue-skate3)
  — BSD 3-Clause.
- **ENet:** `third_party/enet` — MIT.
- **Online layer** (the code listed above): MIT — see
  [`LICENSE-ONLINE.txt`](LICENSE-ONLINE.txt).

Contributions welcome — especially runtime auto-scan of the score/trick memory
addresses so S.K.A.T.E. works across every game title-update, not just the
reference one.
