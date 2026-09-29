#pragma once

#include "Herta/Math/Vector.h"

#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
[[nodiscard]] std::optional<float> EvaluateNumericExpression(std::string_view Expression) noexcept;
[[nodiscard]] std::string FormatTransformVectorClipboard(const FVector3& Value);
[[nodiscard]] std::optional<FVector3> ParseTransformVectorClipboard(std::string_view Text) noexcept;
[[nodiscard]] std::string FormatTransformRotationClipboard(const FVector3& Value);
[[nodiscard]] std::optional<FVector3> ParseTransformRotationClipboard(std::string_view Text) noexcept;
}
