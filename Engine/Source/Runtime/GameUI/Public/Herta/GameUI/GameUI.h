#pragma once

#include "Herta/Math/Vector.h"
#include "Herta/RHI/Graphics.h"
#include "Herta/RHI/Presentation.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FGameUIError
{
	std::string Message;
};

// sRGB color packed as the presenter expects, with alpha in the high byte.
[[nodiscard]] constexpr std::uint32_t MakeGameUIColor(const std::uint8_t Red, const std::uint8_t Green, const std::uint8_t Blue, const std::uint8_t Alpha = 255) noexcept
{
	return static_cast<std::uint32_t>(Alpha) << 24 | static_cast<std::uint32_t>(Blue) << 16 | static_cast<std::uint32_t>(Green) << 8 | Red;
}

enum class EGameUIFont : std::uint8_t
{
	Body,
	Title,
};

// Placement in layout units, which are pixels on a 1440-pixel-tall screen and scale with its height.
struct FGameUIPlacement
{
	// The screen point the element attaches to: (0, 0) is the top-left corner and (1, 1) the bottom-right.
	FVector2 Anchor{};
	// The element point placed on the anchor, in the same fractions of the element's own size.
	FVector2 Pivot{};
	FVector2 Offset{};
};

// One frame of pointer and menu keys. Keyboard and pointer move the same focus.
struct FGameUIInput
{
	std::optional<FVector2> Pointer{};
	bool bClicked = false;
	bool bUp = false;
	bool bDown = false;
	bool bActivate = false;
};

struct FGameUIDescriptor
{
	IGraphicsDevice* Device = nullptr;
	std::filesystem::path FontPath;
	// Makes a texture drawable by the presenter and returns its screen draw ID.
	std::function<std::expected<std::uint64_t, FGameUIError>(const FTextureHandle& Texture)> RegisterTexture;
	std::function<void(std::uint64_t TextureId)> UnregisterTexture;
};

// Immediate-mode screen UI for the game: anchored text, rectangles, images, and a focusable menu.
class FGameUI final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FGameUI>, FGameUIError> Create(FGameUIDescriptor Descriptor);

	~FGameUI();

	FGameUI(const FGameUI&) = delete;
	FGameUI& operator=(const FGameUI&) = delete;
	FGameUI(FGameUI&&) = delete;
	FGameUI& operator=(FGameUI&&) = delete;

	// Clears the previous frame. A new screen height rebuilds the glyph atlas at the matching size.
	[[nodiscard]] std::expected<void, FGameUIError> BeginFrame(FExtent2D Extent);
	// Covers a pixel rectangle with a registered texture, such as the rendered scene.
	void DrawImage(std::uint64_t TextureId, FVector2 Minimum, FVector2 Maximum, std::uint32_t Color = MakeGameUIColor(255, 255, 255));
	// Radius rounds the corners, in layout units.
	void DrawRect(const FGameUIPlacement& Placement, FVector2 Size, std::uint32_t Color, float Radius = 0.f);
	// An anti-aliased dot, such as a crosshair.
	void DrawCircle(const FGameUIPlacement& Placement, float Diameter, std::uint32_t Color);
	void DrawText(std::string_view Text, const FGameUIPlacement& Placement, EGameUIFont Font, std::uint32_t Color);
	// Text size in layout units. The height spans the font's ascender to descender, so text centers in a box of that height.
	[[nodiscard]] FVector2 MeasureText(std::string_view Text, EGameUIFont Font) const;
	// A vertical list of choices centered on the screen. Returns the item chosen this frame.
	std::optional<std::size_t> DrawMenu(std::span<const std::string_view> Items, std::size_t& Focus, const FGameUIInput& Input);
	[[nodiscard]] FScreenDrawList GetDrawList() const noexcept;

private:
	struct FImplementation;
	explicit FGameUI(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
