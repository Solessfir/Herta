#include "Herta/AnimationGraph/AnimationGraph.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <map>
#include <set>
#include <optional>
#include <string>

namespace Herta
{
namespace
{
constexpr std::uint32_t StatePin = 1;

constexpr std::array StateOutputs{FGraphPinDescriptor{.Key = "State", .Name = "State", .Type = StatePin}};
constexpr std::array TransitionInputs{
    FGraphPinDescriptor{.Key = "From", .Name = "From", .Type = StatePin},
    FGraphPinDescriptor{.Key = "To", .Name = "To", .Type = StatePin},
};
constexpr std::array EntryInputs{FGraphPinDescriptor{.Key = "State", .Name = "State", .Type = StatePin}};

const std::array StateProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("State")},
    FGraphPropertyDescriptor{.Key = "clips", .Name = "Clips", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "blend", .Name = "Blend Parameter", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "loop", .Name = "Loop", .Default = true},
    FGraphPropertyDescriptor{.Key = "speed", .Name = "Speed", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "events", .Name = "Events", .Default = std::string()},
};
const std::array TransitionProperties{
    FGraphPropertyDescriptor{.Key = "priority", .Name = "Priority", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "duration", .Name = "Duration", .Default = 0.2f},
    FGraphPropertyDescriptor{.Key = "atEnd", .Name = "At End", .Default = false},
    FGraphPropertyDescriptor{.Key = "conditions", .Name = "Conditions", .Default = std::string()},
};
const std::array ParameterProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("Parameter")},
    FGraphPropertyDescriptor{.Key = "type", .Name = "Type", .Default = std::string("Float")},
    FGraphPropertyDescriptor{.Key = "default", .Name = "Default", .Default = 0.f},
};

const std::array AnimationNodes{
    FGraphNodeDescriptor{.Type = "State", .Name = "State", .Category = "States", .Outputs = StateOutputs, .Properties = StateProperties},
    FGraphNodeDescriptor{.Type = "Transition", .Name = "Transition", .Category = "States", .Inputs = TransitionInputs, .Properties = TransitionProperties},
    FGraphNodeDescriptor{.Type = "Entry", .Name = "Entry", .Category = "States", .Inputs = EntryInputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "Parameter", .Name = "Parameter", .Category = "Parameters", .Properties = ParameterProperties},
};

bool CanConnectAnimationPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To;
}

const FGraphSchema AnimationSchema{.Domain = AnimationGraphDomain, .Version = AnimationGraphVersion, .Nodes = AnimationNodes, .CanConnect = CanConnectAnimationPins};

[[nodiscard]] const FGraphValue& Property(const FGraphNode& Node, const std::string_view Key)
{
	return std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value;
}

[[nodiscard]] std::string_view Trim(std::string_view Text)
{
	while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t'))
	{
		Text.remove_prefix(1);
	}

	while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
	{
		Text.remove_suffix(1);
	}

	return Text;
}

[[nodiscard]] std::vector<std::string_view> SplitList(const std::string_view Text)
{
	std::vector<std::string_view> Items;
	std::size_t Start = 0;
	while (Start <= Text.size())
	{
		const std::size_t End = std::min(Text.find(',', Start), Text.size());
		if (const std::string_view Item = Trim(Text.substr(Start, End - Start)); !Item.empty())
		{
			Items.push_back(Item);
		}

		Start = End + 1;
	}

	return Items;
}

[[nodiscard]] std::optional<float> ParseNumber(const std::string_view Text)
{
	float Value = 0.f;
	const std::string_view Number = Trim(Text);
	const auto [End, Error] = std::from_chars(Number.data(), Number.data() + Number.size(), Value);
	return Error == std::errc{} && End == Number.data() + Number.size() && std::isfinite(Value) ? std::optional(Value) : std::nullopt;
}

