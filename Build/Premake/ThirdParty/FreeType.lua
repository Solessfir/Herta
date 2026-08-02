function HertaFreeType()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local FreeTypeRoot = path.join(RepositoryRoot, "External/freetype")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "FreeType"
        kind "StaticLib"
        language "C"
        cdialect "C99"
        location(path.join(ProjectFilesRoot, "FreeType"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        defines { "FT2_BUILD_LIBRARY" }
        includedirs { path.join(FreeTypeRoot, "include") }
        files {
            path.join(FreeTypeRoot, "include/**.h"),
            path.join(FreeTypeRoot, "src/autofit/autofit.c"),
            path.join(FreeTypeRoot, "src/base/ftbase.c"),
            path.join(FreeTypeRoot, "src/base/ftbbox.c"),
            path.join(FreeTypeRoot, "src/base/ftbdf.c"),
            path.join(FreeTypeRoot, "src/base/ftbitmap.c"),
            path.join(FreeTypeRoot, "src/base/ftcid.c"),
            path.join(FreeTypeRoot, "src/base/ftfstype.c"),
            path.join(FreeTypeRoot, "src/base/ftgasp.c"),
            path.join(FreeTypeRoot, "src/base/ftglyph.c"),
            path.join(FreeTypeRoot, "src/base/ftgxval.c"),
            path.join(FreeTypeRoot, "src/base/ftinit.c"),
            path.join(FreeTypeRoot, "src/base/ftmm.c"),
            path.join(FreeTypeRoot, "src/base/ftotval.c"),
            path.join(FreeTypeRoot, "src/base/ftpatent.c"),
            path.join(FreeTypeRoot, "src/base/ftpfr.c"),
            path.join(FreeTypeRoot, "src/base/ftstroke.c"),
            path.join(FreeTypeRoot, "src/base/ftsynth.c"),
            path.join(FreeTypeRoot, "src/base/fttype1.c"),
            path.join(FreeTypeRoot, "src/base/ftwinfnt.c"),
            path.join(FreeTypeRoot, "src/bdf/bdf.c"),
            path.join(FreeTypeRoot, "src/bzip2/ftbzip2.c"),
            path.join(FreeTypeRoot, "src/cache/ftcache.c"),
            path.join(FreeTypeRoot, "src/cff/cff.c"),
            path.join(FreeTypeRoot, "src/cid/type1cid.c"),
            path.join(FreeTypeRoot, "src/gzip/ftgzip.c"),
            path.join(FreeTypeRoot, "src/lzw/ftlzw.c"),
            path.join(FreeTypeRoot, "src/pcf/pcf.c"),
            path.join(FreeTypeRoot, "src/pfr/pfr.c"),
            path.join(FreeTypeRoot, "src/psaux/psaux.c"),
            path.join(FreeTypeRoot, "src/pshinter/pshinter.c"),
            path.join(FreeTypeRoot, "src/psnames/psnames.c"),
            path.join(FreeTypeRoot, "src/raster/raster.c"),
            path.join(FreeTypeRoot, "src/sdf/sdf.c"),
            path.join(FreeTypeRoot, "src/sfnt/sfnt.c"),
            path.join(FreeTypeRoot, "src/smooth/smooth.c"),
            path.join(FreeTypeRoot, "src/svg/svg.c"),
            path.join(FreeTypeRoot, "src/truetype/truetype.c"),
            path.join(FreeTypeRoot, "src/type1/type1.c"),
            path.join(FreeTypeRoot, "src/type42/type42.c"),
            path.join(FreeTypeRoot, "src/winfonts/winfnt.c")
        }

        filter "system:windows"
            defines {
                "_CRT_NONSTDC_NO_WARNINGS",
                "_CRT_SECURE_NO_WARNINGS"
            }
            files {
                path.join(FreeTypeRoot, "builds/windows/ftdebug.c"),
                path.join(FreeTypeRoot, "builds/windows/ftsystem.c")
            }

        filter "system:linux"
            files {
                path.join(FreeTypeRoot, "src/base/ftdebug.c"),
                path.join(FreeTypeRoot, "src/base/ftsystem.c")
            }

        filter {}
end
