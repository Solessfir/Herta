-- Monocypher signs and verifies packages with Ed25519 and hashes their chunks with BLAKE2b. It is vendored unmodified; see its UPSTREAM.md.
function HertaMonocypher()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local MonocypherRoot = path.join(RepositoryRoot, "External/monocypher")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    project "Monocypher"
        kind "StaticLib"
        language "C"
        location(path.join(ProjectFilesRoot, "Monocypher"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
        warnings "Off"
        files { path.join(MonocypherRoot, "*.c"), path.join(MonocypherRoot, "*.h") }
        externalincludedirs { MonocypherRoot }
end
