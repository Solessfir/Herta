#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace Herta
{
struct FExtent2D
{
	std::uint32_t Width = 0;
	std::uint32_t Height = 0;

	[[nodiscard]] constexpr bool IsEmpty() const noexcept
	{
		return Width == 0 || Height == 0;
	}

	[[nodiscard]] constexpr bool operator==(const FExtent2D&) const noexcept = default;
};

struct FLinearColor
{
	float Red = 0.0f;
	float Green = 0.0f;
	float Blue = 0.0f;
	float Alpha = 1.0f;
};

struct FPresentationViewportHandle
{
	std::uint64_t Value = 0;

	[[nodiscard]] constexpr bool IsValid() const noexcept
	{
		return Value != 0;
	}

	[[nodiscard]] constexpr bool operator==(const FPresentationViewportHandle&) const noexcept = default;
};

enum class EPresentationStatus : std::uint8_t
{
	Ready,
	Minimized,
	SurfaceOutOfDate,
	Suboptimal
};

enum class EPresentationErrorCode : std::uint8_t
{
	InvalidDescriptor,
	InvalidState,
	Unsupported,
	InstanceCreationFailed,
	SurfaceCreationFailed,
	PhysicalDeviceUnavailable,
	DeviceCreationFailed,
	SwapchainCreationFailed,
	FrameAcquisitionFailed,
	CommandSubmissionFailed,
	PresentationFailed,
	DeviceLost
};

struct FPresentationError
{
	EPresentationErrorCode Code = EPresentationErrorCode::Unsupported;
	std::string Message;
};

[[nodiscard]] constexpr std::uint32_t ChooseSwapchainImageCount(const std::uint32_t Minimum, const std::uint32_t Maximum, const std::uint32_t Desired) noexcept
{
	const std::uint32_t EffectiveDesired = std::max(Desired, Minimum);
	return Maximum == 0 ? EffectiveDesired : std::min(EffectiveDesired, Maximum);
}

[[nodiscard]] constexpr FExtent2D ClampPresentationExtent(const FExtent2D Requested, const FExtent2D Minimum, const FExtent2D Maximum) noexcept
{
	return {
	    std::clamp(Requested.Width, Minimum.Width, Maximum.Width),
	    std::clamp(Requested.Height, Minimum.Height, Maximum.Height)};
}

[[nodiscard]] std::string_view ToString(EPresentationErrorCode Code) noexcept;

class IPresentationDevice
{
public:
	virtual ~IPresentationDevice() = default;

	IPresentationDevice(const IPresentationDevice&) = delete;
	IPresentationDevice& operator=(const IPresentationDevice&) = delete;
	IPresentationDevice(IPresentationDevice&&) = delete;
	IPresentationDevice& operator=(IPresentationDevice&&) = delete;

	[[nodiscard]] virtual FExtent2D GetExtent() const noexcept = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> Resize(FExtent2D Extent) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> BeginFrame() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> Clear(FLinearColor Color) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> Present() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WaitIdle() = 0;

protected:
	IPresentationDevice() = default;
};
}
