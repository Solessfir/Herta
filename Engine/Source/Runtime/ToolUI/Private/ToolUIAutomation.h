#pragma once

#include "Herta/ToolUI/ToolUI.h"

#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

struct ImGuiContext;

namespace Herta
{
// Collects the items one frame lays out; the previous complete frame's stay readable while the next one records.
class FToolUIItemRecorder final
{
public:
	void BeginFrame(ImGuiContext& Context, bool bEnabled);
	void Add(std::uint32_t Id, const FToolUICanvasBounds& Bounds, std::string_view Window, bool bDisabled);
	void Describe(std::uint32_t Id, std::string_view Label, bool bCheckable, bool bChecked, bool bOpenable, bool bOpened, bool bInput);

	std::vector<FToolUIItem> Items{};

private:
	std::vector<FToolUIItem> Pending{};
	std::unordered_map<std::uint32_t, std::size_t> Indices{};
};

// Input events go into ImGui's queue, which spreads a press and its release over separate frames.
void QueueToolUIClick(float X, float Y, int Button, bool bDouble);
void QueueToolUIText(std::string_view Text);
[[nodiscard]] bool QueueToolUIKey(std::string_view Chord);
}
