local function ResolveVulkanSdk()
    local SdkRoot = os.getenv("HERTA_VULKAN_SDK")
    if not SdkRoot or SdkRoot == "" then
        error("HERTA_VULKAN_SDK is not set. Run Setup and GenerateProjectFiles so Herta can use the pinned Vulkan SDK.")
    end

    SdkRoot = path.getabsolute(SdkRoot)

    local IncludeDirectory = path.join(SdkRoot, "Include")
    local LibraryDirectory = path.join(SdkRoot, "Lib")
    local BinaryDirectory = path.join(SdkRoot, "Bin")
    local RuntimeLibraryDirectory = BinaryDirectory
    local ValidationLayerDirectory = path.join(SdkRoot, "Bin")
    if os.host() == "linux" then
        IncludeDirectory = path.join(SdkRoot, "x86_64/include")
        LibraryDirectory = path.join(SdkRoot, "x86_64/lib/VulkanLoader/lib")
        BinaryDirectory = path.join(SdkRoot, "x86_64/bin")
        RuntimeLibraryDirectory = path.join(SdkRoot, "x86_64/lib")
        ValidationLayerDirectory = path.join(SdkRoot, "x86_64/share/vulkan/explicit_layer.d")
    end

    if not os.isfile(path.join(IncludeDirectory, "vulkan/vulkan.h")) then
        error("HERTA_VULKAN_SDK does not contain Vulkan headers: " .. IncludeDirectory)
    end

    return {
        Root = SdkRoot,
        IncludeDirectory = IncludeDirectory,
        LibraryDirectory = LibraryDirectory,
        BinaryDirectory = BinaryDirectory,
        RuntimeLibraryDirectory = RuntimeLibraryDirectory,
        ValidationLayerDirectory = ValidationLayerDirectory
    }
end

function HertaGetVulkanSdk()
    return ResolveVulkanSdk()
end

function HertaNVRHI()
    local RepositoryRoot = path.getabsolute(_MAIN_SCRIPT_DIR)
    local NvrhiRoot = path.join(RepositoryRoot, "External/NVRHI")
    local ProjectFilesRoot = path.join(RepositoryRoot, "Intermediate/ProjectFiles", _ACTION or "NoAction")
    local VulkanSdk = ResolveVulkanSdk()

    project "NVRHI"
        kind "StaticLib"
        language "C++"
        cppdialect "C++23"
        location(path.join(ProjectFilesRoot, "NVRHI"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        defines { "NVRHI_WITH_AFTERMATH=0" }
        includedirs { path.join(NvrhiRoot, "include") }
        files {
            path.join(NvrhiRoot, "include/nvrhi/nvrhi.h"),
            path.join(NvrhiRoot, "include/nvrhi/nvrhiHLSL.h"),
            path.join(NvrhiRoot, "include/nvrhi/utils.h"),
            path.join(NvrhiRoot, "include/nvrhi/common/aftermath.h"),
            path.join(NvrhiRoot, "include/nvrhi/common/containers.h"),
            path.join(NvrhiRoot, "include/nvrhi/common/misc.h"),
            path.join(NvrhiRoot, "include/nvrhi/common/resource.h"),
            path.join(NvrhiRoot, "include/nvrhi/validation.h"),
            path.join(NvrhiRoot, "src/common/aftermath.cpp"),
            path.join(NvrhiRoot, "src/common/format-info.cpp"),
            path.join(NvrhiRoot, "src/common/misc.cpp"),
            path.join(NvrhiRoot, "src/common/state-tracking.cpp"),
            path.join(NvrhiRoot, "src/common/state-tracking.h"),
            path.join(NvrhiRoot, "src/common/utils.cpp"),
            path.join(NvrhiRoot, "src/validation/validation-backend.h"),
            path.join(NvrhiRoot, "src/validation/validation-commandlist.cpp"),
            path.join(NvrhiRoot, "src/validation/validation-device.cpp")
        }

        filter "action:vs*"
            files { path.join(NvrhiRoot, "tools/nvrhi.natvis") }

        filter {}

    project "NVRHIVulkanBackend"
        kind "StaticLib"
        language "C++"
        cppdialect "C++23"
        location(path.join(ProjectFilesRoot, "NVRHIVulkanBackend"))
        targetdir(path.join(RepositoryRoot, "Binaries/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}"))
        objdir(path.join(RepositoryRoot, "Intermediate/Build/%{cfg.system}/%{cfg.architecture}/%{cfg.buildcfg}/%{prj.name}"))
        warnings "Off"

        dependson { "NVRHI" }
        defines { "NVRHI_WITH_AFTERMATH=0" }
        includedirs {
            path.join(NvrhiRoot, "include"),
            VulkanSdk.IncludeDirectory
        }
        files {
            path.join(NvrhiRoot, "include/nvrhi/vulkan.h"),
            path.join(NvrhiRoot, "src/common/versioning.h"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-allocator.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-backend.h"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-buffer.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-commandlist.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-compute.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-constants.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-device.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-graphics.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-meshlets.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-queries.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-queue.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-raytracing.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-resource-bindings.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-shader.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-staging-texture.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-state-tracking.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-texture.cpp"),
            path.join(NvrhiRoot, "src/vulkan/vulkan-upload.cpp")
        }

        filter "system:windows"
            defines { "NOMINMAX", "VK_USE_PLATFORM_WIN32_KHR" }

        filter {}
end
