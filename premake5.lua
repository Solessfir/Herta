local PremakeRoot = "Build/Premake"
local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)

newoption {
    trigger = "project",
    value = "DESCRIPTOR",
    description = "Generate an engine workspace including the native modules of a .hertaproject"
}

if _OPTIONS["project"] then
    local DescriptorPath = path.getabsolute(_OPTIONS["project"])
    local File = io.open(DescriptorPath, "rb")
    if not File then
        error("Cannot open project descriptor: " .. DescriptorPath)
    end

    local Text = File:read("*a")
    File:close()
    if #Text > 1024 * 1024 then
        error("Project descriptor exceeds 1 MiB")
    end

    HertaProjectDescriptor = json.decode(Text)
    HertaProjectRoot = path.getdirectory(DescriptorPath)
    local Descriptor = HertaProjectDescriptor
    if not Descriptor or Descriptor.format ~= "HertaProject" or Descriptor.formatVersion ~= 1 or (Descriptor.schemaVersion ~= 1 and Descriptor.schemaVersion ~= 2) or Descriptor.engineAssociation ~= "Herta" or type(Descriptor.modules) ~= "table" then
        error("Invalid project descriptor or incompatible engine association")
    end

    local Seen = {}
    local Dependencies = { Core = true, Math = true, Assets = true, Level = true }
    for _, Module in ipairs(Descriptor.modules) do
        if type(Module.name) ~= "string" or not Module.name:match("^[A-Za-z][A-Za-z0-9_]*$") or #Module.name > 64 or Seen[Module.name] or Module.source ~= "Source/" .. Module.name or type(Module.dependencies) ~= "table" or #Module.dependencies == 0 then
            error("Invalid project module name, source path, or dependencies")
        end

        Seen[Module.name] = true
        for Index, Dependency in ipairs(Module.dependencies) do
            if Descriptor.schemaVersion == 1 and Dependency == "Scene" then
                Dependency = "Level"
                Module.dependencies[Index] = Dependency
            end

            if not Dependencies[Dependency] then
                error("Unsupported native project dependency: " .. tostring(Dependency))
            end
        end

        if not os.isdir(path.join(HertaProjectRoot, Module.source)) then
            error("Project module source directory is missing: " .. Module.source)
        end
    end
end
local Action = _ACTION or "NoAction"
local WorkspaceLocation = path.join(RepositoryRoot, "Intermediate/ProjectFiles", Action)

if Action == "vs2022" or Action == "vs2026" then
    WorkspaceLocation = RepositoryRoot
end

if HertaProjectRoot then
    WorkspaceLocation = path.join(HertaProjectRoot, "Intermediate/ProjectFiles", Action)
    if Action == "vs2022" or Action == "vs2026" then
        WorkspaceLocation = HertaProjectRoot
    end
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
