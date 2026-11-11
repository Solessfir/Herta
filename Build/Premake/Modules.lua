local RepositoryRoot = _MAIN_SCRIPT_DIR
local SourceRoot = path.join(RepositoryRoot, "Engine/Source")
local RuntimeRoot = path.join(SourceRoot, "Runtime")
local EditorRoot = path.join(SourceRoot, "Editor")
local ProgramsRoot = path.join(SourceRoot, "Programs")
local ProjectFilesRoot = path.join(HertaProjectRoot or RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)
local RuntimeModules = {}

local function GetBuildRevision(FullRevision)
    local NullDevice = os.host() == "windows" and "nul" or "/dev/null"
    local Command = 'git -C "' .. RepositoryRoot .. '" rev-parse ' .. (FullRevision and "" or "--short=8 ") .. 'HEAD 2>' .. NullDevice
    local Success, Output = pcall(os.outputof, Command)
    local Revision = Success and Output and Output:match("^%s*([0-9a-fA-F]+)%s*$")
    return Revision and #Revision >= 7 and #Revision <= 40 and Revision or "unknown"
end

-- The revision lives in a generated header rather than a define, so make and MSBuild both rebuild what embeds it when it changes. Rewriting
-- it only on change keeps unchanged builds incremental. Projects force-include it by bare name from the include path, since premake would
-- make a path relative to the project file, which compilers do not resolve it against.
local RevisionHeader = path.join(RepositoryRoot, "Intermediate/Generated/HertaRevision.h")
do
    local Contents = '#pragma once\n#define HERTA_BUILD_REVISION "' .. GetBuildRevision() .. '"\n#define HERTA_ENGINE_REVISION HERTA_BUILD_REVISION\n'
    if io.readfile(RevisionHeader) ~= Contents then
        os.mkdir(path.getdirectory(RevisionHeader))
        io.writefile(RevisionHeader, Contents)
    end
end

local function ApplyCommonProjectSettings(ProjectSourceRoot)
    targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
    objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
    warnings "Extra"
    fatalwarnings "All"

    files {
        path.join(ProjectSourceRoot, "Public/**.h"),
        path.join(ProjectSourceRoot, "Public/**.hpp"),
        path.join(ProjectSourceRoot, "Private/**.h"),
        path.join(ProjectSourceRoot, "Private/**.hpp"),
        path.join(ProjectSourceRoot, "Private/**.cpp")
    }

    includedirs {
        path.join(ProjectSourceRoot, "Public")
    }
end

local function PublicIncludeDirectory(ModuleName)
    local Settings = RuntimeModules[ModuleName]
    local ModuleRoot = Settings and Settings.SourceRoot or path.join(RuntimeRoot, ModuleName)
    return path.join(ModuleRoot, "Public")
end

local function ApplyModuleDependencies(Dependencies)
    Dependencies = Dependencies or {}

    dependson(Dependencies)

    for _, Dependency in ipairs(Dependencies) do
        includedirs {
            PublicIncludeDirectory(Dependency)
        }
    end
end

local function HertaRuntimeModule(ModuleName, Settings)
    Settings = Settings or {}
    Settings.SourceRoot = Settings.SourceRoot or path.join(RuntimeRoot, ModuleName)
    RuntimeModules[ModuleName] = Settings
    local ModuleSourceRoot = Settings.SourceRoot
    project(ModuleName)
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, ModuleName))
        ApplyCommonProjectSettings(ModuleSourceRoot)
        ApplyModuleDependencies(Settings.PublicDependencies)
        ApplyModuleDependencies(Settings.PrivateDependencies)

        dependson(Settings.PrivateThirdPartyDependencies or {})
        defines(Settings.PrivateDefinitions or {})
        libdirs(Settings.PrivateLibraryDirectories or {})
end

-- A game module: a shared library the editor, the game, and the server load, which links no engine library and reaches the engine only
-- through GameModule.h's interfaces. It sees the engine's public headers for value types and inline code, and records the engine revision
-- the host checks before loading it.
local function HertaGameModule(ModuleName, SourceRoot, Definitions)
    project(ModuleName)
        kind "SharedLib"
        location(path.join(ProjectFilesRoot, ModuleName))
        ApplyCommonProjectSettings(SourceRoot)
        for _, Dependency in ipairs({ "Core", "Math", "Assets", "GameFramework" }) do
            includedirs { PublicIncludeDirectory(Dependency) }
        end

        includedirs { path.getdirectory(RevisionHeader) }
        forceincludes { "HertaRevision.h" }
        defines(Definitions or {})
        -- Only the entry point is exported; everything else stays inside the module.
        filter "system:linux"
            visibility "Hidden"
        filter {}
end

local function HertaEditorModule(ModuleName, Settings)
    Settings = Settings or {}
    Settings.SourceRoot = path.join(EditorRoot, ModuleName)
    HertaRuntimeModule(ModuleName, Settings)
end

