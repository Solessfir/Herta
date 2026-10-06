#include "MaterialPanel.h"

#include "DetailsPanel.h"
#include "Herta/Assets/AssetSearch.h"
#include "Herta/ToolUI/ToolUI.h"
#include "NumericField.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <utility>

namespace Herta
{
namespace
{
bool MatchesMaterialSearch(const std::string_view Label, const std::string_view Query)
{
	return Query.empty() || std::ranges::search(Label, Query, [](const char Left, const char Right)
	{
		return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	}).begin() != Label.end();
}

bool BeginMaterialSection(const char* const Name, const float Scale)
{
	ImGui::Spacing();
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 3.f * Scale});
	const bool bOpen = ImGui::CollapsingHeader(Name, ImGuiTreeNodeFlags_DefaultOpen);
	ImGui::PopStyleVar();
	return bOpen;
}
}

void FMaterialPanel::ApplyOpen(FOpenRequest Request)
{
	Asset = Request.Asset;
	Draft = std::move(Request.Material);
	Saved = Draft;
	Source = std::move(Request.Source);
	bReadOnly = Request.bReadOnly;
	bOpen = true;
	bRequestFocus = true;
	bCloseRequested = false;
	bReloadRequested = false;
	bExternalCloseRequested = false;
	Error.clear();
	GestureBefore.reset();
	History.Clear();
	History.MarkSaved();
	Search.fill('\0');
	ShaderPath.fill('\0');
	std::ranges::copy(Draft.ShaderPath.substr(0, ShaderPath.size() - 1), ShaderPath.begin());
	LastPreview.reset();
}

void FMaterialPanel::Open(const FAssetId InAsset, FMaterialAsset Material, std::string InSource, const bool bInReadOnly)
{
	if (bOpen && Asset == InAsset)
	{
		bRequestFocus = true;
		return;
	}

	FOpenRequest Request{.Asset = InAsset, .Material = std::move(Material), .Source = std::move(InSource), .bReadOnly = bInReadOnly};
	if (bOpen && IsDirty())
	{
		PendingOpen = std::move(Request);
		return;
	}

	ApplyOpen(std::move(Request));
}

bool FMaterialPanel::IsOpen() const
{
	return bOpen;
}

bool FMaterialPanel::RequestClose()
{
	PendingOpen.reset();
	bReloadRequested = false;
	CloseResult.reset();
	if (!bOpen || !IsDirty())
	{
		bOpen = false;
		bCloseRequested = false;
		bExternalCloseRequested = false;
		CloseResult = true;
		return true;
	}

	bCloseRequested = true;
	bExternalCloseRequested = true;
	bRequestFocus = true;
	return false;
}

std::optional<bool> FMaterialPanel::TakeCloseResult()
{
	return std::exchange(CloseResult, std::nullopt);
}

std::expected<void, FAssetError> FMaterialPanel::ResolvePendingChanges(const EMaterialCloseChoice Choice, const FMaterialPanelContext& Context)
{
	const bool bExternal = bExternalCloseRequested;
	if (Choice == EMaterialCloseChoice::Cancel)
	{
		PendingOpen.reset();
		bCloseRequested = false;
		bReloadRequested = false;
		bExternalCloseRequested = false;
		if (bExternal)
		{
			CloseResult = false;
		}

		return {};
	}

	if (Choice == EMaterialCloseChoice::Save)
	{
		if (const auto Result = Save(Context); !Result)
		{
			return Result;
		}
	}

	if (bReloadRequested)
	{
		if (const auto Result = Reload(Context); !Result)
		{
			return Result;
		}
	}
	else
	{
		Draft = Saved;
		GestureBefore.reset();
		History.Clear();
		History.MarkSaved();
		LastPreview.reset();
		if (Context.Preview)
		{
			Context.Preview(Asset, Saved);
		}

		CompletePendingAction();
	}

	bExternalCloseRequested = false;
	if (bExternal)
	{
		CloseResult = true;
	}

	return {};
}

