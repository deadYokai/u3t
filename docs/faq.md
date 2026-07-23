# FAQ

## Where do the log and settings files go?

All of them live in the game's `Mods/` directory — `cu3ml.log`,
`cu3ml.toml` and `cu3ml.addrlist`. CU3ML looks for `Mods/` two levels above
the executable, so `Binaries/Win64/MyGame.exe` means `MyGame/Mods/`. Nothing
is written next to the `.exe` except the DLL you copied there.

## The log says `validated=0` — what does that mean?

`resolve_farchive_slots()` couldn't confirm that the `FArchive` vtable slots
it recovered match the expected layout for the build it's looking at. CU3ML
will still try to run, but overrides may not serialize correctly. This is the
"maybe not working" in the badge — it means the disassembly-based resolver
hit a build it wasn't able to fully confirm, not that something is
misconfigured on your end.

## Will this work with my UE3 game?

Probably, but there's no guaranteed list. CU3ML doesn't ship a
per-title offset table — it re-derives the layout by disassembling
whatever `.exe` it's loaded into. Different compilers, different
optimization settings, and different UE3 fork ages can all change how
that goes. Check `Mods/cu3ml.log` after launch; `validated=1` is the signal
that it's on solid ground.

## Why was the first launch so slow?

The initial scan walks the whole executable looking for anchor strings and
decoding around them. The results go into `Mods/cu3ml.addrlist`, keyed to
that exact `.exe`, so later launches skip it. Updating or patching the game
invalidates the cache automatically and you pay the scan once more.

## Should I delete `cu3ml.addrlist`?

Only if you suspect a stale or wrong resolution — it's always safe, the
loader just rebuilds it. You can also launch once with `-cu3ml-noaddrcache`
to force a re-scan without deleting anything.

## Is it safe to use online / in multiplayer?

CU3ML hooks engine internals in memory. Treat it the same way you'd treat
any other injected DLL with respect to anti-cheat and terms of service —
that's a per-game question CU3ML can't answer for you.

## Does it modify my game files?

No. Overrides are intercepted at `Preload` time, in memory. The original
`.upk` package files on disk are never opened for writing. Deleting
`dinput8.dll` and `Mods/` returns the install to stock.

## A mod isn't loading

- Check `Mods/cu3ml.log` for `mod_loader: found '<name>'` — if your mod isn't
  listed, confirm its `mod.toml` is valid and sits directly inside a folder
  under `Mods/`.
- If the line ends in `DISABLED`, the mod is switched off in
  `Mods/cu3ml.toml` under `[mods]` (keyed by *folder* name). Turn it back on
  there or in the [manager](manager.md). There is no `enabled` key in
  `mod.toml`.
- Renaming a mod folder resets its enable state, because the folder name is
  the key.
- If it requires another mod that isn't installed, you'll see a
  `requires unknown mod` warning — the mod still loads, just unordered.
- Confirm the `.bin` / `.namemap` names match what the game actually
  requests — see [Mod Format](mod-format.md).

## My Lua script didn't run

- Scripts must be named `main.lua`, in the mod's root folder.
- Check `lua = true` in `Mods/cu3ml.toml` and that `-nolua` isn't on the
  command line.
- Scripts run only after the layout resolves — if resolution failed, the VM
  never starts.
- Turn on `lua_verbose` (or pass `-luaverbose`) to log every chunk result,
  and remember `print` goes to `cu3ml.log`, not to a console.

## Something crashed on startup

Restore the original game folder (remove `dinput8.dll`) and file an issue
with your `Mods/cu3ml.log` attached. Since the loader is disassembling your
specific game build at runtime, the log is the only way to tell which
step it got to. Worth trying first: launch with `-cu3ml-noaddrcache` to rule
out a bad cached address, and `-nolua` to rule out a mod script.

## How to make a mod

Easiest way is via the `pack-mod` command, check [ue3-tools/Building Mods](https://docs.yokai.digital/ue3-tools/building-mods).
