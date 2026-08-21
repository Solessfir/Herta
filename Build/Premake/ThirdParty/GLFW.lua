function HertaGLFW()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local GlfwRoot = path.join(RepositoryRoot, "External/glfw")
    local GeneratedRoot = path.join(RepositoryRoot, "Intermediate/Generated/GLFW")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "GLFW"
        kind "StaticLib"
        language "C"
        cdialect "C99"
        location(path.join(ProjectFilesRoot, "GLFW"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        files {
            path.join(GlfwRoot, "include/GLFW/glfw3.h"),
            path.join(GlfwRoot, "include/GLFW/glfw3native.h"),
            path.join(GlfwRoot, "src/context.c"),
            path.join(GlfwRoot, "src/egl_context.c"),
            path.join(GlfwRoot, "src/init.c"),
            path.join(GlfwRoot, "src/input.c"),
            path.join(GlfwRoot, "src/internal.h"),
            path.join(GlfwRoot, "src/mappings.h"),
            path.join(GlfwRoot, "src/monitor.c"),
            path.join(GlfwRoot, "src/null_init.c"),
            path.join(GlfwRoot, "src/null_joystick.c"),
            path.join(GlfwRoot, "src/null_monitor.c"),
            path.join(GlfwRoot, "src/null_window.c"),
            path.join(GlfwRoot, "src/osmesa_context.c"),
            path.join(GlfwRoot, "src/platform.c"),
            path.join(GlfwRoot, "src/platform.h"),
            path.join(GlfwRoot, "src/vulkan.c"),
            path.join(GlfwRoot, "src/window.c")
        }

        includedirs {
            path.join(GlfwRoot, "include"),
            path.join(GlfwRoot, "src")
        }

        filter "system:windows"
            defines {
                "_GLFW_WIN32",
                "_CRT_SECURE_NO_WARNINGS"
            }
            files {
                path.join(GlfwRoot, "src/wgl_context.c"),
                path.join(GlfwRoot, "src/win32_init.c"),
                path.join(GlfwRoot, "src/win32_joystick.c"),
                path.join(GlfwRoot, "src/win32_module.c"),
                path.join(GlfwRoot, "src/win32_monitor.c"),
                path.join(GlfwRoot, "src/win32_thread.c"),
                path.join(GlfwRoot, "src/win32_time.c"),
                path.join(GlfwRoot, "src/win32_window.c")
            }

        filter "system:linux"
            defines {
                "_DEFAULT_SOURCE",
                "_GLFW_X11",
                "_GLFW_WAYLAND"
            }
            includedirs { GeneratedRoot }
            files {
                path.join(GeneratedRoot, "*.h"),
                path.join(GlfwRoot, "src/glx_context.c"),
                path.join(GlfwRoot, "src/linux_joystick.c"),
                path.join(GlfwRoot, "src/posix_module.c"),
                path.join(GlfwRoot, "src/posix_poll.c"),
                path.join(GlfwRoot, "src/posix_thread.c"),
                path.join(GlfwRoot, "src/posix_time.c"),
                path.join(GlfwRoot, "src/wl_init.c"),
                path.join(GlfwRoot, "src/wl_monitor.c"),
                path.join(GlfwRoot, "src/wl_window.c"),
                path.join(GlfwRoot, "src/x11_init.c"),
                path.join(GlfwRoot, "src/x11_monitor.c"),
                path.join(GlfwRoot, "src/x11_window.c"),
                path.join(GlfwRoot, "src/xkb_unicode.c")
            }

            local WaylandProtocols = {
                "wayland.xml",
                "viewporter.xml",
                "xdg-shell.xml",
                "idle-inhibit-unstable-v1.xml",
                "pointer-constraints-unstable-v1.xml",
                "relative-pointer-unstable-v1.xml",
                "fractional-scale-v1.xml",
                "tablet-unstable-v2.xml",
                "cursor-shape-v1.xml",
                "xdg-activation-v1.xml",
                "xdg-decoration-unstable-v1.xml"
            }
            local ProtocolCommands = {
                string.format("mkdir -p \"%s\"", GeneratedRoot)
            }
            for _, Protocol in ipairs(WaylandProtocols) do
                local ProtocolRoot = path.getbasename(Protocol)
                local ProtocolPath = path.join(GlfwRoot, "deps/wayland", Protocol)
                table.insert(ProtocolCommands, string.format("wayland-scanner client-header \"%s\" \"%s\"", ProtocolPath, path.join(GeneratedRoot, ProtocolRoot .. "-client-protocol.h")))
                table.insert(ProtocolCommands, string.format("wayland-scanner private-code \"%s\" \"%s\"", ProtocolPath, path.join(GeneratedRoot, ProtocolRoot .. "-client-protocol-code.h")))
            end
            prebuildmessage "Generating GLFW Wayland protocols"
            prebuildcommands(ProtocolCommands)

        filter {}
end
