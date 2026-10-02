function HertaMeshOptimizer()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local MeshOptimizerRoot = path.join(RepositoryRoot, "External/meshoptimizer/src")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "MeshOptimizer"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "MeshOptimizer"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"
        files { path.join(MeshOptimizerRoot, "*.cpp"), path.join(MeshOptimizerRoot, "meshoptimizer.h") }
        externalincludedirs { MeshOptimizerRoot }
end
