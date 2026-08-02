#include "Herta/RHI/Presentation.h"

#include <cmath>

namespace Herta
{
namespace
{
[[nodiscard]] float ConvertSrgb8ChannelToLinear(const std::uint8_t Channel) noexcept
{
	const float Srgb = static_cast<float>(Channel) / 255.0f;
	return Srgb <= 0.04045f ? Srgb / 12.92f : std::pow((Srgb + 0.055f) / 1.055f, 2.4f);
}
}

FLinearColor ConvertSrgb8ToLinearColor(const std::uint8_t Red, const std::uint8_t Green, const std::uint8_t Blue, const std::uint8_t Alpha) noexcept
{
	return {
	    ConvertSrgb8ChannelToLinear(Red),
	    ConvertSrgb8ChannelToLinear(Green),
	    ConvertSrgb8ChannelToLinear(Blue),
	    static_cast<float>(Alpha) / 255.0f};
}

std::string_view ToString(const EPresentationErrorCode Code) noexcept
{
	switch (Code)
	{
		case EPresentationErrorCode::InvalidDescriptor:
			return "Invalid descriptor";
		case EPresentationErrorCode::InvalidState:
			return "Invalid state";
		case EPresentationErrorCode::Unsupported:
			return "Unsupported";
		case EPresentationErrorCode::InstanceCreationFailed:
			return "Instance creation failed";
		case EPresentationErrorCode::SurfaceCreationFailed:
			return "Surface creation failed";
		case EPresentationErrorCode::PhysicalDeviceUnavailable:
			return "Physical device unavailable";
		case EPresentationErrorCode::DeviceCreationFailed:
			return "Device creation failed";
		case EPresentationErrorCode::SwapchainCreationFailed:
			return "Swapchain creation failed";
		case EPresentationErrorCode::FrameAcquisitionFailed:
			return "Frame acquisition failed";
		case EPresentationErrorCode::CommandSubmissionFailed:
			return "Command submission failed";
		case EPresentationErrorCode::PresentationFailed:
			return "Presentation failed";
		case EPresentationErrorCode::DeviceLost:
			return "Device lost";
	}

	return "Unknown presentation error";
}
}
