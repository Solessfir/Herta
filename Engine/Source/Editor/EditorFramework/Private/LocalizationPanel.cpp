#include "EditorFrameworkPrivate.h"
#include "Herta/Assets/VirtualFileSystem.h"
#include "Herta/LocalizationPipeline/LocalizationPipeline.h"
#include "Herta/Platform/FileSystem.h"
#include "Herta/ToolUI/EditorText.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>

namespace Herta
{
namespace
{
namespace Editor = Text::Editor;

constexpr auto CatalogPollInterval = std::chrono::seconds(1);

[[nodiscard]] const FCatalogMessage* FindMessage(const FTextCatalog& Catalog, const std::string_view Key)
{
	const auto Found = std::ranges::find(Catalog.Messages, Key, &FCatalogMessage::Key);
	return Found != Catalog.Messages.end() ? &*Found : nullptr;
}

// Why Text cannot stand in for Source, or nothing when it can.
[[nodiscard]] std::optional<std::string> CheckTranslation(const std::string_view Source, const std::string_view Text)
{
	const auto Compiled = FMessage::Compile(Text);
	if (!Compiled)
	{
		return Compiled.error().Message;
	}

	const auto SourceCompiled = FMessage::Compile(Source);
	if (SourceCompiled && SourceCompiled->GetArgumentNames() != Compiled->GetArgumentNames())
	{
		std::string Names;
		for (const std::string& Name : SourceCompiled->GetArgumentNames())
		{
			Names += (Names.empty() ? "{" : ", {") + Name + "}";
		}

		return std::format("arguments must be {}", Names.empty() ? "none" : Names);
	}

	return std::nullopt;
}

[[nodiscard]] bool ContainsInsensitive(const std::string_view Text, const std::string_view Search)
{
	return std::ranges::search(Text, Search, [](const char Left, const char Right)
	{
		return std::tolower(static_cast<unsigned char>(Left)) == std::tolower(static_cast<unsigned char>(Right));
	}).begin() != Text.end()
	       || Search.empty();
}
}

std::filesystem::path FEditorFramework::FImplementation::GetCatalogRoot(const std::string_view Namespace) const
{
	return EngineRoot / (Namespace == "Editor" ? "Engine/Content/Editor/Localization" : "Engine/Content/Localization");
}

void FEditorFramework::FImplementation::PollTextCatalogs(const bool bForce)
{
	const auto Now = std::chrono::steady_clock::now();
	if (!bForce && Now < NextCatalogPoll)
	{
		return;
	}

	NextCatalogPoll = Now + CatalogPollInterval;
	// The newest write time and the file count change on any save, add, or delete, which is all a reload needs to notice.
	std::filesystem::file_time_type Newest{};
	std::size_t Count = 0;
	for (const std::string_view Namespace : {"Game", "Editor"})
	{
		std::error_code Error;
		for (auto Iterator = std::filesystem::recursive_directory_iterator(GetCatalogRoot(Namespace), Error); !Error && Iterator != std::filesystem::recursive_directory_iterator(); Iterator.increment(Error))
		{
			if (Iterator->path().extension() == ".hloc")
			{
				Newest = std::max(Newest, Iterator->last_write_time(Error));
				++Count;
			}
		}
	}

	if (!bForce && Newest == CatalogStamp && Count == CatalogCount)
	{
		return;
	}

	CatalogStamp = Newest;
	CatalogCount = Count;
	std::vector<FTextCatalog> Catalogs;
	for (const std::string_view Namespace : {"Game", "Editor"})
	{
		FVirtualFileSystem Files(GetCatalogRoot(Namespace));
		Files.MountDirectory(Files.GetRoot());
		auto Loaded = LoadTextCatalogs(Files, {});
		if (!Loaded)
		{
			HERTA_LOG_WARNING(*Log, EditorLog, "Kept the previous text: {}", Loaded.error().Message);
			return;
		}

		std::ranges::move(*Loaded, std::back_inserter(Catalogs));
	}

	// A broken catalog keeps the text that last loaded, so a translator's typo never blanks the editor.
	if (auto Applied = Localization.SetCatalogs(Catalogs); !Applied)
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Kept the previous text: {}", Applied.error().Message);
		return;
	}

	if (!TextCatalogs.empty())
	{
		HERTA_LOG_INFO(*Log, EditorLog, "{}", Localization.Format(Editor::Localization_Reloaded, std::array{FMessageArgument{"count", static_cast<std::int64_t>(Catalogs.size())}}));
	}

	TextCatalogs = std::move(Catalogs);
}

