-- Runs once, in dependency order, after CU3ML resolves the UE3 layout.

-- log modloader build info
local b = ue3.build()
ue3.log(("main.lua up on %s (%s)"):format(b.arch, b.loader))

-- list the mods the loader saw
for _, m in ipairs(ue3.mods()) do
	print("mod:", m.name, m.version or "?", "by", m.author or "unknown")
end

-- command line the game launched with
print("cmdline:", ue3.cmdline())
if ue3.has_switch("example") then
	ue3.log("simple example of cmdline arg")
end

-- FName index 0 is always "None" — a cheap sanity check that the name
-- table resolved correctly
print("fname(0) =", ue3.fname(0))

-- load an asset by path (StaticLoadObject). nil if not found / unresolved.
local obj = ue3.load("EngineMaterials.DefaultDiffuse")
ue3.log("load DefaultDiffuse -> " .. tostring(obj ~= nil))

-- run an engine command and capture its output.
-- StaticExec handles obj/get/set/listprops without needing GEngine.
local text, handled = ue3.exec("obj classes")
if handled and text then
	ue3.log("obj classes returned " .. tostring(#text) .. " bytes")
end

