local RepositoryRoot = _MAIN_SCRIPT_DIR
local SourceRoot = path.join(RepositoryRoot, "Engine/Source")
local RuntimeRoot = path.join(SourceRoot, "Runtime")
local ProgramsRoot = path.join(SourceRoot, "Programs")
local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

local function ApplyCommonProjectSettings(ProjectSourceRoot)
    targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
    objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))

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

local function HertaRuntimeModule(ModuleName)
    local ModuleSourceRoot = path.join(RuntimeRoot, ModuleName)
    project(ModuleName)
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, ModuleName))
        ApplyCommonProjectSettings(ModuleSourceRoot)
end

HertaRuntimeModule("Core")

HertaRuntimeModule("Math")

HertaRuntimeModule("Platform")
    filter "system:windows"
        removefiles {
            RuntimeRoot .. "/Platform/Private/Linux/**"
        }

    filter "system:linux"
        removefiles {
            RuntimeRoot .. "/Platform/Private/Windows/**"
        }

    filter {}

project "HertaTests"
    kind "ConsoleApp"
    location(path.join(ProjectFilesRoot, "HertaTests"))
    ApplyCommonProjectSettings(path.join(ProgramsRoot, "HertaTests"))

    includedirs {
        RuntimeRoot .. "/Core/Public",
        RuntimeRoot .. "/Math/Public",
        RuntimeRoot .. "/Platform/Public"
    }

    externalincludedirs {
        path.join(RepositoryRoot, "External/doctest")
    }

    links {
        "Core",
        "Math",
        "Platform"
    }
