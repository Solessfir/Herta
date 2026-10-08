#pragma once

#include <algorithm>
#include <cmath>
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

struct FSrgbColor
{
	float Red = 0.f;
	float Green = 0.f;
	float Blue = 0.f;
	float Alpha = 1.f;
};

[[nodiscard]] constexpr FSrgbColor ConvertSrgb8ToSrgbColor(const std::uint8_t Red, const std::uint8_t Green, const std::uint8_t Blue, const std::uint8_t Alpha = 255) noexcept
{
	return {
	    static_cast<float>(Red) / 255.f,
	    static_cast<float>(Green) / 255.f,
	    static_cast<float>(Blue) / 255.f,
	    static_cast<float>(Alpha) / 255.f,
	};
}

// SMPTE ST 2084 (PQ) signal for an absolute luminance in cd/m^2, as HDR10 swapchains expect.
[[nodiscard]] inline float EncodePqLuminance(const float Luminance) noexcept
{
	const float Y = std::pow(std::clamp(Luminance / 10000.f, 0.f, 1.f), 0.1593017578125f);
	return std::pow((0.8359375f + 18.8515625f * Y) / (1.f + 18.6875f * Y), 78.84375f);
}

// An sRGB color shown at paper white on an HDR10 target: decoded, converted from Rec.709 to Rec.2020 primaries, and PQ encoded. ToolUI.frag does the same per pixel.
[[nodiscard]] inline FSrgbColor EncodeHdr10Color(const FSrgbColor Color, const float PaperWhite) noexcept
{
	const auto Decode = [](const float Value)
	{
		return Value <= 0.04045f ? Value / 12.92f : std::pow((Value + 0.055f) / 1.055f, 2.4f);
	};

	const float Red = Decode(Color.Red);
	const float Green = Decode(Color.Green);
	const float Blue = Decode(Color.Blue);
	return {
	    .Red = EncodePqLuminance((0.627404f * Red + 0.329283f * Green + 0.043313f * Blue) * PaperWhite),
	    .Green = EncodePqLuminance((0.069097f * Red + 0.919540f * Green + 0.011362f * Blue) * PaperWhite),
	    .Blue = EncodePqLuminance((0.016391f * Red + 0.088013f * Green + 0.895595f * Blue) * PaperWhite),
	    .Alpha = Color.Alpha,
	};
}

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
	    std::clamp(Requested.Height, Minimum.Height, Maximum.Height),
	};
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
	[[nodiscard]] virtual bool IsVSyncEnabled() const noexcept = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> SetVSyncEnabled(bool bEnabled) = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> Resize(FExtent2D Extent) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> BeginFrame() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> Clear(FSrgbColor Color) = 0;
	[[nodiscard]] virtual std::expected<EPresentationStatus, FPresentationError> Present() = 0;
	[[nodiscard]] virtual std::expected<void, FPresentationError> WaitIdle() = 0;

protected:
	IPresentationDevice() = default;
};
}
