local PremakeRoot = "Build/Premake"
local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
local Action = _ACTION or "NoAction"
local WorkspaceLocation = path.join(RepositoryRoot, "Intermediate/ProjectFiles", Action)

if Action == "vs2022" or Action == "vs2026" then
    WorkspaceLocation = RepositoryRoot
end

include(PremakeRoot .. "/Toolchains.lua")
include(PremakeRoot .. "/ThirdParty/EnkiTS.lua")
include(PremakeRoot .. "/ThirdParty/FastGltf.lua")
include(PremakeRoot .. "/ThirdParty/FreeType.lua")
include(PremakeRoot .. "/ThirdParty/GLFW.lua")
include(PremakeRoot .. "/ThirdParty/ImGui.lua")
include(PremakeRoot .. "/ThirdParty/Im3d.lua")
include(PremakeRoot .. "/ThirdParty/Jolt.lua")
include(PremakeRoot .. "/ThirdParty/MeshOptimizer.lua")
include(PremakeRoot .. "/ThirdParty/NVRHI.lua")
include(PremakeRoot .. "/ThirdParty/Spdlog.lua")
include(PremakeRoot .. "/ThirdParty/SimdJson.lua")

workspace "Herta"
    architecture "x86_64"
    configurations { "Debug", "Debug-ASan", "Development", "Shipping" }
    location(WorkspaceLocation)
    startproject "HertaEditor"

    language "C++"
    cppdialect "C++23"
    staticruntime "Off"

    multiprocessorcompile "On"

    HertaApplyToolchainSettings()

HertaEnkiTS()
HertaFastGltf()
HertaFreeType()
HertaGLFW()
HertaImGui()
HertaIm3d()
HertaJolt()
HertaMeshOptimizer()
HertaNVRHI()
HertaSpdlog()
HertaSimdJson()

include(PremakeRoot .. "/Modules.lua")
