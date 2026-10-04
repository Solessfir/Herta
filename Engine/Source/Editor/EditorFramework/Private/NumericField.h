#pragma once

#include <imgui.h>

#include <functional>
#include <string>

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

std::string FormatCompactNumericValue(float Value, const char* Format);
bool DrawNumericDragFloat(const char* Label, float* Value, float Speed, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr, bool bCompactDisplay = false);
bool DrawNumericSliderFloat(const char* Label, float* Value, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr);
}
