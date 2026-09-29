#pragma once

#include <imgui.h>

namespace Herta
{
bool DrawNumericDragFloat(const char* Label, float* Value, float Speed, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0);
bool DrawNumericSliderFloat(const char* Label, float* Value, float Minimum, float Maximum, const char* Format, ImGuiSliderFlags Flags = 0);
}
