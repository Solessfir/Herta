function HertaApplyToolchainSettings()
    filter "system:windows"
        systemversion "latest"
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

    filter "configurations:Shipping"
        defines { "HERTA_SHIPPING=1" }
        runtime "Release"
        symbols "Off"
        optimize "Full"

    filter {}
end
