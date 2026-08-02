function HertaSpdlog()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local SpdlogRoot = path.join(RepositoryRoot, "External/spdlog")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "Spdlog"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "Spdlog"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        files {
            path.join(SpdlogRoot, "src/async.cpp"),
            path.join(SpdlogRoot, "src/bundled_fmtlib_format.cpp"),
            path.join(SpdlogRoot, "src/cfg.cpp"),
            path.join(SpdlogRoot, "src/color_sinks.cpp"),
            path.join(SpdlogRoot, "src/file_sinks.cpp"),
            path.join(SpdlogRoot, "src/spdlog.cpp"),
            path.join(SpdlogRoot, "src/stdout_sinks.cpp")
        }

        externalincludedirs {
            path.join(SpdlogRoot, "include")
        }

        defines {
            "SPDLOG_COMPILED_LIB"
        }

        filter "system:windows"
            defines { "SPDLOG_WCHAR_FILENAMES" }

        filter {}
end
