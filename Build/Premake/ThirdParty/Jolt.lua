function HertaJolt()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local JoltRoot = path.join(RepositoryRoot, "External/Jolt/Jolt")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "Jolt"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "Jolt"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        files {
            path.join(JoltRoot, "**.cpp"),
            path.join(JoltRoot, "**.h")
        }

        removefiles {
            path.join(JoltRoot, "Compute/CPU/**"),
            path.join(JoltRoot, "Compute/DX12/**"),
            path.join(JoltRoot, "Compute/VK/**"),
            path.join(JoltRoot, "ObjectStream/ObjectStream*.cpp"),
            path.join(JoltRoot, "ObjectStream/TypeDeclarations.cpp"),
            path.join(JoltRoot, "Shaders/**")
        }

        externalincludedirs { path.join(RepositoryRoot, "External/Jolt") }

        filter "configurations:Debug or Debug-ASan"
            defines { "JPH_ENABLE_ASSERTS" }

        filter "configurations:Development or Shipping"
            defines { "JPH_NO_DEBUG" }

        filter {}
end
