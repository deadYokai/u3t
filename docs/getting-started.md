# Getting Started

## Install

1. Download the build matching your game (`x64` or `x86` — most modern UE3
   games are `x64`, but check your game's `.exe` if unsure).
2. Extract `dinput8.dll` into the same folder as the game's `.exe`
   (usually `Binaries/Win64/` or `Binaries/Win32/`).
3. Create a `Mods/` folder in the game root, if one doesn't already exist.
4. Drop a mod folder (containing a `mod.toml`) into `Mods/`.
5. Launch the game normally.

```text
MyGame/
├── Binaries/
│   └── Win64/
│       ├── MyGame.exe
│       └── dinput8.dll         ← CU3ML
└── Mods/                       ← everything CU3ML reads and writes
    ├── cu3ml.toml              ← loader settings (written on first run)
    ├── cu3ml.addrlist          ← cached engine addresses
    ├── cu3ml.log               ← log from the last launch
    └── ExampleMod/
        ├── mod.toml
        ├── main.lua
        └── overrides/
```

!!! important "Where `Mods/` goes"
    CU3ML looks for `Mods/` two directories above the executable —
    `Binaries/Win64/MyGame.exe` means `MyGame/Mods/`. It is **not** next to
    the `.exe`. If your game keeps its executable somewhere else, the same
    rule applies: go up two levels from the `.exe`, then into `Mods`.

That's it — no launcher, no injector, no extra command line flags. Windows
loads `dinput8.dll` because the game already expects a system copy of it
for DirectInput; CU3ML sits in that slot and forwards everything the game
doesn't care about straight through to the real DLL.

!!! note
    If the game ships its own `dinput8.dll` already (rare, but it happens),
    CU3ML will look for `dinput8.orig.dll` next to itself and forward through
    that instead. Rename the original file if you hit a conflict.

## Verifying it loaded

CU3ML writes `Mods/cu3ml.log`, recreated from scratch on every launch. On a
successful start you should see something like:

```text
[INFO ] CU3ML mod loader (x64 build)
[INFO ] mod_loader: found 'ExampleMod'  v1.0.0  by deadYokai  deps=[0]  enabled
[INFO ] install_engine_loop_init_hook: GEngineLoop::Init = 0x... (scanned)
[INFO ] engine_loop_init: FArchive slots = ... (validated=1)
[INFO ] override_loader: 2 override(s), 0 new export(s) discovered
[INFO ] lua: VM ready (sol 3.x.x, Lua 5.4.x)
[INFO ] engine_loop_init: ready
```

If `validated=0` shows up instead, the disassembly-based resolver couldn't
confirm the engine layout it recovered — see [FAQ](faq.md).

## First launch is the slow one

The first time CU3ML runs against a given `.exe` it scans the whole image to
find its anchor functions. What it finds is written to `Mods/cu3ml.addrlist`,
keyed to that executable's timestamp, size and checksum, so subsequent
launches load the addresses directly. Patch or replace the game's `.exe` and
the cache invalidates itself automatically.

## Opening the manager

Launch the game with `-cu3ml-manager` to get a window before the engine
finishes booting, where you can toggle mods and loader settings and then
either launch or quit. See [Manager](manager.md).

## Uninstalling

Delete `dinput8.dll` and the `Mods/` folder. Nothing else changes — no
registry entries, no modified `.upk` files, no leftover state in the game's
save data.

## Next

Head to [Mod Format](mod-format.md) to package your own mod,
[Lua Scripting](lua-api.md) to drive the engine from a script, or
[Architecture](architecture.md) if you want to know what's actually
happening under the hood.
