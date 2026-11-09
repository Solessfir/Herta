function HertaFastGltf()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local FastGltfRoot = path.join(RepositoryRoot, "External/fastgltf")
    local SimdjsonRoot = path.join(RepositoryRoot, "External/simdjson")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    project "FastGltf"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "FastGltf"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
        warnings "Off"
        dependson { "SimdJson" }

        files {
            path.join(FastGltfRoot, "src/base64.cpp"),
            path.join(FastGltfRoot, "src/fastgltf.cpp"),
            path.join(FastGltfRoot, "src/io.cpp"),
            path.join(FastGltfRoot, "include/fastgltf/*.hpp")
        }

        externalincludedirs {
            path.join(FastGltfRoot, "include"),
            SimdjsonRoot
        }

        filter "system:windows"
            buildoptions { "/bigobj" }

        filter {}
end
