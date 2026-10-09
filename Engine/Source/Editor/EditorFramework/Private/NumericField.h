#pragma once

#include <imgui.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
struct FNumericEditLifecycle
{
	std::function<void()> Begin;
	std::function<void(bool)> Flush;
	bool bStarted = false;
	bool bFinished = false;
	bool bCanceled = false;
};

// Reads typed text for a field whose display is not a plain number, such as a clock time. Arithmetic expressions still work as a fallback.
using FNumericTextParser = std::function<std::optional<float>(std::string_view)>;

std::string FormatCompactNumericValue(float Value, const char* Format);
bool DrawNumericDragFloat(const char* Label, float* Value, float Speed, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr, bool bCompactDisplay = false);
bool DrawNumericSliderFloat(const char* Label, float* Value, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr, const FNumericTextParser& ParseText = {});
}
