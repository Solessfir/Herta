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
    -- Engine headers a game module may include; it links none of them.
    local Dependencies = { Core = true, Math = true, Assets = true, Level = true, GameFramework = true }
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
newoption {
    trigger = "clangcl",
    description = "Generate a separate Windows workspace that builds with ClangCL into its own project and output folders"
}

-- Separate folders let compiler verification switch between MSVC and ClangCL without rebuilding either one from scratch.
HertaToolsetSuffix = (os.target() == "windows" and _OPTIONS["clangcl"]) and "-ClangCL" or ""

local Action = _ACTION or "NoAction"
local WorkspaceLocation = path.join(RepositoryRoot, "Intermediate/ProjectFiles", Action .. HertaToolsetSuffix)

if (Action == "vs2022" or Action == "vs2026") and HertaToolsetSuffix == "" then
    WorkspaceLocation = RepositoryRoot
end

if HertaProjectRoot then
    WorkspaceLocation = path.join(HertaProjectRoot, "Intermediate/ProjectFiles", Action .. HertaToolsetSuffix)
    if (Action == "vs2022" or Action == "vs2026") and HertaToolsetSuffix == "" then
        WorkspaceLocation = HertaProjectRoot
    end
end

-- ar -rcs only adds and replaces members, so an archive keeps objects from moved or deleted sources.
-- Source lists change only on regeneration, so drop the archives and let make re-archive the existing objects without recompiling.
if Action:match("^gmake") and os.target() == "linux" then
    for _, Root in ipairs({ RepositoryRoot, HertaProjectRoot }) do
        for _, Archive in ipairs(os.matchfiles(path.join(Root, "Binaries/linux/*/*/lib*.a"))) do
            os.remove(Archive)
        end
    end
end

include(PremakeRoot .. "/Toolchains.lua")
include(PremakeRoot .. "/ThirdParty/EnkiTS.lua")
include(PremakeRoot .. "/ThirdParty/FastGltf.lua")
include(PremakeRoot .. "/ThirdParty/FreeType.lua")
include(PremakeRoot .. "/ThirdParty/GameNetworkingSockets.lua")
include(PremakeRoot .. "/ThirdParty/GLFW.lua")
include(PremakeRoot .. "/ThirdParty/ImGui.lua")
include(PremakeRoot .. "/ThirdParty/Im3d.lua")
include(PremakeRoot .. "/ThirdParty/Jolt.lua")
include(PremakeRoot .. "/ThirdParty/MeshOptimizer.lua")
include(PremakeRoot .. "/ThirdParty/Monocypher.lua")
include(PremakeRoot .. "/ThirdParty/NVRHI.lua")
include(PremakeRoot .. "/ThirdParty/Ozz.lua")
include(PremakeRoot .. "/ThirdParty/Protobuf.lua")
include(PremakeRoot .. "/ThirdParty/Recast.lua")
include(PremakeRoot .. "/ThirdParty/Spdlog.lua")
include(PremakeRoot .. "/ThirdParty/SimdJson.lua")
include(PremakeRoot .. "/ThirdParty/Zstd.lua")
include(PremakeRoot .. "/ThirdParty/HarfBuzz.lua")

workspace "Herta"
    architecture "x86_64"
    configurations { "Debug", "Debug-ASan", "Development", "Shipping" }
    location(WorkspaceLocation)
    startproject "Herta"

    language "C++"
    cppdialect "C++23"
    staticruntime "Off"

    multiprocessorcompile "On"

    if HertaToolsetSuffix ~= "" then
        toolset "clang"
        -- clang-cl lowers _Float16 conversions, which fastgltf uses, to compiler-rt calls that it does not link by itself.
        libdirs { "$(LLVMInstallDir)/lib/clang/$(LLVMIncludeVersion)/lib/windows" }
        links { "clang_rt.builtins-x86_64" }
    end

    HertaApplyToolchainSettings()

HertaEnkiTS()
HertaFastGltf()
HertaFreeType()
HertaGameNetworkingSockets()
HertaGLFW()
HertaImGui()
HertaIm3d()
HertaJolt()
HertaMeshOptimizer()
HertaMonocypher()
HertaNVRHI()
HertaOzz()
HertaProtobuf()
HertaRecast()
HertaSpdlog()
HertaSimdJson()
HertaZstd()
HertaHarfBuzz()

include(PremakeRoot .. "/Modules.lua")
