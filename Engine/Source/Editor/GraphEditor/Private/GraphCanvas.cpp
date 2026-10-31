#include "Herta/GraphEditor/GraphCanvas.h"

#include "Herta/GraphEditor/GraphCanvasEdits.h"
#include "Herta/GraphEditor/GraphCanvasFormat.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <tuple>

namespace Herta
{
namespace
{
ImU32 ToImU32(const FToolUIColor Color)
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Color.Alpha);
}

ImU32 WithAlpha(const FToolUIColor Color, const std::uint8_t Alpha)
{
	return IM_COL32(Color.Red, Color.Green, Color.Blue, Alpha);
}

ImVec2 ToImVec2(const FCanvasPoint Point)
{
	return {Point[0], Point[1]};
}

bool Contains(const std::vector<FGraphNodeId>& Nodes, const FGraphNodeId Id)
{
	return std::ranges::find(Nodes, Id) != Nodes.end();
}

bool MatchesSearch(const std::string_view Text, const std::string_view Search)
{
	return std::ranges::search(Text, Search, [](const char Left, const char Right)
	{
		return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	}).begin() != Text.end() || Search.empty();
}

std::string FormatValue(const FGraphValue& Value)
{
	if (const bool* const Flag = std::get_if<bool>(&Value))
	{
		return *Flag ? "On" : "Off";
	}

	if (const std::int32_t* const Integer = std::get_if<std::int32_t>(&Value))
	{
		return std::to_string(*Integer);
	}

	if (const float* const Scalar = std::get_if<float>(&Value))
	{
		return std::format("{:.3g}", *Scalar);
	}

	const std::string* const Text = std::get_if<std::string>(&Value);
	return Text ? *Text : std::string();
}

// Copies the selected nodes beside themselves with the links among them, skipping types at their per-graph limit, and selects the copies.
std::optional<FGraphCanvasEdit> Duplicate(const FGraph& Graph, const FGraphSchema& Schema, std::vector<FGraphNodeId>& Selection)
{
	FGraph After = Graph;
	std::map<std::uint32_t, FGraphNodeId> Copies;
	FGraphNodeId Next = NextGraphNodeId(Graph);
	for (const FGraphNode& Node : Graph.Nodes)
	{
		const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Node.Type);
		const auto Count = std::ranges::count(After.Nodes, Node.Type, &FGraphNode::Type);
		if (Contains(Selection, Node.Id) && Descriptor && static_cast<std::uint32_t>(Count) < Descriptor->MaximumCount)
		{
			FGraphNode Copy = Node;
			Copy.Id = Next;
			Copy.Position = {Node.Position[0] + 24.f, Node.Position[1] + 24.f};
			Copies.emplace(Node.Id.Value, Next);
			After.Nodes.push_back(std::move(Copy));
			Next.Value += 1;
		}
	}

	for (const FGraphLink& Link : Graph.Links)
	{
		const auto From = Copies.find(Link.From.Node.Value);
		const auto To = Copies.find(Link.To.Node.Value);
		if (From != Copies.end() && To != Copies.end())
		{
			After.Links.push_back({.From = {.Node = From->second, .Key = Link.From.Key}, .To = {.Node = To->second, .Key = Link.To.Key}});
		}
	}

	if (Copies.empty())
	{
		return std::nullopt;
	}

	Selection.clear();
	for (const auto& [Original, Copy] : Copies)
	{
		Selection.push_back(Copy);
	}

	return FGraphCanvasEdit{.Label = Copies.size() == 1 ? "Duplicate node" : "Duplicate nodes", .Graph = std::move(After)};
}

// An edit that moves nodes to new positions, or nothing when none of them actually moves.
std::optional<FGraphCanvasEdit> MoveNodes(const FGraph& Graph, const FGraphNodePositions& Positions, std::string Label)
{
	FGraph After = Graph;
	for (FGraphNode& Node : After.Nodes)
	{
		if (const auto Found = Positions.find(Node.Id.Value); Found != Positions.end())
		{
			Node.Position = Found->second;
		}
	}

	if (After == Graph)
	{
		return std::nullopt;
	}

	return FGraphCanvasEdit{.Label = std::move(Label), .Graph = std::move(After)};
}

FGraph RemoveNodes(const FGraph& Graph, const std::vector<FGraphNodeId>& Nodes)
{
	FGraph Result = Graph;
	std::erase_if(Result.Nodes, [&Nodes](const FGraphNode& Node)
	{
		return Contains(Nodes, Node.Id);
	});

	std::erase_if(Result.Comments, [&Nodes](const FGraphComment& Comment)
	{
		return Contains(Nodes, Comment.Id);
	});

	std::erase_if(Result.Links, [&Nodes](const FGraphLink& Link)
	{
		return Contains(Nodes, Link.From.Node) || Contains(Nodes, Link.To.Node);
	});

	return Result;
}
}

const std::vector<FGraphNodeId>& FGraphCanvas::GetSelection() const
{
	return Selection;
}

void FGraphCanvas::SetSelection(std::vector<FGraphNodeId> Nodes)
{
	Selection = std::move(Nodes);
}

void FGraphCanvas::RequestFrame()
{
	bFrameRequested = true;
}

void FGraphCanvas::SetFormatSettings(const FGraphFormatSettings& Settings)
{
	FormatSettings = Settings;
}

