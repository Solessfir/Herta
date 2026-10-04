local RepositoryRoot = _MAIN_SCRIPT_DIR
local SourceRoot = path.join(RepositoryRoot, "Engine/Source")
local RuntimeRoot = path.join(SourceRoot, "Runtime")
local EditorRoot = path.join(SourceRoot, "Editor")
local ProgramsRoot = path.join(SourceRoot, "Programs")
local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")
local RuntimeModules = {}

local function GetBuildRevision()
    local NullDevice = os.host() == "windows" and "nul" or "/dev/null"
    local Command = 'git -C "' .. RepositoryRoot .. '" rev-parse --short=8 HEAD 2>' .. NullDevice
    local Success, Output = pcall(os.outputof, Command)
    local Revision = Success and Output and Output:match("^%s*([0-9a-fA-F]+)%s*$")
    return Revision and #Revision >= 7 and #Revision <= 40 and Revision or "unknown"
end

local function ApplyCommonProjectSettings(ProjectSourceRoot)
    targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
    objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
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

    for ModuleName in pairs(VisibleModules) do
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
    PrivateDefinitions = { "SPDLOG_COMPILED_LIB", 'HERTA_BUILD_REVISION="' .. GetBuildRevision() .. '"' }
})

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
    PublicDependencies = { "Core" }
})

HertaRuntimeModule("Scene", {
    PublicDependencies = { "Math", "Assets" },
    PrivateThirdPartyDependencies = { "SimdJson" }
})
    externalincludedirs {
        path.join(RepositoryRoot, "External/entt/src"),
        path.join(RepositoryRoot, "External/simdjson")
    }

HertaRuntimeModule("Physics", {
    PublicDependencies = { "Math" },
    PrivateThirdPartyDependencies = { "Jolt" }
})
    externalincludedirs { path.join(RepositoryRoot, "External/Jolt") }

    filter "configurations:Debug or Debug-ASan"
        defines { "JPH_ENABLE_ASSERTS" }

    filter "configurations:Development or Shipping"
        defines { "JPH_NO_DEBUG" }

    filter {}

