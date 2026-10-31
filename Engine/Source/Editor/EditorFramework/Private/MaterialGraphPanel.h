#pragma once

#include "Herta/EditorCore/TransactionHistory.h"
#include "Herta/GraphCore/Graph.h"
#include "Herta/GraphEditor/GraphCanvas.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
class FToolUIContext;

struct FMaterialGraphPanelContext
{
	// The pipeline's state from the material shader watcher, which recompiles a graph once it is saved.
	std::string_view ShaderStatus{};
	// The graph's material on a studio-lit sphere, or zero while there is none.
	std::uint64_t PreviewTexture = 0;
	std::string_view PreviewStatus{};
	// Called each frame the graph is in front, so the owner can keep the preview current.
	std::function<void()> RequestPreview{};
};

// Edits one Material Graph file with its own undo history; saving publishes it to the material shader watcher.
class FMaterialGraphPanel final
{
public:
	FMaterialGraphPanel() = default;
	~FMaterialGraphPanel() = default;
	FMaterialGraphPanel(const FMaterialGraphPanel&) = delete;
	FMaterialGraphPanel& operator=(const FMaterialGraphPanel&) = delete;
	FMaterialGraphPanel(FMaterialGraphPanel&&) = delete;
	FMaterialGraphPanel& operator=(FMaterialGraphPanel&&) = delete;

	// Path is the graph's mounted shader key, such as Game/Materials/Rock.hmatgraph; File is where it lives on disk.
	void Open(std::string Path, std::filesystem::path File, FGraph Graph, bool bReadOnly);
	void Draw(FToolUIContext& ToolUI, const FMaterialGraphPanelContext& Context);
	[[nodiscard]] std::expected<void, FGraphError> Save();
	// Closes now when nothing is unsaved; otherwise asks to save and stays open, and the caller tries again once the user has answered.
	bool RequestClose();
	[[nodiscard]] bool IsOpen() const;
	[[nodiscard]] bool IsFocused() const;
	[[nodiscard]] bool IsDirty() const;
	[[nodiscard]] const std::string& GetPath() const;

private:
	void Apply(FGraph After, std::string Label);
	void Compile();
	void DrawDetails();
	void DrawClosePrompt();

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

	// History callbacks refer to the draft, so history is destroyed first.
	FEditorTransactionHistory History{256, 16 * 1024 * 1024};
};

// The default graph for a new material: the material output fed by the base color texture, which matches the engine material until edited.
[[nodiscard]] FGraph MakeDefaultMaterialGraph();
}
