#pragma once

#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

namespace Herta
{
inline constexpr char ImmersiveViewportName[] = "Viewport###HertaImmersiveViewport";

[[nodiscard]] inline bool BeginImmersiveViewport(const FToolUICanvasBounds& Canvas, const bool bFocus)
{
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
	ImGui::SetNextWindowPos({Canvas.X, Canvas.Y});
	ImGui::SetNextWindowSize({Canvas.Width, Canvas.Height});
	ImGui::SetNextWindowBgAlpha(0.0f);
	if (bFocus)
		ImGui::SetNextWindowFocus();
	return ImGui::Begin(ImmersiveViewportName, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
}
}