bool FMaterialPanel::IsFocused() const
{
	return bOpen && bFocused;
}

bool FMaterialPanel::IsDirty() const
{
	return Draft != Saved;
}

FAssetId FMaterialPanel::GetAsset() const
{
	return Asset;
}

const FMaterialAsset& FMaterialPanel::GetDraft() const
{
	return Draft;
}

void FMaterialPanel::BeginEdit(const std::string_view Label)
{
	if (bReadOnly || (GestureBefore && GestureLabel == Label))
	{
		return;
	}

	if (GestureBefore)
	{
		if (const auto Result = EndEdit(); !Result)
		{
			ShowError(Result.error().Message);
			return;
		}
	}

	GestureBefore = Draft;
	GestureLabel = Label;
}

std::expected<void, FAssetError> FMaterialPanel::SetDraft(FMaterialAsset Material, const std::string_view Label)
{
	if (bReadOnly)
	{
		return std::unexpected(FAssetError{.Message = "Engine material assets are read-only"});
	}

	if (auto Valid = ValidateMaterial(Material); !Valid)
	{
		return Valid;
	}

	if (Draft == Material)
	{
		return {};
	}

	const bool bImmediate = !GestureBefore;
	if (bImmediate)
	{
		BeginEdit(Label);
	}

	Draft = std::move(Material);
	Error.clear();
	return bImmediate ? EndEdit() : std::expected<void, FAssetError>{};
}

std::expected<void, FAssetError> FMaterialPanel::EndEdit()
{
	if (!GestureBefore)
	{
		return {};
	}

	const FMaterialAsset Before = *GestureBefore;
	GestureBefore.reset();
	if (Before == Draft)
	{
		return {};
	}

	const FMaterialAsset After = Draft;
	const auto Result = History.RecordApplied({
	    .Label = GestureLabel,
	    .Apply = [this, Before, After](const bool bUndo) -> std::expected<void, FEditorCommandError>
	{
		Draft = bUndo ? Before : After;
		ShaderPath.fill('\0');
		std::ranges::copy(Draft.ShaderPath.substr(0, ShaderPath.size() - 1), ShaderPath.begin());
		LastPreview.reset();
		return {};
	},
	    .MemoryCost = sizeof(FMaterialAsset) * 2 + Before.Name.size() + After.Name.size() + Before.ShaderPath.size() + After.ShaderPath.size(),
	});

	if (!Result)
	{
		Draft = Before;
		return std::unexpected(FAssetError{.Message = Result.error().Message});
	}

	return {};
}

void FMaterialPanel::CancelEdit()
{
	if (GestureBefore)
	{
		Draft = std::move(*GestureBefore);
		GestureBefore.reset();
		ShaderPath.fill('\0');
		std::ranges::copy(Draft.ShaderPath.substr(0, ShaderPath.size() - 1), ShaderPath.begin());
		LastPreview.reset();
	}
}

std::expected<void, FEditorCommandError> FMaterialPanel::Undo()
{
	if (auto Result = EndEdit(); !Result)
	{
		return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
	}

	return History.Undo();
}

std::expected<void, FEditorCommandError> FMaterialPanel::Redo()
{
	if (auto Result = EndEdit(); !Result)
	{
		return std::unexpected(FEditorCommandError{.Message = Result.error().Message});
	}

	return History.Redo();
}

std::expected<void, FAssetError> FMaterialPanel::Save(const FMaterialPanelContext& Context)
{
	if (auto Result = EndEdit(); !Result)
	{
		return Result;
	}

	if (bReadOnly || !Context.Save)
	{
		return std::unexpected(FAssetError{.Message = "This material cannot be saved"});
	}

	const auto Result = Context.Save(Asset, Draft);
	if (!Result)
	{
		return Result;
	}

	Saved = Draft;
	History.MarkSaved();
	Error.clear();
	return {};
}

