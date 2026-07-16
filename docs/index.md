# CU3ML

[**Crappy Unreal Engine 3 Mod Loader**](https://projects.yokai.digital/cu3ml) — a `dinput8.dll` proxy that hijacks a UE3
game's load order, re-derives the engine's internal layout at runtime by
disassembling the executable, and serves mod overrides without ever
repacking a single `.upk`.

!!! warning "Status"
    CU3ML resolves engine internals by disassembling the target at runtime
    instead of using known-good offsets, so behavior varies between game
    builds and isn't guaranteed to work at all. Keep backups, expect rough
    edges, and treat every hook as a controlled experiment against someone
    else's binary.

## Why it exists

Most UE3 modding workflows repack `.upk` package files on disk — slow to
iterate on, easy to corrupt, and tied to a specific game build's export
table. CU3ML instead:

- Loads as a drop-in `dinput8.dll` proxy, no game files touched.
- Figures out the running game's internal structure by disassembling it
  the first time it starts, instead of shipping a hardcoded offset table
  per title.
- Intercepts object loading in memory and substitutes mod content at
  `Preload` time — the original `.upk` on disk is never modified.

## Where to go next

| I want to… | Read |
|---|---|
| Install CU3ML and try an existing mod | [Getting Started](getting-started.md) |
| Package my own mod | [Mod Format](mod-format.md) |
| Understand how the runtime disassembly works | [Architecture](architecture.md) |
| Build CU3ML from source | [Building from Source](building.md) |
| Something isn't working | [FAQ](faq.md) |

## At a glance

| | |
|---|---|
| Target | `win-x64` / `win-x86` |
| Proxy | `dinput8.dll` |
| Disassembler | [Zydis](https://github.com/zyantific/zydis) |
| Language | C++ |
| Source layout | 13 translation units, ~5k lines — see [Architecture](architecture.md) |
