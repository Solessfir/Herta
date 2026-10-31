#pragma once

#include "Herta/GraphEditor/GraphCanvasFormat.h"
#include "Herta/GraphEditor/GraphCanvasGeometry.h"
#include "Herta/ToolUI/Theme.h"

#include <array>
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
	// Spacing and grid for Format and for snapping moved nodes.
	void SetFormatSettings(const FGraphFormatSettings& Settings);

private:
	enum class EMode : std::uint8_t
	{
		Idle,
		Panning,
		Moving,
		Resizing,
		BoxSelecting,
		Linking,
	};

	FGraphCanvasView View{};
	FGraphCanvasMetrics Metrics{};
	FGraphFormatSettings FormatSettings{};
	std::vector<FGraphNodeId> Selection{};
	bool bFrameRequested = true;
	std::array<float, 2> FramedSize{};

	// A pin of the single selected node that Tab walks through, for following and swapping links from the keyboard. Empty when none.
	std::string CursorPin{};
	bool bCursorOutput = false;

	// The gesture in progress.
	EMode Mode = EMode::Idle;
	FCanvasPoint DragStart{};
	FCanvasPoint MoveOffset{};
	// A wire in hand: the pins it stays attached to, which side they are on, and the links Ctrl lifted, which a drop elsewhere replaces.
	std::vector<FGraphPin> LinkAnchors{};
	bool bAnchorsAreOutputs = false;
	std::vector<FGraphLink> DetachedLinks{};
	// A wire dropped on empty canvas waits for the add-node menu to connect the node it places; closing the menu drops the lifted links.
	bool bPendingWire = false;
	// The link a lone, unlinked node being moved would be inserted into if dropped now.
	std::optional<FGraphLink> SpliceTarget{};
	// What a move carries: the selection plus the nodes inside selected comments, fixed when the drag starts.
	std::vector<FGraphNodeId> MovingNodes{};
	FGraphNodeId ResizingComment{};

	// The comment whose title is being edited.
	FGraphNodeId RenamingComment{};
	std::array<char, 256> CommentText{};
	bool bFocusCommentText = false;

	// The add-node menu.
	FCanvasPoint MenuPosition{};
	std::array<char, 96> Search{};
	bool bFocusSearch = false;
};
}
