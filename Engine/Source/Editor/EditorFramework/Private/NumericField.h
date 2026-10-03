#pragma once

#include <imgui.h>

#include <functional>

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

bool DrawNumericDragFloat(const char* Label, float* Value, float Speed, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr);
bool DrawNumericSliderFloat(const char* Label, float* Value, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0, FNumericEditLifecycle* Edit = nullptr);
}