// "Name" or "Name@Number", where the number defaults to zero.
[[nodiscard]] std::optional<std::pair<std::string, float>> ParseNamedNumber(const std::string_view Item)
{
	const std::size_t At = Item.find('@');
	const std::string_view Name = Trim(Item.substr(0, At));
	if (Name.empty())
	{
		return std::nullopt;
	}

	if (At == std::string_view::npos)
	{
		return std::pair{std::string(Name), 0.f};
	}

	const auto Number = ParseNumber(Item.substr(At + 1));
	return Number ? std::optional(std::pair{std::string(Name), *Number}) : std::nullopt;
}

[[nodiscard]] std::string FormatNumber(const float Value)
{
	return std::format("{}", Value);
}

[[nodiscard]] std::string ParameterTypeName(const EAnimationParameterType Type)
{
	switch (Type)
	{
		case EAnimationParameterType::Bool:
			return "Bool";
		case EAnimationParameterType::Trigger:
			return "Trigger";
		case EAnimationParameterType::Float:
			break;
	}

	return "Float";
}
}

const FGraphSchema& GetAnimationGraphSchema()
{
	return AnimationSchema;
}

std::expected<FAnimationGraphCompilation, std::vector<FGraphDiagnostic>> CompileAnimationGraph(const FGraph& Graph)
{
	std::vector<FGraphDiagnostic> Errors;
	for (FGraphDiagnostic& Diagnostic : ValidateGraph(Graph, AnimationSchema))
	{
		if (Diagnostic.Severity == EGraphSeverity::Error)
		{
			Errors.push_back(std::move(Diagnostic));
		}
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	const auto Fail = [&Errors](const FGraphNodeId Node, std::string Message, std::string Pin = {})
	{
		Errors.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Pin = std::move(Pin), .Message = std::move(Message)});
	};

	std::vector<const FGraphNode*> Nodes;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Nodes.push_back(&Node);
	}

	std::ranges::sort(Nodes, {}, [](const FGraphNode* Node)
	{
		return Node->Id;
	});

	const auto Source = [&Graph](const FGraphNodeId Node, const std::string_view Pin) -> std::optional<FGraphNodeId>
	{
		const auto Link = std::ranges::find_if(Graph.Links, [&](const FGraphLink& Candidate)
		{
			return Candidate.To.Node == Node && Candidate.To.Key == Pin;
		});

		return Link == Graph.Links.end() ? std::nullopt : std::optional(Link->From.Node);
	};

	FAnimationGraphCompilation Result;
	FAnimationStateMachineDesc& Machine = Result.StateMachine;
	for (const FGraphNode* Node : Nodes)
	{
		if (Node->Type != "Parameter")
		{
			continue;
		}

		const std::string& Name = std::get<std::string>(Property(*Node, "name"));
		const std::string& Type = std::get<std::string>(Property(*Node, "type"));
		const auto Kind = Type == "Float" ? std::optional(EAnimationParameterType::Float) : Type == "Bool" ? std::optional(EAnimationParameterType::Bool) : Type == "Trigger" ? std::optional(EAnimationParameterType::Trigger) : std::nullopt;
		if (Name.empty() || std::ranges::contains(Machine.Parameters, Name, &FAnimationParameterDesc::Name))
		{
			Fail(Node->Id, std::format("Parameter names must be unique and not empty ('{}')", Name));
		}

		if (!Kind)
		{
			Fail(Node->Id, std::format("Parameter type '{}' must be Float, Bool, or Trigger", Type));
		}

		Machine.Parameters.push_back({.Name = Name, .Type = Kind.value_or(EAnimationParameterType::Float), .Default = std::get<float>(Property(*Node, "default"))});
	}

	const auto FindParameter = [&Machine](const std::string_view Name) -> std::optional<std::size_t>
	{
		const auto Found = std::ranges::find(Machine.Parameters, Name, &FAnimationParameterDesc::Name);
		return Found == Machine.Parameters.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Machine.Parameters.begin()));
	};

	std::map<FGraphNodeId, std::size_t> StateIndices;
	for (const FGraphNode* Node : Nodes)
	{
		if (Node->Type != "State")
		{
			continue;
		}

		FAnimationStateDesc State{.Name = std::get<std::string>(Property(*Node, "name")), .bLoop = std::get<bool>(Property(*Node, "loop")), .Speed = std::get<float>(Property(*Node, "speed"))};
		if (State.Name.empty() || std::ranges::contains(Machine.States, State.Name, &FAnimationStateDesc::Name))
		{
			Fail(Node->Id, std::format("State names must be unique and not empty ('{}')", State.Name));
		}

		if (!std::isfinite(State.Speed) || State.Speed < 0.f)
		{
			Fail(Node->Id, "A state's speed must be zero or more");
		}

		for (const std::string_view Item : SplitList(std::get<std::string>(Property(*Node, "clips"))))
		{
			const auto Sample = ParseNamedNumber(Item);
			if (!Sample)
			{
				Fail(Node->Id, std::format("Clip '{}' must be a clip name, optionally followed by @ and its blend position", Item));
				continue;
			}

			State.Samples.push_back({.Clip = Sample->first, .Position = Sample->second});
		}

		if (State.Samples.empty())
		{
			Fail(Node->Id, std::format("State '{}' plays no clip", State.Name));
		}

		const std::string& Blend = std::get<std::string>(Property(*Node, "blend"));
		if (!Blend.empty())
		{
			State.BlendParameter = FindParameter(Blend);
			if (!State.BlendParameter || Machine.Parameters[*State.BlendParameter].Type != EAnimationParameterType::Float)
			{
				Fail(Node->Id, std::format("Blend parameter '{}' must be a float parameter", Blend));
			}
		}
		else if (State.Samples.size() > 1)
		{
			Fail(Node->Id, std::format("State '{}' blends several clips, so it needs a blend parameter", State.Name));
		}

		for (const std::string_view Item : SplitList(std::get<std::string>(Property(*Node, "events"))))
		{
			const auto Event = ParseNamedNumber(Item);
			if (!Event || Event->second < 0.f || Event->second > 1.f)
			{
				Fail(Node->Id, std::format("Event '{}' must be a name, optionally followed by @ and a phase from 0 to 1", Item));
				continue;
			}

			State.Events.push_back({.Name = Event->first, .Phase = Event->second});
		}

		StateIndices[Node->Id] = Machine.States.size();
		Result.StateNodes.push_back(Node->Id);
		Machine.States.push_back(std::move(State));
	}

	if (Machine.States.empty())
	{
		Fail({}, "An animation graph needs at least one state");
	}

	const auto Entry = std::ranges::find(Nodes, std::string_view("Entry"), [](const FGraphNode* Node)
	{
		return std::string_view(Node->Type);
	});

	const std::optional<FGraphNodeId> EntryState = Entry != Nodes.end() ? Source((*Entry)->Id, "State") : std::nullopt;
	if (!EntryState)
	{
		Fail(Entry != Nodes.end() ? (*Entry)->Id : FGraphNodeId{}, "Link the Entry node to the state the machine starts in", "State");
	}
	else
	{
		Machine.Entry = StateIndices.at(*EntryState);
	}

	std::vector<std::pair<std::int32_t, const FGraphNode*>> Transitions;
	for (const FGraphNode* Node : Nodes)
	{
		if (Node->Type == "Transition")
		{
			Transitions.emplace_back(std::get<std::int32_t>(Property(*Node, "priority")), Node);
		}
	}

	// Nodes are already in identity order, so a stable sort by priority keeps it among equal priorities.
	std::ranges::stable_sort(Transitions, {}, &std::pair<std::int32_t, const FGraphNode*>::first);
	for (const auto& [Priority, Node] : Transitions)
	{
		const std::optional<FGraphNodeId> From = Source(Node->Id, "From");
		const std::optional<FGraphNodeId> To = Source(Node->Id, "To");
		if (!To)
		{
			Fail(Node->Id, "Link the state this transition enters to its To pin", "To");
			continue;
		}

		FAnimationTransitionDesc Transition{.From = From ? StateIndices.at(*From) : AnyAnimationState, .To = StateIndices.at(*To), .DurationSeconds = std::get<float>(Property(*Node, "duration")), .bAtEnd = std::get<bool>(Property(*Node, "atEnd"))};
		if (!std::isfinite(Transition.DurationSeconds) || Transition.DurationSeconds < 0.f)
		{
			Fail(Node->Id, "A transition's duration must be zero or more");
		}

		for (const std::string_view Item : SplitList(std::get<std::string>(Property(*Node, "conditions"))))
		{
			const std::size_t Operator = Item.find_first_of("<>");
			const bool bNot = Item.starts_with('!');
			const std::string_view Name = Trim(bNot ? Item.substr(1) : Item.substr(0, Operator));
			const auto Parameter = FindParameter(Name);
			if (!Parameter)
			{
				Fail(Node->Id, std::format("Condition '{}' tests '{}', which is not a parameter", Item, Name));
				continue;
			}

			const EAnimationParameterType Type = Machine.Parameters[*Parameter].Type;
			FAnimationConditionDesc Condition{.Parameter = *Parameter};
			if (Operator != std::string_view::npos && !bNot)
			{
				const auto Value = ParseNumber(Item.substr(Operator + 1));
				if (!Value || Type != EAnimationParameterType::Float)
				{
					Fail(Node->Id, std::format("Condition '{}' compares a float parameter with a number", Item));
					continue;
				}

				Condition.Test = Item[Operator] == '>' ? EAnimationCondition::Greater : EAnimationCondition::Less;
				Condition.Value = *Value;
			}
			else if (bNot ? Type != EAnimationParameterType::Bool : Type == EAnimationParameterType::Float)
			{
				Fail(Node->Id, std::format("Condition '{}' needs a {}", Item, bNot ? "bool parameter" : "bool or trigger parameter, or a comparison"));
				continue;
			}
			else
			{
				Condition.Test = bNot ? EAnimationCondition::IsFalse : Type == EAnimationParameterType::Trigger ? EAnimationCondition::Triggered : EAnimationCondition::IsTrue;
			}

			Transition.Conditions.push_back(Condition);
		}

		Result.TransitionNodes.push_back(Node->Id);
		Machine.Transitions.push_back(std::move(Transition));
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	return Result;
}

