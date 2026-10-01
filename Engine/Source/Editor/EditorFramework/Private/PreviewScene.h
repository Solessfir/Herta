#pragma once

#include <array>
#include <im3d_math.h>
#include <string_view>

namespace Herta
{
struct FPreviewObject
{
	std::string_view Label;
	Im3d::Vec3 Translation;
	Im3d::Mat3 Rotation{1.0f};
	Im3d::Vec3 Scale{1.0f};
};

inline constexpr int PreviewCubeIndex = 0;
inline constexpr int PreviewFloorIndex = 1;

[[nodiscard]] inline std::array<FPreviewObject, 2> CreatePreviewObjects()
{
	return {{{"Preview Cube", {0.0f, 4.0f, 0.0f}}, {"Floor", {0.0f, -0.25f, 0.0f}, Im3d::Mat3(1.0f), {10.0f, 0.25f, 10.0f}}}};
}
}
