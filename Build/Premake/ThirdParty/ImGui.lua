function HertaImGui()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local ImGuiRoot = path.join(RepositoryRoot, "External/imgui")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")

    project "ImGui"
        kind "StaticLib"
        language "C++"
        cppdialect "C++23"
        location(path.join(ProjectFilesRoot, "ImGui"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        dependson { "FreeType", "GLFW" }
        defines {
            "GLFW_INCLUDE_NONE",
            "IMGUI_ENABLE_FREETYPE"
        }
        includedirs {
            path.join(RepositoryRoot, "External/freetype/include"),
            path.join(RepositoryRoot, "External/glfw/include"),
            ImGuiRoot
        }
        files {
            path.join(ImGuiRoot, "imconfig.h"),
            path.join(ImGuiRoot, "imgui.cpp"),
            path.join(ImGuiRoot, "imgui.h"),
            path.join(ImGuiRoot, "imgui_demo.cpp"),
            path.join(ImGuiRoot, "imgui_draw.cpp"),
            path.join(ImGuiRoot, "imgui_internal.h"),
            path.join(ImGuiRoot, "imgui_tables.cpp"),
            path.join(ImGuiRoot, "imgui_widgets.cpp"),
            path.join(ImGuiRoot, "backends/imgui_impl_glfw.cpp"),
            path.join(ImGuiRoot, "backends/imgui_impl_glfw.h"),
            path.join(ImGuiRoot, "misc/freetype/imgui_freetype.cpp"),
            path.join(ImGuiRoot, "misc/freetype/imgui_freetype.h")
        }
end
