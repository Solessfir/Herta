#include "EditorFrameworkPrivate.h"

#include "Herta/StateTreeCompiler/StateTreeCompiler.h"

namespace Herta
{
void FEditorFramework::FImplementation::DrawStateTreePanel()
{
	if (!StateTreePanel.IsOpen())
	{
		return;
	}

	if (StateTreeRevision != StateTreePanel.GetDraftRevision())
	{
		StateTreeRevision = StateTreePanel.GetDraftRevision();
		auto Compiled = CompileStateTree(StateTreePanel.GetDraft());
		StateTreeOutline = Compiled ? std::optional(std::move(Compiled->Tree)) : std::nullopt;
	}

	// Play's agents that run the tree this panel shows, and the active states of the one being followed.
	FAssetId Tree;
	if (Assets)
	{
		const auto Options = Assets->GetOptions();
		const auto Found = std::ranges::find(Options, StateTreePanel.GetPath(), &FPreviewAssetOption::Label);
		Tree = Found != Options.end() ? Found->Id : FAssetId{};
	}

	std::vector<std::pair<std::size_t, std::string>> Agents;
	StateTreeActive.clear();
	std::string Leaf;
	if (PlaySession && Tree.IsValid())
	{
		const std::vector<FGameAgentState> States = PlaySession->GetAgentStates();
		for (std::size_t Index = 0; Index < States.size(); ++Index)
		{
			const auto Debug = PlaySession->GetAgentDebug(Index);
			if (Debug && Debug->Tree == Tree)
			{
				Agents.emplace_back(Index, States[Index].Name);
			}
		}

		const auto Followed = std::ranges::find(Agents, StateTreeAgent, &std::pair<std::size_t, std::string>::first);
		if (Followed == Agents.end() && !Agents.empty())
		{
			StateTreeAgent = Agents.front().first;
		}

		if (const auto Debug = Agents.empty() ? std::nullopt : PlaySession->GetAgentDebug(StateTreeAgent))
		{
			for (const std::uint16_t State : Debug->Active)
			{
				StateTreeActive.push_back(FGraphNodeId{Debug->Program->States[State].Node});
			}

			Leaf = Debug->Active.empty() ? std::string() : Debug->Program->States[Debug->Active.back()].Name;
		}
	}

	const std::string Status = !Agents.empty() && !Leaf.empty() ? std::format("Play's {} is in {}", std::ranges::find(Agents, StateTreeAgent, &std::pair<std::size_t, std::string>::first)->second, Leaf)
	                           : StateTreePanel.IsDirty()       ? std::string("Saving recooks the tree; Play's agents run it from their next start")
	                                                            : std::string("Agents run the tree as saved");
	// A document tab comes to the front when its window asks for focus as it begins.
	if (PendingFocusPanel == "StateTree")
	{
		PendingFocusPanel.clear();
		ImGui::SetNextWindowFocus();
	}

	StateTreePanel.Draw(*ToolUI, {.Status = Status, .Active = StateTreeActive, .DrawSidebar = [&]
	{
		if (!Agents.empty())
		{
			ImGui::TextDisabled("Following");
			ImGui::SetNextItemWidth(-FLT_MIN);
			const auto Current = std::ranges::find(Agents, StateTreeAgent, &std::pair<std::size_t, std::string>::first);
			if (ImGui::BeginCombo("##Agent", Current != Agents.end() ? Current->second.c_str() : ""))
			{
				for (const auto& [Index, Name] : Agents)
				{
					if (ImGui::Selectable(Name.c_str(), Index == StateTreeAgent))
					{
						StateTreeAgent = Index;
					}
				}

				ImGui::EndCombo();
			}
		}

		ImGui::TextDisabled("Tree");
		if (!StateTreeOutline)
		{
			ImGui::TextDisabled("Fix the graph to see its tree");
			return;
		}

		// The tree as agents run it, in sibling order; Play's active states stand out.
		const float Indent = ImGui::GetStyle().IndentSpacing * 0.6f;
		for (std::size_t Index = 0; Index < StateTreeOutline->States.size(); ++Index)
		{
			const FStateTreeStateDesc& State = StateTreeOutline->States[Index];
			const bool bActive = std::ranges::contains(StateTreeActive, FGraphNodeId{State.Node});
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + Indent * static_cast<float>(State.Depth));
			if (bActive)
			{
				// The green the canvas outlines live nodes with.
				ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(77, 177, 122, 255));
			}

			const std::string Label = State.TaskCount > 0 ? std::format("{} ({} task{})", State.Name, State.TaskCount, State.TaskCount == 1 ? "" : "s") : State.Name;
			ImGui::TextUnformatted(Label.c_str());
			if (bActive)
			{
				ImGui::PopStyleColor();
			}

			if (State.TransitionCount > 0 && ImGui::IsItemHovered())
			{
				std::string Tip;
				for (std::uint16_t Transition = State.FirstTransition; Transition < State.FirstTransition + State.TransitionCount; ++Transition)
				{
					const FStateTreeTransitionDesc& Desc = StateTreeOutline->Transitions[Transition];
					constexpr std::array<std::string_view, 5> Triggers{"When done", "When succeeded", "When failed", "Every think", "On event"};
					const std::string Event = Desc.Trigger == EStateTreeTrigger::OnEvent ? " " + StateTreeOutline->Names[Desc.Event] : std::string();
					const std::string Target = Desc.Kind == EStateTreeTransitionKind::GotoState ? StateTreeOutline->States[Desc.Target].Name : Desc.Kind == EStateTreeTransitionKind::Succeed ? "succeed the parent" : "fail the parent";
					Tip += std::format("{}{}{}: {}{}", Tip.empty() ? "" : "\n", Triggers[static_cast<std::size_t>(Desc.Trigger)], Event, Target, Desc.ConditionCount > 0 ? " (if its conditions hold)" : "");
				}

				ImGui::SetTooltip("%s", Tip.c_str());
			}
		}
	}});
}

void FEditorFramework::FImplementation::OpenStateTreeKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetStateTreeSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open the StateTree {}: {}", Key, Graph.error().Message);
		return;
	}

	if (StateTreePanel.IsOpen() && StateTreePanel.GetPath() != Key && StateTreePanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another StateTree", StateTreePanel.GetPath());
		return;
	}

	StateTreePanel.Open(Key, File, std::move(*Graph), bEngine);
	StateTreeRevision = UINT64_MAX;
}
}