local function ApplyRuntimeDependencies(Dependencies)
    local VisitedModules = {}
    local VisibleModules = {}
    local OrderedModules = {}

    local function VisitVisibleModule(ModuleName)
        if VisibleModules[ModuleName] then
            return
        end

        local Settings = RuntimeModules[ModuleName]
        if not Settings then
            error("Unknown Herta runtime module dependency: " .. ModuleName)
        end

        VisibleModules[ModuleName] = true
        for _, Dependency in ipairs(Settings.PublicDependencies or {}) do
            VisitVisibleModule(Dependency)
        end
    end

    local function VisitModule(ModuleName)
        if VisitedModules[ModuleName] then
            return
        end

        local Settings = RuntimeModules[ModuleName]
        if not Settings then
            error("Unknown Herta runtime module dependency: " .. ModuleName)
        end

        VisitedModules[ModuleName] = true
        for _, Dependency in ipairs(Settings.PublicDependencies or {}) do
            VisitModule(Dependency)
        end
        for _, Dependency in ipairs(Settings.PrivateDependencies or {}) do
            VisitModule(Dependency)
        end
        table.insert(OrderedModules, ModuleName)
    end

    for _, Dependency in ipairs(Dependencies) do
        VisitModule(Dependency)
        VisitVisibleModule(Dependency)
    end

    local LinkDependencies = {}
    local ThirdPartyDependencies = {}
    local VisitedThirdPartyDependencies = {}
    local LinuxSystemDependencies = {}
    local VisitedLinuxSystemDependencies = {}
    local WindowsSystemDependencies = {}
    local VisitedWindowsSystemDependencies = {}
    local LibraryDirectories = {}
    local VisitedLibraryDirectories = {}

    for Index = #OrderedModules, 1, -1 do
        local ModuleName = OrderedModules[Index]
        local Settings = RuntimeModules[ModuleName]
        table.insert(LinkDependencies, ModuleName)

        for _, Dependency in ipairs(Settings.PrivateThirdPartyDependencies or {}) do
            if not VisitedThirdPartyDependencies[Dependency] then
                VisitedThirdPartyDependencies[Dependency] = true
                table.insert(ThirdPartyDependencies, Dependency)
            end
        end

        for _, Dependency in ipairs(Settings.PrivateLinuxSystemDependencies or {}) do
            if not VisitedLinuxSystemDependencies[Dependency] then
                VisitedLinuxSystemDependencies[Dependency] = true
                table.insert(LinuxSystemDependencies, Dependency)
            end
        end

        for _, Dependency in ipairs(Settings.PrivateWindowsSystemDependencies or {}) do
            if not VisitedWindowsSystemDependencies[Dependency] then
                VisitedWindowsSystemDependencies[Dependency] = true
                table.insert(WindowsSystemDependencies, Dependency)
            end
        end

        for _, Directory in ipairs(Settings.PrivateLibraryDirectories or {}) do
            if not VisitedLibraryDirectories[Directory] then
                VisitedLibraryDirectories[Directory] = true
                table.insert(LibraryDirectories, Directory)
            end
        end
    end

    links(LinkDependencies)
    links(ThirdPartyDependencies)
    libdirs(LibraryDirectories)

    -- pairs() order changes between runs; a reordered include path makes MSBuild and make recompile every file of the project.
    local VisibleModuleNames = table.keys(VisibleModules)
    table.sort(VisibleModuleNames)
    for _, ModuleName in ipairs(VisibleModuleNames) do
        includedirs {
            PublicIncludeDirectory(ModuleName)
        }
    end

    filter "system:linux"
        links(LinuxSystemDependencies)

    filter "system:windows"
        links(WindowsSystemDependencies)

    filter {}
end

HertaRuntimeModule("Core", {
    PrivateThirdPartyDependencies = { "Spdlog" },
    PrivateDefinitions = { "SPDLOG_COMPILED_LIB" }
})
    includedirs { path.getdirectory(RevisionHeader) }
    forceincludes { "HertaRevision.h" }

    externalincludedirs {
        path.join(RepositoryRoot, "External/spdlog/include"),
        path.join(RepositoryRoot, "External/xxHash")
    }

    filter "system:windows"
        defines { "SPDLOG_WCHAR_FILENAMES" }

    filter {}

HertaRuntimeModule("Math", {
    PublicDependencies = { "Core" }
})

