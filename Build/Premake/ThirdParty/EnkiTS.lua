function HertaEnkiTS()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local EnkiRoot = path.join(RepositoryRoot, "External/enkiTS")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "EnkiTS"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "EnkiTS"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        files {
            path.join(EnkiRoot, "src/TaskScheduler.cpp"),
            path.join(EnkiRoot, "src/TaskScheduler.h"),
            path.join(EnkiRoot, "src/LockLessMultiReadPipe.h")
        }

        externalincludedirs {
            path.join(EnkiRoot, "src")
        }

        filter "system:linux"
            links { "pthread" }

        filter {}
end
