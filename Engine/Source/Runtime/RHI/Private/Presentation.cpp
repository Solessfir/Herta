#include "Herta/RHI/Presentation.h"

namespace Herta
{
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
