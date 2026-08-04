#include "Herta/NvrhiVulkan/NvrhiVulkan.h"

#include "Herta/Core/Log.h"
#include "Shaders/ToolUIShaders.h"

#include <vulkan/vulkan.h>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <imgui.h>
#include <limits>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace Herta
{
namespace
{
inline constexpr FLogCategory RhiLog{"RHI"};
inline constexpr std::string_view ValidationLayerName = "VK_LAYER_KHRONOS_validation";
inline constexpr std::size_t VulkanBufferUpdateAlignment = 4;

[[nodiscard]] constexpr std::size_t AlignVulkanBufferUpdateSourceSize(const std::size_t Size) noexcept
{
	return (Size + VulkanBufferUpdateAlignment - 1) & ~(VulkanBufferUpdateAlignment - 1);
}

static_assert(AlignVulkanBufferUpdateSourceSize(12'510) == 12'512);

[[nodiscard]] FPresentationError MakeVulkanError(const EPresentationErrorCode Code, const std::string_view Operation, const VkResult Result)
{
	return {Code, std::format("{} failed with VkResult {}", Operation, static_cast<int>(Result))};
}

[[nodiscard]] EPresentationErrorCode ToDeviceErrorCode(const VkResult Result, const EPresentationErrorCode Fallback) noexcept
{
	return Result == VK_ERROR_DEVICE_LOST ? EPresentationErrorCode::DeviceLost : Fallback;
}

[[nodiscard]] bool ContainsExtension(const std::span<const VkExtensionProperties> Extensions, const std::string_view Name) noexcept
{
	return std::ranges::any_of(Extensions, [Name](const VkExtensionProperties& Extension)
	                           {
		                           return Name == Extension.extensionName;
	                           });
}

[[nodiscard]] bool ContainsLayer(const std::span<const VkLayerProperties> Layers, const std::string_view Name) noexcept
{
	return std::ranges::any_of(Layers, [Name](const VkLayerProperties& Layer)
	                           {
		                           return Name == Layer.layerName;
	                           });
}

[[nodiscard]] nvrhi::Format ToNvrhiFormat(const VkFormat Format) noexcept
{
	switch (Format)
	{
		case VK_FORMAT_B8G8R8A8_SRGB:
			return nvrhi::Format::SBGRA8_UNORM;
		case VK_FORMAT_R8G8B8A8_SRGB:
			return nvrhi::Format::SRGBA8_UNORM;
		case VK_FORMAT_B8G8R8A8_UNORM:
			return nvrhi::Format::BGRA8_UNORM;
		case VK_FORMAT_R8G8B8A8_UNORM:
			return nvrhi::Format::RGBA8_UNORM;
		default:
			return nvrhi::Format::UNKNOWN;
	}
}

[[nodiscard]] VkSurfaceFormatKHR ChooseSurfaceFormat(const std::span<const VkSurfaceFormatKHR> Formats) noexcept
{
	constexpr std::array PreferredFormats{
	    VK_FORMAT_B8G8R8A8_SRGB,
	    VK_FORMAT_R8G8B8A8_SRGB,
	    VK_FORMAT_B8G8R8A8_UNORM,
	    VK_FORMAT_R8G8B8A8_UNORM};

	for (const VkFormat Preferred : PreferredFormats)
	{
		const auto Match = std::ranges::find_if(Formats, [Preferred](const VkSurfaceFormatKHR& Format)
		                                        {
			                                        return Format.format == Preferred && Format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		                                        });
		if (Match != Formats.end())
		{
			return *Match;
		}
	}

	return Formats.front();
}

[[nodiscard]] VkPresentModeKHR ChoosePresentMode(const std::span<const VkPresentModeKHR> Modes, const bool bVSync) noexcept
{
	if (!bVSync && std::ranges::find(Modes, VK_PRESENT_MODE_MAILBOX_KHR) != Modes.end())
	{
		return VK_PRESENT_MODE_MAILBOX_KHR;
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

[[nodiscard]] VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkCompositeAlphaFlagsKHR Supported) noexcept
{
	constexpr std::array Candidates{
	    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
	    VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
	    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};

	for (const VkCompositeAlphaFlagBitsKHR Candidate : Candidates)
	{
		if ((Supported & Candidate) != 0)
		{
			return Candidate;
		}
	}

	return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

struct FValidationState
{
	FLogService* Log = nullptr;
	std::atomic_bool bHasErrors = false;
};

class FNvrhiMessageCallback final : public nvrhi::IMessageCallback
{
public:
	explicit FNvrhiMessageCallback(FValidationState& State) noexcept
	    : State(State)
	{
	}

	void message(const nvrhi::MessageSeverity Severity, const char* const MessageText) override
	{
		ELogLevel Level = ELogLevel::Info;
		switch (Severity)
		{
			case nvrhi::MessageSeverity::Info:
				Level = ELogLevel::Info;
				break;
			case nvrhi::MessageSeverity::Warning:
				Level = ELogLevel::Warning;
				break;
			case nvrhi::MessageSeverity::Error:
				Level = ELogLevel::Error;
				State.bHasErrors.store(true, std::memory_order_release);
				break;
			case nvrhi::MessageSeverity::Fatal:
				Level = ELogLevel::Critical;
				State.bHasErrors.store(true, std::memory_order_release);
				break;
		}

		if (State.Log)
		{
			State.Log->LogText(RhiLog, Level, MessageText ? MessageText : "NVRHI reported an empty diagnostic");
		}
	}

private:
	FValidationState& State;
};

struct FQueueSelection
{
	std::uint32_t FamilyIndex = 0;
	std::uint32_t Score = 0;
};

[[nodiscard]] std::optional<FQueueSelection> FindGraphicsPresentationQueue(const VkPhysicalDevice Device, const VkSurfaceKHR Surface)
{
	std::uint32_t QueueCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(Device, &QueueCount, nullptr);
	std::vector<VkQueueFamilyProperties> Queues(QueueCount);
	vkGetPhysicalDeviceQueueFamilyProperties(Device, &QueueCount, Queues.data());

	for (std::uint32_t Index = 0; Index < QueueCount; ++Index)
	{
		VkBool32 bSupportsPresentation = VK_FALSE;
		if (vkGetPhysicalDeviceSurfaceSupportKHR(Device, Index, Surface, &bSupportsPresentation) != VK_SUCCESS)
		{
			continue;
		}

		if ((Queues[Index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && bSupportsPresentation == VK_TRUE)
		{
			return FQueueSelection{Index, Queues[Index].queueCount};
		}
	}

	return std::nullopt;
}

struct FPhysicalDeviceSelection
{
	VkPhysicalDevice Device = VK_NULL_HANDLE;
	std::uint32_t QueueFamilyIndex = 0;
	std::uint32_t Score = 0;
};

[[nodiscard]] std::optional<FPhysicalDeviceSelection> EvaluatePhysicalDevice(const VkPhysicalDevice Device, const VkSurfaceKHR Surface)
{
	VkPhysicalDeviceProperties Properties{};
	vkGetPhysicalDeviceProperties(Device, &Properties);
	if (Properties.apiVersion < VK_API_VERSION_1_3)
	{
		return std::nullopt;
	}

	VkPhysicalDeviceVulkan12Features Features12{};
	Features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	VkPhysicalDeviceVulkan13Features Features13{};
	Features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	Features13.pNext = &Features12;
	VkPhysicalDeviceFeatures2 Features{};
	Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	Features.pNext = &Features13;
	vkGetPhysicalDeviceFeatures2(Device, &Features);
	if (Features12.timelineSemaphore != VK_TRUE || Features13.synchronization2 != VK_TRUE || Features13.dynamicRendering != VK_TRUE)
	{
		return std::nullopt;
	}

	std::uint32_t ExtensionCount = 0;
	if (vkEnumerateDeviceExtensionProperties(Device, nullptr, &ExtensionCount, nullptr) != VK_SUCCESS)
	{
		return std::nullopt;
	}
	std::vector<VkExtensionProperties> Extensions(ExtensionCount);
	if (vkEnumerateDeviceExtensionProperties(Device, nullptr, &ExtensionCount, Extensions.data()) != VK_SUCCESS || !ContainsExtension(Extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
	{
		return std::nullopt;
	}

	const std::optional Queue = FindGraphicsPresentationQueue(Device, Surface);
	if (!Queue)
	{
		return std::nullopt;
	}

	std::uint32_t Score = Properties.limits.maxImageDimension2D;
	if (Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		Score += 1'000'000;
	}
	else if (Properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
	{
		Score += 500'000;
	}

	return FPhysicalDeviceSelection{Device, Queue->FamilyIndex, Score};
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(const VkDebugUtilsMessageSeverityFlagBitsEXT Severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* const CallbackData, void* const UserData)
{
	FValidationState* const State = static_cast<FValidationState*>(UserData);
	if (!State)
	{
		return VK_FALSE;
	}

	ELogLevel Level = ELogLevel::Debug;
	if ((Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
	{
		Level = ELogLevel::Error;
		State->bHasErrors.store(true, std::memory_order_release);
	}
	else if ((Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0)
	{
		Level = ELogLevel::Warning;
	}
	else if ((Severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT) != 0)
	{
		Level = ELogLevel::Info;
	}

	if (State->Log)
	{
		State->Log->LogText(RhiLog, Level, CallbackData && CallbackData->pMessage ? CallbackData->pMessage : "Vulkan reported an empty diagnostic");
	}
	return VK_FALSE;
}

class FNvrhiVulkanPresentation final : public INvrhiVulkanPresentation
{
private:
	struct FFrameSync
	{
		VkSemaphore ImageAvailable = VK_NULL_HANDLE;
		std::uint64_t SubmissionInstance = 0;
	};

	struct FSecondaryViewport
	{
		void* WindowBackendHandle = nullptr;
		VkSurfaceKHR Surface = VK_NULL_HANDLE;
		VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
		VkSurfaceFormatKHR SurfaceFormat{};
		FExtent2D Extent;
		nvrhi::CommandListHandle CommandList;
		std::vector<nvrhi::TextureHandle> BackBuffers;
		std::vector<nvrhi::FramebufferHandle> Framebuffers;
		nvrhi::GraphicsPipelineHandle Pipeline;
		std::vector<VkSemaphore> RenderFinished;
		std::vector<FFrameSync> FrameSync;
		std::size_t FrameSlot = 0;
		std::uint32_t ActiveImageIndex = 0;
		bool bFrameActive = false;
		bool bFrameSuboptimal = false;
	};

public:
	explicit FNvrhiVulkanPresentation(FNvrhiVulkanPresentationDescriptor Descriptor)
	    : Descriptor(std::move(Descriptor))
	    , NvrhiCallback(ValidationState)
	{
		ValidationState.Log = this->Descriptor.Log;
	}

	~FNvrhiVulkanPresentation() override
	{
		Shutdown();
	}

	[[nodiscard]] std::expected<void, FPresentationError> Initialize()
	{
		if (!Descriptor.WindowBackendHandle || Descriptor.RequiredInstanceExtensions.empty() || Descriptor.DesiredImageCount == 0)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Vulkan presentation requires a window, instance extensions, and at least one desired image"});
		}

		std::uint32_t LoaderVersion = VK_API_VERSION_1_0;
		if (const auto EnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")))
		{
			const VkResult Result = EnumerateInstanceVersion(&LoaderVersion);
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkEnumerateInstanceVersion", Result));
			}
		}

		if (LoaderVersion < VK_API_VERSION_1_3)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "Herta requires a Vulkan 1.3 loader"});
		}

		std::vector<const char*> InstanceExtensions;
		InstanceExtensions.reserve(Descriptor.RequiredInstanceExtensions.size() + 1);
		for (const std::string& Extension : Descriptor.RequiredInstanceExtensions)
		{
			InstanceExtensions.push_back(Extension.c_str());
		}

		std::vector<const char*> Layers;
		bool bEnableValidation = Descriptor.bEnableValidation;
		if (bEnableValidation)
		{
			std::uint32_t LayerCount = 0;
			VkResult Result = vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkEnumerateInstanceLayerProperties", Result));
			}
			std::vector<VkLayerProperties> AvailableLayers(LayerCount);
			Result = vkEnumerateInstanceLayerProperties(&LayerCount, AvailableLayers.data());
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkEnumerateInstanceLayerProperties", Result));
			}
			if (!ContainsLayer(AvailableLayers, ValidationLayerName))
			{
				bEnableValidation = false;
				if (Descriptor.Log)
				{
					Descriptor.Log->LogText(RhiLog, ELogLevel::Warning, "VK_LAYER_KHRONOS_validation is unavailable; continuing without Vulkan or NVRHI validation");
				}
			}
			else
			{
				Layers.push_back(ValidationLayerName.data());

				std::uint32_t ExtensionCount = 0;
				Result = vkEnumerateInstanceExtensionProperties(nullptr, &ExtensionCount, nullptr);
				if (Result != VK_SUCCESS)
				{
					return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkEnumerateInstanceExtensionProperties", Result));
				}
				std::vector<VkExtensionProperties> AvailableExtensions(ExtensionCount);
				Result = vkEnumerateInstanceExtensionProperties(nullptr, &ExtensionCount, AvailableExtensions.data());
				if (Result != VK_SUCCESS)
				{
					return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkEnumerateInstanceExtensionProperties", Result));
				}
				if (ContainsExtension(AvailableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
				{
					InstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
					bDebugUtilsEnabled = true;
				}
			}
		}

		VkApplicationInfo ApplicationInfo{};
		ApplicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		ApplicationInfo.pApplicationName = Descriptor.ApplicationName.c_str();
		ApplicationInfo.applicationVersion = Descriptor.ApplicationVersion;
		ApplicationInfo.pEngineName = "Herta";
		ApplicationInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
		ApplicationInfo.apiVersion = VK_API_VERSION_1_3;
		VkInstanceCreateInfo InstanceInfo{};
		InstanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		InstanceInfo.pApplicationInfo = &ApplicationInfo;
		InstanceInfo.enabledLayerCount = static_cast<std::uint32_t>(Layers.size());
		InstanceInfo.ppEnabledLayerNames = Layers.data();
		InstanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(InstanceExtensions.size());
		InstanceInfo.ppEnabledExtensionNames = InstanceExtensions.data();

		VkResult Result = vkCreateInstance(&InstanceInfo, nullptr, &Instance);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkCreateInstance", Result));
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(Instance));

		if (bDebugUtilsEnabled)
		{
			const auto CreateDebugMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkCreateDebugUtilsMessengerEXT"));
			if (CreateDebugMessenger)
			{
				VkDebugUtilsMessengerCreateInfoEXT DebugInfo{};
				DebugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
				DebugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
				DebugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
				DebugInfo.pfnUserCallback = VulkanDebugCallback;
				DebugInfo.pUserData = &ValidationState;
				Result = CreateDebugMessenger(Instance, &DebugInfo, nullptr, &DebugMessenger);
				if (Result != VK_SUCCESS)
				{
					return std::unexpected(MakeVulkanError(EPresentationErrorCode::InstanceCreationFailed, "vkCreateDebugUtilsMessengerEXT", Result));
				}
			}
		}

		Result = glfwCreateWindowSurface(Instance, static_cast<GLFWwindow*>(Descriptor.WindowBackendHandle), nullptr, &Surface);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SurfaceCreationFailed, "glfwCreateWindowSurface", Result));
		}

		std::uint32_t DeviceCount = 0;
		Result = vkEnumeratePhysicalDevices(Instance, &DeviceCount, nullptr);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::PhysicalDeviceUnavailable, "vkEnumeratePhysicalDevices", Result));
		}
		std::vector<VkPhysicalDevice> PhysicalDevices(DeviceCount);
		Result = vkEnumeratePhysicalDevices(Instance, &DeviceCount, PhysicalDevices.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::PhysicalDeviceUnavailable, "vkEnumeratePhysicalDevices", Result));
		}

		std::optional<FPhysicalDeviceSelection> Selection;
		for (const VkPhysicalDevice Candidate : PhysicalDevices)
		{
			const std::optional CandidateSelection = EvaluatePhysicalDevice(Candidate, Surface);
			if (CandidateSelection && (!Selection || CandidateSelection->Score > Selection->Score))
			{
				Selection = CandidateSelection;
			}
		}
		if (!Selection)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::PhysicalDeviceUnavailable, "No Vulkan 1.3 device supports timeline semaphores, synchronization2, dynamic rendering, graphics, presentation, and VK_KHR_swapchain"});
		}

		PhysicalDevice = Selection->Device;
		QueueFamilyIndex = Selection->QueueFamilyIndex;
		if (Descriptor.Log)
		{
			VkPhysicalDeviceProperties SelectedDeviceProperties{};
			vkGetPhysicalDeviceProperties(PhysicalDevice, &SelectedDeviceProperties);
			HERTA_LOG_INFO(
			    *Descriptor.Log,
			    RhiLog,
			    "Vulkan loader {}.{}.{} selected '{}' with API {}.{}.{}, graphics queue {}, validation {}",
			    VK_API_VERSION_MAJOR(LoaderVersion),
			    VK_API_VERSION_MINOR(LoaderVersion),
			    VK_API_VERSION_PATCH(LoaderVersion),
			    SelectedDeviceProperties.deviceName,
			    VK_API_VERSION_MAJOR(SelectedDeviceProperties.apiVersion),
			    VK_API_VERSION_MINOR(SelectedDeviceProperties.apiVersion),
			    VK_API_VERSION_PATCH(SelectedDeviceProperties.apiVersion),
			    QueueFamilyIndex,
			    bEnableValidation ? "enabled" : "disabled");
		}
		constexpr float QueuePriority = 1.0f;
		VkDeviceQueueCreateInfo QueueInfo{};
		QueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		QueueInfo.queueFamilyIndex = QueueFamilyIndex;
		QueueInfo.queueCount = 1;
		QueueInfo.pQueuePriorities = &QueuePriority;
		VkPhysicalDeviceVulkan12Features EnabledFeatures12{};
		EnabledFeatures12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		EnabledFeatures12.timelineSemaphore = VK_TRUE;
		VkPhysicalDeviceVulkan13Features EnabledFeatures13{};
		EnabledFeatures13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		EnabledFeatures13.pNext = &EnabledFeatures12;
		EnabledFeatures13.synchronization2 = VK_TRUE;
		EnabledFeatures13.dynamicRendering = VK_TRUE;
		VkPhysicalDeviceFeatures2 EnabledFeatures{};
		EnabledFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		EnabledFeatures.pNext = &EnabledFeatures13;
		constexpr std::array DeviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		VkDeviceCreateInfo DeviceInfo{};
		DeviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		DeviceInfo.pNext = &EnabledFeatures;
		DeviceInfo.queueCreateInfoCount = 1;
		DeviceInfo.pQueueCreateInfos = &QueueInfo;
		DeviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(DeviceExtensions.size());
		DeviceInfo.ppEnabledExtensionNames = DeviceExtensions.data();
		Result = vkCreateDevice(PhysicalDevice, &DeviceInfo, nullptr, &Device);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::DeviceCreationFailed, "vkCreateDevice", Result));
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(Device));
		vkGetDeviceQueue(Device, QueueFamilyIndex, 0, &GraphicsQueue);

		nvrhi::vulkan::DeviceDesc NvrhiDescriptor;
		NvrhiDescriptor.errorCB = &NvrhiCallback;
		NvrhiDescriptor.instance = Instance;
		NvrhiDescriptor.physicalDevice = PhysicalDevice;
		NvrhiDescriptor.device = Device;
		NvrhiDescriptor.graphicsQueue = GraphicsQueue;
		NvrhiDescriptor.graphicsQueueIndex = static_cast<int>(QueueFamilyIndex);
		// NVRHI's descriptor predates const-correct pointer arrays but does not mutate extension names.
		NvrhiDescriptor.instanceExtensions = const_cast<const char**>(InstanceExtensions.data());
		NvrhiDescriptor.numInstanceExtensions = InstanceExtensions.size();
		NvrhiDescriptor.deviceExtensions = const_cast<const char**>(DeviceExtensions.data());
		NvrhiDescriptor.numDeviceExtensions = DeviceExtensions.size();
		VulkanNvrhiDevice = nvrhi::vulkan::createDevice(NvrhiDescriptor);
		if (!VulkanNvrhiDevice)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not wrap the Vulkan device"});
		}
		NvrhiDevice = bEnableValidation ? nvrhi::validation::createValidationLayer(VulkanNvrhiDevice) : nvrhi::DeviceHandle(VulkanNvrhiDevice);
		CommandList = NvrhiDevice->createCommandList();
		if (!CommandList)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create the presentation command list"});
		}

		return Resize(Descriptor.InitialExtent);
	}

	[[nodiscard]] FExtent2D GetExtent() const noexcept override
	{
		return Extent;
	}

	[[nodiscard]] std::expected<void, FPresentationError> Resize(const FExtent2D RequestedExtent) override
	{
		if (bFrameActive)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Cannot resize presentation while a frame is active"});
		}

		if (RequestedExtent.IsEmpty())
		{
			const std::expected IdleResult = WaitIdle();
			if (!IdleResult)
			{
				return IdleResult;
			}
			DestroySwapchain();
			Extent = RequestedExtent;
			return {};
		}

		return CreateSwapchain(RequestedExtent);
	}

	[[nodiscard]] std::expected<EPresentationStatus, FPresentationError> BeginFrame() override
	{
		if (bFrameActive || HasActiveSecondaryViewport())
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "BeginFrame was called while another presentation frame is active"});
		}
		if (Extent.IsEmpty() || Swapchain == VK_NULL_HANDLE)
		{
			return EPresentationStatus::Minimized;
		}

		FFrameSync& Frame = FrameSync[FrameSlot];
		if (Frame.SubmissionInstance != 0)
		{
			const VkSemaphore TrackingSemaphore = VulkanNvrhiDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
			VkSemaphoreWaitInfo WaitInfo{};
			WaitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			WaitInfo.semaphoreCount = 1;
			WaitInfo.pSemaphores = &TrackingSemaphore;
			WaitInfo.pValues = &Frame.SubmissionInstance;
			const VkResult WaitResult = vkWaitSemaphores(Device, &WaitInfo, std::numeric_limits<std::uint64_t>::max());
			if (WaitResult != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(ToDeviceErrorCode(WaitResult, EPresentationErrorCode::FrameAcquisitionFailed), "vkWaitSemaphores", WaitResult));
			}
		}

		const VkResult AcquireResult = vkAcquireNextImageKHR(Device, Swapchain, std::numeric_limits<std::uint64_t>::max(), Frame.ImageAvailable, VK_NULL_HANDLE, &ActiveImageIndex);
		if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			return EPresentationStatus::SurfaceOutOfDate;
		}
		if (AcquireResult != VK_SUCCESS && AcquireResult != VK_SUBOPTIMAL_KHR)
		{
			return std::unexpected(MakeVulkanError(ToDeviceErrorCode(AcquireResult, EPresentationErrorCode::FrameAcquisitionFailed), "vkAcquireNextImageKHR", AcquireResult));
		}

		VulkanNvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, Frame.ImageAvailable, 0);
		CommandList->open();
		bFrameActive = true;
		bFrameSuboptimal = AcquireResult == VK_SUBOPTIMAL_KHR;
		return bFrameSuboptimal ? EPresentationStatus::Suboptimal : EPresentationStatus::Ready;
	}

	[[nodiscard]] std::expected<void, FPresentationError> Clear(const FLinearColor Color) override
	{
		if (!bFrameActive)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Clear requires an active presentation frame"});
		}

		CommandList->clearTextureFloat(BackBuffers[ActiveImageIndex], nvrhi::AllSubresources, nvrhi::Color(Color.Red, Color.Green, Color.Blue, Color.Alpha));
		return {};
	}

	[[nodiscard]] std::expected<EPresentationStatus, FPresentationError> Present() override
	{
		if (!bFrameActive)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Present requires an active presentation frame"});
		}

		CommandList->close();
		FFrameSync& Frame = FrameSync[FrameSlot];
		VulkanNvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, RenderFinished[ActiveImageIndex], 0);
		Frame.SubmissionInstance = NvrhiDevice->executeCommandList(CommandList);

		const VkSemaphore WaitSemaphore = RenderFinished[ActiveImageIndex];
		VkPresentInfoKHR PresentInfo{};
		PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		PresentInfo.waitSemaphoreCount = 1;
		PresentInfo.pWaitSemaphores = &WaitSemaphore;
		PresentInfo.swapchainCount = 1;
		PresentInfo.pSwapchains = &Swapchain;
		PresentInfo.pImageIndices = &ActiveImageIndex;
		const VkResult PresentResult = vkQueuePresentKHR(GraphicsQueue, &PresentInfo);
		bFrameActive = false;
		FrameSlot = (FrameSlot + 1) % FrameSync.size();

		if (PresentResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			return EPresentationStatus::SurfaceOutOfDate;
		}
		if (PresentResult != VK_SUCCESS && PresentResult != VK_SUBOPTIMAL_KHR)
		{
			return std::unexpected(MakeVulkanError(ToDeviceErrorCode(PresentResult, EPresentationErrorCode::PresentationFailed), "vkQueuePresentKHR", PresentResult));
		}

		return bFrameSuboptimal || PresentResult == VK_SUBOPTIMAL_KHR ? EPresentationStatus::Suboptimal : EPresentationStatus::Ready;
	}

	[[nodiscard]] std::expected<void, FPresentationError> WaitIdle() override
	{
		if (bFrameActive || HasActiveSecondaryViewport())
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Cannot wait for idle while a presentation frame is active"});
		}
		try
		{
			if (NvrhiDevice && !NvrhiDevice->waitForIdle())
			{
				return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceLost, "NVRHI could not wait for the Vulkan device to become idle"});
			}
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceLost, Exception.what()});
		}
		catch (...)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceLost, "Unknown exception while waiting for the Vulkan device"});
		}
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> InitializeToolUIRenderer() override
	{
		if (bFrameActive || HasActiveSecondaryViewport() || bToolUIInitialized)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI renderer initialization requires an idle, uninitialized presentation device"});
		}

		nvrhi::ShaderDesc VertexShaderDescriptor;
		VertexShaderDescriptor.shaderType = nvrhi::ShaderType::Vertex;
		VertexShaderDescriptor.debugName = "ToolUI vertex shader";
		ToolUIVertexShader = NvrhiDevice->createShader(VertexShaderDescriptor, GToolUIVertexShader, sizeof(GToolUIVertexShader));
		nvrhi::ShaderDesc PixelShaderDescriptor;
		PixelShaderDescriptor.shaderType = nvrhi::ShaderType::Pixel;
		PixelShaderDescriptor.debugName = "ToolUI fragment shader";
		ToolUIPixelShader = NvrhiDevice->createShader(PixelShaderDescriptor, GToolUIFragmentShader, sizeof(GToolUIFragmentShader));
		if (!ToolUIVertexShader || !ToolUIPixelShader)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create ToolUI shaders"});
		}

		const std::array VertexAttributes{
		    nvrhi::VertexAttributeDesc().setName("POSITION").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, pos)).setElementStride(sizeof(ImDrawVert)),
		    nvrhi::VertexAttributeDesc().setName("TEXCOORD").setFormat(nvrhi::Format::RG32_FLOAT).setOffset(offsetof(ImDrawVert, uv)).setElementStride(sizeof(ImDrawVert)),
		    nvrhi::VertexAttributeDesc().setName("COLOR").setFormat(nvrhi::Format::RGBA8_UNORM).setOffset(offsetof(ImDrawVert, col)).setElementStride(sizeof(ImDrawVert))};
		ToolUIInputLayout = NvrhiDevice->createInputLayout(VertexAttributes.data(), static_cast<std::uint32_t>(VertexAttributes.size()), ToolUIVertexShader);

		nvrhi::BindingLayoutDesc BindingLayoutDescriptor;
		BindingLayoutDescriptor.visibility = nvrhi::ShaderType::AllGraphics;
		BindingLayoutDescriptor.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
		BindingLayoutDescriptor.addItem(nvrhi::BindingLayoutItem::Sampler(0));
		BindingLayoutDescriptor.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(FToolUIPushConstants)));
		ToolUIBindingLayout = NvrhiDevice->createBindingLayout(BindingLayoutDescriptor);

		ToolUISampler = NvrhiDevice->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
		if (!ToolUIInputLayout || !ToolUIBindingLayout || !ToolUISampler)
		{
			ShutdownToolUIResources();
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create ToolUI pipeline resources"});
		}

		bToolUIInitialized = true;
		const std::expected PipelineResult = CreateToolUIPipeline();
		if (!PipelineResult)
		{
			ShutdownToolUIRenderer();
			return std::unexpected(PipelineResult.error());
		}
		for (auto& [Handle, Viewport] : Viewports)
		{
			(void)Handle;
			const std::expected ViewportPipelineResult = CreateToolUIRenderTargets(Viewport->BackBuffers, Viewport->Framebuffers, Viewport->Pipeline);
			if (!ViewportPipelineResult)
			{
				ShutdownToolUIRenderer();
				return std::unexpected(ViewportPipelineResult.error());
			}
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> RenderToolUIDrawData(const void* const OpaqueDrawData) override
	{
		if (!bFrameActive || !bToolUIInitialized || !OpaqueDrawData)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI rendering requires initialized resources, draw data, and an active presentation frame"});
		}

		const ImDrawData& DrawData = *static_cast<const ImDrawData*>(OpaqueDrawData);
		nvrhi::ICommandList* const RenderCommandList = ToolUIOverrideCommandList ? ToolUIOverrideCommandList : CommandList.Get();
		if (!RenderCommandList)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI rendering requires an active command list"});
		}
		const std::expected TextureResult = UpdateToolUITextures(DrawData, *RenderCommandList);
		if (!TextureResult)
		{
			return TextureResult;
		}
		const int FramebufferWidth = static_cast<int>(DrawData.DisplaySize.x * DrawData.FramebufferScale.x);
		const int FramebufferHeight = static_cast<int>(DrawData.DisplaySize.y * DrawData.FramebufferScale.y);
		if (FramebufferWidth <= 0 || FramebufferHeight <= 0 || DrawData.TotalVtxCount == 0 || DrawData.TotalIdxCount == 0)
		{
			return {};
		}
		nvrhi::IGraphicsPipeline* const RenderPipeline = ToolUIOverridePipeline ? ToolUIOverridePipeline : ToolUIPipeline.Get();
		nvrhi::IFramebuffer* const RenderFramebuffer = ToolUIOverrideFramebuffer ? ToolUIOverrideFramebuffer : (ActiveImageIndex < ToolUIFramebuffers.size() ? ToolUIFramebuffers[ActiveImageIndex].Get() : nullptr);
		if (!RenderCommandList || !RenderPipeline || !RenderFramebuffer)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI pipeline is not compatible with the active swapchain"});
		}

		const std::size_t VertexBytes = static_cast<std::size_t>(DrawData.TotalVtxCount) * sizeof(ImDrawVert);
		const std::size_t IndexBytes = static_cast<std::size_t>(DrawData.TotalIdxCount) * sizeof(ImDrawIdx);
		const std::size_t IndexSourceBytes = AlignVulkanBufferUpdateSourceSize(IndexBytes);
		const std::expected BufferResult = EnsureToolUIBuffers(VertexBytes, IndexSourceBytes);
		if (!BufferResult)
		{
			return BufferResult;
		}

		static_assert(VulkanBufferUpdateAlignment % sizeof(ImDrawIdx) == 0);
		static_assert(sizeof(ImDrawVert) % VulkanBufferUpdateAlignment == 0);
		ToolUIVertices.resize(static_cast<std::size_t>(DrawData.TotalVtxCount));
		// NVRHI rounds Vulkan inline update source reads to four bytes for vkCmdUpdateBuffer.
		ToolUIIndices.resize(IndexSourceBytes / sizeof(ImDrawIdx));
		std::size_t VertexOffset = 0;
		std::size_t IndexOffset = 0;
		for (const ImDrawList* const DrawList : DrawData.CmdLists)
		{
			std::memcpy(ToolUIVertices.data() + VertexOffset, DrawList->VtxBuffer.Data, static_cast<std::size_t>(DrawList->VtxBuffer.Size) * sizeof(ImDrawVert));
			std::memcpy(ToolUIIndices.data() + IndexOffset, DrawList->IdxBuffer.Data, static_cast<std::size_t>(DrawList->IdxBuffer.Size) * sizeof(ImDrawIdx));
			VertexOffset += static_cast<std::size_t>(DrawList->VtxBuffer.Size);
			IndexOffset += static_cast<std::size_t>(DrawList->IdxBuffer.Size);
		}

		RenderCommandList->writeBuffer(ToolUIVertexBuffer, ToolUIVertices.data(), VertexBytes);
		RenderCommandList->writeBuffer(ToolUIIndexBuffer, ToolUIIndices.data(), IndexBytes);
		const FToolUIPushConstants PushConstants{
		    .Scale = {2.0f / DrawData.DisplaySize.x, -2.0f / DrawData.DisplaySize.y},
		    .Translate = {-1.0f - DrawData.DisplayPos.x * (2.0f / DrawData.DisplaySize.x), 1.0f + DrawData.DisplayPos.y * (2.0f / DrawData.DisplaySize.y)}};

		std::uint32_t GlobalVertexOffset = 0;
		std::uint32_t GlobalIndexOffset = 0;
		for (const ImDrawList* const DrawList : DrawData.CmdLists)
		{
			for (const ImDrawCmd& DrawCommand : DrawList->CmdBuffer)
			{
				if (DrawCommand.UserCallback)
				{
					if (DrawCommand.UserCallback != ImDrawCallback_ResetRenderState)
					{
						DrawCommand.UserCallback(DrawList, &DrawCommand);
					}
					continue;
				}
				const auto Texture = ToolUITextures.find(static_cast<std::uint64_t>(DrawCommand.GetTexID()));
				if (Texture == ToolUITextures.end())
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI draw data references an unavailable renderer texture"});
				}

				const ImVec2 ClipMinimum{
				    (DrawCommand.ClipRect.x - DrawData.DisplayPos.x) * DrawData.FramebufferScale.x,
				    (DrawCommand.ClipRect.y - DrawData.DisplayPos.y) * DrawData.FramebufferScale.y};
				const ImVec2 ClipMaximum{
				    (DrawCommand.ClipRect.z - DrawData.DisplayPos.x) * DrawData.FramebufferScale.x,
				    (DrawCommand.ClipRect.w - DrawData.DisplayPos.y) * DrawData.FramebufferScale.y};
				const int ClipLeft = std::clamp(static_cast<int>(ClipMinimum.x), 0, FramebufferWidth);
				const int ClipTop = std::clamp(static_cast<int>(ClipMinimum.y), 0, FramebufferHeight);
				const int ClipRight = std::clamp(static_cast<int>(ClipMaximum.x), 0, FramebufferWidth);
				const int ClipBottom = std::clamp(static_cast<int>(ClipMaximum.y), 0, FramebufferHeight);
				if (ClipRight <= ClipLeft || ClipBottom <= ClipTop)
				{
					continue;
				}

				nvrhi::GraphicsState State;
				State.pipeline = RenderPipeline;
				State.framebuffer = RenderFramebuffer;
				State.bindings.push_back(Texture->second.BindingSet);
				State.vertexBuffers.push_back(nvrhi::VertexBufferBinding().setBuffer(ToolUIVertexBuffer).setSlot(0).setOffset(0));
				State.indexBuffer = nvrhi::IndexBufferBinding().setBuffer(ToolUIIndexBuffer).setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT).setOffset(0);
				State.viewport.addViewport(nvrhi::Viewport(static_cast<float>(FramebufferWidth), static_cast<float>(FramebufferHeight)));
				State.viewport.addScissorRect(nvrhi::Rect(ClipLeft, ClipRight, ClipTop, ClipBottom));
				RenderCommandList->setGraphicsState(State);
				RenderCommandList->setPushConstants(&PushConstants, sizeof(PushConstants));

				nvrhi::DrawArguments Arguments;
				Arguments.vertexCount = DrawCommand.ElemCount;
				Arguments.startIndexLocation = GlobalIndexOffset + DrawCommand.IdxOffset;
				Arguments.startVertexLocation = GlobalVertexOffset + DrawCommand.VtxOffset;
				RenderCommandList->drawIndexed(Arguments);
			}

			GlobalIndexOffset += static_cast<std::uint32_t>(DrawList->IdxBuffer.Size);
			GlobalVertexOffset += static_cast<std::uint32_t>(DrawList->VtxBuffer.Size);
		}

		return {};
	}

	void ShutdownToolUIRenderer() noexcept override
	{
		if (bFrameActive || HasActiveSecondaryViewport())
		{
			return;
		}
		if (NvrhiDevice)
		{
			try
			{
				if (!NvrhiDevice->waitForIdle())
				{
					return;
				}
			}
			catch (...)
			{
				return;
			}
		}
		ShutdownToolUIResources();
	}

	[[nodiscard]] std::expected<FPresentationViewportHandle, FPresentationError> CreateViewport(void* const WindowBackendHandle, const FExtent2D RequestedExtent) override
	{
		std::unique_ptr<FSecondaryViewport> Viewport;
		try
		{
			if (!bToolUIInitialized || !WindowBackendHandle)
			{
				return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "A secondary viewport requires initialized ToolUI and a native window handle"});
			}
			const std::expected IdleResult = WaitIdle();
			if (!IdleResult)
			{
				return std::unexpected(IdleResult.error());
			}

			Viewport = std::make_unique<FSecondaryViewport>();
			Viewport->WindowBackendHandle = WindowBackendHandle;
			VkResult Result = glfwCreateWindowSurface(Instance, static_cast<GLFWwindow*>(WindowBackendHandle), nullptr, &Viewport->Surface);
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::SurfaceCreationFailed, "glfwCreateWindowSurface", Result));
			}

			VkBool32 bSupportsPresentation = VK_FALSE;
			Result = vkGetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, QueueFamilyIndex, Viewport->Surface, &bSupportsPresentation);
			if (Result != VK_SUCCESS || bSupportsPresentation != VK_TRUE)
			{
				DestroySecondaryViewport(*Viewport);
				return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The selected Vulkan graphics queue cannot present to this viewport"});
			}

			Viewport->CommandList = NvrhiDevice->createCommandList();
			if (!Viewport->CommandList)
			{
				DestroySecondaryViewport(*Viewport);
				return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create a secondary viewport command list"});
			}

			Viewport->Extent = RequestedExtent;
			if (!RequestedExtent.IsEmpty())
			{
				const std::expected SwapchainResult = CreateSecondarySwapchain(*Viewport, RequestedExtent);
				if (!SwapchainResult)
				{
					DestroySecondaryViewport(*Viewport);
					return std::unexpected(SwapchainResult.error());
				}
			}

			if (NextViewportHandle == 0)
			{
				++NextViewportHandle;
			}
			const FPresentationViewportHandle Handle{NextViewportHandle++};
			Viewports.emplace(Handle.Value, std::move(Viewport));
			return Handle;
		}
		catch (const std::exception& Exception)
		{
			if (Viewport)
			{
				DestroySecondaryViewport(*Viewport);
			}
			return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, Exception.what()});
		}
		catch (...)
		{
			if (Viewport)
			{
				DestroySecondaryViewport(*Viewport);
			}
			return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "Unknown exception while creating a secondary viewport"});
		}
	}

	[[nodiscard]] std::expected<void, FPresentationError> DestroyViewport(const FPresentationViewportHandle Handle) override
	{
		const auto Match = Viewports.find(Handle.Value);
		if (Match == Viewports.end())
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Secondary viewport handle is invalid"});
		}
		if (Match->second->bFrameActive)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Cannot destroy a secondary viewport while its frame is active"});
		}

		const std::expected IdleResult = WaitIdle();
		if (!IdleResult)
		{
			return IdleResult;
		}
		DestroySecondaryViewport(*Match->second);
		Viewports.erase(Match);
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> ResizeViewport(const FPresentationViewportHandle Handle, const FExtent2D RequestedExtent) override
	{
		FSecondaryViewport* Viewport = nullptr;
		try
		{
			Viewport = FindViewport(Handle);
			if (!Viewport)
			{
				return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Secondary viewport handle is invalid"});
			}
			if (Viewport->bFrameActive)
			{
				return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Cannot resize a secondary viewport while its frame is active"});
			}

			const std::expected IdleResult = WaitIdle();
			if (!IdleResult)
			{
				return IdleResult;
			}
			DestroySecondarySwapchain(*Viewport);
			Viewport->Extent = RequestedExtent;
			return RequestedExtent.IsEmpty() ? std::expected<void, FPresentationError>{} : CreateSecondarySwapchain(*Viewport, RequestedExtent);
		}
		catch (const std::exception& Exception)
		{
			if (Viewport)
			{
				DestroySecondarySwapchain(*Viewport);
			}
			return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, Exception.what()});
		}
		catch (...)
		{
			if (Viewport)
			{
				DestroySecondarySwapchain(*Viewport);
			}
			return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "Unknown exception while resizing a secondary viewport"});
		}
	}

	[[nodiscard]] std::expected<EPresentationStatus, FPresentationError> BeginViewportFrame(const FPresentationViewportHandle Handle) override
	{
		FSecondaryViewport* const Viewport = FindViewport(Handle);
		if (!Viewport)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidDescriptor, "Secondary viewport handle is invalid"});
		}
		if (bFrameActive || HasActiveSecondaryViewport())
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "A presentation frame is already active"});
		}
		if (Viewport->Extent.IsEmpty() || Viewport->Swapchain == VK_NULL_HANDLE)
		{
			return EPresentationStatus::Minimized;
		}

		FFrameSync& Frame = Viewport->FrameSync[Viewport->FrameSlot];
		if (Frame.SubmissionInstance != 0)
		{
			const VkSemaphore TrackingSemaphore = VulkanNvrhiDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
			VkSemaphoreWaitInfo WaitInfo{};
			WaitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			WaitInfo.semaphoreCount = 1;
			WaitInfo.pSemaphores = &TrackingSemaphore;
			WaitInfo.pValues = &Frame.SubmissionInstance;
			const VkResult WaitResult = vkWaitSemaphores(Device, &WaitInfo, std::numeric_limits<std::uint64_t>::max());
			if (WaitResult != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(ToDeviceErrorCode(WaitResult, EPresentationErrorCode::FrameAcquisitionFailed), "vkWaitSemaphores", WaitResult));
			}
		}

		const VkResult AcquireResult = vkAcquireNextImageKHR(Device, Viewport->Swapchain, std::numeric_limits<std::uint64_t>::max(), Frame.ImageAvailable, VK_NULL_HANDLE, &Viewport->ActiveImageIndex);
		if (AcquireResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			return EPresentationStatus::SurfaceOutOfDate;
		}
		if (AcquireResult != VK_SUCCESS && AcquireResult != VK_SUBOPTIMAL_KHR)
		{
			return std::unexpected(MakeVulkanError(ToDeviceErrorCode(AcquireResult, EPresentationErrorCode::FrameAcquisitionFailed), "vkAcquireNextImageKHR", AcquireResult));
		}

		try
		{
			VulkanNvrhiDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, Frame.ImageAvailable, 0);
			Viewport->CommandList->open();
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, Exception.what()});
		}
		catch (...)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, "Unknown exception while beginning a secondary viewport command list"});
		}
		Viewport->bFrameActive = true;
		Viewport->bFrameSuboptimal = AcquireResult == VK_SUBOPTIMAL_KHR;
		return Viewport->bFrameSuboptimal ? EPresentationStatus::Suboptimal : EPresentationStatus::Ready;
	}

	[[nodiscard]] std::expected<void, FPresentationError> RenderViewportToolUIDrawData(const FPresentationViewportHandle Handle, const void* const DrawData) override
	{
		FSecondaryViewport* const Viewport = FindViewport(Handle);
		if (!Viewport || !Viewport->bFrameActive || Viewport->ActiveImageIndex >= Viewport->Framebuffers.size())
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Secondary viewport rendering requires a valid active frame"});
		}
		if (bFrameActive || ToolUIOverrideCommandList)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI viewport rendering is serialized on the presentation thread"});
		}

		ToolUIOverrideCommandList = Viewport->CommandList.Get();
		ToolUIOverridePipeline = Viewport->Pipeline.Get();
		ToolUIOverrideFramebuffer = Viewport->Framebuffers[Viewport->ActiveImageIndex].Get();
		bFrameActive = true;
		std::expected<void, FPresentationError> Result;
		try
		{
			Result = RenderToolUIDrawData(DrawData);
		}
		catch (const std::exception& Exception)
		{
			Result = std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, Exception.what()});
		}
		catch (...)
		{
			Result = std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, "Unknown exception while rendering a ToolUI viewport"});
		}
		bFrameActive = false;
		ToolUIOverrideFramebuffer = nullptr;
		ToolUIOverridePipeline = nullptr;
		ToolUIOverrideCommandList = nullptr;
		return Result;
	}

	[[nodiscard]] std::expected<EPresentationStatus, FPresentationError> PresentViewport(const FPresentationViewportHandle Handle) override
	{
		FSecondaryViewport* const Viewport = FindViewport(Handle);
		if (!Viewport || !Viewport->bFrameActive)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "Secondary viewport presentation requires an active frame"});
		}

		FFrameSync& Frame = Viewport->FrameSync[Viewport->FrameSlot];
		try
		{
			Viewport->CommandList->close();
			VulkanNvrhiDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, Viewport->RenderFinished[Viewport->ActiveImageIndex], 0);
			Frame.SubmissionInstance = NvrhiDevice->executeCommandList(Viewport->CommandList);
		}
		catch (const std::exception& Exception)
		{
			Viewport->bFrameActive = false;
			return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, Exception.what()});
		}
		catch (...)
		{
			Viewport->bFrameActive = false;
			return std::unexpected(FPresentationError{EPresentationErrorCode::CommandSubmissionFailed, "Unknown exception while submitting a secondary viewport command list"});
		}
		const VkSemaphore WaitSemaphore = Viewport->RenderFinished[Viewport->ActiveImageIndex];
		VkPresentInfoKHR PresentInfo{};
		PresentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		PresentInfo.waitSemaphoreCount = 1;
		PresentInfo.pWaitSemaphores = &WaitSemaphore;
		PresentInfo.swapchainCount = 1;
		PresentInfo.pSwapchains = &Viewport->Swapchain;
		PresentInfo.pImageIndices = &Viewport->ActiveImageIndex;
		const VkResult PresentResult = vkQueuePresentKHR(GraphicsQueue, &PresentInfo);
		Viewport->bFrameActive = false;
		Viewport->FrameSlot = (Viewport->FrameSlot + 1) % Viewport->FrameSync.size();
		if (PresentResult == VK_ERROR_OUT_OF_DATE_KHR)
		{
			return EPresentationStatus::SurfaceOutOfDate;
		}
		if (PresentResult != VK_SUCCESS && PresentResult != VK_SUBOPTIMAL_KHR)
		{
			return std::unexpected(MakeVulkanError(ToDeviceErrorCode(PresentResult, EPresentationErrorCode::PresentationFailed), "vkQueuePresentKHR", PresentResult));
		}
		return Viewport->bFrameSuboptimal || PresentResult == VK_SUBOPTIMAL_KHR ? EPresentationStatus::Suboptimal : EPresentationStatus::Ready;
	}

	[[nodiscard]] bool HasValidationErrors() const noexcept override
	{
		return ValidationState.bHasErrors.load(std::memory_order_acquire);
	}