std::optional<FGraphCanvasEdit> FGraphCanvas::Draw(const char* const Id, const FGraph& Graph, const FGraphCanvasDomain& Domain, const std::span<const FGraphDiagnostic> Diagnostics)
{
	const FGraphSchema& Schema = *Domain.Schema;
	std::optional<FGraphCanvasEdit> Edit;
	ImGui::PushID(Id);

	const ImVec2 Origin = ImGui::GetCursorScreenPos();
	const ImVec2 Size = ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.f), std::max(ImGui::GetContentRegionAvail().y, 1.f));
	ImGui::InvisibleButton("Canvas", Size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
	const bool bHovered = ImGui::IsItemHovered();
	const bool bFocused = ImGui::IsItemFocused() || ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	View.Origin = {Origin.x, Origin.y};

	// Labels scale with zoom through the font size itself, so text stays sharp instead of stretching a rasterized size.
	ImFont* const Font = ImGui::GetFont();
	const float BaseFontSize = ImGui::GetStyle().FontSizeBase;
	const float FontSize = BaseFontSize * View.Zoom;
	const bool bLabels = FontSize >= 6.f;
	const auto MeasureText = [Font, BaseFontSize](const std::string_view Text)
	{
		return Font->CalcTextSizeA(BaseFontSize, FLT_MAX, 0.f, Text.data(), Text.data() + Text.size()).x;
	};

	const auto Offset = [this](const FGraphNodeId Node)
	{
		return Mode == EMode::Moving && Contains(MovingNodes, Node) ? MoveOffset : FCanvasPoint{};
	};

	std::vector<FGraphNodeLayout> Layouts;
	Layouts.reserve(Graph.Nodes.size());
	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Node.Type))
		{
			Layouts.push_back(LayoutGraphNode(Node, *Descriptor, Offset(Node.Id), Metrics, MeasureText));
		}
	}

	const auto FrameBounds = [this, Size](const std::array<float, 4>& Bounds)
	{
		const float Margin = 48.f;
		View.Zoom = std::clamp(std::min((Size.x - 2.f * Margin) / std::max(Bounds[2] - Bounds[0], 1.f), (Size.y - 2.f * Margin) / std::max(Bounds[3] - Bounds[1], 1.f)), MinimumGraphCanvasZoom, 1.f);
		View.Pan = {Size.x * 0.5f - (Bounds[0] + Bounds[2]) * 0.5f * View.Zoom, Size.y * 0.5f - (Bounds[1] + Bounds[3]) * 0.5f * View.Zoom};
	};

	std::vector<FGraphNodeId> AllNodes;
	AllNodes.reserve(Layouts.size());
	for (const FGraphNodeLayout& Layout : Layouts)
	{
		AllNodes.push_back(Layout.Id);
	}

	if (bFrameRequested)
	{
		// A new document tab can begin floating and dock a frame later, so keep framing until the canvas size settles.
		bFrameRequested = Size.x != FramedSize[0] || Size.y != FramedSize[1];
		FramedSize = {Size.x, Size.y};
		View.Zoom = 1.f;
		View.Pan = {Size.x * 0.5f, Size.y * 0.5f};
		if (const auto Bounds = GraphNodeBounds(Layouts, AllNodes))
		{
			FrameBounds(*Bounds);
		}
	}

	// The cursor pin belongs to the one selected node and goes away with it.
	if (Selection.size() != 1)
	{
		CursorPin.clear();
	}

	const ImGuiIO& IO = ImGui::GetIO();
	const FCanvasPoint Mouse = View.ToCanvas({IO.MousePos.x, IO.MousePos.y});
	const float Tolerance = 8.f / View.Zoom;
	const FGraphCanvasHitResult Hit = bHovered ? HitTestGraphCanvas(Graph, Schema, Layouts, Mouse, Tolerance) : FGraphCanvasHitResult{};
	const bool bPopupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

	// Comments sit behind everything, so only their title bar and resize grip take clicks, and only where nothing else does.
	const float CommentTitle = Metrics.TitleHeight;
	const float Grip = 14.f / View.Zoom;
	FGraphNodeId CommentHit{};
	bool bGripHit = false;
	if (Hit.Kind == EGraphCanvasHit::None)
	{
		for (auto Comment = Graph.Comments.rbegin(); Comment != Graph.Comments.rend(); ++Comment)
		{
			const auto& Rect = Comment->Rect;
			const bool bInside = Mouse[0] >= Rect[0] && Mouse[0] <= Rect[2] && Mouse[1] >= Rect[1] && Mouse[1] <= Rect[3];
			if (bInside && (Mouse[1] <= Rect[1] + CommentTitle || (Mouse[0] >= Rect[2] - Grip && Mouse[1] >= Rect[3] - Grip)))
			{
				CommentHit = Comment->Id;
				bGripHit = Mouse[1] > Rect[1] + CommentTitle;
				break;
			}
		}
	}

	const auto StartMoving = [&]
	{
		MovingNodes = Selection;
		for (const FGraphComment& Comment : Graph.Comments)
		{
			if (Contains(Selection, Comment.Id))
			{
				for (const FGraphNodeId Inside : GraphNodesInComment(Layouts, Comment))
				{
					if (!Contains(MovingNodes, Inside))
					{
						MovingNodes.push_back(Inside);
					}
				}
			}
		}

		Mode = EMode::Moving;
		DragStart = Mouse;
		MoveOffset = {};
	};

	// Navigation: the wheel zooms about the cursor, and the middle or right button drags the view.
	if (bHovered && IO.MouseWheel != 0.f && !bPopupOpen)
	{
		View.ZoomAbout({IO.MousePos.x, IO.MousePos.y}, std::pow(1.15f, IO.MouseWheel));
	}

	if (Mode == EMode::Idle && bHovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.f)))
	{
		Mode = EMode::Panning;
	}

	if (Mode == EMode::Panning)
	{
		View.Pan = {View.Pan[0] + IO.MouseDelta.x, View.Pan[1] + IO.MouseDelta.y};
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			Mode = EMode::Idle;
		}
	}

	// A right click that never dragged opens a menu for whatever is under the cursor.
	if (Mode == EMode::Idle && bHovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).x == 0.f && ImGui::GetMouseDragDelta(ImGuiMouseButton_Right).y == 0.f)
	{
		if (Hit.Kind == EGraphCanvasHit::Node)
		{
			if (!Contains(Selection, Hit.Node))
			{
				Selection = {Hit.Node};
			}

			ImGui::OpenPopup("NodeMenu");
		}
		else
		{
			MenuPosition = Mouse;
			Search.fill('\0');
			bFocusSearch = true;
			ImGui::OpenPopup("AddNode");
		}
	}

	if (Mode == EMode::Idle && bHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		const bool bAdditive = IO.KeyCtrl || IO.KeyShift;
		if (Hit.Kind == EGraphCanvasHit::Pin)
		{
			Mode = EMode::Linking;
			LinkStart = {.Node = Hit.Node, .Key = Hit.Pin};
			bLinkFromOutput = Hit.bOutput;
			DetachedLink.reset();
			const auto Existing = std::ranges::find(Graph.Links, LinkStart, &FGraphLink::To);
			if (!Hit.bOutput && Existing != Graph.Links.end())
			{
				DetachedLink = *Existing;
				LinkStart = Existing->From;
				bLinkFromOutput = true;
			}
		}
		else if (Hit.Kind == EGraphCanvasHit::Node)
		{
			if (IO.KeyCtrl && Contains(Selection, Hit.Node))
			{
				std::erase(Selection, Hit.Node);
			}
			else
			{
				if (!bAdditive && !Contains(Selection, Hit.Node))
				{
					Selection.clear();
				}

				if (!Contains(Selection, Hit.Node))
				{
					Selection.push_back(Hit.Node);
				}

				StartMoving();
			}
		}
		else if (Hit.Kind == EGraphCanvasHit::Link && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		{
			// Double-clicking a link drops a reroute knot into it under the cursor.
			const auto Reroute = std::ranges::find_if(Schema.Nodes, &FGraphNodeDescriptor::bReroute);
			if (Reroute != Schema.Nodes.end())
			{
				if (auto Knot = MakeGraphNode(Schema, Reroute->Type, NextGraphNodeId(Graph), {Mouse[0] - Metrics.RerouteWidth * 0.5f, Mouse[1] - Metrics.RerouteHeight * 0.5f}))
				{
					FGraph After = Graph;
					const FGraphNodeId Knotted = Knot->Id;
					After.Nodes.push_back(std::move(*Knot));
					if (auto Spliced = SpliceGraphNode(After, Schema, Knotted, Graph.Links[Hit.Link]))
					{
						Selection = {Knotted};
						Edit = FGraphCanvasEdit{.Label = "Add reroute", .Graph = std::move(*Spliced)};
					}
				}
			}
		}
		else if (Hit.Kind == EGraphCanvasHit::Link && IO.KeyAlt)
		{
			FGraph After = Graph;
			After.Links.erase(After.Links.begin() + static_cast<std::ptrdiff_t>(Hit.Link));
			Edit = FGraphCanvasEdit{.Label = "Disconnect pins", .Graph = std::move(After)};
		}
		else if (CommentHit.Value != 0 && bGripHit)
		{
			Selection = {CommentHit};
			ResizingComment = CommentHit;
			Mode = EMode::Resizing;
		}
		else if (CommentHit.Value != 0)
		{
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			{
				const FGraphComment& Comment = *FindGraphComment(Graph, CommentHit);
				RenamingComment = CommentHit;
				CommentText.fill('\0');
				Comment.Text.copy(CommentText.data(), CommentText.size() - 1);
				bFocusCommentText = true;
				ImGui::OpenPopup("RenameComment");
			}
			else
			{
				if (!bAdditive && !Contains(Selection, CommentHit))
				{
					Selection.clear();
				}

				if (!Contains(Selection, CommentHit))
				{
					Selection.push_back(CommentHit);
				}

				StartMoving();
			}
		}
		else
		{
			if (!bAdditive)
			{
				Selection.clear();
			}

			Mode = EMode::BoxSelecting;
			DragStart = Mouse;
		}
	}

	if (Mode == EMode::Resizing && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (const FGraphComment* const Comment = FindGraphComment(Graph, ResizingComment))
		{
			FGraph After = Graph;
			auto& Rect = std::ranges::find(After.Comments, ResizingComment, &FGraphComment::Id)->Rect;
			Rect[2] = std::max(Rect[0] + 96.f, Mouse[0]);
			Rect[3] = std::max(Rect[1] + CommentTitle + 32.f, Mouse[1]);
			if (After.Comments != Graph.Comments)
			{
				Edit = FGraphCanvasEdit{.Label = "Resize comment", .Graph = std::move(After)};
			}
		}

		Mode = EMode::Idle;
	}

	if (Mode == EMode::Moving)
	{
		const float Grid = FormatSettings.Grid > 0.f ? FormatSettings.Grid : 1.f;
		MoveOffset = {std::round((Mouse[0] - DragStart[0]) / Grid) * Grid, std::round((Mouse[1] - DragStart[1]) / Grid) * Grid};

		// A lone node without links that is dragged over a link drops into it, as in Unreal.
		SpliceTarget.reset();
		const bool bLoose = Selection.size() == 1 && MovingNodes.size() == 1 && FindGraphNode(Graph, Selection.front()) && std::ranges::none_of(Graph.Links, [this](const FGraphLink& Link)
		{
			return Link.From.Node == Selection.front() || Link.To.Node == Selection.front();
		});

		if (bLoose && (MoveOffset[0] != 0.f || MoveOffset[1] != 0.f))
		{
			for (const FGraphLink& Link : Graph.Links)
			{
				const FCanvasPoint* const From = FindGraphPinPosition(Graph, Schema, Layouts, Link.From, true);
				const FCanvasPoint* const To = FindGraphPinPosition(Graph, Schema, Layouts, Link.To, false);
				if (From && To && DistanceToGraphLink(Mouse, *From, *To) <= Tolerance * 2.f && SpliceGraphNode(Graph, Schema, Selection.front(), Link))
				{
					SpliceTarget = Link;
					break;
				}
			}
		}

		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (MoveOffset[0] != 0.f || MoveOffset[1] != 0.f)
			{
				FGraph After = Graph;
				for (FGraphNode& Node : After.Nodes)
				{
					if (Contains(MovingNodes, Node.Id))
					{
						Node.Position = {Node.Position[0] + MoveOffset[0], Node.Position[1] + MoveOffset[1]};
					}
				}

				for (FGraphComment& Comment : After.Comments)
				{
					if (Contains(MovingNodes, Comment.Id))
					{
						Comment.Rect = {Comment.Rect[0] + MoveOffset[0], Comment.Rect[1] + MoveOffset[1], Comment.Rect[2] + MoveOffset[0], Comment.Rect[3] + MoveOffset[1]};
					}
				}

				if (auto Spliced = SpliceTarget ? SpliceGraphNode(After, Schema, Selection.front(), *SpliceTarget) : std::unexpected(FGraphError{}))
				{
					Edit = FGraphCanvasEdit{.Label = "Insert node into link", .Graph = std::move(*Spliced)};
				}
				else
				{
					Edit = FGraphCanvasEdit{.Label = MovingNodes.size() == 1 ? "Move node" : "Move nodes", .Graph = std::move(After)};
				}
			}

			Mode = EMode::Idle;
			MoveOffset = {};
			SpliceTarget.reset();
			MovingNodes.clear();
		}
	}

	std::optional<std::string> LinkProblem;
	std::optional<FGraphLink> Candidate;
	if (Mode == EMode::Linking && Hit.Kind == EGraphCanvasHit::Pin && Hit.bOutput != bLinkFromOutput)
	{
		const FGraphPin Target{.Node = Hit.Node, .Key = Hit.Pin};
		FGraphLink Link = bLinkFromOutput ? FGraphLink{.From = LinkStart, .To = Target} : FGraphLink{.From = Target, .To = LinkStart};
		FGraph Without = Graph;
		if (DetachedLink)
		{
			std::erase(Without.Links, *DetachedLink);
		}

		if (const auto Allowed = CanLinkGraphPins(Without, Schema, Link); Allowed)
		{
			Candidate = std::move(Link);
		}
		else
		{
			LinkProblem = Allowed.error().Message;
		}
	}

	if (Mode == EMode::Linking && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (Candidate)
		{
			FGraph After = Graph;
			if (DetachedLink)
			{
				std::erase(After.Links, *DetachedLink);
			}

			std::erase_if(After.Links, [&Candidate](const FGraphLink& Link)
			{
				return Link.To == Candidate->To;
			});

			After.Links.push_back(*Candidate);
			Edit = FGraphCanvasEdit{.Label = "Connect pins", .Graph = std::move(After)};
		}
		else if (DetachedLink && Hit.Kind != EGraphCanvasHit::Pin)
		{
			FGraph After = Graph;
			std::erase(After.Links, *DetachedLink);
			Edit = FGraphCanvasEdit{.Label = "Disconnect pins", .Graph = std::move(After)};
		}

		Mode = EMode::Idle;
		DetachedLink.reset();
	}

	if (Mode == EMode::BoxSelecting && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		for (const FGraphNodeId Node : GraphNodesInBox(Layouts, DragStart, Mouse))
		{
			if (!Contains(Selection, Node))
			{
				Selection.push_back(Node);
			}
		}

		for (const FGraphComment& Comment : Graph.Comments)
		{
			const bool bInside = std::min(DragStart[0], Mouse[0]) <= Comment.Rect[0] && std::max(DragStart[0], Mouse[0]) >= Comment.Rect[2] && std::min(DragStart[1], Mouse[1]) <= Comment.Rect[1] && std::max(DragStart[1], Mouse[1]) >= Comment.Rect[3];
			if (bInside && !Contains(Selection, Comment.Id))
			{
				Selection.push_back(Comment.Id);
			}
		}

		Mode = EMode::Idle;
	}

	// The canvas takes the arrows and Tab from ImGui's keyboard navigation while it has focus, so they move through the graph instead of the panel.
	const ImGuiID CanvasId = ImGui::GetID("Canvas");
	const bool bKeyboard = bFocused && !IO.WantTextInput && Mode == EMode::Idle && !bPopupOpen;
	if (bKeyboard)
	{
		for (const ImGuiKey Key : {ImGuiKey_LeftArrow, ImGuiKey_RightArrow, ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab, ImGuiKey_Enter})
		{
			ImGui::SetKeyOwner(Key, CanvasId);
		}
	}

	const auto Pressed = [CanvasId](const ImGuiKey Key)
	{
		return ImGui::IsKeyPressed(Key, ImGuiInputFlags_Repeat, CanvasId);
	};

	if (bKeyboard && !Edit)
	{
		constexpr std::array Arrows{std::pair{ImGuiKey_LeftArrow, EGraphDirection::Left}, std::pair{ImGuiKey_RightArrow, EGraphDirection::Right}, std::pair{ImGuiKey_UpArrow, EGraphDirection::Up}, std::pair{ImGuiKey_DownArrow, EGraphDirection::Down}};
		const FGraphNode* const Single = Selection.size() == 1 ? FindGraphNode(Graph, Selection.front()) : nullptr;
		const FGraphNodeDescriptor* const SingleDescriptor = Single ? Schema.FindNode(Single->Type) : nullptr;
		for (const auto& [Key, Direction] : Arrows)
		{
			if (!Pressed(Key) || !Single)
			{
				continue;
			}

			const bool bVertical = Direction == EGraphDirection::Up || Direction == EGraphDirection::Down;
			if (IO.KeyAlt && bVertical && !CursorPin.empty() && !bCursorOutput && SingleDescriptor)
			{
				// Alt+Up or Down swaps the cursor input's link with the input above or below it, and the cursor follows.
				const auto Current = std::ranges::find(SingleDescriptor->Inputs, CursorPin, &FGraphPinDescriptor::Key);
				const auto Index = Current - SingleDescriptor->Inputs.begin();
				const auto Other = Index + (Direction == EGraphDirection::Down ? 1 : -1);
				if (Current != SingleDescriptor->Inputs.end() && Other >= 0 && Other < static_cast<std::ptrdiff_t>(SingleDescriptor->Inputs.size()))
				{
					if (auto Swapped = SwapGraphInputs(Graph, Schema, Single->Id, CursorPin, SingleDescriptor->Inputs[static_cast<std::size_t>(Other)].Key))
					{
						Edit = FGraphCanvasEdit{.Label = "Swap pins", .Graph = std::move(*Swapped)};
						CursorPin = SingleDescriptor->Inputs[static_cast<std::size_t>(Other)].Key;
					}
				}
			}
			else if (const auto Neighbour = FindGraphNeighbour(Layouts, Single->Id, Direction))
			{
				if (IO.KeyAlt)
				{
					Edit = MoveNodes(Graph, SwapGraphNodes(Layouts, Single->Id, *Neighbour), "Swap nodes");
				}
				else
				{
					Selection = {*Neighbour};
					CursorPin.clear();
				}
			}
		}

		// Tab walks the selected node's pins, inputs first; Enter follows the cursor pin's link to the node at its other end.
		if (SingleDescriptor && Pressed(ImGuiKey_Tab))
		{
			std::vector<std::pair<bool, std::string_view>> Pins;
			for (const FGraphPinDescriptor& Input : SingleDescriptor->Inputs)
			{
				Pins.emplace_back(false, Input.Key);
			}

			for (const FGraphPinDescriptor& Output : SingleDescriptor->Outputs)
			{
				Pins.emplace_back(true, Output.Key);
			}

			if (!Pins.empty())
			{
				const auto Current = std::ranges::find(Pins, std::pair<bool, std::string_view>{bCursorOutput, CursorPin});
				const std::ptrdiff_t Count = static_cast<std::ptrdiff_t>(Pins.size());
				const std::ptrdiff_t Index = Current == Pins.end() ? (IO.KeyShift ? Count - 1 : 0) : ((Current - Pins.begin()) + (IO.KeyShift ? Count - 1 : 1)) % Count;
				bCursorOutput = Pins[static_cast<std::size_t>(Index)].first;
				CursorPin = Pins[static_cast<std::size_t>(Index)].second;
			}
		}

		if (Single && !CursorPin.empty() && Pressed(ImGuiKey_Enter))
		{
			const FGraphPin Pin{.Node = Single->Id, .Key = CursorPin};
			const auto Link = std::ranges::find_if(Graph.Links, [&Pin, this](const FGraphLink& Candidate)
			{
				return (bCursorOutput ? Candidate.From : Candidate.To) == Pin;
			});

			if (Link != Graph.Links.end())
			{
				Selection = {bCursorOutput ? Link->To.Node : Link->From.Node};
				bCursorOutput = !bCursorOutput;
				CursorPin = bCursorOutput ? Link->From.Key : Link->To.Key;
			}
		}
	}

	// The clipboard holds canonical graph text, so nodes copy between graphs of one domain and paste where the cursor is.
	if (bKeyboard && !Edit && IO.KeyCtrl && !IO.KeyAlt)
	{
		const FCanvasPoint PastePosition = bHovered ? Mouse : View.ToCanvas({Origin.x + Size.x * 0.5f, Origin.y + Size.y * 0.5f});
		if (ImGui::IsKeyPressed(ImGuiKey_V, false))
		{
			std::vector<FGraphNodeId> Pasted;
			const char* const Clipboard = ImGui::GetClipboardText();
			if (auto After = Clipboard ? PasteGraphNodes(Graph, Schema, Clipboard, PastePosition, Pasted) : std::unexpected(FGraphError{}))
			{
				Selection = std::move(Pasted);
				Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Paste node" : "Paste nodes", .Graph = std::move(*After)};
			}
		}
		else if (!Selection.empty() && (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)))
		{
			if (const auto Text = CopyGraphNodes(Graph, Schema, Selection))
			{
				ImGui::SetClipboardText(Text->c_str());
				if (ImGui::IsKeyPressed(ImGuiKey_X, false))
				{
					// Ctrl+Shift+X cuts like Shift+Delete deletes: the gap the nodes leave is bridged.
					Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Cut node" : "Cut nodes", .Graph = IO.KeyShift ? RemoveGraphNodesKeepingLinks(Graph, Schema, Selection) : RemoveNodes(Graph, Selection)};
					Selection.clear();
				}
			}
		}
	}

	// C wraps the selected nodes in a comment, or drops one at the cursor.
	if (bKeyboard && !Edit && !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C, false))
	{
		const float Margin = 24.f;
		const auto Bounds = GraphNodeBounds(Layouts, Selection);
		const FCanvasPoint At = bHovered ? Mouse : View.ToCanvas({Origin.x + Size.x * 0.5f, Origin.y + Size.y * 0.5f});
		const std::array<float, 4> Rect = Bounds ? std::array{(*Bounds)[0] - Margin, (*Bounds)[1] - Margin - CommentTitle, (*Bounds)[2] + Margin, (*Bounds)[3] + Margin} : std::array{At[0], At[1], At[0] + 320.f, At[1] + 200.f};
		FGraph After = Graph;
		const FGraphNodeId CommentId = NextGraphNodeId(Graph);
		After.Comments.push_back({.Id = CommentId, .Text = "Comment", .Rect = Rect});
		Selection = {CommentId};
		Edit = FGraphCanvasEdit{.Label = "Add comment", .Graph = std::move(After)};
	}

	// Z frames the trees the selection belongs to, or the whole graph with nothing selected.
	if (bKeyboard && !Edit && !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Z, false))
	{
		if (const auto Bounds = GraphNodeBounds(Layouts, Selection.empty() ? AllNodes : GraphConnectedNodes(Graph, Selection)))
		{
			FrameBounds(*Bounds);
		}
	}

	// Keyboard edits apply to the selection while the canvas has focus and no text field does.
	if (bFocused && !IO.WantTextInput && Mode == EMode::Idle && !Selection.empty() && !Edit)
	{
		if (IO.KeyShift && !IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
		{
			Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Delete node keeping links" : "Delete nodes keeping links", .Graph = RemoveGraphNodesKeepingLinks(Graph, Schema, Selection)};
			Selection.clear();
		}
		else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
		{
			Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Delete node" : "Delete nodes", .Graph = RemoveNodes(Graph, Selection)};
			Selection.clear();
		}
		else if (IO.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
		{
			Edit = Duplicate(Graph, Schema, Selection);
		}
		else if (!IO.KeyCtrl && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Q, false))
		{
			Edit = MoveNodes(Graph, StraightenGraphLinks(Graph, Schema, Layouts, Selection), "Straighten links");
		}
		else if (IO.KeyShift && !IO.KeyCtrl && !IO.KeyAlt)
		{
			// Unreal's alignment keys: Shift with W, S, A, or D aligns the selection's top, bottom, left, or right edges.
			constexpr std::array Keys{std::pair{ImGuiKey_W, EGraphAlignment::Top}, std::pair{ImGuiKey_S, EGraphAlignment::Bottom}, std::pair{ImGuiKey_A, EGraphAlignment::Left}, std::pair{ImGuiKey_D, EGraphAlignment::Right}};
			for (const auto& [Key, Alignment] : Keys)
			{
				if (ImGui::IsKeyPressed(Key, false))
				{
					Edit = MoveNodes(Graph, AlignGraphNodes(Layouts, Selection, Alignment), "Align nodes");
				}
			}
		}
	}

	// Format works on the selection's trees, or on the whole graph when nothing is selected.
	if (bFocused && !IO.WantTextInput && Mode == EMode::Idle && !Edit && !IO.KeyCtrl && !IO.KeyShift && !IO.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
	{
		Edit = MoveNodes(Graph, FormatGraph(Graph, Schema, Layouts, GraphFormatRoots(Graph, Selection), FormatSettings), "Format nodes");
	}

	ImDrawList* const DrawList = ImGui::GetWindowDrawList();
	DrawList->PushClipRect(Origin, ImVec2(Origin.x + Size.x, Origin.y + Size.y), true);
	DrawList->AddRectFilled(Origin, ImVec2(Origin.x + Size.x, Origin.y + Size.y), ToImU32(ToolUITheme::Canvas));

	// The grid thins out instead of crowding when zoomed far out.
	const float Spacing = Metrics.GridSpacing * View.Zoom;
	if (Spacing >= 8.f)
	{
		const float StartX = std::fmod(View.Pan[0], Spacing);
		const float StartY = std::fmod(View.Pan[1], Spacing);
		for (float X = StartX; X < Size.x; X += Spacing)
		{
			DrawList->AddLine(ImVec2(Origin.x + X, Origin.y), ImVec2(Origin.x + X, Origin.y + Size.y), ToImU32(ToolUITheme::BorderSoft));
		}

		for (float Y = StartY; Y < Size.y; Y += Spacing)
		{
			DrawList->AddLine(ImVec2(Origin.x, Origin.y + Y), ImVec2(Origin.x + Size.x, Origin.y + Y), ToImU32(ToolUITheme::BorderSoft));
		}
	}

	const FCanvasPoint VisibleMin = View.ToCanvas({Origin.x, Origin.y});
	const FCanvasPoint VisibleMax = View.ToCanvas({Origin.x + Size.x, Origin.y + Size.y});
	const auto Visible = [&VisibleMin, &VisibleMax](const std::array<float, 4>& Rect)
	{
		return Rect[2] >= VisibleMin[0] && Rect[0] <= VisibleMax[0] && Rect[3] >= VisibleMin[1] && Rect[1] <= VisibleMax[1];
	};

	const auto PinColor = [&Domain](const std::uint32_t Type)
	{
		return Domain.PinColor ? Domain.PinColor(Type) : ToolUITheme::NeutralAccent;
	};

	for (const FGraphComment& Comment : Graph.Comments)
	{
		std::array<float, 4> Rect = Comment.Rect;
		if (Mode == EMode::Moving && Contains(MovingNodes, Comment.Id))
		{
			Rect = {Rect[0] + MoveOffset[0], Rect[1] + MoveOffset[1], Rect[2] + MoveOffset[0], Rect[3] + MoveOffset[1]};
		}
		else if (Mode == EMode::Resizing && ResizingComment == Comment.Id)
		{
			Rect[2] = std::max(Rect[0] + 96.f, Mouse[0]);
			Rect[3] = std::max(Rect[1] + CommentTitle + 32.f, Mouse[1]);
		}

		if (!Visible(Rect))
		{
			continue;
		}

		const ImVec2 Min = ToImVec2(View.ToScreen({Rect[0], Rect[1]}));
		const ImVec2 Max = ToImVec2(View.ToScreen({Rect[2], Rect[3]}));
		const float TitleBottom = Min.y + CommentTitle * View.Zoom;
		const float CommentRounding = Metrics.Rounding * View.Zoom;
		const bool bSelected = Contains(Selection, Comment.Id);
		DrawList->AddRectFilled(Min, Max, WithAlpha(ToolUITheme::NeutralAccent, 14), CommentRounding);
		DrawList->AddRectFilled(Min, ImVec2(Max.x, TitleBottom), WithAlpha(ToolUITheme::NeutralAccent, 38), CommentRounding, ImDrawFlags_RoundCornersTop);
		DrawList->AddRect(Min, Max, bSelected ? ToImU32(ToolUITheme::NeutralAccentHover) : WithAlpha(ToolUITheme::NeutralAccent, CommentHit == Comment.Id ? 120 : 60), CommentRounding, 0, bSelected ? 2.f : 1.f);
		if (bLabels)
		{
			const ImVec4 Clip(Min.x, Min.y, Max.x - Metrics.Padding * View.Zoom, TitleBottom);
			DrawList->AddText(Font, FontSize * 1.15f, ImVec2(Min.x + Metrics.Padding * View.Zoom, Min.y + (CommentTitle * View.Zoom - FontSize * 1.15f) * 0.5f), ToImU32(ToolUITheme::TextPrimary), Comment.Text.c_str(), nullptr, 0.f, &Clip);
		}

		// The resize grip: two short diagonals in the bottom-right corner.
		const float Step = 4.f * View.Zoom;
		for (const float Inset : {Step, Step * 2.f})
		{
			DrawList->AddLine(ImVec2(Max.x - Inset - Step, Max.y - Step), ImVec2(Max.x - Step, Max.y - Inset - Step), WithAlpha(ToolUITheme::TextMuted, 160), std::max(1.f, View.Zoom));
		}
	}

	const auto LayoutOf = [&Layouts](const FGraphNodeId Node) -> const FGraphNodeLayout*
	{
		const auto Found = std::ranges::find(Layouts, Node, &FGraphNodeLayout::Id);
		return Found == Layouts.end() ? nullptr : &*Found;
	};

	const auto PinOf = [&](const FGraphPin& Pin, const bool bOutput) -> std::optional<std::pair<FCanvasPoint, std::uint32_t>>
	{
		const FGraphNode* const Node = FindGraphNode(Graph, Pin.Node);
		const FGraphNodeDescriptor* const Descriptor = Node ? Schema.FindNode(Node->Type) : nullptr;
		const FGraphNodeLayout* const Layout = LayoutOf(Pin.Node);
		if (!Descriptor || !Layout)
		{
			return std::nullopt;
		}

		const auto Pins = bOutput ? Descriptor->Outputs : Descriptor->Inputs;
		const auto Found = std::ranges::find(Pins, Pin.Key, &FGraphPinDescriptor::Key);
		if (Found == Pins.end())
		{
			return std::nullopt;
		}

		const auto Index = static_cast<std::size_t>(Found - Pins.begin());
		return std::pair{(bOutput ? Layout->Outputs : Layout->Inputs)[Index], Found->Type};
	};

	const float Thickness = std::max(1.f, 2.f * View.Zoom);
	const auto DrawCurve = [&](const FCanvasPoint From, const FCanvasPoint To, const ImU32 Color, const float Width)
	{
		const auto Curve = GraphLinkCurve(From, To);
		const std::array<float, 4> Bounds{std::min({Curve[0][0], Curve[1][0], Curve[2][0], Curve[3][0]}), std::min(From[1], To[1]), std::max({Curve[0][0], Curve[1][0], Curve[2][0], Curve[3][0]}), std::max(From[1], To[1])};
		if (Visible(Bounds))
		{
			DrawList->AddBezierCubic(ToImVec2(View.ToScreen(Curve[0])), ToImVec2(View.ToScreen(Curve[1])), ToImVec2(View.ToScreen(Curve[2])), ToImVec2(View.ToScreen(Curve[3])), Color, Width);
		}
	};

	for (std::size_t Index = 0; Index < Graph.Links.size(); ++Index)
	{
		const FGraphLink& Link = Graph.Links[Index];
		if (Mode == EMode::Linking && DetachedLink && Link == *DetachedLink)
		{
			continue;
		}

		const auto From = PinOf(Link.From, true);
		const auto To = PinOf(Link.To, false);
		if (From && To)
		{
			const bool bSplice = SpliceTarget && *SpliceTarget == Link;
			const bool bHighlight = bSplice || (Hit.Kind == EGraphCanvasHit::Link && Hit.Link == Index) || Contains(Selection, Link.From.Node) || Contains(Selection, Link.To.Node);
			DrawCurve(From->first, To->first, bSplice ? ToImU32(ToolUITheme::NeutralAccentHover) : WithAlpha(PinColor(From->second), bHighlight ? 255 : 170), bHighlight ? Thickness * (bSplice ? 2.5f : 1.5f) : Thickness);
		}
	}

	std::map<std::uint32_t, std::pair<EGraphSeverity, std::string>> Problems;
	for (const FGraphDiagnostic& Diagnostic : Diagnostics)
	{
		auto& [Severity, Message] = Problems.try_emplace(Diagnostic.Node.Value, Diagnostic.Severity, std::string()).first->second;
		Severity = std::min(Severity, Diagnostic.Severity);
		Message += (Message.empty() ? "" : "\n") + Diagnostic.Message;
	}

	const float Rounding = Metrics.Rounding * View.Zoom;
	for (const FGraphNodeLayout& Layout : Layouts)
	{
		if (!Visible(Layout.Rect))
		{
			continue;
		}

		const FGraphNode& Node = *FindGraphNode(Graph, Layout.Id);
		const FGraphNodeDescriptor& Descriptor = *Schema.FindNode(Node.Type);
		const ImVec2 Min = ToImVec2(View.ToScreen({Layout.Rect[0], Layout.Rect[1]}));
		const ImVec2 Max = ToImVec2(View.ToScreen({Layout.Rect[2], Layout.Rect[3]}));
		if (Descriptor.bReroute)
		{
			// A knot takes the color of what flows through it.
			const auto Feed = std::ranges::find_if(Graph.Links, [&Node](const FGraphLink& Link)
			{
				return Link.To.Node == Node.Id;
			});

			const auto Source = Feed != Graph.Links.end() ? PinOf(Feed->From, true) : std::nullopt;
			const ImU32 Color = ToImU32(Source ? PinColor(Source->second) : ToolUITheme::TextMuted);
			const ImVec2 Center((Min.x + Max.x) * 0.5f, (Min.y + Max.y) * 0.5f);
			const float Radius = Metrics.PinRadius * View.Zoom * 1.2f;
			DrawList->AddLine(ImVec2(Min.x, Center.y), ImVec2(Max.x, Center.y), Color, Thickness);
			if (Contains(Selection, Node.Id))
			{
				DrawList->AddCircle(Center, Radius * 2.f, ToImU32(ToolUITheme::NeutralAccentHover), 0, std::max(1.5f, 2.f * View.Zoom));
			}

			const bool bHot = Hit.Kind != EGraphCanvasHit::None && Hit.Node == Node.Id;
			DrawList->AddCircleFilled(Center, bHot ? Radius * 1.3f : Radius, Color);
			continue;
		}

		const float TitleBottom = Min.y + Metrics.TitleHeight * View.Zoom;
		const FToolUIColor Category = Domain.CategoryColor ? Domain.CategoryColor(Descriptor.Category) : ToolUITheme::Surface2;
		DrawList->AddRectFilled(ImVec2(Min.x + 2.f, Min.y + 3.f), ImVec2(Max.x + 2.f, Max.y + 3.f), IM_COL32(0, 0, 0, 70), Rounding);
		DrawList->AddRectFilled(Min, Max, ToImU32(ToolUITheme::Surface1), Rounding);
		DrawList->AddRectFilled(Min, ImVec2(Max.x, TitleBottom), ToImU32(MixToolUIColor(ToolUITheme::Surface2, Category, 0.35f)), Rounding, ImDrawFlags_RoundCornersTop);

		const auto Problem = Problems.find(Node.Id.Value);
		const bool bSelected = Contains(Selection, Node.Id);
		const bool bHot = Hit.Kind != EGraphCanvasHit::None && Hit.Node == Node.Id;
		const FToolUIColor Outline = Problem != Problems.end() ? (Problem->second.first == EGraphSeverity::Error ? ToolUITheme::Error : ToolUITheme::Warning) : bSelected ? ToolUITheme::NeutralAccentHover
		                                                                                                                                            : bHot ? ToolUITheme::Border
		                                                                                                                                                   : ToolUITheme::BorderSoft;
		DrawList->AddRect(Min, Max, ToImU32(Outline), Rounding, 0, bSelected || Problem != Problems.end() ? 2.f : 1.f);

		if (bLabels)
		{
			DrawList->AddText(Font, FontSize, ImVec2(Min.x + Metrics.Padding * View.Zoom, Min.y + (Metrics.TitleHeight * View.Zoom - FontSize) * 0.5f), ToImU32(ToolUITheme::TextPrimary), Descriptor.Name.data(), Descriptor.Name.data() + Descriptor.Name.size());
		}

		for (const bool bOutput : {false, true})
		{
			const auto Pins = bOutput ? Descriptor.Outputs : Descriptor.Inputs;
			const auto& Positions = bOutput ? Layout.Outputs : Layout.Inputs;
			for (std::size_t Index = 0; Index < Pins.size(); ++Index)
			{
				const ImVec2 Center = ToImVec2(View.ToScreen(Positions[Index]));
				const FGraphPin Pin{.Node = Node.Id, .Key = std::string(Pins[Index].Key)};
				const bool bConnected = std::ranges::any_of(Graph.Links, [&Pin, bOutput](const FGraphLink& Link)
				{
					return (bOutput ? Link.From : Link.To) == Pin;
				});

				const float Radius = Metrics.PinRadius * View.Zoom;
				const bool bPinHot = Hit.Kind == EGraphCanvasHit::Pin && Hit.Node == Node.Id && Hit.Pin == Pins[Index].Key && Hit.bOutput == bOutput;
				if (bSelected && Selection.size() == 1 && CursorPin == Pins[Index].Key && bCursorOutput == bOutput)
				{
					DrawList->AddCircle(Center, Radius * 2.f, ToImU32(ToolUITheme::NeutralAccentHover), 0, std::max(1.5f, 2.f * View.Zoom));
				}

				const ImU32 Color = ToImU32(PinColor(Pins[Index].Type));
				if (bConnected || bPinHot)
				{
					DrawList->AddCircleFilled(Center, bPinHot ? Radius * 1.3f : Radius, Color);
				}
				else
				{
					DrawList->AddCircleFilled(Center, Radius, ToImU32(ToolUITheme::Surface0));
					DrawList->AddCircle(Center, Radius, Color, 0, std::max(1.f, 1.5f * View.Zoom));
				}

				if (bLabels)
				{
					const std::string_view Name = Pins[Index].Name;
					const float Width = Font->CalcTextSizeA(FontSize, FLT_MAX, 0.f, Name.data(), Name.data() + Name.size()).x;
					const float X = bOutput ? Center.x - (Metrics.PinRadius + 6.f) * View.Zoom - Width : Center.x + (Metrics.PinRadius + 6.f) * View.Zoom;
					DrawList->AddText(Font, FontSize, ImVec2(X, Center.y - FontSize * 0.5f), ToImU32(ToolUITheme::TextSecondary), Name.data(), Name.data() + Name.size());
				}
			}
		}

		if (bLabels)
		{
			const std::size_t PinRows = std::max(Descriptor.Inputs.size(), Descriptor.Outputs.size());
			for (std::size_t Index = 0; Index < Node.Properties.size(); ++Index)
			{
				const float RowTop = TitleBottom + Metrics.RowHeight * View.Zoom * static_cast<float>(PinRows + Index);
				const float Y = RowTop + (Metrics.RowHeight * View.Zoom - FontSize) * 0.5f;
				const std::string_view Name = Descriptor.Properties[Index].Name;
				DrawList->AddText(Font, FontSize, ImVec2(Min.x + Metrics.Padding * View.Zoom, Y), ToImU32(ToolUITheme::TextMuted), Name.data(), Name.data() + Name.size());
				const float Right = Max.x - Metrics.Padding * View.Zoom;
				// Vectors read as colors; their four numbers do not fit a node and are edited in the details panel instead.
				if (const auto* const Vector = std::get_if<std::array<float, 4>>(&Node.Properties[Index].Value))
				{
					const auto Channel = [](const float Component)
					{
						return static_cast<std::uint8_t>(std::clamp(Component, 0.f, 1.f) * 255.f + 0.5f);
					};

					DrawList->AddRectFilled(ImVec2(Right - FontSize * 3.f, Y), ImVec2(Right, Y + FontSize), IM_COL32(Channel((*Vector)[0]), Channel((*Vector)[1]), Channel((*Vector)[2]), 255), 3.f * View.Zoom);
					continue;
				}

				const std::string Value = FormatValue(Node.Properties[Index].Value);
				const float Width = Font->CalcTextSizeA(FontSize, FLT_MAX, 0.f, Value.data(), Value.data() + Value.size()).x;
				DrawList->AddText(Font, FontSize, ImVec2(Right - Width, Y), ToImU32(ToolUITheme::TextSecondary), Value.data(), Value.data() + Value.size());
			}
		}

		if (bHovered && Problem != Problems.end() && Hit.Kind == EGraphCanvasHit::Node && Hit.Node == Node.Id && Mode == EMode::Idle)
		{
			ImGui::SetTooltip("%s", Problem->second.second.c_str());
		}
	}

	if (Mode == EMode::Linking)
	{
		const auto Start = PinOf(LinkStart, bLinkFromOutput);
		if (Start)
		{
			const FCanvasPoint End = Candidate ? (PinOf(bLinkFromOutput ? Candidate->To : Candidate->From, !bLinkFromOutput))->first : Mouse;
			const ImU32 Color = LinkProblem ? ToImU32(ToolUITheme::Error) : ToImU32(PinColor(Start->second));
			if (bLinkFromOutput)
			{
				DrawCurve(Start->first, End, Color, Thickness);
			}
			else
			{
				DrawCurve(End, Start->first, Color, Thickness);
			}
		}

		if (LinkProblem)
		{
			ImGui::SetTooltip("%s", LinkProblem->c_str());
		}
	}

	if (Mode == EMode::BoxSelecting)
	{
		const ImVec2 First = ToImVec2(View.ToScreen(DragStart));
		const ImVec2 Second = IO.MousePos;
		const ImVec2 Min(std::min(First.x, Second.x), std::min(First.y, Second.y));
		const ImVec2 Max(std::max(First.x, Second.x), std::max(First.y, Second.y));
		DrawList->AddRectFilled(Min, Max, WithAlpha(ToolUITheme::NeutralAccent, 24), 3.f);
		DrawList->AddRect(Min, Max, WithAlpha(ToolUITheme::NeutralAccent, 140), 3.f);
	}

	DrawList->PopClipRect();

	if (ImGui::BeginPopup("RenameComment"))
	{
		if (bFocusCommentText)
		{
			ImGui::SetKeyboardFocusHere();
			bFocusCommentText = false;
		}

		ImGui::SetNextItemWidth(280.f);
		if (ImGui::InputText("##CommentText", CommentText.data(), CommentText.size(), ImGuiInputTextFlags_EnterReturnsTrue))
		{
			FGraph After = Graph;
			if (const auto Comment = std::ranges::find(After.Comments, RenamingComment, &FGraphComment::Id); Comment != After.Comments.end() && Comment->Text != CommentText.data())
			{
				Comment->Text = CommentText.data();
				Edit = FGraphCanvasEdit{.Label = "Rename comment", .Graph = std::move(After)};
			}

			ImGui::CloseCurrentPopup();
		}

		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("NodeMenu"))
	{
		if (ImGui::MenuItem("Duplicate", "Ctrl+D") && !Selection.empty())
		{
			Edit = Duplicate(Graph, Schema, Selection);
		}

		if (ImGui::MenuItem("Format", "F"))
		{
			Edit = MoveNodes(Graph, FormatGraph(Graph, Schema, Layouts, GraphFormatRoots(Graph, Selection), FormatSettings), "Format nodes");
		}

		if (ImGui::MenuItem("Straighten links", "Q"))
		{
			Edit = MoveNodes(Graph, StraightenGraphLinks(Graph, Schema, Layouts, Selection), "Straighten links");
		}

		if (ImGui::BeginMenu("Align", Selection.size() >= 2))
		{
			constexpr std::array Items{
			    std::tuple{"Top", "Shift+W", EGraphAlignment::Top},
			    std::tuple{"Middle", "", EGraphAlignment::Middle},
			    std::tuple{"Bottom", "Shift+S", EGraphAlignment::Bottom},
			    std::tuple{"Left", "Shift+A", EGraphAlignment::Left},
			    std::tuple{"Center", "", EGraphAlignment::Center},
			    std::tuple{"Right", "Shift+D", EGraphAlignment::Right},
			};

			for (const auto& [Name, Shortcut, Alignment] : Items)
			{
				if (ImGui::MenuItem(Name, Shortcut))
				{
					Edit = MoveNodes(Graph, AlignGraphNodes(Layouts, Selection, Alignment), "Align nodes");
				}
			}

			ImGui::Separator();
			if (ImGui::MenuItem("Distribute horizontally", nullptr, false, Selection.size() >= 3))
			{
				Edit = MoveNodes(Graph, DistributeGraphNodes(Layouts, Selection, true), "Distribute nodes");
			}

			if (ImGui::MenuItem("Distribute vertically", nullptr, false, Selection.size() >= 3))
			{
				Edit = MoveNodes(Graph, DistributeGraphNodes(Layouts, Selection, false), "Distribute nodes");
			}

			ImGui::EndMenu();
		}

		if (ImGui::MenuItem("Comment selection", "C"))
		{
			if (const auto Bounds = GraphNodeBounds(Layouts, Selection))
			{
				FGraph After = Graph;
				const FGraphNodeId CommentId = NextGraphNodeId(Graph);
				After.Comments.push_back({.Id = CommentId, .Text = "Comment", .Rect = {(*Bounds)[0] - 24.f, (*Bounds)[1] - 24.f - CommentTitle, (*Bounds)[2] + 24.f, (*Bounds)[3] + 24.f}});
				Selection = {CommentId};
				Edit = FGraphCanvasEdit{.Label = "Add comment", .Graph = std::move(After)};
			}
		}

		if (ImGui::MenuItem("Swap nodes", nullptr, false, Selection.size() == 2))
		{
			Edit = MoveNodes(Graph, SwapGraphNodes(Layouts, Selection[0], Selection[1]), "Swap nodes");
		}

		if (ImGui::MenuItem("Zoom to tree", "Z"))
		{
			if (const auto Bounds = GraphNodeBounds(Layouts, GraphConnectedNodes(Graph, Selection)))
			{
				FrameBounds(*Bounds);
			}
		}

		ImGui::Separator();
		if (ImGui::MenuItem("Copy", "Ctrl+C") && !Selection.empty())
		{
			if (const auto Text = CopyGraphNodes(Graph, Schema, Selection))
			{
				ImGui::SetClipboardText(Text->c_str());
			}
		}

		if (ImGui::MenuItem("Delete", "Del") && !Selection.empty())
		{
			Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Delete node" : "Delete nodes", .Graph = RemoveNodes(Graph, Selection)};
			Selection.clear();
		}

		if (ImGui::MenuItem("Delete keeping links", "Shift+Del") && !Selection.empty())
		{
			Edit = FGraphCanvasEdit{.Label = Selection.size() == 1 ? "Delete node keeping links" : "Delete nodes keeping links", .Graph = RemoveGraphNodesKeepingLinks(Graph, Schema, Selection)};
			Selection.clear();
		}

		ImGui::EndPopup();
	}

	if (ImGui::BeginPopup("AddNode"))
	{
		if (bFocusSearch)
		{
			ImGui::SetKeyboardFocusHere();
			bFocusSearch = false;
		}

		ImGui::SetNextItemWidth(220.f);
		ImGui::InputTextWithHint("##Search", "Search nodes", Search.data(), Search.size());
		const std::string_view Query(Search.data());
		std::string_view Category;
		for (const FGraphNodeDescriptor& Descriptor : Schema.Nodes)
		{
			if (!MatchesSearch(Descriptor.Name, Query) && !MatchesSearch(Descriptor.Category, Query))
			{
				continue;
			}

			if (Descriptor.Category != Category)
			{
				Category = Descriptor.Category;
				ImGui::SeparatorText(std::string(Category).c_str());
			}

			const auto Count = std::ranges::count(Graph.Nodes, Descriptor.Type, &FGraphNode::Type);
			if (ImGui::Selectable(std::string(Descriptor.Name).c_str(), false, static_cast<std::uint32_t>(Count) < Descriptor.MaximumCount ? 0 : ImGuiSelectableFlags_Disabled))
			{
				auto Node = MakeGraphNode(Schema, Descriptor.Type, NextGraphNodeId(Graph), MenuPosition);
				if (Node)
				{
					FGraph After = Graph;
					Selection = {Node->Id};
					After.Nodes.push_back(std::move(*Node));
					Edit = FGraphCanvasEdit{.Label = std::format("Add {}", Descriptor.Name), .Graph = std::move(After)};
				}
			}
		}

		ImGui::EndPopup();
	}

	ImGui::PopID();
	return Edit;
}
}