void FEditorFramework::FImplementation::DrawLocalizationPanel()
{
	if (!bLocalizationOpen)
	{
		return;
	}

	if (!ToolUI->BeginPanel("Localization", &bLocalizationOpen))
	{
		ToolUI->EndPanel();
		return;
	}

	FLocalizationPanelState& State = LocalizationState;
	std::set<std::string> Cultures;
	for (const FTextCatalog& Catalog : TextCatalogs)
	{
		if (Catalog.Culture != SourceCulture)
		{
			Cultures.insert(Catalog.Culture);
		}
	}

	if (Cultures.empty())
	{
		ToolUI->EndPanel();
		return;
	}

	if (!Cultures.contains(State.Culture))
	{
		State.Culture = *Cultures.begin();
		State.Drafts.clear();
	}

	const float Scale = ImGui::GetFontSize() / ToolUI->GetMetrics().BaseFontSize;
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(Localization.Format(Editor::Localization_Culture).c_str());
	ImGui::SameLine();
	ImGui::SetNextItemWidth(160.f * Scale);
	if (ImGui::BeginCombo("##LocalizationCulture", Localization.FormatIn(State.Culture, Editor::Language_Name).c_str()))
	{
		for (const std::string& Culture : Cultures)
		{
			if (ImGui::Selectable(Localization.FormatIn(Culture, Editor::Language_Name).c_str(), Culture == State.Culture) && Culture != State.Culture)
			{
				State.Culture = Culture;
				State.Drafts.clear();
			}
		}

		ImGui::EndCombo();
	}

	ImGui::SameLine();
	const std::string SearchHint = Localization.Format(Editor::Localization_Search);
	ToolUIInputText("##LocalizationSearch", State.Search, SearchHint.c_str(), 260.f * Scale);
	ImGui::SameLine();
	ImGui::Checkbox(Localization.Format(Editor::Localization_MissingOnly).c_str(), &State.bMissingOnly);

	// A draft replaces its catalog until saved; the rest of the table reads the catalogs as loaded.
	const auto FindTranslation = [&](const std::string& Namespace) -> const FTextCatalog*
	{
		const auto Draft = std::ranges::find_if(State.Drafts, [&](const FTextCatalog& Catalog)
		{
			return Catalog.Namespace == Namespace;
		});

		if (Draft != State.Drafts.end())
		{
			return &*Draft;
		}

		const auto Loaded = std::ranges::find_if(TextCatalogs, [&](const FTextCatalog& Catalog)
		{
			return Catalog.Culture == State.Culture && Catalog.Namespace == Namespace;
		});

		return Loaded != TextCatalogs.end() ? &*Loaded : nullptr;
	};

	std::int64_t Missing = 0;
	std::int64_t Errors = 0;
	for (const FTextCatalog& Source : TextCatalogs)
	{
		if (Source.Culture != SourceCulture)
		{
			continue;
		}

		const FTextCatalog* const Translation = FindTranslation(Source.Namespace);
		for (const FCatalogMessage& Message : Source.Messages)
		{
			const FCatalogMessage* const Translated = Translation ? FindMessage(*Translation, Message.Key) : nullptr;
			Missing += Translated ? 0 : 1;
			Errors += Translated && CheckTranslation(Message.Text, Translated->Text) ? 1 : 0;
		}
	}

	const std::array SummaryArguments{FMessageArgument{"missing", Missing}, FMessageArgument{"errors", Errors}};
	ImGui::TextColored(Errors > 0 ? ImVec4{0.95f, 0.45f, 0.4f, 1.f} : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", Localization.Format(Editor::Localization_Summary, SummaryArguments).c_str());
	ImGui::SameLine();
	ImGui::TextDisabled("%s", State.Drafts.empty() ? State.Status.c_str() : Localization.Format(Editor::Localization_Unsaved).c_str());
	ImGui::SameLine(ImGui::GetContentRegionMax().x - 190.f * Scale);
	ImGui::BeginDisabled(State.Drafts.empty());
	if (ToolUIButton(Localization.Format(Editor::Localization_Save), EToolUIMenuIcon::Save))
	{
		for (const FTextCatalog& Draft : State.Drafts)
		{
			const std::filesystem::path Path = GetCatalogRoot(Draft.Namespace) / Draft.Culture / (Draft.Namespace + ".hloc");
			std::filesystem::path Temporary = Path;
			Temporary += ".tmp";
			{
				std::ofstream File(Temporary, std::ios::binary | std::ios::trunc);
				File << SerializeTextCatalog(Draft);
			}

			if (auto Moved = MoveAtomically(Temporary, Path, EMoveMode::ReplaceExisting); !Moved)
			{
				HERTA_LOG_WARNING(*Log, EditorLog, "Could not save {}: {}", Path.generic_string(), Moved.error().message());
				continue;
			}

			State.Status = Localization.Format(Editor::Localization_Saved, std::array{FMessageArgument{"file", Path.filename().generic_string()}});
		}

		State.Drafts.clear();
		PollTextCatalogs(true);
	}

	ImGui::SameLine();
	if (ToolUIButton(Localization.Format(Editor::Localization_Revert), EToolUIMenuIcon::Undo))
	{
		State.Drafts.clear();
	}

	ImGui::EndDisabled();

	const FCulture* const Culture = FCulture::Find(State.Culture);
	constexpr ImGuiTableFlags TableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
	if (Culture && ImGui::BeginTable("##LocalizationMessages", 4, TableFlags))
	{
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(Localization.Format(Editor::Localization_Key).c_str(), ImGuiTableColumnFlags_WidthStretch, 0.8f);
		ImGui::TableSetupColumn(Localization.Format(Editor::Localization_Source).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.2f);
		ImGui::TableSetupColumn(Localization.Format(Editor::Localization_Translation).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn(Localization.Format(Editor::Localization_Preview).c_str(), ImGuiTableColumnFlags_WidthStretch, 1.f);
		ImGui::TableHeadersRow();
		int Row = 0;
		for (const FTextCatalog& Source : TextCatalogs)
		{
			if (Source.Culture != SourceCulture)
			{
				continue;
			}

			for (const FCatalogMessage& Message : Source.Messages)
			{
				const FTextCatalog* const Translation = FindTranslation(Source.Namespace);
				const FCatalogMessage* const Translated = Translation ? FindMessage(*Translation, Message.Key) : nullptr;
				const std::string FullKey = Source.Namespace + "." + Message.Key;
				if ((State.bMissingOnly && Translated) || !(ContainsInsensitive(FullKey, State.Search) || ContainsInsensitive(Message.Text, State.Search) || (Translated && ContainsInsensitive(Translated->Text, State.Search))))
				{
					continue;
				}

				ImGui::PushID(Row++);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(FullKey.c_str());
				if (!Message.Comment.empty())
				{
					ImGui::SetItemTooltip("%s", Message.Comment.c_str());
				}

				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", Message.Text.c_str());
				ImGui::TableNextColumn();
				std::string Edited = Translated ? Translated->Text : std::string{};
				if (ToolUIInputText("##Translation", Edited, Message.Text.c_str()))
				{
					auto Draft = std::ranges::find_if(State.Drafts, [&](const FTextCatalog& Catalog)
					{
						return Catalog.Namespace == Source.Namespace;
					});

					if (Draft == State.Drafts.end())
					{
						State.Drafts.push_back(Translation ? *Translation : FTextCatalog{.Culture = State.Culture, .Namespace = Source.Namespace});
						Draft = std::prev(State.Drafts.end());
					}

					// An emptied field means untranslated, which falls back to the source, rather than a blank message.
					std::erase_if(Draft->Messages, [&](const FCatalogMessage& Existing)
					{
						return Existing.Key == Message.Key;
					});

					if (!Edited.empty())
					{
						Draft->Messages.push_back({.Key = Message.Key, .Text = Edited});
					}
				}

				const std::optional<std::string> Problem = Edited.empty() ? std::nullopt : CheckTranslation(Message.Text, Edited);
				if (Problem)
				{
					ImGui::PushTextWrapPos(0.f);
					ImGui::TextColored({0.95f, 0.45f, 0.4f, 1.f}, "%s", Localization.Format(Editor::Localization_Invalid, std::array{FMessageArgument{"error", *Problem}}).c_str());
					ImGui::PopTextWrapPos();
				}

				ImGui::TableNextColumn();
				if (const auto Compiled = FMessage::Compile(Edited.empty() ? Message.Text : Edited); Compiled && !Problem)
				{
					// Three picks the few form where a culture has one, which shows plural branches a translator is likely to miss.
					const std::vector<std::string> Names = Compiled->GetArgumentNames();
					std::vector<FMessageArgument> Samples;
					for (const std::string& Name : Names)
					{
						Samples.push_back({.Name = Name, .Value = std::int64_t{3}});
					}

					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(Edited.empty() ? ImGuiCol_TextDisabled : ImGuiCol_Text));
					ImGui::TextWrapped("%s", Compiled->Format(*Culture, Samples).c_str());
					ImGui::PopStyleColor();
				}

				ImGui::PopID();
			}
		}

		ImGui::EndTable();
	}

	ToolUI->EndPanel();
}
}