HertaRuntimeModule("Assets", {
    PublicDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "SimdJson", "Zstd", "Monocypher" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson"), path.join(RepositoryRoot, "External/zstd/lib"), path.join(RepositoryRoot, "External/monocypher") }

-- Schema-neutral graph data, validation, and canonical text; no ImGui, so compilers and tests run headless.
HertaRuntimeModule("GraphCore", {
    PublicDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson") }

-- CLDR-backed cultures, ICU-style messages, and text catalogs with fallback and pseudolocalization.
HertaRuntimeModule("Localization", {
    PublicDependencies = { "Core", "Assets" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson"), path.join(RepositoryRoot, "External/cldr") }

HertaRuntimeModule("Project", {
    PublicDependencies = { "Core" },
    PrivateDependencies = { "Assets", "Math", "Level", "Platform" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson") }

HertaRuntimeModule("Level", {
    PublicDependencies = { "Math", "Assets" },
    PrivateDependencies = { "Platform" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs {
        path.join(RepositoryRoot, "External/entt/src"),
        path.join(RepositoryRoot, "External/simdjson")
    }

HertaRuntimeModule("Platform", {
    PublicDependencies = { "Core" },
    PrivateWindowsSystemDependencies = { "ole32", "shell32", "bcrypt", "dbghelp" },
    PrivateLinuxSystemDependencies = { "dl" }
})
    filter "system:windows"
        removefiles {
            RuntimeRoot .. "/Platform/Private/Linux/**"
        }

    filter "system:linux"
        removefiles {
            RuntimeRoot .. "/Platform/Private/Windows/**"
        }

    filter {}

HertaRuntimeModule("Tasks", {
    PrivateDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "EnkiTS" },
    PrivateLinuxSystemDependencies = { "pthread" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/enkiTS/src")
    }

HertaRuntimeModule("Physics", {
    PublicDependencies = { "Math" },
    PrivateDependencies = { "Core", "Tasks" },
    PrivateThirdPartyDependencies = { "Jolt" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/Jolt") }
    -- Must match the Jolt library: Jolt's own headers change layout with it. See Docs/Physics.md.
    defines { "JPH_CROSS_PLATFORM_DETERMINISTIC" }

    filter "system:linux"
        buildoptions { "-ffp-contract=off" }

    filter "configurations:Debug or Debug-ASan"
        defines { "JPH_ENABLE_ASSERTS" }

    filter "configurations:Development or Shipping"
        defines { "JPH_NO_DEBUG" }

    filter {}

HertaRuntimeModule("Animation", {
    PublicDependencies = { "Math", "Assets" },
    PrivateDependencies = { "Core" },
    -- Linkers that resolve static libraries in order need the runtime jobs before the base library they use.
    PrivateThirdPartyDependencies = { "OzzAnimation", "OzzBase" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/ozz-animation/include"), path.join(RepositoryRoot, "External/earcut") }

-- Detour, DetourTileCache, and DetourCrowd stay private; 64-bit polygon references must match the Detour library.
HertaRuntimeModule("Navigation", {
    PublicDependencies = { "Core", "Math" },
    PrivateThirdPartyDependencies = { "Detour" }
})
    defines { "DT_POLYREF64" }
    externalincludedirs {
        path.join(RepositoryRoot, "External/recastnavigation/Recast/Include"),
        path.join(RepositoryRoot, "External/recastnavigation/Detour/Include"),
        path.join(RepositoryRoot, "External/recastnavigation/DetourCrowd/Include"),
        path.join(RepositoryRoot, "External/recastnavigation/DetourTileCache/Include")
    }

-- StateTree execution, perception, working memory, and scheduling for agents; games give it bodies and an environment.
HertaRuntimeModule("AI", {
    PublicDependencies = { "Math", "Assets" }
})

-- Automation for games, dedicated servers, and networked clients over EditorCore's protocol; programs link it outside Shipping only.
HertaRuntimeModule("GameAutomation", {
    PublicDependencies = { "Core", "Math", "Application", "Assets", "Audio", "Level", "Localization", "Navigation", "Particles", "Physics", "RHI", "Renderer", "GameUI", "GameFramework" },
    PrivateDependencies = { "EditorCore", "Replication", "Networking", "Platform" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson"), path.join(RepositoryRoot, "External/stb") }

-- GameNetworkingSockets and Protobuf stay private: messages, connections, and simulated network conditions behind Herta types.
HertaRuntimeModule("Networking", {
    PublicDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "GameNetworkingSockets", "Protobuf" },
    PrivateWindowsSystemDependencies = { "ws2_32", "crypt32", "winmm", "iphlpapi", "bcrypt" },
    PrivateLinuxSystemDependencies = { "crypto", "pthread" }
})
    defines { "STEAMNETWORKINGSOCKETS_STATIC_LINK" }
    externalincludedirs { path.join(RepositoryRoot, "External/GameNetworkingSockets/include") }

-- Servers and clients of the authoritative game session: handshake, input, snapshot deltas, relevancy, prediction, and interpolation.
HertaRuntimeModule("Replication", {
    PublicDependencies = { "Core", "Math", "GameFramework", "Networking", "Application", "Assets", "Audio", "Level", "Navigation", "Particles", "Physics", "RHI", "Renderer", "GameUI" }
})

-- miniaudio opens devices and decodes sources; Herta mixes. Its Linux backends load at runtime, so only the loader and threads link.
HertaRuntimeModule("Audio", {
    PublicDependencies = { "Math", "Assets" },
    PrivateDependencies = { "Core" },
    PrivateLinuxSystemDependencies = { "dl", "m", "pthread" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/miniaudio") }

HertaRuntimeModule("EditorCore", {
    PublicDependencies = { "Math" },
    PrivateDependencies = { "Core", "Level", "Assets", "Project", "Platform" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson") }

HertaRuntimeModule("Application", {
    PrivateDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "GLFW" },
    PrivateLinuxSystemDependencies = { "X11", "dl", "m", "pthread", "rt" },
    PrivateWindowsSystemDependencies = { "dxgi", "gdi32", "shell32", "user32" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/glfw/include")
    }

HertaRuntimeModule("ToolUI", {
    PublicDependencies = { "Localization" },
    PrivateDependencies = { "Core", "Application", "TextLayout" },
    PrivateThirdPartyDependencies = { "ImGui", "FreeType", "GLFW" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/freetype/include"),
        path.join(RepositoryRoot, "External/glfw/include")
    }

-- Unicode text: grapheme segmentation and editing, HarfBuzz shaping with font fallback, and FreeType rasterization.
HertaRuntimeModule("TextLayout", {
    PublicDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "FreeType", "HarfBuzz" }
})

    externalincludedirs { path.join(RepositoryRoot, "External/freetype/include"), path.join(RepositoryRoot, "External/harfbuzz/src"), path.join(RepositoryRoot, "External/unicode") }

HertaRuntimeModule("GameUI", {
    PublicDependencies = { "Core", "Math", "RHI" },
    PrivateDependencies = { "TextLayout" }
})

HertaRuntimeModule("GameFramework", {
    PublicDependencies = { "Core", "Math", "Application", "Assets", "Audio", "Level", "Localization", "Navigation", "Particles", "Physics", "RHI", "Renderer", "GameUI" },
    PrivateDependencies = { "Tasks", "Animation", "Platform", "AI" }
})
    -- Publisher keys the game trusts its packages to be signed with: one "<id>:<64 hex digits>" per line. Only public keys belong here; a
    -- Shipping build that lists any plays only packages signed with them.
    local PublicKeysFile = path.join(RepositoryRoot, "Config/PackagePublicKeys.txt")
    if os.isfile(PublicKeysFile) then
        local Keys = {}
        for Line in io.lines(PublicKeysFile) do
            local Key = Line:gsub("#.*", ""):match("^%s*(.-)%s*$")
            if Key ~= "" then
                if not Key:match("^%d+:" .. string.rep("%x", 64) .. "$") then
                    error("Config/PackagePublicKeys.txt: '" .. Key .. "' is not a package public key <id>:<64 hex digits>")
                end

                table.insert(Keys, Key)
            end
        end

        defines { 'HERTA_PACKAGE_PUBLIC_KEYS="' .. table.concat(Keys, ",") .. '"' }
    end

    -- The fixed-step simulation feeds Jolt, so it must not contract floating point either.
    filter "system:linux"
        buildoptions { "-ffp-contract=off" }

    filter {}

-- Particle effects simulated within fixed budgets; their renderer view entries come from Renderer.
HertaRuntimeModule("Particles", {
    PublicDependencies = { "Math", "Assets", "Level", "RHI", "Renderer" }
})

-- Builds navigation tiles with Recast for the editor, headless commands, and tests; games load the cooked tiles through Navigation.
-- Writes and signs content packages; players only read them, through Assets.
HertaRuntimeModule("PackageBuilder", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/PackageBuilder"),
    PublicDependencies = { "Core", "Assets" },
    PrivateDependencies = { "Platform" },
    PrivateThirdPartyDependencies = { "Zstd", "Monocypher" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/zstd/lib"), path.join(RepositoryRoot, "External/monocypher") }

HertaRuntimeModule("NavigationBuilder", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/NavigationBuilder"),
    PublicDependencies = { "Core", "Math", "Assets", "Level", "Navigation" },
    PrivateDependencies = { "Tasks" },
    PrivateThirdPartyDependencies = { "Recast", "Detour" }
})
    defines { "DT_POLYREF64" }
    includedirs { path.join(RepositoryRoot, "Engine/Source/Runtime/Navigation/Private") }
    externalincludedirs {
        path.join(RepositoryRoot, "External/recastnavigation/Recast/Include"),
        path.join(RepositoryRoot, "External/recastnavigation/Detour/Include"),
        path.join(RepositoryRoot, "External/recastnavigation/DetourTileCache/Include")
    }

-- The Material Graph schema and its compiler to Slang, run by the editor, headless commands, and tests; game targets load cooked shaders instead.
HertaRuntimeModule("MaterialGraph", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/MaterialGraph"),
    PublicDependencies = { "GraphCore", "Assets" }
})

-- The Animation graph schema and its compiler to state machine data, run by the editor, the asset cooker, and tests; games load the cooked data.
HertaRuntimeModule("AnimationGraph", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/AnimationGraph"),
    PublicDependencies = { "GraphCore", "Assets" }
})

-- The Audio Graph schema and its compiler to sound graph data, run by the editor, the asset cooker, and tests; games play the cooked data.
HertaRuntimeModule("AudioGraph", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/AudioGraph"),
    PublicDependencies = { "GraphCore", "Assets" }
})

-- The StateTree schema and its compiler to contiguous StateTree programs, run by the editor, the asset cooker, and tests; games run the cooked programs.
HertaRuntimeModule("StateTreeCompiler", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/StateTreeCompiler"),
    PublicDependencies = { "GraphCore", "Assets" }
})

-- The VFX Graph schema and its compiler to particle systems, run by the editor, the asset cooker, and tests; games play the cooked data.
HertaRuntimeModule("VfxGraph", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/VfxGraph"),
    PublicDependencies = { "GraphCore", "Assets" }
})

-- The gameplay graph schema, its compiler to flat programs, and the C++ generator for them, run by the editor, the asset cooker, and tests;
-- games run the cooked programs through GameFramework.
HertaRuntimeModule("GameplayGraph", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/GameplayGraph"),
    PublicDependencies = { "GraphCore", "Assets" },
    PrivateDependencies = { "Level", "Math" }
})

-- Validates catalogs and generates text ID headers, without ToolUI or a GPU.
HertaRuntimeModule("LocalizationPipeline", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/LocalizationPipeline"),
    PublicDependencies = { "Localization" },
    PrivateDependencies = { "Assets" }
})

HertaRuntimeModule("AssetPipeline", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/AssetPipeline"),
    PublicDependencies = { "Core", "Assets", "EditorCore", "Level", "Math", "Navigation", "NavigationBuilder" },
    PrivateDependencies = { "Platform", "Project", "GraphCore", "MaterialGraph", "AnimationGraph", "AudioGraph", "VfxGraph", "GameplayGraph", "StateTreeCompiler", "Audio", "Animation", "Localization", "LocalizationPipeline" },
    PrivateThirdPartyDependencies = { "FastGltf", "SimdJson", "MeshOptimizer", "OzzAnimationOffline", "OzzAnimation", "OzzBase" }
})
    externalincludedirs {
        path.join(RepositoryRoot, "External/ozz-animation/include"),
        path.join(RepositoryRoot, "External/fastgltf/include"),
        path.join(RepositoryRoot, "External/simdjson"),
        path.join(RepositoryRoot, "External/meshoptimizer/src"),
        path.join(RepositoryRoot, "External/stb")
    }

-- The first-party node canvas: drawing and editing any schema's graph, with storage, undo, and semantics left to each domain's panel.
HertaEditorModule("GraphEditor", {
    PublicDependencies = { "GraphCore", "ToolUI" },
    PrivateThirdPartyDependencies = { "ImGui" }
})

    externalincludedirs { path.join(RepositoryRoot, "External/imgui") }

HertaEditorModule("EditorFramework", {
    PublicDependencies = { "Core", "Math", "EditorCore", "ToolUI", "RHI", "Renderer", "Assets", "Project" },
    PrivateDependencies = { "Application", "Audio", "AudioGraph", "VfxGraph", "GameplayGraph", "StateTreeCompiler", "Navigation", "NavigationBuilder", "Particles", "GameFramework", "GameUI", "GraphCore", "GraphEditor", "MaterialGraph", "AnimationGraph", "Physics", "Platform", "Tasks", "AssetPipeline", "Level", "Localization", "LocalizationPipeline", "TextLayout" },
    PrivateThirdPartyDependencies = { "ImGui", "Im3d", "SimdJson" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/simdjson"),
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/im3d")
    }

HertaRuntimeModule("RHI")

HertaRuntimeModule("RenderGraph", {
    PublicDependencies = { "RHI" }
})

HertaRuntimeModule("Renderer", {
    PublicDependencies = { "RHI", "Math", "Assets", "Level" },
    PrivateDependencies = { "RenderGraph" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/smaa/Textures"), path.join(RepositoryRoot, "External/ltc"), path.join(RepositoryRoot, "External") }

local VulkanSdk = HertaGetVulkanSdk()
HertaRuntimeModule("NvrhiVulkan", {
    PublicDependencies = { "RHI" },
    PrivateDependencies = { "Core", "Platform" },
    PrivateThirdPartyDependencies = { "NVRHIVulkanBackend", "NVRHI", "GLFW", "ImGui" },
    PrivateLibraryDirectories = { VulkanSdk.LibraryDirectory },
    PrivateLinuxSystemDependencies = { "vulkan", "dl", "m", "pthread", "rt", "X11", "Xrandr", "Xinerama", "Xcursor", "Xi", "wayland-client", "xkbcommon" },
    PrivateWindowsSystemDependencies = { "vulkan-1", "gdi32", "shell32", "user32" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/glfw/include"),
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/NVRHI/include"),
        VulkanSdk.IncludeDirectory
    }

    filter "system:windows"
        defines { "VK_USE_PLATFORM_WIN32_KHR" }

    filter {}

    files {
        path.join(RuntimeRoot, "NvrhiVulkan/Private/Shaders/**.frag"),
        path.join(RuntimeRoot, "NvrhiVulkan/Private/Shaders/**.vert")
    }

local SlangLibraryDirectory = VulkanSdk.LibraryDirectory
if os.host() == "linux" then
    SlangLibraryDirectory = VulkanSdk.RuntimeLibraryDirectory
end

HertaRuntimeModule("ShaderCompiler", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/ShaderCompiler"),
    PublicDependencies = { "RHI" },
    PrivateDependencies = { "Platform" },
    PrivateThirdPartyDependencies = { "slang-compiler" },
    PrivateLibraryDirectories = { SlangLibraryDirectory }
})
    externalincludedirs { path.join(VulkanSdk.IncludeDirectory, "slang") }

project "HertaAssetWorker"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaAssetWorker"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaAssetWorker"))
    ApplyRuntimeDependencies { "AssetPipeline" }

project "HertaShaderWorker"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaShaderWorker"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaShaderWorker"))
    ApplyRuntimeDependencies { "ShaderCompiler" }
    filter "system:windows"
        postbuildcommands { '{COPYFILE} "' .. path.join(VulkanSdk.BinaryDirectory, "slang-compiler.dll") .. '" "%{cfg.targetdir}"' }
    filter "system:linux"
        linkoptions { '-Wl,-rpath,"' .. SlangLibraryDirectory .. '"' }
    filter {}

local ShaderOutput = path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/Shaders")
local ShaderWorker = path.join(ShaderOutput, "../HertaShaderWorker") .. (os.host() == "windows" and ".exe" or "")
local ShaderInputs = os.matchfiles(path.join(RepositoryRoot, "Engine/Shaders/**"))
table.insert(ShaderInputs, path.join(RepositoryRoot, "External/smaa/SMAA.hlsl"))
table.insert(ShaderInputs, ShaderWorker)
project "HertaShaders"
    kind "Utility"
    location(path.join(ProjectFilesRoot, "HertaShaders"))
    dependson { "HertaShaderWorker" }
    files { path.join(RepositoryRoot, "Engine/Shaders/*.slang"), path.join(RepositoryRoot, "Engine/Shaders/Compute/*.slang") }
    filter "files:**.slang"
        buildmessage "Cooking %{file.basename} shaders"
        buildinputs(ShaderInputs)
    filter { "files:**.slang", "files:not **/Compute/*.slang" }
        buildoutputs { path.join(ShaderOutput, "%{file.basename}.vert.hshader"), path.join(ShaderOutput, "%{file.basename}.frag.hshader") }
    filter { "files:**.slang", "files:not **/Compute/*.slang", "configurations:Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex vertexMain "' .. path.join(ShaderOutput, "%{file.basename}.vert.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment fragmentMain "' .. path.join(ShaderOutput, "%{file.basename}.frag.hshader") .. '"'
        }
    filter { "files:**.slang", "files:not **/Compute/*.slang", "configurations:not Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex vertexMain "' .. path.join(ShaderOutput, "%{file.basename}.vert.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment fragmentMain "' .. path.join(ShaderOutput, "%{file.basename}.frag.hshader") .. '" --debug'
        }
    filter "files:**/TexturedMesh.slang"
        buildoutputs { path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader"), path.join(ShaderOutput, "TexturedMesh.velocity.frag.hshader"), path.join(ShaderOutput, "TexturedMesh.instanced.velocity.vert.hshader"), path.join(ShaderOutput, "TexturedMesh.surface.frag.hshader"), path.join(ShaderOutput, "TexturedMesh.velocity.surface.frag.hshader") }
    filter { "files:**/TexturedMesh.slang", "configurations:Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment velocityFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.velocity.frag.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVelocityVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.velocity.vert.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment surfaceFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.surface.frag.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment velocitySurfaceFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.velocity.surface.frag.hshader") .. '"'
        }
    filter { "files:**/TexturedMesh.slang", "configurations:not Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment velocityFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.velocity.frag.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVelocityVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.velocity.vert.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment surfaceFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.surface.frag.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment velocitySurfaceFragmentMain "' .. path.join(ShaderOutput, "TexturedMesh.velocity.surface.frag.hshader") .. '" --debug'
        }
    -- Compute shaders live in their own folder and cook a single computeMain entry point.
    filter "files:**/Compute/*.slang"
        buildoutputs { path.join(ShaderOutput, "%{file.basename}.comp.hshader") }
    filter { "files:**/Compute/*.slang", "configurations:Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" compute computeMain "' .. path.join(ShaderOutput, "%{file.basename}.comp.hshader") .. '"'
        }
    filter { "files:**/Compute/*.slang", "configurations:not Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" compute computeMain "' .. path.join(ShaderOutput, "%{file.basename}.comp.hshader") .. '" --debug'
        }
    filter "files:**/Shadow.slang"
        buildoutputs { path.join(ShaderOutput, "Shadow.instanced.vert.hshader") }
    filter { "files:**/Shadow.slang", "configurations:Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "Shadow.instanced.vert.hshader") .. '"'
        }
    filter { "files:**/Shadow.slang", "configurations:not Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "Shadow.instanced.vert.hshader") .. '" --debug'
        }
    filter {}

-- The same test game module built with two state versions, so HertaTests can reload one that keeps the running state and one that cannot.
HertaGameModule("HertaTestModule", path.join(ProgramsRoot, "HertaTestModule"), { "HERTA_TEST_MODULE_STATE_VERSION=1" })
HertaGameModule("HertaTestModuleV2", path.join(ProgramsRoot, "HertaTestModule"), { "HERTA_TEST_MODULE_STATE_VERSION=2" })

project "HertaTests"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaTests"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaTests"))
    includedirs { path.join(RepositoryRoot, "Engine/Source/Editor/EditorFramework/Private") }

    externalincludedirs {
        path.join(RepositoryRoot, "External/doctest"),
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/im3d"),
        path.join(RepositoryRoot, "External/stb"),
        path.join(RepositoryRoot, "External/entt/src")
    }

    ApplyRuntimeDependencies { "Core", "Math", "Localization", "LocalizationPipeline", "TextLayout", "AI", "Animation", "Audio", "Navigation", "Networking", "Replication", "NavigationBuilder", "PackageBuilder", "Particles", "GraphCore", "GraphEditor", "MaterialGraph", "AnimationGraph", "AudioGraph", "VfxGraph", "GameplayGraph", "StateTreeCompiler", "Level", "Project", "Physics", "Platform", "Tasks", "Application", "Assets", "AssetPipeline", "EditorCore", "ToolUI", "EditorFramework", "GameFramework", "GameUI", "RHI", "RenderGraph", "Renderer", "ShaderCompiler" }
    dependson { "HertaShaderWorker", "HertaAssetWorker", "HertaEditorCmd", "HertaServer", "HertaTestModule", "HertaTestModuleV2" }
    filter "system:linux"
        linkoptions { '-Wl,-rpath,"' .. SlangLibraryDirectory .. '"' }
    filter {}

-- The MCP stdio server coding agents launch; it reaches a running editor through EditorCore automation and never links the editor itself.
-- A running exe is locked on Windows but can still be renamed, so the build moves it aside when it is locked and links a new one; old copies
-- are deleted once nothing runs them. An unlocked exe stays in place, so incremental linking can patch it.
local function AllowRelinkWhileRunning()
    filter "system:windows"
        prelinkcommands {
            'del /q "$(TargetPath).old-*" >nul 2>nul || cd .',
            'if exist "$(TargetPath)" ((call ) 2>nul >>"$(TargetPath)" || move "$(TargetPath)" "$(TargetPath).old-%RANDOM%%RANDOM%" >nul 2>nul || cd .)',
        }

    filter {}
end

project "HertaEditorMcp"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditorMcp"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditorMcp"))
    externalincludedirs { path.join(RepositoryRoot, "External/simdjson") }
    ApplyRuntimeDependencies { "Core", "EditorCore", "Platform" }

    -- MCP clients keep the server running for a whole session.
    AllowRelinkWhileRunning()

project "HertaEditorCmd"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditorCmd"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditorCmd"))
    ApplyRuntimeDependencies { "EditorCore", "Level", "Project", "AssetPipeline", "Platform" }
    dependson { "HertaAssetWorker", "HertaShaderWorker" }

-- Cooks a game's content for distribution: staged loose files, external packages, or packages embedded in the executables.
project "HertaCooker"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaCooker"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaCooker"))
    ApplyRuntimeDependencies { "AssetPipeline", "PackageBuilder", "Assets", "Localization", "Platform" }
    dependson { "HertaAssetWorker" }

project "HertaGame"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaGame"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaGame"))
    debugdir(RepositoryRoot)
    ApplyRuntimeDependencies { "Application", "Tasks", "Audio", "GameFramework", "GameAutomation", "EditorCore", "Replication", "Networking", "GameUI", "NvrhiVulkan", "Renderer", "Math", "Platform", "Project", "Level", "Assets", "Physics" }
    dependson { "HertaShaders" }

    -- Shipping players should not see a console; development builds keep it for logs.
    filter { "system:windows", "configurations:Shipping" }
        kind "WindowedApp"
        entrypoint "mainCRTStartup"

    filter { "system:windows", "configurations:Debug or Debug-ASan or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ";%PATH%"
        }

    filter { "system:linux", "configurations:Debug or Debug-ASan or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ":$PATH",
            "LD_LIBRARY_PATH=" .. VulkanSdk.RuntimeLibraryDirectory .. ":$LD_LIBRARY_PATH"
        }

    filter {}

-- A dedicated server: the game session and replication with no window, renderer device, ImGui, or audio device.
project "HertaServer"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaServer"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaServer"))
    debugdir(RepositoryRoot)
    ApplyRuntimeDependencies { "Replication", "Networking", "GameFramework", "GameAutomation", "EditorCore", "Tasks", "Math", "Platform", "Project", "Level", "Assets", "Physics" }

    -- GameFramework links window and audio libraries for the game; the server never calls them, so the linker leaves them out.
    filter "system:linux"
        linkoptions { "-Wl,--as-needed" }

    filter {}

project "Herta"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "Herta"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "Herta"))
    debugdir(RepositoryRoot)
    ApplyRuntimeDependencies { "Application", "Tasks", "EditorFramework", "NvrhiVulkan", "Renderer", "Math", "Platform", "Project", "AssetPipeline", "GraphCore", "MaterialGraph", "AnimationGraph" }
    dependson { "HertaShaders", "HertaAssetWorker", "HertaShaderWorker" }
    externalincludedirs { path.join(RepositoryRoot, "External/stb") }
    -- The editor can stay open while it is rebuilt.
    AllowRelinkWhileRunning()

    files {
        path.join(RepositoryRoot, "Engine/Content/Editor/Icons/Herta.svg")
    }

    filter "system:windows"
        files {
            path.join(ProgramsRoot, "Herta/Resources/Windows/Herta.ico"),
            path.join(ProgramsRoot, "Herta/Resources/Windows/Herta.rc")
        }

    filter { "system:windows", "configurations:Debug or Debug-ASan or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ";%PATH%"
        }

    filter { "system:linux", "configurations:Debug or Debug-ASan or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ":$PATH",
            "LD_LIBRARY_PATH=" .. VulkanSdk.RuntimeLibraryDirectory .. ":$LD_LIBRARY_PATH"
        }

    filter {}

if HertaProjectDescriptor then
    local BuildIdentityDirectory = path.join(HertaProjectRoot, "Intermediate")
    os.mkdir(BuildIdentityDirectory)
    local BuildIdentityFile, BuildIdentityError = io.open(path.join(BuildIdentityDirectory, "HertaEngineBuild.json"), "wb")
    if not BuildIdentityFile then
        error("Cannot record the native module engine build identity: " .. tostring(BuildIdentityError))
    end

    BuildIdentityFile:write(json.encode({ engineAssociation = "Herta", revision = GetBuildRevision(true) }))
    BuildIdentityFile:write("\n")
    BuildIdentityFile:close()

    for _, Module in ipairs(HertaProjectDescriptor.modules) do
        local ReservedNames = { "NVRHI", "NVRHIVulkanBackend", "MeshOptimizer", "Jolt", "FreeType", "FastGltf", "GLFW", "EnkiTS", "ImGui", "Im3d", "Spdlog", "SimdJson", "Herta", "HertaEditorCmd", "HertaGame", "HertaTests", "HertaAssetWorker", "HertaShaderWorker", "HertaShaders" }
        for Name in pairs(RuntimeModules) do
            table.insert(ReservedNames, Name)
        end

        local Conflicts = false
        for _, Name in ipairs(ReservedNames) do
            Conflicts = Conflicts or Module.name:lower() == Name:lower()
        end

        if Conflicts then
            error("Project module name conflicts with an engine module: " .. Module.name)
        end

        -- Each project module is a game module; its dependencies are the engine headers it includes, never libraries it links.
        HertaGameModule(Module.name, path.join(HertaProjectRoot, Module.source))
        RuntimeModules[Module.name] = { SourceRoot = path.join(HertaProjectRoot, Module.source) }
        targetdir(path.join(HertaProjectRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(HertaProjectRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
    end

    -- A project's own checks, such as its levels staying canonical and its graphs cooking, in Source/Tests with their own doctest main.
    local TestsRoot = path.join(HertaProjectRoot, "Source/Tests")
    if os.isdir(TestsRoot) then
        local TestsName = HertaProjectDescriptor.name .. "Tests"
        project(TestsName)
            kind "ConsoleApp"
            location(path.join(ProjectFilesRoot, TestsName))
            ApplyCommonProjectSettings(TestsRoot)
            targetdir(path.join(HertaProjectRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
            objdir(path.join(HertaProjectRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
            debugdir(HertaProjectRoot)
            externalincludedirs { path.join(RepositoryRoot, "External/doctest") }
            ApplyRuntimeDependencies { "Core", "Math", "Localization", "TextLayout", "AI", "Animation", "Audio", "Navigation", "Networking", "Particles", "GraphCore", "MaterialGraph", "AnimationGraph", "AudioGraph", "VfxGraph", "GameplayGraph", "StateTreeCompiler", "NavigationBuilder", "LocalizationPipeline", "Level", "Project", "Physics", "Platform", "Tasks", "Application", "Assets", "AssetPipeline", "EditorCore", "GameFramework", "GameUI", "RHI", "RenderGraph", "Renderer", "ShaderCompiler" }
            local ProjectModules = { "HertaAssetWorker" }
            for _, Module in ipairs(HertaProjectDescriptor.modules) do
                table.insert(ProjectModules, Module.name)
            end

            -- Project modules are loaded at run time, as the editor loads them, so the tests only build them first.
            dependson(ProjectModules)
            filter "system:linux"
                linkoptions { '-Wl,-rpath,"' .. SlangLibraryDirectory .. '"' }
            filter {}
    end
end