FGraph MakeAnimationGraph(const FAnimationStateMachineDesc& StateMachine)
{
	FGraph Graph{.Domain = std::string(AnimationGraphDomain), .DomainVersion = AnimationGraphVersion};
	std::uint32_t NextId = 1;
	const auto Add = [&](const std::string_view Type, const std::array<float, 2> Position)
	{
		Graph.Nodes.push_back(*MakeGraphNode(AnimationSchema, Type, FGraphNodeId{NextId++}, Position));
		return &Graph.Nodes.back();
	};

	const auto Set = [](FGraphNode& Node, const std::string_view Key, FGraphValue Value)
	{
		std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value = std::move(Value);
	};

	for (std::size_t Index = 0; Index < StateMachine.Parameters.size(); ++Index)
	{
		const FAnimationParameterDesc& Parameter = StateMachine.Parameters[Index];
		FGraphNode& Node = *Add("Parameter", {-640.f, 120.f * static_cast<float>(Index)});
		Set(Node, "name", Parameter.Name);
		Set(Node, "type", ParameterTypeName(Parameter.Type));
		Set(Node, "default", Parameter.Default);
	}

	const FGraphNodeId Entry = Add("Entry", {-320.f, -200.f})->Id;
	// States sit on a grid four wide; their nodes keep their order, which is the compiled order.
	constexpr float ColumnWidth = 560.f;
	constexpr float RowHeight = 400.f;
	std::vector<FGraphNodeId> States;
	std::vector<std::array<float, 2>> Positions;
	for (std::size_t Index = 0; Index < StateMachine.States.size(); ++Index)
	{
		const FAnimationStateDesc& State = StateMachine.States[Index];
		Positions.push_back({ColumnWidth * static_cast<float>(Index % 4), RowHeight * static_cast<float>(Index / 4)});
		FGraphNode& Node = *Add("State", Positions.back());
		std::string Clips;
		for (const FAnimationBlendSample& Sample : State.Samples)
		{
			Clips += (Clips.empty() ? "" : ", ") + Sample.Clip + (State.Samples.size() > 1 || Sample.Position != 0.f ? "@" + FormatNumber(Sample.Position) : "");
		}

		std::string Events;
		for (const FAnimationEventDesc& Event : State.Events)
		{
			Events += (Events.empty() ? "" : ", ") + Event.Name + "@" + FormatNumber(Event.Phase);
		}

		Set(Node, "name", State.Name);
		Set(Node, "clips", Clips);
		Set(Node, "blend", State.BlendParameter ? StateMachine.Parameters[*State.BlendParameter].Name : std::string());
		Set(Node, "loop", State.bLoop);
		Set(Node, "speed", State.Speed);
		Set(Node, "events", Events);
		States.push_back(Node.Id);
	}

	if (StateMachine.Entry < States.size())
	{
		Graph.Links.push_back({.From = {.Node = States[StateMachine.Entry], .Key = "State"}, .To = {.Node = Entry, .Key = "State"}});
	}

	// Transitions sit between their states, any-state ones above and left of their target, each moved down into the first free cell so none
	// covers another node.
	constexpr float CellWidth = 240.f;
	constexpr float CellHeight = 100.f;
	std::set<std::pair<int, int>> Occupied;
	const auto Cell = [&](const std::array<float, 2> Position)
	{
		return std::pair{static_cast<int>(std::floor(Position[0] / CellWidth)), static_cast<int>(std::floor(Position[1] / CellHeight))};
	};

	for (const std::array<float, 2>& Position : Positions)
	{
		Occupied.insert(Cell(Position));
	}

	for (std::size_t Index = 0; Index < StateMachine.Transitions.size(); ++Index)
	{
		const FAnimationTransitionDesc& Transition = StateMachine.Transitions[Index];
		const bool bAny = Transition.From == AnyAnimationState || Transition.From >= States.size();
		if (Transition.To >= States.size())
		{
			continue;
		}

		const std::array<float, 2> To = Positions[Transition.To];
		const std::array<float, 2> From = bAny ? std::array{To[0] - 280.f, To[1] - 220.f} : Positions[Transition.From];
		std::array<float, 2> Position{std::round((From[0] + To[0]) * 0.5f / CellWidth) * CellWidth + 20.f, std::round((From[1] + To[1]) * 0.5f / CellHeight) * CellHeight + 20.f};
		while (Occupied.contains(Cell(Position)))
		{
			Position[1] += CellHeight;
		}

		Occupied.insert(Cell(Position));
		FGraphNode& Node = *Add("Transition", Position);
		std::string Conditions;
		for (const FAnimationConditionDesc& Condition : Transition.Conditions)
		{
			const std::string& Name = StateMachine.Parameters[Condition.Parameter].Name;
			const std::string Text = Condition.Test == EAnimationCondition::Greater ? Name + " > " + FormatNumber(Condition.Value)
			                         : Condition.Test == EAnimationCondition::Less  ? Name + " < " + FormatNumber(Condition.Value)
			                         : Condition.Test == EAnimationCondition::IsFalse ? "!" + Name
			                                                                          : Name;
			Conditions += (Conditions.empty() ? "" : ", ") + Text;
		}

		Set(Node, "priority", static_cast<std::int32_t>(Index));
		Set(Node, "duration", Transition.DurationSeconds);
		Set(Node, "atEnd", Transition.bAtEnd);
		Set(Node, "conditions", Conditions);
		if (!bAny)
		{
			Graph.Links.push_back({.From = {.Node = States[Transition.From], .Key = "State"}, .To = {.Node = Node.Id, .Key = "From"}});
		}

		Graph.Links.push_back({.From = {.Node = States[Transition.To], .Key = "State"}, .To = {.Node = Node.Id, .Key = "To"}});
	}

	return Graph;
}
}
