function HertaIm3d()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local Im3dRoot = path.join(RepositoryRoot, "External/im3d")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "Im3d"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "Im3d"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"
        files { path.join(Im3dRoot, "im3d.cpp"), path.join(Im3dRoot, "im3d*.h") }
        externalincludedirs { Im3dRoot }
end
