#pragma once

#include "Herta/ToolUI/ToolUI.h"

#include <imgui_internal.h>

namespace Herta
{
inline constexpr char ImmersiveViewportName[] = "Viewport###HertaImmersiveViewport";

inline bool IsToolUIPanelFocused(const std::string_view Name, const bool bImmersive) noexcept
{
	const ImGuiID Id = Name == "Viewport" && bImmersive ? ImHashStr(ImmersiveViewportName) : ImHashStr(Name.data(), Name.size());
	const ImGuiWindow* const Panel = ImGui::FindWindowByID(Id);
	const ImGuiWindow* const Focused = GImGui->NavWindow;
	return Panel != nullptr && Focused != nullptr && Panel->RootWindow == Focused->RootWindow;
}

inline bool BeginImmersiveViewport(const FToolUICanvasBounds& Canvas, const bool bFocus)
{
	ImGui::SetNextWindowViewport(ImGui::GetMainViewport()->ID);
	ImGui::SetNextWindowPos({Canvas.X, Canvas.Y});
	ImGui::SetNextWindowSize({Canvas.Width, Canvas.Height});
	ImGui::SetNextWindowBgAlpha(0.f);
	if (bFocus)
	{
		ImGui::SetNextWindowFocus();
	}

	return ImGui::Begin(ImmersiveViewportName, nullptr, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
}
}
