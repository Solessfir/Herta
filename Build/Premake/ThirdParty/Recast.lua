-- Recast builds navmesh tiles, Detour queries them, DetourTileCache rebuilds tiles around dynamic obstacles, and DetourCrowd steers agents.
-- 64-bit polygon references leave room for the tile counts of large streamed worlds; the modules that include Detour define it too.
function HertaRecast()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local RecastRoot = path.join(RepositoryRoot, "External/recastnavigation")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    local function RecastProject(Name, Libraries)
        project(Name)
            kind "StaticLib"
            location(path.join(ProjectFilesRoot, Name))
            targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
            objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
            warnings "Off"
            defines { "DT_POLYREF64" }

            for _, Library in ipairs(Libraries) do
                files { path.join(RecastRoot, Library, "Source/*.cpp"), path.join(RecastRoot, Library, "Include/*.h") }
            end

            externalincludedirs { path.join(RecastRoot, "Recast/Include"), path.join(RecastRoot, "Detour/Include"), path.join(RecastRoot, "DetourCrowd/Include"), path.join(RecastRoot, "DetourTileCache/Include") }
    end

    -- Games link only Detour; Recast runs where navigation is cooked.
    RecastProject("Detour", { "Detour", "DetourCrowd", "DetourTileCache" })
    RecastProject("Recast", { "Recast" })
end
