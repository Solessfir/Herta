local RepositoryRoot = _MAIN_SCRIPT_DIR
local SourceRoot = path.join(RepositoryRoot, "Engine/Source")
local RuntimeRoot = path.join(SourceRoot, "Runtime")
local EditorRoot = path.join(SourceRoot, "Editor")
local ProgramsRoot = path.join(SourceRoot, "Programs")
local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")
local RuntimeModules = {}

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
    PrivateDefinitions = { "SPDLOG_COMPILED_LIB" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/spdlog/include")
    }

    filter "system:windows"
        defines { "SPDLOG_WCHAR_FILENAMES" }

    filter {}

HertaRuntimeModule("Math", {
    PublicDependencies = { "Core" }
})

HertaRuntimeModule("Platform", {
    PublicDependencies = { "Core" }
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

HertaRuntimeModule("EditorCore")

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
    PrivateDependencies = { "Application" },
    PrivateThirdPartyDependencies = { "ImGui", "FreeType", "GLFW" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/imgui"),
        path.join(RepositoryRoot, "External/freetype/include"),
        path.join(RepositoryRoot, "External/glfw/include")
    }

HertaEditorModule("EditorFramework", {
    PublicDependencies = { "Core", "EditorCore", "ToolUI" },
    PrivateThirdPartyDependencies = { "ImGui" }
})

    externalincludedirs {
        path.join(RepositoryRoot, "External/imgui")
    }

HertaRuntimeModule("RHI")

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

project "HertaTests"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaTests"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaTests"))

    externalincludedirs {
        path.join(RepositoryRoot, "External/doctest")
    }

    ApplyRuntimeDependencies { "Core", "Math", "Platform", "Tasks", "Application", "EditorCore", "ToolUI", "EditorFramework", "RHI" }

project "HertaEditorCmd"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditorCmd"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditorCmd"))
    ApplyRuntimeDependencies { "EditorCore" }

project "HertaEditor"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaEditor"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaEditor"))
    debugdir(RepositoryRoot)
    ApplyRuntimeDependencies { "Application", "Tasks", "EditorFramework", "NvrhiVulkan" }

    files {
        path.join(RepositoryRoot, "Engine/Content/Editor/Icons/Herta.svg")
    }

    filter "system:windows"
        files {
            path.join(ProgramsRoot, "HertaEditor/Resources/Windows/HertaEditor.ico"),
            path.join(ProgramsRoot, "HertaEditor/Resources/Windows/HertaEditor.rc")
        }

    filter { "system:windows", "configurations:Debug or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ";%PATH%"
        }

    filter { "system:linux", "configurations:Debug or Development" }
        debugenvs {
            "VULKAN_SDK=" .. VulkanSdk.Root,
            "VK_ADD_LAYER_PATH=" .. VulkanSdk.ValidationLayerDirectory,
            "PATH=" .. VulkanSdk.BinaryDirectory .. ":$PATH",
            "LD_LIBRARY_PATH=" .. VulkanSdk.RuntimeLibraryDirectory .. ":$LD_LIBRARY_PATH"
        }

    filter {}
