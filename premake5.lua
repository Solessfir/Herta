local PremakeRoot = "Build/Premake"

include(PremakeRoot .. "/Toolchains.lua")

workspace "Herta"
    architecture "x86_64"
    configurations { "Debug", "Debug-ASan", "Development", "Shipping" }
    location("Intermediate/ProjectFiles/" .. (_ACTION or "NoAction"))
    startproject "HertaTests"

    language "C++"
    cppdialect "C++23"
    staticruntime "Off"
    warnings "Extra"
    fatalwarnings "All"

    multiprocessorcompile "On"

    HertaApplyToolchainSettings()

include(PremakeRoot .. "/Modules.lua")
