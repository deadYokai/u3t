# FAQ

## The log says `validated=0` — what does that mean?

`farchive_vtable::resolve_farchive_slots()` couldn't confirm that the
`FArchive` vtable slots it recovered match the expected layout for the
build it's looking at. CU3ML will still try to run, but overrides may not
serialize correctly. This is the "maybe not working" in the badge — it
means the disassembly-based resolver hit a build it wasn't able to fully
confirm, not that something is misconfigured on your end.

## Will this work with my UE3 game?

Probably, but there's no guaranteed list. CU3ML doesn't ship a
per-title offset table — it re-derives the layout by disassembling
whatever `.exe` it's loaded into. Different compilers, different
optimization settings, and different UE3 fork ages can all change how
that goes. Check `dinput8.log` after launch; `validated=1` is the signal
that it's on solid ground.

## Is it safe to use online / in multiplayer?

CU3ML hooks engine internals in memory. Treat it the same way you'd treat
any other injected DLL with respect to anti-cheat and terms of service —
that's a per-game question CU3ML can't answer for you.

## Does it modify my game files?

No. Overrides are intercepted at `Preload` time, in memory. The original
`.upk` package files on disk are never opened for writing. Deleting
`dinput8.dll` and `mods/` returns the install to stock.

## A mod isn't loading

- Check `dinput8.log` for `found N mod(s): …` — if your mod isn't in that
  list, check that its `mod.toml` is valid TOML and sits directly inside
  a folder under `mods/`.
- Check that `enabled` isn't set to `false` in the mod's `mod.toml`.
- Confirm the `.bin`/`.namemap` pair names match what the game is
  actually requesting — see [Mod Format](mod-format.md).

## Something crashed on startup

Restore the original game folder (remove `dinput8.dll`) and file an issue
with your `dinput8.log` attached. Since the loader is disassembling your
specific game build at runtime, the log is the only way to tell which
step it got to.

## How to make Mod

Easiest way is via `pack-mod` command in [ue3-tools](https://github.com/deadYokai/ue3-tools)

