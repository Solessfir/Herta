-- Zstandard compresses package chunks. Only the core compressor and decompressor are built: no dictionary builder, legacy formats, threads,
-- or hand-written assembly, so every platform and compiler runs the same C code and writes the same bytes.
function HertaZstd()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local ZstdRoot = path.join(RepositoryRoot, "External/zstd/lib")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", (_ACTION or "NoAction") .. HertaToolsetSuffix)

    project "Zstd"
        kind "StaticLib"
        language "C"
        location(path.join(ProjectFilesRoot, "Zstd"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}" .. HertaToolsetSuffix .. "/%{prj.name}"))
        warnings "Off"
        defines { "ZSTD_DISABLE_ASM", "ZSTD_LEGACY_SUPPORT=0", "ZSTD_NO_TRACE", "DEBUGLEVEL=0" }
        files {
            path.join(ZstdRoot, "common/*.c"),
            path.join(ZstdRoot, "common/*.h"),
            path.join(ZstdRoot, "compress/*.c"),
            path.join(ZstdRoot, "compress/*.h"),
            path.join(ZstdRoot, "decompress/*.c"),
            path.join(ZstdRoot, "decompress/*.h"),
            path.join(ZstdRoot, "zstd.h"),
            path.join(ZstdRoot, "zstd_errors.h"),
        }
        externalincludedirs { ZstdRoot }
end
