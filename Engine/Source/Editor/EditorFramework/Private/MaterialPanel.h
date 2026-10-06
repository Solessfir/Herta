#pragma once

#include "Herta/Assets/Material.h"
#include "Herta/EditorCore/TransactionHistory.h"

#include <chrono>
#include <functional>
#include <optional>
#include <span>

namespace Herta
{
class FToolUIContext;

enum class EMaterialCloseChoice
{
	Save,
	Discard,
	Cancel,
};

struct FMaterialTextureOption
{
	FAssetId Id;
	std::string Label;
};

struct FMaterialPanelContext
{
	std::span<const FMaterialTextureOption> Textures{};
	std::uint64_t PreviewTexture = 0;
	std::string_view PreviewStatus{};
	std::string_view ShaderStatus{};
	std::function<std::expected<void, FAssetError>(FAssetId, const FMaterialAsset&)> Save{};
	std::function<std::expected<FMaterialAsset, FAssetError>(FAssetId)> Reload{};
	std::function<void(FAssetId, const FMaterialAsset&)> Preview{};
};

class FMaterialPanel final
{
public:
	FMaterialPanel() = default;
	~FMaterialPanel() = default;
	FMaterialPanel(const FMaterialPanel&) = delete;
	FMaterialPanel& operator=(const FMaterialPanel&) = delete;
	FMaterialPanel(FMaterialPanel&&) = delete;
	FMaterialPanel& operator=(FMaterialPanel&&) = delete;

	void Open(FAssetId Asset, FMaterialAsset Material, std::string Source, bool bReadOnly = false);
	bool RequestClose();
	std::optional<bool> TakeCloseResult();
	[[nodiscard]] std::expected<void, FAssetError> ResolvePendingChanges(EMaterialCloseChoice Choice, const FMaterialPanelContext& Context);
	void Draw(FToolUIContext& ToolUI, const FMaterialPanelContext& Context);
	bool IsOpen() const;
	bool IsFocused() const;
	bool IsDirty() const;
	FAssetId GetAsset() const;
	const FMaterialAsset& GetDraft() const;
	void BeginEdit(std::string_view Label);
	[[nodiscard]] std::expected<void, FAssetError> SetDraft(FMaterialAsset Material, std::string_view Label = "Edit material");
	[[nodiscard]] std::expected<void, FAssetError> EndEdit();
	void CancelEdit();
	[[nodiscard]] std::expected<void, FEditorCommandError> Undo();
	[[nodiscard]] std::expected<void, FEditorCommandError> Redo();
	[[nodiscard]] std::expected<void, FAssetError> Save(const FMaterialPanelContext& Context);
	[[nodiscard]] std::expected<void, FAssetError> Reload(const FMaterialPanelContext& Context);

private:
	struct FOpenRequest
	{
		FAssetId Asset;
		FMaterialAsset Material;
		std::string Source;
		bool bReadOnly = false;
	};

	void ApplyOpen(FOpenRequest Request);
	void CompletePendingAction();
	void ShowError(std::string Message);

	FAssetId Asset{};
	FMaterialAsset Draft;
	FMaterialAsset Saved;
	std::string Source;
	std::string Error;
	std::array<char, 96> Search{};
	std::array<char, 96> TextureSearch{};
	std::array<char, 512> ShaderPath{};
	std::optional<FMaterialAsset> GestureBefore;
	std::string GestureLabel;
	std::optional<FOpenRequest> PendingOpen;
	std::optional<FMaterialAsset> LastPreview;
	std::chrono::steady_clock::time_point NextPreview{};
	bool bOpen = false;
	bool bFocused = false;
	bool bReadOnly = false;
	bool bCloseRequested = false;
	bool bReloadRequested = false;
	bool bRequestFocus = false;
	bool bExternalCloseRequested = false;
	std::optional<bool> CloseResult{};

	// History callbacks refer to the draft, so history is destroyed first.
	FEditorTransactionHistory History{128, 4 * 1024 * 1024};
};
}
