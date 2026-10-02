function HertaFastGltf()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local FastGltfRoot = path.join(RepositoryRoot, "External/fastgltf")
    local SimdjsonRoot = path.join(RepositoryRoot, "External/simdjson")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    -- simdjson is compiled into the same library because fastgltf is its only consumer.
    project "FastGltf"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "FastGltf"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        files {
            path.join(FastGltfRoot, "src/base64.cpp"),
            path.join(FastGltfRoot, "src/fastgltf.cpp"),
            path.join(FastGltfRoot, "src/io.cpp"),
            path.join(FastGltfRoot, "include/fastgltf/*.hpp"),
            path.join(SimdjsonRoot, "simdjson.cpp"),
            path.join(SimdjsonRoot, "simdjson.h")
        }

        externalincludedirs {
            path.join(FastGltfRoot, "include"),
            SimdjsonRoot
        }

        filter "system:windows"
            buildoptions { "/bigobj" }

        filter {}
end
