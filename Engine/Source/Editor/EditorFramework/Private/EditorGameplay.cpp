#include "EditorFrameworkPrivate.h"
#include "Herta/GameplayGraph/GameplayGraph.h"

#include <fstream>
#include <iterator>

namespace Herta
{
void FEditorFramework::FImplementation::DrawGameplayGraphPanel()
{
	if (!GameplayGraphPanel.IsOpen())
	{
		return;
	}

	// Nodes Play ran in the last second light up, so a wire that fires is easy to follow.
	GameplayGraphActive.clear();
	if (PlaySession && Assets)
	{
		const auto Options = Assets->GetOptions();
		const auto Found = std::ranges::find(Options, GameplayGraphPanel.GetPath(), &FPreviewAssetOption::Label);
		if (Found != Options.end())
		{
			for (const FGameGraphNodeActivity& Activity : PlaySession->GetGameplayGraphActivity(Found->Id))
			{
				if (Activity.SecondsAgo < 1.f)
				{
					GameplayGraphActive.push_back(FGraphNodeId{Activity.Node});
				}
			}
		}
	}

	if (PendingFocusPanel == "Gameplay Graph")
	{
		PendingFocusPanel.clear();
		ImGui::SetNextWindowFocus();
	}

	std::string_view Status = PlaySession ? "Nodes Play ran in the last second light up" : "Entities run the graph as saved";
	if (GameplayGraphPanel.IsDirty())
	{
		Status = "Saving recooks the graph; Play swaps it in and starts its entities over";
	}

	GameplayGraphPanel.Draw(*ToolUI, {.Status = Status, .Active = GameplayGraphActive});
}

void FEditorFramework::FImplementation::OpenGameplayGraphKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetGameplayGraphSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open the gameplay graph {}: {}", Key, Graph.error().Message);
		return;
	}

	if (GameplayGraphPanel.IsOpen() && GameplayGraphPanel.GetPath() != Key && GameplayGraphPanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another gameplay graph", GameplayGraphPanel.GetPath());
		return;
	}

	GameplayGraphPanel.Open(Key, File, std::move(*Graph), bEngine);
}
}
