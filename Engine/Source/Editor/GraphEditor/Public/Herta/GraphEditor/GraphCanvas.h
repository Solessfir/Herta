#pragma once

#include "Herta/GraphEditor/GraphCanvasGeometry.h"
#include "Herta/ToolUI/Theme.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
// How one graph domain looks on the canvas.
struct FGraphCanvasDomain
{
	const FGraphSchema* Schema = nullptr;
	FToolUIColor (*PinColor)(std::uint32_t Type) = nullptr;
	FToolUIColor (*CategoryColor)(std::string_view Category) = nullptr;
};

// A finished edit: the whole graph after it, which the caller applies as one undoable transaction.
struct FGraphCanvasEdit
{
	std::string Label{};
	FGraph Graph{};
};

// A pannable, zoomable node canvas. It draws and edits any schema's graph and leaves storage, undo, and semantics to its owner.
class FGraphCanvas final
{
public:
	FGraphCanvas() = default;
	FGraphCanvas(const FGraphCanvas&) = delete;
	FGraphCanvas& operator=(const FGraphCanvas&) = delete;
	FGraphCanvas(FGraphCanvas&&) = default;
	FGraphCanvas& operator=(FGraphCanvas&&) = default;
	~FGraphCanvas() = default;

	// Fills the remaining content region and handles input. Returns the edit the user finished this frame; drags preview without one until released.
	[[nodiscard]] std::optional<FGraphCanvasEdit> Draw(const char* Id, const FGraph& Graph, const FGraphCanvasDomain& Domain, std::span<const FGraphDiagnostic> Diagnostics);
	[[nodiscard]] const std::vector<FGraphNodeId>& GetSelection() const;
	void SetSelection(std::vector<FGraphNodeId> Nodes);
	// Fits the whole graph in view on the next draw.
	void RequestFrame();

private:
	enum class EMode : std::uint8_t
	{
		Idle,
		Panning,
		Moving,
		BoxSelecting,
		Linking,
	};

	FGraphCanvasView View{};
	FGraphCanvasMetrics Metrics{};
	std::vector<FGraphNodeId> Selection{};
	bool bFrameRequested = true;

	// The gesture in progress.
	EMode Mode = EMode::Idle;
	FCanvasPoint DragStart{};
	FCanvasPoint MoveOffset{};
	FGraphPin LinkStart{};
	bool bLinkFromOutput = false;
	// Dragging away from a connected input picks up its link; dropping it on nothing removes it.
	std::optional<FGraphLink> DetachedLink{};

	// The add-node menu.
	FCanvasPoint MenuPosition{};
	std::array<char, 96> Search{};
	bool bFocusSearch = false;
};
}
