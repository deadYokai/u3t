# Manager

The manager is a small ImGui window that opens *before* the engine finishes
starting, so mods can be toggled without editing files by hand.

## Opening it

Either:

- launch the game with `-cu3ml-manager`, or
- set `manager_at_start = true` under `[loader]` in `Mods/cu3ml.toml` (the
  manager can set this itself).

Both are checked at the top of the hooked `GEngineLoop::Init`. Boot halts
there and doesn't continue until the window closes, which means the engine
hasn't set up rendering or input yet — the manager runs in its own Win32
window with its own OpenGL 3 context, not as an in-game overlay.

## What's in it

- **Header and meta row** — loader build (`x64` / `x86`), mod counts.
- **Loader settings** — toggles for `lua`, `lua_verbose` and
  `manager_at_start`, with the path of the `cu3ml.toml` they're written to.
- **Discovered mods** — every folder with a valid `mod.toml`, in resolved
  dependency order, each with a switch. If none were found, the panel says
  so and points at the `Mods` directory.
- **Detail pane** — for the selected mod: enable state, folder key, full
  path, counts of content paths / spawn patches / replace patches, and the
  `requires` list.
- **QUIT** and **LAUNCH GAME**.

## What the buttons do

| Button | Result |
|---|---|
| `LAUNCH GAME ->` | Closes the window, re-evaluates the enabled mod set, and continues booting |
| `QUIT` | Removes every installed hook and exits the process immediately |

Closing the window with the title-bar X is treated as quit.

Toggling a mod marks the loader config dirty and updates the in-memory
enable state; on launch, `mod_loader::refresh()` rebuilds the active mod list
and replace tables from it, so changes take effect for that same session —
no restart needed.

!!! note "Rendering"
    If the window or GL context can't be
    created at all, the manager is skipped and the game boots normally
    rather than failing.
