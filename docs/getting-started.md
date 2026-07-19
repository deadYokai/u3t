# Getting Started

## Install

1. Download the build matching your game (`x64` or `x86` — most modern UE3
   games are `x64`, but check your game's `.exe` if unsure).
2. Extract `dinput8.dll` into the same folder as the game's `.exe`.
3. Create a `Mods/` folder in root, if one doesn't already exist.
4. Drop a mod folder (containing a `mod.toml`) into `Mods/`.
5. Launch the game normally.

```text
MyGame/
├── Binaries/
    └── Win64
        └── MyGame.exe
        └── dinput8.dll          ← CU3ML
└── Mods/
    └── ExampleMod/
        ├── mod.toml
        └── overrides/
```

That's it — no launcher, no injector, no extra command line flags. Windows
loads `dinput8.dll` because the game already expects a system copy of it
for DirectInput; CU3ML sits in that slot and forwards everything the game
doesn't care about straight through to the real DLL.

!!! note
    If the game ships its own `dinput8.dll` already (rare, but it happens),
    CU3ML will look for `dinput8.orig.dll` next to itself and forward through
    that instead. Rename the original file if you hit a conflict.

## Verifying it loaded

CU3ML writes a rotating log file (`cu3ml.log`) next to the game's `.exe`.
On a successful start you should see something like:

```text
[INFO ] CU3ML mod loader (x64 build)
[INFO ]   found 1 mod(s): ExampleMod
[INFO ]   UE3 layout resolved (validated=1)
[INFO ]   init_thread: ready
```

If `validated=0` shows up instead, the disassembly-based resolver couldn't
confirm the engine layout it recovered — see [FAQ](faq.md).

## Uninstalling

Delete `dinput8.dll` and the `Mods/` folder. Nothing else changes — no
registry entries, no modified `.upk` files, no leftover state in the game's
save data.

## Next

Head to [Mod Format](mod-format.md) to package your own mod, or
[Architecture](architecture.md) if you want to know what's actually
happening under the hood.