std::expected<void, FAssetError> FMaterialPanel::Reload(const FMaterialPanelContext& Context)
{
	if (!Context.Reload)
	{
		return std::unexpected(FAssetError{.Message = "Material reload is unavailable"});
	}

	auto Result = Context.Reload(Asset);
	if (!Result)
	{
		return std::unexpected(Result.error());
	}

	if (auto Valid = ValidateMaterial(*Result); !Valid)
	{
		return Valid;
	}

	ApplyOpen({.Asset = Asset, .Material = std::move(*Result), .Source = Source, .bReadOnly = bReadOnly});
	return {};
}

void FMaterialPanel::ShowError(std::string Message)
{
	Error = std::move(Message);
}

void FMaterialPanel::CompletePendingAction()
{
	if (PendingOpen)
	{
		FOpenRequest Request = std::move(*PendingOpen);
		PendingOpen.reset();
		ApplyOpen(std::move(Request));
	}
	else if (bCloseRequested)
	{
		bOpen = false;
		bCloseRequested = false;
	}
}

void FMaterialPanel::Draw(FToolUIContext& ToolUI, const FMaterialPanelContext& Context)
{
	if (!bOpen)
	{
		return;
	}

	if (std::exchange(bRequestFocus, false))
	{
		ImGui::SetNextWindowFocus();
	}

	const float Scale = ImGui::GetFontSize() / ToolUI.GetMetrics().BaseFontSize;
	const ImGuiViewport* const Viewport = ImGui::GetMainViewport();
	const ImVec2 Available{std::max(1.f, Viewport->WorkSize.x - 32.f * Scale), std::max(1.f, Viewport->WorkSize.y - 32.f * Scale)};
	ImGui::SetNextWindowSize({std::min(520.f * Scale, Available.x), std::min(760.f * Scale, Available.y)}, ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos({Viewport->WorkPos.x + Viewport->WorkSize.x * 0.5f, Viewport->WorkPos.y + Viewport->WorkSize.y * 0.5f}, ImGuiCond_FirstUseEver, {0.5f, 0.5f});
	ImGui::SetNextWindowSizeConstraints({std::min(360.f * Scale, Available.x), std::min(300.f * Scale, Available.y)}, Available);

	const bool bVisible = ToolUI.BeginPanel("Material", &bOpen);
	bFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
	if (bVisible)
	{
		ImGui::PushTextWrapPos();
		ImGui::TextUnformatted((Draft.Name + (IsDirty() ? " *" : "")).c_str());
		ImGui::TextDisabled("%s", Source.c_str());
		if (bReadOnly)
		{
			ImGui::TextDisabled("Engine content is read-only. Create a Game material to edit.");
		}

		ImGui::PopTextWrapPos();
		const auto NextToolbarButton = [Scale](const char* const Label)
		{
			const float Width = ImGui::CalcTextSize(Label).x + 44.f * Scale;
			if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + Width <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
			{
				ImGui::SameLine();
			}
		};

		const auto Report = [this](const auto& Result)
		{
			if (!Result)
			{
				ShowError(Result.error().Message);
			}
		};

		ImGui::BeginDisabled(bReadOnly || !Context.Save || !IsDirty());
		if (ToolUIButton("Save", EToolUIMenuIcon::Save) || (!bReadOnly && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused)))
		{
			Report(Save(Context));
		}

		ImGui::EndDisabled();
		NextToolbarButton("Reload");
		ImGui::BeginDisabled(!Context.Reload);
		if (ToolUIButton("Reload", EToolUIMenuIcon::Sync))
		{
			if (IsDirty())
			{
				bReloadRequested = true;
			}
			else
			{
				Report(Reload(Context));
			}
		}

		ImGui::EndDisabled();
		NextToolbarButton("Undo");
		ImGui::BeginDisabled(!History.CanUndo() || bReadOnly);
		if (ToolUIButton("Undo", EToolUIMenuIcon::Undo))
		{
			Report(Undo());
		}

		ImGui::EndDisabled();
		NextToolbarButton("Redo");

		ImGui::BeginDisabled(!History.CanRedo() || bReadOnly);
		if (ToolUIButton("Redo", EToolUIMenuIcon::Redo))
		{
			Report(Redo());
		}

		ImGui::EndDisabled();
		if (!bReadOnly && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteFocused))
		{
			if (History.CanUndo())
			{
				Report(Undo());
			}
		}

		if (!bReadOnly && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteFocused) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)))
		{
			if (History.CanRedo())
			{
				Report(Redo());
			}
		}

		if (!Error.empty())
		{
			ImGui::PushStyleColor(ImGuiCol_Text, {0.94f, 0.48f, 0.4f, 1.f});
			ImGui::TextWrapped("%s", Error.c_str());
			ImGui::PopStyleColor();
		}

		if (Context.PreviewTexture != 0)
		{
			const float Width = std::min(ImGui::GetContentRegionAvail().x, 256.f * Scale);
			const ImVec2 Position = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddImageRounded(ImTextureRef(static_cast<ImTextureID>(Context.PreviewTexture)), Position, {Position.x + Width, Position.y + Width}, {0, 0}, {1, 1}, IM_COL32_WHITE, ImGui::GetStyle().FrameRounding);
			ImGui::Dummy({Width, Width});
		}

		if (!Context.PreviewStatus.empty())
		{
			ImGui::PushTextWrapPos();
			ImGui::TextDisabled("%.*s", static_cast<int>(Context.PreviewStatus.size()), Context.PreviewStatus.data());
			ImGui::PopTextWrapPos();
		}

		ImGui::SetNextItemWidth(-FLT_MIN);
		ToolUI.DrawSearchField("##MaterialSearch", "Search material properties", Search.data(), Search.size());
		const std::string_view Query(Search.data());
		ImGui::BeginDisabled(bReadOnly);
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6.f * Scale, 4.f * Scale});
		ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {0.f, 2.f * Scale});
		const auto BeginTable = [Scale]
		{
			if (!ImGui::BeginTable("##Parameters", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings))
			{
				return false;
			}

			ImGui::TableSetupColumn("##Label", ImGuiTableColumnFlags_WidthFixed, std::min(112.f * Scale, ImGui::GetContentRegionAvail().x * 0.4f));
			ImGui::TableSetupColumn("##Value", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("##Reset", ImGuiTableColumnFlags_WidthFixed, 22.f * Scale);
			return true;
		};
		const auto Row = [](const char* Label)
		{
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled("%s", Label);
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
		};
		const auto Apply = [&](FMaterialAsset Candidate, const std::string_view Label)
		{
			Report(SetDraft(std::move(Candidate), Label));
		};
		const auto FloatRow = [&](const char* Label, float FMaterialParameters::* const Member, const float Minimum, const float Maximum, const float Speed = 0.01f, const char* Format = "%.3f")
		{
			if (!MatchesMaterialSearch(Label, Query))
			{
				return;
			}

			ImGui::PushID(Label);
			Row(Label);
			float Value = Draft.Parameters.*Member;
			FNumericEditLifecycle Edit{
			    .Begin = [&]
			{
				BeginEdit(Label);
				Value = Draft.Parameters.*Member;
			},
			    .Flush = [&](const bool bCanceled)
			{
				if (bCanceled)
				{
					CancelEdit();
				}
				else
				{
					Report(EndEdit());
				}

				Value = Draft.Parameters.*Member;
			},
			};
			if (DrawNumericDragFloat("##Value", &Value, Speed, Minimum, Maximum, Format, ImGuiSliderFlags_AlwaysClamp, &Edit, true) && !Edit.bCanceled)
			{
				FMaterialAsset Candidate = Draft;
				Candidate.Parameters.*Member = Value;
				Apply(std::move(Candidate), Label);
			}

			if (Edit.bCanceled)
			{
				CancelEdit();
			}
			else if (Edit.bFinished)
			{
				Report(EndEdit());
			}

			ImGui::TableSetColumnIndex(2);
			if (Draft.Parameters.*Member != FMaterialParameters{}.*Member && DrawDetailsResetButton(Label))
			{
				Report(EndEdit());
				FMaterialAsset Candidate = Draft;
				Candidate.Parameters.*Member = FMaterialParameters{}.*Member;
				Apply(std::move(Candidate), Label);
			}

			ImGui::PopID();
		};
		const auto ColorRow = [&](const char* Label, const bool bEmission)
		{
			if (!MatchesMaterialSearch(Label, Query))
			{
				return;
			}

			ImGui::PushID(Label);
			Row(Label);
			FMaterialAsset Candidate = Draft;
			float* Values = bEmission ? Candidate.Parameters.Emissive.data() : Candidate.Parameters.BaseColor.data();
			const bool bChanged = DrawLinearColorField("##Color", std::span<float>{Values, bEmission ? 3u : 4u});
			if (ImGui::IsItemActivated() || bChanged)
			{
				BeginEdit(Label);
			}

			if (bChanged)
			{
				Apply(std::move(Candidate), Label);
			}

			if (ImGui::IsItemDeactivatedAfterEdit())
			{
				Report(EndEdit());
			}

			ImGui::TableSetColumnIndex(2);
			const bool bDefault = bEmission ? Draft.Parameters.Emissive == FMaterialParameters{}.Emissive : Draft.Parameters.BaseColor == FMaterialParameters{}.BaseColor;
			if (!bDefault && DrawDetailsResetButton(Label))
			{
				Report(EndEdit());
				FMaterialAsset Reset = Draft;
				if (bEmission)
				{
					Reset.Parameters.Emissive = FMaterialParameters{}.Emissive;
				}
				else
				{
					Reset.Parameters.BaseColor = FMaterialParameters{}.BaseColor;
				}

				Apply(std::move(Reset), Label);
			}

			ImGui::PopID();
		};

		if (MatchesMaterialSearch("Base color Metallic Roughness Normal strength Occlusion Blend mode Alpha cutoff", Query) && BeginMaterialSection("Surface", Scale) && BeginTable())
		{
			ColorRow("Base color", false);
			FloatRow("Metallic", &FMaterialParameters::Metallic, 0.f, 1.f);
			FloatRow("Roughness", &FMaterialParameters::Roughness, 0.f, 1.f);
			FloatRow("Normal strength", &FMaterialParameters::NormalStrength, 0.f, 8.f);
			FloatRow("Occlusion", &FMaterialParameters::OcclusionStrength, 0.f, 1.f);
			if (MatchesMaterialSearch("Blend mode", Query))
			{
				Row("Blend mode");
				if (ImGui::BeginCombo("##BlendMode", Draft.Parameters.BlendMode == EMaterialBlendMode::Masked ? "Masked" : "Opaque"))
				{
					for (const auto& [Label, Mode] : {std::pair{"Opaque", EMaterialBlendMode::Opaque}, std::pair{"Masked", EMaterialBlendMode::Masked}})
					{
						if (ImGui::Selectable(Label, Draft.Parameters.BlendMode == Mode))
						{
							Report(EndEdit());
							FMaterialAsset Candidate = Draft;
							Candidate.Parameters.BlendMode = Mode;
							Apply(std::move(Candidate), "Blend mode");
						}
					}

					ImGui::EndCombo();
				}
			}

			if (Draft.Parameters.BlendMode == EMaterialBlendMode::Masked)
			{
				FloatRow("Alpha cutoff", &FMaterialParameters::AlphaCutoff, 0.f, 1.f);
			}

			ImGui::EndTable();
		}

		ImGui::PushID("Emission");
		if (MatchesMaterialSearch("Emissive color Intensity", Query) && BeginMaterialSection("Emission", Scale) && BeginTable())
		{
			ColorRow("Emissive color", true);
			FloatRow("Intensity", &FMaterialParameters::EmissiveIntensity, 0.f, 1'000'000.f, 0.1f);
			ImGui::EndTable();
		}

		ImGui::PopID();
		ImGui::PushID("Coordinates");
		if (MatchesMaterialSearch("UV scale UV offset", Query) && BeginMaterialSection("Coordinates", Scale) && BeginTable())
		{
			for (const bool bOffset : {false, true})
			{
				const char* const Label = bOffset ? "UV offset" : "UV scale";
				if (!MatchesMaterialSearch(Label, Query))
				{
					continue;
				}

				ImGui::PushID(Label);
				Row(Label);
				std::array<float, 2> Values = bOffset ? Draft.Parameters.UVOffset : Draft.Parameters.UVScale;
				const float Width = std::max(1.f, (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
				for (int Axis = 0; Axis < 2; ++Axis)
				{
					ImGui::PushID(Axis);
					if (Axis != 0)
					{
						ImGui::SameLine();
					}

					ImGui::SetNextItemWidth(Width);
					FNumericEditLifecycle Edit{.Begin = [&]
					{
						BeginEdit(Label);
					},
					    .Flush = [&](const bool bCanceled)
					{
						if (bCanceled)
						{
							CancelEdit();
						}
						else
						{
							Report(EndEdit());
						}
					}};
					if (DrawNumericDragFloat("##UV", &Values[static_cast<std::size_t>(Axis)], 0.01f, -10'000.f, 10'000.f, "%.3f", ImGuiSliderFlags_AlwaysClamp, &Edit, true) && !Edit.bCanceled)
					{
						FMaterialAsset Candidate = Draft;
						(bOffset ? Candidate.Parameters.UVOffset : Candidate.Parameters.UVScale)[static_cast<std::size_t>(Axis)] = Values[static_cast<std::size_t>(Axis)];
						Apply(std::move(Candidate), Label);
					}

					if (Edit.bCanceled)
					{
						CancelEdit();
					}
					else if (Edit.bFinished)
					{
						Report(EndEdit());
					}

					ImGui::PopID();
				}

				ImGui::TableSetColumnIndex(2);
				const auto Default = bOffset ? FMaterialParameters{}.UVOffset : FMaterialParameters{}.UVScale;
				if ((bOffset ? Draft.Parameters.UVOffset : Draft.Parameters.UVScale) != Default && DrawDetailsResetButton(Label))
				{
					Report(EndEdit());
					FMaterialAsset Candidate = Draft;
					(bOffset ? Candidate.Parameters.UVOffset : Candidate.Parameters.UVScale) = Default;
					Apply(std::move(Candidate), Label);
				}

				ImGui::PopID();
			}

			ImGui::EndTable();
		}

		ImGui::PopID();
		if (MatchesMaterialSearch("Base color Metallic Roughness Normal Occlusion Emissive", Query) && BeginMaterialSection("Textures", Scale))
		{
			constexpr std::array<const char*, MaterialTextureSlotCount> Names{"Base color", "Metallic", "Roughness", "Normal", "Occlusion", "Emissive"};
			constexpr std::array<const char*, 4> Channels{"R", "G", "B", "A"};
			for (std::size_t Slot = 0; Slot < Names.size(); ++Slot)
			{
				if (!MatchesMaterialSearch(Names[Slot], Query))
				{
					continue;
				}

				ImGui::PushID(static_cast<int>(Slot));
				ImGui::AlignTextToFramePadding();
				ImGui::TextUnformatted(Names[Slot]);
				const bool bColor = Slot == static_cast<std::size_t>(EMaterialTextureSlot::BaseColor) || Slot == static_cast<std::size_t>(EMaterialTextureSlot::Emissive);
				const bool bScalar = !bColor && Slot != static_cast<std::size_t>(EMaterialTextureSlot::Normal);
				FMaterialTextureBinding Binding = Draft.Textures[Slot];
				const char* Current = Binding.Texture.IsValid() ? "Missing texture" : "None";
				for (const auto& Option : Context.Textures)
				{
					if (Option.Id == Binding.Texture)
					{
						Current = Option.Label.c_str();
						break;
					}
				}

				bool bChanged = false;
				const float ResetWidth = 22.f * Scale;
				ImGui::SetNextItemWidth(std::max(1.f, ImGui::GetContentRegionAvail().x - ResetWidth - ImGui::GetStyle().ItemSpacing.x));
				if (ImGui::BeginCombo("##Texture", Current))
				{
					if (ImGui::IsWindowAppearing())
					{
						TextureSearch.fill('\0');
						ImGui::SetKeyboardFocusHere();
					}

					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::PushStyleColor(ImGuiCol_NavCursor, {0, 0, 0, 0});
					ToolUI.DrawSearchField("##TextureSearch", "Search textures", TextureSearch.data(), TextureSearch.size());
					ImGui::PopStyleColor();
					if (ImGui::Selectable("None", !Binding.Texture.IsValid()))
					{
						Binding.Texture = {};
						bChanged = true;
					}

					std::vector<std::string_view> Labels;
					Labels.reserve(Context.Textures.size());
					for (const auto& Option : Context.Textures)
					{
						Labels.push_back(Option.Label);
					}

					const auto Matches = SearchAssets(Labels, TextureSearch.data());
					ImGuiListClipper Clipper;
					Clipper.Begin(Matches ? static_cast<int>(Matches->size()) : 0);
					while (Clipper.Step())
					{
						for (int MatchIndex = Clipper.DisplayStart; MatchIndex < Clipper.DisplayEnd; ++MatchIndex)
						{
							const auto& Option = Context.Textures[(*Matches)[static_cast<std::size_t>(MatchIndex)].Index];
							if (ImGui::Selectable(Option.Label.c_str(), Binding.Texture == Option.Id))
							{
								Binding.Texture = Option.Id;
								bChanged = true;
							}
						}
					}

					ImGui::EndCombo();
				}

				ImGui::SameLine();
				if (Binding != DefaultMaterialTextureBindings[Slot] && DrawDetailsResetButton(Names[Slot]))
				{
					Binding = DefaultMaterialTextureBindings[Slot];
					bChanged = true;
				}

				if (bScalar)
				{
					ImGui::AlignTextToFramePadding();
					ImGui::TextDisabled("Channel");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(64.f * Scale);
					if (ImGui::BeginCombo("##Channel", Channels[static_cast<std::size_t>(Binding.Channel)]))
					{
						for (std::size_t Channel = 0; Channel < Channels.size(); ++Channel)
						{
							if (ImGui::Selectable(Channels[Channel], Channel == static_cast<std::size_t>(Binding.Channel)))
							{
								Binding.Channel = static_cast<EMaterialChannel>(Channel);
								bChanged = true;
							}
						}

						ImGui::EndCombo();
					}

					if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ImGui::CalcTextSize("Linear data").x <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
					{
						ImGui::SameLine();
					}

					ImGui::TextDisabled("Linear data");
				}
				else if (bColor)
				{
					ImGui::SetNextItemWidth(std::min(148.f * Scale, ImGui::GetContentRegionAvail().x));
					if (ImGui::BeginCombo("##ColorSpace", Binding.ColorSpace == ETextureColorSpace::Srgb ? "sRGB color" : "Linear color"))
					{
						for (const auto& [Label, Space] : {std::pair{"sRGB color", ETextureColorSpace::Srgb}, std::pair{"Linear color", ETextureColorSpace::Linear}})
						{
							if (ImGui::Selectable(Label, Binding.ColorSpace == Space))
							{
								Binding.ColorSpace = Space;
								bChanged = true;
							}
						}

						ImGui::EndCombo();
					}
				}
				else
				{
					ImGui::TextDisabled("RGB - Linear data");
				}

				if (bChanged)
				{
					Report(EndEdit());
					FMaterialAsset Candidate = Draft;
					Candidate.Textures[Slot] = Binding;
					Apply(std::move(Candidate), std::string("Set ") + Names[Slot] + " texture");
				}

				ImGui::Spacing();
				ImGui::PopID();
			}
		}

		if (MatchesMaterialSearch("Shader", Query) && BeginMaterialSection("Shader", Scale))
		{
			ImGui::TextDisabled("Compatible Slang shader");
			ImGui::SetNextItemWidth(-FLT_MIN);
			const bool bEnter = ImGui::InputTextWithHint("##ShaderPath", "Engine PBR (default)", ShaderPath.data(), ShaderPath.size(), ImGuiInputTextFlags_EnterReturnsTrue);
			if (bEnter || ImGui::IsItemDeactivatedAfterEdit())
			{
				Report(EndEdit());
				FMaterialAsset Candidate = Draft;
				Candidate.ShaderPath = ShaderPath.data();
				Apply(std::move(Candidate), "Change material shader");
			}

			ImGui::SetItemTooltip("Path relative to the material's content root. A failed compile keeps the last working pipeline.");
			if (!Context.ShaderStatus.empty())
			{
				ImGui::TextWrapped("%.*s", static_cast<int>(Context.ShaderStatus.size()), Context.ShaderStatus.data());
			}

			if (!Draft.ShaderPath.empty() && ToolUIButton("Use engine PBR", EToolUIMenuIcon::Undo))
			{
				Report(EndEdit());
				FMaterialAsset Candidate = Draft;
				Candidate.ShaderPath.clear();
				Apply(std::move(Candidate), "Reset material shader");
				ShaderPath.fill('\0');
			}
		}

		ImGui::PopStyleVar(2);
		ImGui::EndDisabled();
	}

	ToolUI.EndPanel();
	if (!bOpen && IsDirty())
	{
		bOpen = true;
		bCloseRequested = true;
	}

	const bool bPending = PendingOpen.has_value() || bCloseRequested || bReloadRequested;
	if (bPending)
	{
		ImGui::OpenPopup("Unsaved material");
	}

	ImGui::SetNextWindowBgAlpha(0.f);
	if (ImGui::BeginPopupModal("Unsaved material", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
	{
		const ImVec2 Position = ImGui::GetWindowPos();
		const ImVec2 Size = ImGui::GetWindowSize();
		ToolUI.DrawGlassSurface(Position.x, Position.y, Size.x, Size.y, ToolUI.GetMetrics().PopupRounding * Scale);
		ImGui::Text("Save changes to %s?", Draft.Name.c_str());
		ImGui::TextDisabled("Unsaved material edits will otherwise be discarded.");
		ImGui::Spacing();
		const auto Proceed = [&](const EMaterialCloseChoice Choice)
		{
			if (const auto Result = ResolvePendingChanges(Choice, Context); !Result)
			{
				ShowError(Result.error().Message);
				return;
			}

			ImGui::CloseCurrentPopup();
		};

		ImGui::BeginDisabled(bReadOnly || !Context.Save);
		if (ImGui::Button("Save", {100.f * Scale, 0.f}))
		{
			Proceed(EMaterialCloseChoice::Save);
		}

		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Discard", {100.f * Scale, 0.f}))
		{
			Proceed(EMaterialCloseChoice::Discard);
		}

		ImGui::SameLine();
		if (ImGui::Button("Cancel", {100.f * Scale, 0.f}) || ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			Proceed(EMaterialCloseChoice::Cancel);
		}

		if (!Error.empty())
		{
			ImGui::TextWrapped("%s", Error.c_str());
		}

		ImGui::EndPopup();
	}

	const auto Now = std::chrono::steady_clock::now();
	if (bOpen && Context.Preview && (!LastPreview || *LastPreview != Draft) && Now >= NextPreview)
	{
		Context.Preview(Asset, Draft);
		LastPreview = Draft;
		NextPreview = Now + std::chrono::milliseconds(33);
	}
}
}
