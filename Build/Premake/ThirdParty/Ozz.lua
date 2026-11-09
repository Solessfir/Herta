-- ozz-animation as three static libraries: base, the runtime sampling and blending jobs, and the offline builders that only the asset worker runs.
function HertaOzz()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local OzzRoot = path.join(RepositoryRoot, "External/ozz-animation")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    local function OzzProject(Name, Sources)
        project(Name)
            kind "StaticLib"
            location(path.join(ProjectFilesRoot, Name))
            targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
            objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
            warnings "Off"
            files(Sources)
            includedirs { path.join(OzzRoot, "include"), path.join(OzzRoot, "src") }
    end

    OzzProject("OzzBase", {
        path.join(OzzRoot, "src/base/**.cc"),
        path.join(OzzRoot, "src/base/**.h"),
        path.join(OzzRoot, "include/ozz/base/**.h"),
    })

    OzzProject("OzzAnimation", {
        path.join(OzzRoot, "src/animation/runtime/**.cc"),
        path.join(OzzRoot, "src/animation/runtime/**.h"),
        path.join(OzzRoot, "include/ozz/animation/runtime/**.h"),
    })

    OzzProject("OzzAnimationOffline", {
        path.join(OzzRoot, "src/animation/offline/*.cc"),
        path.join(OzzRoot, "src/animation/offline/*.h"),
        path.join(OzzRoot, "include/ozz/animation/offline/*.h"),
    })
end