HertaRuntimeModule("Platform", {
    PublicDependencies = { "Core" },
    PrivateWindowsSystemDependencies = { "ole32" }
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

HertaRuntimeModule("EditorCore", {
    PublicDependencies = { "Math" },
    PrivateDependencies = { "Scene", "Assets" }
})

HertaRuntimeModule("Application", {
    PrivateDependencies = { "Core" },
    PrivateThirdPartyDependencies = { "GLFW" },
    PrivateLinuxSystemDependencies = { "X11", "dl", "m", "pthread", "rt" },
    PrivateWindowsSystemDependencies = { "gdi32", "shell32", "user32" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/glfw/include")
    }

HertaRuntimeModule("ToolUI", {
    PrivateDependencies = { "Core", "Application" },
    PrivateThirdPartyDependencies = { "ImGui", "FreeType", "GLFW" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/freetype/include"),
        path.join(RepositoryRoot, "External/glfw/include")
    }

HertaRuntimeModule("AssetPipeline", {
    SourceRoot = path.join(RepositoryRoot, "Engine/Source/Developer/AssetPipeline"),
    PublicDependencies = { "Core", "Assets", "EditorCore" },
    PrivateDependencies = { "Platform" },
    PrivateThirdPartyDependencies = { "FastGltf", "SimdJson", "MeshOptimizer" }
})
    externalincludedirs {
        path.join(RepositoryRoot, "External/fastgltf/include"),
        path.join(RepositoryRoot, "External/simdjson"),
        path.join(RepositoryRoot, "External/meshoptimizer/src"),
        path.join(RepositoryRoot, "External/stb")
    }

HertaEditorModule("EditorFramework", {
    PublicDependencies = { "Core", "Math", "EditorCore", "ToolUI", "RHI", "Renderer", "Assets" },
    PrivateDependencies = { "Physics", "Platform", "Tasks", "AssetPipeline", "Scene" },
    PrivateThirdPartyDependencies = { "ImGui", "Im3d" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/im3d")
    }

HertaRuntimeModule("RHI")

HertaRuntimeModule("RenderGraph", {
    PublicDependencies = { "RHI" }
})

HertaRuntimeModule("Renderer", {
    PublicDependencies = { "RHI", "Math", "Assets" },
    PrivateDependencies = { "RenderGraph" }
})

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

local ShaderOutput = path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/Shaders")
local ShaderWorker = path.join(ShaderOutput, "../HertaShaderWorker") .. (os.host() == "windows" and ".exe" or "")
local ShaderInputs = os.matchfiles(path.join(RepositoryRoot, "Engine/Shaders/**"))
table.insert(ShaderInputs, ShaderWorker)
project "HertaShaders"
    kind "Utility"
    location(path.join(ProjectFilesRoot, "HertaShaders"))
    dependson { "HertaShaderWorker" }
    files { path.join(RepositoryRoot, "Engine/Shaders/TexturedMesh.slang"), path.join(RepositoryRoot, "Engine/Shaders/DebugDraw.slang"), path.join(RepositoryRoot, "Engine/Shaders/WorldGrid.slang") }
    filter "files:**.slang"
        buildmessage "Cooking %{file.basename} shaders"
        buildinputs(ShaderInputs)
        buildoutputs { path.join(ShaderOutput, "%{file.basename}.vert.hshader"), path.join(ShaderOutput, "%{file.basename}.frag.hshader") }
    filter { "files:**.slang", "configurations:Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex vertexMain "' .. path.join(ShaderOutput, "%{file.basename}.vert.hshader") .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment fragmentMain "' .. path.join(ShaderOutput, "%{file.basename}.frag.hshader") .. '"'
        }
    filter { "files:**.slang", "configurations:not Shipping" }
        buildcommands {
            '{MKDIR} "' .. ShaderOutput .. '"',
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex vertexMain "' .. path.join(ShaderOutput, "%{file.basename}.vert.hshader") .. '" --debug',
            '"' .. ShaderWorker .. '" "%{file.abspath}" fragment fragmentMain "' .. path.join(ShaderOutput, "%{file.basename}.frag.hshader") .. '" --debug'
        }
    filter "files:**/TexturedMesh.slang"
        buildoutputs { path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader") }
    filter { "files:**/TexturedMesh.slang", "configurations:Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader") .. '"'
        }
    filter { "files:**/TexturedMesh.slang", "configurations:not Shipping" }
        buildcommands {
            '"' .. ShaderWorker .. '" "%{file.abspath}" vertex instancedVertexMain "' .. path.join(ShaderOutput, "TexturedMesh.instanced.vert.hshader") .. '" --debug'
        }
    filter {}

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

    ApplyRuntimeDependencies { "Core", "Math", "Scene", "Physics", "Platform", "Tasks", "Application", "Assets", "AssetPipeline", "EditorCore", "ToolUI", "EditorFramework", "RHI", "RenderGraph", "Renderer", "ShaderCompiler" }
    dependson { "HertaShaderWorker", "HertaAssetWorker", "HertaEditorCmd" }
    filter "system:linux"
        linkoptions { '-Wl,-rpath,"' .. SlangLibraryDirectory .. '"' }
    filter {}

project "HertaEditorCmd"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditorCmd"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditorCmd"))
    ApplyRuntimeDependencies { "EditorCore", "Scene", "AssetPipeline", "Platform" }
    dependson { "HertaAssetWorker" }

project "HertaEditor"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditor"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditor"))
    debugdir(RepositoryRoot)
    ApplyRuntimeDependencies { "Application", "Tasks", "EditorFramework", "NvrhiVulkan", "Renderer", "Math", "Platform" }
    dependson { "HertaShaders", "HertaAssetWorker" }

    files {
        path.join(RepositoryRoot, "Engine/Content/Editor/Icons/Herta.svg")
    }

    filter "system:windows"
        files {
            path.join(ProgramsRoot, "HertaEditor/Resources/Windows/HertaEditor.ico"),
            path.join(ProgramsRoot, "HertaEditor/Resources/Windows/HertaEditor.rc")
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