private:
	struct FToolUIPushConstants
	{
		std::array<float, 2> Scale;
		std::array<float, 2> Translate;
	};

	struct FToolUITexture
	{
		nvrhi::TextureHandle Texture;
		nvrhi::BindingSetHandle BindingSet;
		std::uint32_t Width = 0;
		std::uint32_t Height = 0;
	};

	[[nodiscard]] std::size_t GetToolUITextureRetirementFrames() const noexcept
	{
		std::size_t Frames = std::max<std::size_t>(1, FrameSync.size());
		for (const auto& [Handle, Viewport] : Viewports)
		{
			(void)Handle;
			Frames = std::max(Frames, Viewport->FrameSync.size());
		}
		return Frames;
	}

	[[nodiscard]] std::expected<void, FPresentationError> UpdateToolUITextures(const ImDrawData& DrawData, nvrhi::ICommandList& RenderCommandList)
	{
		if (DrawData.Textures == nullptr)
		{
			return {};
		}

		for (ImTextureData* const TextureData : *DrawData.Textures)
		{
			if (TextureData == nullptr || TextureData->Status == ImTextureStatus_OK)
			{
				continue;
			}

			if (TextureData->Status == ImTextureStatus_WantCreate)
			{
				if (TextureData->Format != ImTextureFormat_RGBA32 || TextureData->Width <= 0 || TextureData->Height <= 0 || TextureData->Pixels == nullptr)
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "ToolUI dynamic textures require non-empty RGBA32 pixels"});
				}

				nvrhi::TextureDesc TextureDescriptor;
				TextureDescriptor.width = static_cast<std::uint32_t>(TextureData->Width);
				TextureDescriptor.height = static_cast<std::uint32_t>(TextureData->Height);
				TextureDescriptor.format = nvrhi::Format::RGBA8_UNORM;
				TextureDescriptor.dimension = nvrhi::TextureDimension::Texture2D;
				TextureDescriptor.debugName = "ToolUI dynamic texture";
				TextureDescriptor.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
				FToolUITexture Texture;
				Texture.Texture = NvrhiDevice->createTexture(TextureDescriptor);
				Texture.Width = TextureDescriptor.width;
				Texture.Height = TextureDescriptor.height;
				if (!Texture.Texture)
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create a ToolUI dynamic texture"});
				}

				nvrhi::BindingSetDesc BindingSetDescriptor;
				BindingSetDescriptor.addItem(nvrhi::BindingSetItem::Texture_SRV(0, Texture.Texture));
				BindingSetDescriptor.addItem(nvrhi::BindingSetItem::Sampler(0, ToolUISampler));
				BindingSetDescriptor.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(FToolUIPushConstants)));
				Texture.BindingSet = NvrhiDevice->createBindingSet(BindingSetDescriptor, ToolUIBindingLayout);
				if (!Texture.BindingSet)
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create a ToolUI dynamic texture binding set"});
				}

				const std::uint64_t TextureId = NextToolUITextureId++;
				ToolUITextures.emplace(TextureId, std::move(Texture));
				TextureData->SetTexID(static_cast<ImTextureID>(TextureId));
			}

			if (TextureData->Status == ImTextureStatus_WantCreate || TextureData->Status == ImTextureStatus_WantUpdates)
			{
				const auto Texture = ToolUITextures.find(static_cast<std::uint64_t>(TextureData->GetTexID()));
				if (Texture == ToolUITextures.end() || TextureData->Pixels == nullptr || TextureData->Format != ImTextureFormat_RGBA32 || Texture->second.Width != static_cast<std::uint32_t>(TextureData->Width) || Texture->second.Height != static_cast<std::uint32_t>(TextureData->Height))
				{
					return std::unexpected(FPresentationError{EPresentationErrorCode::InvalidState, "ToolUI requested an invalid dynamic texture update"});
				}

				// NVRHI exposes full-mip writes here. Atlas updates are infrequent, so a full upload keeps the backend simple and correct.
				RenderCommandList.writeTexture(Texture->second.Texture, 0, 0, TextureData->Pixels, static_cast<std::size_t>(TextureData->Width) * 4);
				TextureData->SetStatus(ImTextureStatus_OK);
				continue;
			}

			if (TextureData->Status == ImTextureStatus_WantDestroy && TextureData->UnusedFrames >= static_cast<int>(GetToolUITextureRetirementFrames()))
			{
				ToolUITextures.erase(static_cast<std::uint64_t>(TextureData->GetTexID()));
				TextureData->SetTexID(ImTextureID_Invalid);
				TextureData->BackendUserData = nullptr;
				TextureData->SetStatus(ImTextureStatus_Destroyed);
			}
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> EnsureToolUIBuffers(const std::size_t VertexBytes, const std::size_t IndexBytes)
	{
		if (ToolUIVertexBufferCapacity < VertexBytes)
		{
			ToolUIVertexBufferCapacity = std::bit_ceil(std::max(VertexBytes, std::size_t{64} * 1024));
			nvrhi::BufferDesc BufferDescriptor;
			BufferDescriptor.byteSize = ToolUIVertexBufferCapacity;
			BufferDescriptor.debugName = "ToolUI vertices";
			BufferDescriptor.isVertexBuffer = true;
			BufferDescriptor.enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer);
			ToolUIVertexBuffer = NvrhiDevice->createBuffer(BufferDescriptor);
		}

		if (ToolUIIndexBufferCapacity < IndexBytes)
		{
			ToolUIIndexBufferCapacity = std::bit_ceil(std::max(IndexBytes, std::size_t{32} * 1024));
			nvrhi::BufferDesc BufferDescriptor;
			BufferDescriptor.byteSize = ToolUIIndexBufferCapacity;
			BufferDescriptor.debugName = "ToolUI indices";
			BufferDescriptor.isIndexBuffer = true;
			BufferDescriptor.enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer);
			ToolUIIndexBuffer = NvrhiDevice->createBuffer(BufferDescriptor);
		}

		if (!ToolUIVertexBuffer || !ToolUIIndexBuffer)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not allocate ToolUI draw buffers"});
		}
		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> CreateToolUIRenderTargets(const std::span<const nvrhi::TextureHandle> RenderTargets, std::vector<nvrhi::FramebufferHandle>& Framebuffers, nvrhi::GraphicsPipelineHandle& Pipeline)
	{
		Framebuffers.clear();
		Pipeline = nullptr;
		if (!bToolUIInitialized || RenderTargets.empty())
		{
			return {};
		}

		Framebuffers.reserve(RenderTargets.size());
		for (const nvrhi::TextureHandle& RenderTarget : RenderTargets)
		{
			nvrhi::FramebufferDesc FramebufferDescriptor;
			FramebufferDescriptor.addColorAttachment(RenderTarget);
			nvrhi::FramebufferHandle Framebuffer = NvrhiDevice->createFramebuffer(FramebufferDescriptor);
			if (!Framebuffer)
			{
				Framebuffers.clear();
				return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "NVRHI could not create a ToolUI framebuffer"});
			}
			Framebuffers.push_back(std::move(Framebuffer));
		}

		nvrhi::BlendState::RenderTarget BlendTarget;
		BlendTarget.enableBlend();
		BlendTarget.srcBlend = nvrhi::BlendFactor::SrcAlpha;
		BlendTarget.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
		BlendTarget.srcBlendAlpha = nvrhi::BlendFactor::One;
		BlendTarget.destBlendAlpha = nvrhi::BlendFactor::InvSrcAlpha;
		nvrhi::RenderState RenderState;
		RenderState.blendState.setRenderTarget(0, BlendTarget);
		RenderState.depthStencilState.disableDepthTest();
		RenderState.depthStencilState.disableDepthWrite();
		RenderState.rasterState.setCullNone();
		RenderState.rasterState.enableScissor();

		nvrhi::GraphicsPipelineDesc PipelineDescriptor;
		PipelineDescriptor.primType = nvrhi::PrimitiveType::TriangleList;
		PipelineDescriptor.inputLayout = ToolUIInputLayout;
		PipelineDescriptor.VS = ToolUIVertexShader;
		PipelineDescriptor.PS = ToolUIPixelShader;
		PipelineDescriptor.renderState = RenderState;
		PipelineDescriptor.bindingLayouts.push_back(ToolUIBindingLayout);
		Pipeline = NvrhiDevice->createGraphicsPipeline(PipelineDescriptor, Framebuffers.front()->getFramebufferInfo());
		if (!Pipeline)
		{
			Framebuffers.clear();
			return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "NVRHI could not create the ToolUI graphics pipeline"});
		}

		return {};
	}

	[[nodiscard]] std::expected<void, FPresentationError> CreateToolUIPipeline()
	{
		ToolUIFramebuffers.clear();
		ToolUIPipeline = nullptr;
		if (Swapchain == VK_NULL_HANDLE)
		{
			return {};
		}
		return CreateToolUIRenderTargets(BackBuffers, ToolUIFramebuffers, ToolUIPipeline);
	}

	void ShutdownToolUIResources() noexcept
	{
		bToolUIInitialized = false;
		for (auto& [Handle, Viewport] : Viewports)
		{
			(void)Handle;
			Viewport->Framebuffers.clear();
			Viewport->Pipeline = nullptr;
		}
		ToolUIVertices.clear();
		ToolUIIndices.clear();
		ToolUIVertexBuffer = nullptr;
		ToolUIIndexBuffer = nullptr;
		ToolUIVertexBufferCapacity = 0;
		ToolUIIndexBufferCapacity = 0;
		ToolUIFramebuffers.clear();
		ToolUIPipeline = nullptr;
		ToolUITextures.clear();
		ToolUIBindingLayout = nullptr;
		ToolUISampler = nullptr;
		ToolUIInputLayout = nullptr;
		ToolUIPixelShader = nullptr;
		ToolUIVertexShader = nullptr;
	}

	[[nodiscard]] bool HasActiveSecondaryViewport() const noexcept
	{
		return std::ranges::any_of(Viewports, [](const auto& Entry)
		                           {
			                           return Entry.second->bFrameActive;
		                           });
	}

	[[nodiscard]] FSecondaryViewport* FindViewport(const FPresentationViewportHandle Handle) noexcept
	{
		const auto Match = Viewports.find(Handle.Value);
		return Match == Viewports.end() ? nullptr : Match->second.get();
	}

	[[nodiscard]] std::expected<void, FPresentationError> CreateSecondarySwapchain(FSecondaryViewport& Viewport, const FExtent2D RequestedExtent)
	{
		VkSurfaceCapabilitiesKHR Capabilities{};
		VkResult Result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Viewport.Surface, &Capabilities);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", Result));
		}
		constexpr VkImageUsageFlags RequiredUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ((Capabilities.supportedUsageFlags & RequiredUsage) != RequiredUsage)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The secondary Vulkan surface does not support color attachment and transfer destination usage"});
		}

		std::uint32_t FormatCount = 0;
		Result = vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Viewport.Surface, &FormatCount, nullptr);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceFormatsKHR", Result));
		}
		if (FormatCount == 0)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The secondary Vulkan surface exposes no swapchain formats"});
		}
		std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
		Result = vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Viewport.Surface, &FormatCount, Formats.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceFormatsKHR", Result));
		}

		std::uint32_t PresentModeCount = 0;
		Result = vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Viewport.Surface, &PresentModeCount, nullptr);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfacePresentModesKHR", Result));
		}
		if (PresentModeCount == 0)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The secondary Vulkan surface exposes no presentation modes"});
		}
		std::vector<VkPresentModeKHR> PresentModes(PresentModeCount);
		Result = vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Viewport.Surface, &PresentModeCount, PresentModes.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfacePresentModesKHR", Result));
		}

		Viewport.SurfaceFormat = ChooseSurfaceFormat(Formats);
		const nvrhi::Format NvrhiFormat = ToNvrhiFormat(Viewport.SurfaceFormat.format);
		if (NvrhiFormat == nvrhi::Format::UNKNOWN)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The secondary Vulkan surface does not expose a supported 8-bit RGBA swapchain format"});
		}

		if (Capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
		{
			Viewport.Extent = {Capabilities.currentExtent.width, Capabilities.currentExtent.height};
		}
		else
		{
			Viewport.Extent = ClampPresentationExtent(RequestedExtent, {Capabilities.minImageExtent.width, Capabilities.minImageExtent.height}, {Capabilities.maxImageExtent.width, Capabilities.maxImageExtent.height});
		}

		VkSwapchainCreateInfoKHR SwapchainInfo{};
		SwapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		SwapchainInfo.surface = Viewport.Surface;
		SwapchainInfo.minImageCount = ChooseSwapchainImageCount(Capabilities.minImageCount, Capabilities.maxImageCount, Descriptor.DesiredImageCount);
		SwapchainInfo.imageFormat = Viewport.SurfaceFormat.format;
		SwapchainInfo.imageColorSpace = Viewport.SurfaceFormat.colorSpace;
		SwapchainInfo.imageExtent = {Viewport.Extent.Width, Viewport.Extent.Height};
		SwapchainInfo.imageArrayLayers = 1;
		SwapchainInfo.imageUsage = RequiredUsage;
		SwapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		SwapchainInfo.preTransform = Capabilities.currentTransform;
		SwapchainInfo.compositeAlpha = ChooseCompositeAlpha(Capabilities.supportedCompositeAlpha);
		SwapchainInfo.presentMode = ChoosePresentMode(PresentModes, Descriptor.bVSync);
		SwapchainInfo.clipped = VK_TRUE;
		Result = vkCreateSwapchainKHR(Device, &SwapchainInfo, nullptr, &Viewport.Swapchain);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSwapchainKHR", Result));
		}

		std::uint32_t ImageCount = 0;
		Result = vkGetSwapchainImagesKHR(Device, Viewport.Swapchain, &ImageCount, nullptr);
		if (Result != VK_SUCCESS || ImageCount == 0)
		{
			DestroySecondarySwapchain(Viewport);
			return Result == VK_SUCCESS
			           ? std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "The secondary Vulkan swapchain exposes no images"})
			           : std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetSwapchainImagesKHR", Result));
		}
		std::vector<VkImage> Images(ImageCount);
		Result = vkGetSwapchainImagesKHR(Device, Viewport.Swapchain, &ImageCount, Images.data());
		if (Result != VK_SUCCESS)
		{
			DestroySecondarySwapchain(Viewport);
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetSwapchainImagesKHR", Result));
		}

		nvrhi::TextureDesc TextureDescriptor;
		TextureDescriptor.width = Viewport.Extent.Width;
		TextureDescriptor.height = Viewport.Extent.Height;
		TextureDescriptor.format = NvrhiFormat;
		TextureDescriptor.dimension = nvrhi::TextureDimension::Texture2D;
		TextureDescriptor.debugName = "Herta secondary viewport swapchain image";
		TextureDescriptor.isShaderResource = false;
		TextureDescriptor.isRenderTarget = true;
		TextureDescriptor.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);

		Viewport.BackBuffers.reserve(Images.size());
		for (const VkImage Image : Images)
		{
			const std::uint64_t ImageValue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Image));
			nvrhi::TextureHandle BackBuffer = NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(ImageValue), TextureDescriptor);
			if (!BackBuffer)
			{
				DestroySecondarySwapchain(Viewport);
				return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "NVRHI could not wrap a secondary Vulkan swapchain image"});
			}
			Viewport.BackBuffers.push_back(std::move(BackBuffer));
		}

		VkSemaphoreCreateInfo SemaphoreInfo{};
		SemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		Viewport.RenderFinished.resize(Images.size());
		Viewport.FrameSync.resize(Images.size());
		for (std::size_t Index = 0; Index < Images.size(); ++Index)
		{
			Result = vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &Viewport.RenderFinished[Index]);
			if (Result != VK_SUCCESS)
			{
				DestroySecondarySwapchain(Viewport);
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSemaphore", Result));
			}
			Result = vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &Viewport.FrameSync[Index].ImageAvailable);
			if (Result != VK_SUCCESS)
			{
				DestroySecondarySwapchain(Viewport);
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSemaphore", Result));
			}
		}

		Viewport.FrameSlot = 0;
		const std::expected RenderTargetsResult = CreateToolUIRenderTargets(Viewport.BackBuffers, Viewport.Framebuffers, Viewport.Pipeline);
		if (!RenderTargetsResult)
		{
			DestroySecondarySwapchain(Viewport);
			return RenderTargetsResult;
		}
		return {};
	}

	void DestroySecondarySwapchain(FSecondaryViewport& Viewport) noexcept
	{
		Viewport.bFrameActive = false;
		Viewport.Framebuffers.clear();
		Viewport.Pipeline = nullptr;
		Viewport.BackBuffers.clear();
		if (Device != VK_NULL_HANDLE)
		{
			for (FFrameSync& Frame : Viewport.FrameSync)
			{
				if (Frame.ImageAvailable != VK_NULL_HANDLE)
				{
					vkDestroySemaphore(Device, Frame.ImageAvailable, nullptr);
				}
			}
			for (const VkSemaphore Semaphore : Viewport.RenderFinished)
			{
				if (Semaphore != VK_NULL_HANDLE)
				{
					vkDestroySemaphore(Device, Semaphore, nullptr);
				}
			}
		}
		Viewport.FrameSync.clear();
		Viewport.RenderFinished.clear();
		if (Device != VK_NULL_HANDLE && Viewport.Swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(Device, Viewport.Swapchain, nullptr);
		}
		Viewport.Swapchain = VK_NULL_HANDLE;
		Viewport.FrameSlot = 0;
		Viewport.ActiveImageIndex = 0;
		Viewport.bFrameSuboptimal = false;
	}

	void DestroySecondaryViewport(FSecondaryViewport& Viewport) noexcept
	{
		DestroySecondarySwapchain(Viewport);
		Viewport.CommandList = nullptr;
		if (Instance != VK_NULL_HANDLE && Viewport.Surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(Instance, Viewport.Surface, nullptr);
		}
		Viewport.Surface = VK_NULL_HANDLE;
		Viewport.WindowBackendHandle = nullptr;
	}

	[[nodiscard]] std::expected<void, FPresentationError> CreateSwapchain(const FExtent2D RequestedExtent)
	{
		const std::expected IdleResult = WaitIdle();
		if (!IdleResult)
		{
			return IdleResult;
		}
		DestroySwapchain();

		VkSurfaceCapabilitiesKHR Capabilities{};
		VkResult Result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(PhysicalDevice, Surface, &Capabilities);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", Result));
		}
		constexpr VkImageUsageFlags RequiredUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ((Capabilities.supportedUsageFlags & RequiredUsage) != RequiredUsage)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The Vulkan surface does not support color attachment and transfer destination usage"});
		}

		std::uint32_t FormatCount = 0;
		Result = vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &FormatCount, nullptr);
		if (Result != VK_SUCCESS || FormatCount == 0)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceFormatsKHR", Result));
		}
		std::vector<VkSurfaceFormatKHR> Formats(FormatCount);
		Result = vkGetPhysicalDeviceSurfaceFormatsKHR(PhysicalDevice, Surface, &FormatCount, Formats.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfaceFormatsKHR", Result));
		}

		std::uint32_t PresentModeCount = 0;
		Result = vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &PresentModeCount, nullptr);
		if (Result != VK_SUCCESS || PresentModeCount == 0)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfacePresentModesKHR", Result));
		}
		std::vector<VkPresentModeKHR> PresentModes(PresentModeCount);
		Result = vkGetPhysicalDeviceSurfacePresentModesKHR(PhysicalDevice, Surface, &PresentModeCount, PresentModes.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetPhysicalDeviceSurfacePresentModesKHR", Result));
		}

		SurfaceFormat = ChooseSurfaceFormat(Formats);
		const nvrhi::Format NvrhiFormat = ToNvrhiFormat(SurfaceFormat.format);
		if (NvrhiFormat == nvrhi::Format::UNKNOWN)
		{
			return std::unexpected(FPresentationError{EPresentationErrorCode::Unsupported, "The Vulkan surface does not expose a supported 8-bit RGBA swapchain format"});
		}

		if (Capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max())
		{
			Extent = {Capabilities.currentExtent.width, Capabilities.currentExtent.height};
		}
		else
		{
			Extent = ClampPresentationExtent(RequestedExtent, {Capabilities.minImageExtent.width, Capabilities.minImageExtent.height}, {Capabilities.maxImageExtent.width, Capabilities.maxImageExtent.height});
		}

		const std::uint32_t ImageCount = ChooseSwapchainImageCount(Capabilities.minImageCount, Capabilities.maxImageCount, Descriptor.DesiredImageCount);
		VkSwapchainCreateInfoKHR SwapchainInfo{};
		SwapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		SwapchainInfo.surface = Surface;
		SwapchainInfo.minImageCount = ImageCount;
		SwapchainInfo.imageFormat = SurfaceFormat.format;
		SwapchainInfo.imageColorSpace = SurfaceFormat.colorSpace;
		SwapchainInfo.imageExtent = {Extent.Width, Extent.Height};
		SwapchainInfo.imageArrayLayers = 1;
		SwapchainInfo.imageUsage = RequiredUsage;
		SwapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		SwapchainInfo.preTransform = Capabilities.currentTransform;
		SwapchainInfo.compositeAlpha = ChooseCompositeAlpha(Capabilities.supportedCompositeAlpha);
		SwapchainInfo.presentMode = ChoosePresentMode(PresentModes, Descriptor.bVSync);
		SwapchainInfo.clipped = VK_TRUE;
		Result = vkCreateSwapchainKHR(Device, &SwapchainInfo, nullptr, &Swapchain);
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSwapchainKHR", Result));
		}

		std::uint32_t SwapchainImageCount = 0;
		Result = vkGetSwapchainImagesKHR(Device, Swapchain, &SwapchainImageCount, nullptr);
		if (Result != VK_SUCCESS || SwapchainImageCount == 0)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetSwapchainImagesKHR", Result));
		}
		std::vector<VkImage> Images(SwapchainImageCount);
		Result = vkGetSwapchainImagesKHR(Device, Swapchain, &SwapchainImageCount, Images.data());
		if (Result != VK_SUCCESS)
		{
			return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkGetSwapchainImagesKHR", Result));
		}

		nvrhi::TextureDesc TextureDescriptor;
		TextureDescriptor.width = Extent.Width;
		TextureDescriptor.height = Extent.Height;
		TextureDescriptor.format = NvrhiFormat;
		TextureDescriptor.dimension = nvrhi::TextureDimension::Texture2D;
		TextureDescriptor.debugName = "Herta swapchain image";
		TextureDescriptor.isShaderResource = false;
		TextureDescriptor.isRenderTarget = true;
		TextureDescriptor.enableAutomaticStateTracking(nvrhi::ResourceStates::Present);

		BackBuffers.reserve(Images.size());
		for (const VkImage Image : Images)
		{
			const std::uint64_t ImageValue = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Image));
			nvrhi::TextureHandle BackBuffer = NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(ImageValue), TextureDescriptor);
			if (!BackBuffer)
			{
				return std::unexpected(FPresentationError{EPresentationErrorCode::SwapchainCreationFailed, "NVRHI could not wrap a Vulkan swapchain image"});
			}
			BackBuffers.push_back(std::move(BackBuffer));
		}

		VkSemaphoreCreateInfo SemaphoreInfo{};
		SemaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		RenderFinished.resize(Images.size());
		FrameSync.resize(Images.size());
		for (std::size_t Index = 0; Index < Images.size(); ++Index)
		{
			Result = vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &RenderFinished[Index]);
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSemaphore", Result));
			}
			Result = vkCreateSemaphore(Device, &SemaphoreInfo, nullptr, &FrameSync[Index].ImageAvailable);
			if (Result != VK_SUCCESS)
			{
				return std::unexpected(MakeVulkanError(EPresentationErrorCode::SwapchainCreationFailed, "vkCreateSemaphore", Result));
			}
		}

		FrameSlot = 0;
		const std::expected ToolUIPipelineResult = CreateToolUIPipeline();
		if (!ToolUIPipelineResult)
		{
			return ToolUIPipelineResult;
		}
		return {};
	}

	void DestroySwapchain() noexcept
	{
		if (Device != VK_NULL_HANDLE)
		{
			for (FFrameSync& Frame : FrameSync)
			{
				if (Frame.ImageAvailable != VK_NULL_HANDLE)
				{
					vkDestroySemaphore(Device, Frame.ImageAvailable, nullptr);
				}
			}
			for (const VkSemaphore Semaphore : RenderFinished)
			{
				if (Semaphore != VK_NULL_HANDLE)
				{
					vkDestroySemaphore(Device, Semaphore, nullptr);
				}
			}
		}

		ToolUIFramebuffers.clear();
		ToolUIPipeline = nullptr;
		FrameSync.clear();
		RenderFinished.clear();
		BackBuffers.clear();
		if (Device != VK_NULL_HANDLE && Swapchain != VK_NULL_HANDLE)
		{
			vkDestroySwapchainKHR(Device, Swapchain, nullptr);
		}
		Swapchain = VK_NULL_HANDLE;
	}

	void Shutdown() noexcept
	{
		bFrameActive = false;
		for (auto& [Handle, Viewport] : Viewports)
		{
			(void)Handle;
			Viewport->bFrameActive = false;
		}
		if (NvrhiDevice)
		{
			try
			{
				(void)NvrhiDevice->waitForIdle();
			}
			catch (...) // NOLINT(bugprone-empty-catch)
			{
				// Teardown still needs to release the native device after a backend failure.
			}
		}
		for (auto& [Handle, Viewport] : Viewports)
		{
			(void)Handle;
			DestroySecondaryViewport(*Viewport);
		}
		Viewports.clear();
		DestroySwapchain();
		ShutdownToolUIResources();
		CommandList = nullptr;
		NvrhiDevice = nullptr;
		VulkanNvrhiDevice = nullptr;

		if (Device != VK_NULL_HANDLE)
		{
			vkDestroyDevice(Device, nullptr);
			Device = VK_NULL_HANDLE;
		}
		if (Instance != VK_NULL_HANDLE && Surface != VK_NULL_HANDLE)
		{
			vkDestroySurfaceKHR(Instance, Surface, nullptr);
			Surface = VK_NULL_HANDLE;
		}
		if (Instance != VK_NULL_HANDLE && DebugMessenger != VK_NULL_HANDLE)
		{
			const auto DestroyDebugMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(Instance, "vkDestroyDebugUtilsMessengerEXT"));
			if (DestroyDebugMessenger)
			{
				DestroyDebugMessenger(Instance, DebugMessenger, nullptr);
			}
			DebugMessenger = VK_NULL_HANDLE;
		}
		if (Instance != VK_NULL_HANDLE)
		{
			vkDestroyInstance(Instance, nullptr);
			Instance = VK_NULL_HANDLE;
		}
	}

	FNvrhiVulkanPresentationDescriptor Descriptor;
	FValidationState ValidationState;
	FNvrhiMessageCallback NvrhiCallback;
	VkInstance Instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT DebugMessenger = VK_NULL_HANDLE;
	VkSurfaceKHR Surface = VK_NULL_HANDLE;
	VkPhysicalDevice PhysicalDevice = VK_NULL_HANDLE;
	VkDevice Device = VK_NULL_HANDLE;
	VkQueue GraphicsQueue = VK_NULL_HANDLE;
	std::uint32_t QueueFamilyIndex = 0;
	VkSwapchainKHR Swapchain = VK_NULL_HANDLE;
	VkSurfaceFormatKHR SurfaceFormat{};
	FExtent2D Extent;
	nvrhi::vulkan::DeviceHandle VulkanNvrhiDevice;
	nvrhi::DeviceHandle NvrhiDevice;
	nvrhi::CommandListHandle CommandList;
	std::vector<nvrhi::TextureHandle> BackBuffers;
	std::vector<VkSemaphore> RenderFinished;
	std::vector<FFrameSync> FrameSync;
	std::size_t FrameSlot = 0;
	std::uint32_t ActiveImageIndex = 0;
	bool bDebugUtilsEnabled = false;
	bool bFrameActive = false;
	bool bFrameSuboptimal = false;
	nvrhi::ShaderHandle ToolUIVertexShader;
	nvrhi::ShaderHandle ToolUIPixelShader;
	nvrhi::InputLayoutHandle ToolUIInputLayout;
	nvrhi::BindingLayoutHandle ToolUIBindingLayout;
	nvrhi::SamplerHandle ToolUISampler;
	std::unordered_map<std::uint64_t, FToolUITexture> ToolUITextures;
	std::uint64_t NextToolUITextureId = 1;
	nvrhi::GraphicsPipelineHandle ToolUIPipeline;
	nvrhi::ICommandList* ToolUIOverrideCommandList = nullptr;
	nvrhi::IGraphicsPipeline* ToolUIOverridePipeline = nullptr;
	nvrhi::IFramebuffer* ToolUIOverrideFramebuffer = nullptr;
	nvrhi::BufferHandle ToolUIVertexBuffer;
	nvrhi::BufferHandle ToolUIIndexBuffer;
	std::vector<nvrhi::FramebufferHandle> ToolUIFramebuffers;
	std::vector<ImDrawVert> ToolUIVertices;
	std::vector<ImDrawIdx> ToolUIIndices;
	std::size_t ToolUIVertexBufferCapacity = 0;
	std::size_t ToolUIIndexBufferCapacity = 0;
	bool bToolUIInitialized = false;
	std::unordered_map<std::uint64_t, std::unique_ptr<FSecondaryViewport>> Viewports;
	std::uint64_t NextViewportHandle = 1;
};
}

std::expected<std::unique_ptr<INvrhiVulkanPresentation>, FPresentationError> CreateNvrhiVulkanPresentation(FNvrhiVulkanPresentationDescriptor Descriptor)
{
	try
	{
		auto Presentation = std::make_unique<FNvrhiVulkanPresentation>(std::move(Descriptor));
		const std::expected Result = Presentation->Initialize();
		if (!Result)
		{
			return std::unexpected(Result.error());
		}
		return std::unique_ptr<INvrhiVulkanPresentation>(std::move(Presentation));
	}
	catch (const std::exception& Exception)
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, Exception.what()});
	}
	catch (...)
	{
		return std::unexpected(FPresentationError{EPresentationErrorCode::DeviceCreationFailed, "Unknown exception while creating Vulkan presentation"});
	}
}
}
