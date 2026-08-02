local PremakeRoot = "Build/Premake"
local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
local Action = _ACTION or "NoAction"
local WorkspaceLocation = path.join(RepositoryRoot, "Intermediate/ProjectFiles", Action)

if Action == "vs2022" or Action == "vs2026" then
    WorkspaceLocation = RepositoryRoot
end

include(PremakeRoot .. "/Toolchains.lua")

workspace "Herta"
    architecture "x86_64"
    configurations { "Debug", "Debug-ASan", "Development", "Shipping" }
    location(WorkspaceLocation)
    startproject "HertaTests"

    language "C++"
    cppdialect "C++23"
    staticruntime "Off"
    warnings "Extra"
    fatalwarnings "All"

    multiprocessorcompile "On"

    HertaApplyToolchainSettings()

include(PremakeRoot .. "/Modules.lua")
