function HertaFastGltf()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local FastGltfRoot = path.join(RepositoryRoot, "External/fastgltf")
    local SimdjsonRoot = path.join(RepositoryRoot, "External/simdjson")
    local SimdutfRoot = path.join(RepositoryRoot, "External/simdutf")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    project "FastGltf"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "FastGltf"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
        warnings "Off"
        dependson { "SimdJson" }

        files {
            path.join(FastGltfRoot, "src/*.cpp"),
            path.join(FastGltfRoot, "include/fastgltf/**.hpp"),
            -- simdutf decodes base64 buffers and validates UTF-8 for fastgltf alone, so it builds into this library.
            path.join(SimdutfRoot, "simdutf.cpp")
        }

        externalincludedirs {
            path.join(FastGltfRoot, "include"),
            SimdjsonRoot,
            SimdutfRoot
        }

        filter "system:windows"
            buildoptions { "/bigobj" }

        filter {}
end
