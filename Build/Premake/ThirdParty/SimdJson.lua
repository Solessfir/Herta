function HertaSimdJson()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local SimdjsonRoot = path.join(RepositoryRoot, "External/simdjson")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "SimdJson"
        kind "StaticLib"
        location(path.join(ProjectFilesRoot, "SimdJson"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"
        files { path.join(SimdjsonRoot, "simdjson.cpp"), path.join(SimdjsonRoot, "simdjson.h") }
        externalincludedirs { SimdjsonRoot }

        filter "system:windows"
            buildoptions { "/bigobj" }

        filter {}
end
