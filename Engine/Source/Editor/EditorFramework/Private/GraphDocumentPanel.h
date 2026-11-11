#pragma once

#include "Herta/AudioGraph/AudioGraph.h"
#include "Herta/EditorCore/TransactionHistory.h"
#include "Herta/VfxGraph/VfxGraph.h"
#include "Herta/GraphCore/Graph.h"
#include "Herta/GraphEditor/GraphCanvas.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FToolUIContext;

// What makes a graph document one domain's: its canvas look, its compiler's diagnostics, and any property that needs its own editor.
struct FGraphDocumentDomain
{
	const FGraphCanvasDomain* Canvas = nullptr;
	// Names the document in its tab, prompts, and errors, such as "Material Graph".
	std::string_view Title{};
	std::vector<FGraphDiagnostic> (*Compile)(const FGraph& Graph) = nullptr;
	// Draws a property the domain edits its own way, such as a choice from a fixed list, setting bChanged when it edits Value. Returns false
	// to leave the property to the default editor.
	bool (*DrawProperty)(std::string_view NodeType, const FGraphPropertyDescriptor& Property, FGraphValue& Value, bool& bChanged) = nullptr;
	// A hint for a property's editor, or empty for none.
	std::string_view (*PropertyTooltip)(std::string_view NodeType, std::string_view Key) = nullptr;
};

struct FGraphDocumentPanelContext
{
	// The document's state from whatever consumes it once saved, such as the material shader watcher.
	std::string_view Status{};
	// A preview image above the details, or zero while there is none.
	std::uint64_t PreviewTexture = 0;
	std::string_view PreviewStatus{};
	// Called each frame the graph is in front, so the owner can keep the preview current.
	std::function<void()> RequestPreview{};
	// The mouse wheel over the preview, in wheel notches.
	std::function<void(float)> ZoomPreview{};
	// Nodes that are live, such as the state a playing character's machine is in.
	std::span<const FGraphNodeId> Active{};
	// Domain controls above the details, such as an Audio Graph's audition.
	std::function<void()> DrawSidebar{};
};

// Edits one graph file with its own undo history.
class FGraphDocumentPanel final
{
public:
	explicit FGraphDocumentPanel(const FGraphDocumentDomain& InDomain);
	~FGraphDocumentPanel() = default;
	FGraphDocumentPanel(const FGraphDocumentPanel&) = delete;
	FGraphDocumentPanel& operator=(const FGraphDocumentPanel&) = delete;
	FGraphDocumentPanel(FGraphDocumentPanel&&) = delete;
	FGraphDocumentPanel& operator=(FGraphDocumentPanel&&) = delete;

	// Path is the graph's mounted key, such as Game/Materials/Rock.hmatgraph; File is where it lives on disk.
	void Open(std::string Path, std::filesystem::path File, FGraph Graph, bool bReadOnly);
	void Draw(FToolUIContext& ToolUI, const FGraphDocumentPanelContext& Context);
	[[nodiscard]] std::expected<void, FGraphError> Save();
	// Closes now when nothing is unsaved; otherwise asks to save and stays open, and the caller tries again once the user has answered.
	bool RequestClose();
	[[nodiscard]] bool IsOpen() const;
	[[nodiscard]] bool IsFocused() const;
	[[nodiscard]] bool IsDirty() const;
	[[nodiscard]] const std::string& GetPath() const;
	[[nodiscard]] const std::filesystem::path& GetFile() const;
	[[nodiscard]] const FGraph& GetDraft() const;
	// Changes whenever the draft does, including during a property drag.
	[[nodiscard]] std::uint64_t GetDraftRevision() const;
	// Compiles the draft again for its diagnostics when something it depends on changed, such as the content it names.
	void Recompile();

private:
	void Apply(FGraph After, std::string Label);
	void Compile();
	void DrawDetails();
	void DrawClosePrompt();

	const FGraphDocumentDomain& Domain;
	std::string Path{};
	std::filesystem::path File{};
	FGraph Draft{};
	FGraph Saved{};
	std::vector<FGraphDiagnostic> Diagnostics{};
	std::string Error{};
	FGraphCanvas Canvas{};
	// The draft before a property drag or text edit began, recorded as one transaction when it ends.
	std::optional<FGraph> GestureBefore{};
	bool bOpen = false;
	bool bFocused = false;
	bool bReadOnly = false;
	bool bClosePrompt = false;
	std::uint64_t DraftRevision = 0;

	// History callbacks refer to the draft, so history is destroyed first.
	FEditorTransactionHistory History{256, 16 * 1024 * 1024};
};

[[nodiscard]] const FGraphDocumentDomain& GetMaterialGraphDocumentDomain();
// The default graph for a new material: the material output fed by the base color texture, which matches the engine material until edited.
[[nodiscard]] FGraph MakeDefaultMaterialGraph();
[[nodiscard]] const FGraphDocumentDomain& GetAnimationGraphDocumentDomain();
[[nodiscard]] const FGraphDocumentDomain& GetAudioGraphDocumentDomain();
// Audio Graphs name waves by content path, so their diagnostics need the open document's content to resolve them.
void SetAudioGraphDocumentResolver(FAudioWaveResolver Resolver, FAudioGraphLoader Loader);
// A new Audio Graph: one wave player, still without waves, feeding the output.
[[nodiscard]] FGraph MakeDefaultAudioGraph();
[[nodiscard]] const FGraphDocumentDomain& GetVfxGraphDocumentDomain();
[[nodiscard]] const FGraphDocumentDomain& GetStateTreeDocumentDomain();
[[nodiscard]] const FGraphDocumentDomain& GetGameplayGraphDocumentDomain();
}
