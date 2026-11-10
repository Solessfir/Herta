-- HarfBuzz shapes text for TextLayout through its single-file amalgamation, reading fonts from memory with its own OpenType functions,
-- so it needs neither FreeType nor any system library. Its own sources are not external includes, so their warnings stay off.
function HertaHarfBuzz()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local HarfBuzzRoot = path.join(RepositoryRoot, "External/harfbuzz/src")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    project "HarfBuzz"
        kind "StaticLib"
        language "C++"
        cppdialect "C++17"
        location(path.join(ProjectFilesRoot, "HarfBuzz"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
        warnings "Off"
        exceptionhandling "Off"
        rtti "Off"
        files { path.join(HarfBuzzRoot, "harfbuzz.cc") }
        includedirs { HarfBuzzRoot }
end
