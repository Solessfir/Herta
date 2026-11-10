function HertaApplyToolchainSettings()
    filter "system:windows"
        systemversion "latest"
        characterset "Unicode"
        usestandardpreprocessor "On"
        buildoptions {
            "/utf-8"
        }
        defines {
            "HERTA_PLATFORM_WINDOWS=1",
            "NOMINMAX",
            "UNICODE",
            "_UNICODE",
            "WIN32_LEAN_AND_MEAN"
        }

    filter "system:linux"
        pic "On"
        defines {
            "HERTA_PLATFORM_LINUX=1"
        }

    filter "configurations:Debug"
        defines { "HERTA_DEBUG=1" }
        runtime "Debug"
        symbols "On"

    filter "configurations:Debug-ASan"
        defines { "HERTA_DEBUG=1" }
        runtime "Debug"
        symbols "On"

    filter { "system:linux", "configurations:Debug-ASan" }
        defines { "HERTA_ADDRESS_SANITIZER=1" }
        sanitize { "Address", "UndefinedBehavior" }
        buildoptions { "-fno-omit-frame-pointer" }

    filter "configurations:Development"
        defines { "HERTA_DEVELOPMENT=1" }
        runtime "Release"
        symbols "On"
        optimize "Speed"

    -- Incremental linking patches only the changed functions into the exe, so a one-file edit relinks the editor, game, and tests in about
    -- a fifth of the time. It requires keeping unreferenced and duplicate functions; Shipping still strips and folds them.
    filter { "system:windows", "configurations:Development", "kind:ConsoleApp or WindowedApp" }
        incrementallink "On"
        linkoptions { "/OPT:NOREF", "/OPT:NOICF" }

    filter "configurations:Shipping"
        defines { "HERTA_SHIPPING=1" }
        runtime "Release"
        symbols "Off"
        optimize "Full"

    filter { "system:windows", "configurations:Shipping" }
        staticruntime "On"

    -- Shipping builds are reproducible: no timestamps in the binaries, and source paths relative to the checkout instead of where it sits.
    -- Symbols are generated beside the binaries, never packaged, so crash reports can be read.
    filter { "system:windows", "configurations:Shipping", "toolset:msc*" }
        symbols "On"
        buildoptions { "/Brepro", "/experimental:deterministic", "/pathmap:" .. path.getabsolute(_MAIN_SCRIPT_DIR) .. "=." }

    filter { "system:windows", "configurations:Shipping", "toolset:msc*", "kind:ConsoleApp or WindowedApp or SharedLib" }
        linkoptions { "/Brepro", "/PDBALTPATH:%_PDB%" }

    filter { "system:linux", "configurations:Shipping" }
        buildoptions { "-ffile-prefix-map=" .. path.getabsolute(_MAIN_SCRIPT_DIR) .. "=." }

    -- A GNU build id names each Linux binary, so a crash report or core finds its symbols.
    filter "system:linux"
        linkoptions { "-Wl,--build-id=sha1" }

    filter {}
end
