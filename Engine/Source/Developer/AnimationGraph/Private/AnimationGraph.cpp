#include "Herta/AnimationGraph/AnimationGraph.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace Herta
{
namespace
{
constexpr std::uint32_t StatePin = 1;
constexpr std::uint32_t PosePin = 2;

constexpr std::array StateInputs{FGraphPinDescriptor{.Key = "Pose", .Name = "Pose", .Type = PosePin}};
constexpr std::array StateOutputs{FGraphPinDescriptor{.Key = "State", .Name = "State", .Type = StatePin}};
constexpr std::array TransitionInputs{
    FGraphPinDescriptor{.Key = "From", .Name = "From", .Type = StatePin},
    FGraphPinDescriptor{.Key = "To", .Name = "To", .Type = StatePin},
};
constexpr std::array EntryInputs{FGraphPinDescriptor{.Key = "State", .Name = "State", .Type = StatePin}};
constexpr std::array PoseOutputs{FGraphPinDescriptor{.Key = "Pose", .Name = "Pose", .Type = PosePin}};
// Blends take up to eight poses, matched in order to their positions or points.
constexpr std::array BlendInputs{
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = PosePin},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = PosePin},
    FGraphPinDescriptor{.Key = "C", .Name = "C", .Type = PosePin},
    FGraphPinDescriptor{.Key = "D", .Name = "D", .Type = PosePin},
    FGraphPinDescriptor{.Key = "E", .Name = "E", .Type = PosePin},
    FGraphPinDescriptor{.Key = "F", .Name = "F", .Type = PosePin},
    FGraphPinDescriptor{.Key = "G", .Name = "G", .Type = PosePin},
    FGraphPinDescriptor{.Key = "H", .Name = "H", .Type = PosePin},
};
constexpr std::array AdditiveInputs{
    FGraphPinDescriptor{.Key = "Base", .Name = "Base", .Type = PosePin},
    FGraphPinDescriptor{.Key = "Additive", .Name = "Additive", .Type = PosePin},
};
constexpr std::array LayeredInputs{
    FGraphPinDescriptor{.Key = "Base", .Name = "Base", .Type = PosePin},
    FGraphPinDescriptor{.Key = "Layer", .Name = "Layer", .Type = PosePin},
};

const std::array StateProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("State")},
    FGraphPropertyDescriptor{.Key = "clips", .Name = "Clips", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "blend", .Name = "Blend Parameter", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "loop", .Name = "Loop", .Default = true},
    FGraphPropertyDescriptor{.Key = "speed", .Name = "Speed", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "events", .Name = "Events", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "reverse", .Name = "Reverse", .Default = false},
    FGraphPropertyDescriptor{.Key = "upperBody", .Name = "Upper Body", .Default = false},
    FGraphPropertyDescriptor{.Key = "machine", .Name = "Machine", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "plays", .Name = "Plays Machine", .Default = std::string()},
};
const std::array TransitionProperties{
    FGraphPropertyDescriptor{.Key = "priority", .Name = "Priority", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "duration", .Name = "Duration", .Default = 0.2f},
    FGraphPropertyDescriptor{.Key = "atEnd", .Name = "At End", .Default = false},
    FGraphPropertyDescriptor{.Key = "conditions", .Name = "Conditions", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "machine", .Name = "Machine", .Default = std::string()},
};
const std::array EntryProperties{
    FGraphPropertyDescriptor{.Key = "machine", .Name = "Machine", .Default = std::string()},
};
const std::array ParameterProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("Parameter")},
    FGraphPropertyDescriptor{.Key = "type", .Name = "Type", .Default = std::string("Float")},
    FGraphPropertyDescriptor{.Key = "default", .Name = "Default", .Default = 0.f},
};
const std::array ClipProperties{
    FGraphPropertyDescriptor{.Key = "clip", .Name = "Clip", .Default = std::string()},
};
const std::array Blend1DProperties{
    FGraphPropertyDescriptor{.Key = "parameter", .Name = "Parameter", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "positions", .Name = "Positions", .Default = std::string()},
};
const std::array Blend2DProperties{
    FGraphPropertyDescriptor{.Key = "parameterX", .Name = "Parameter X", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "parameterY", .Name = "Parameter Y", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "points", .Name = "Points", .Default = std::string()},
};
const std::array AdditiveProperties{
    FGraphPropertyDescriptor{.Key = "weight", .Name = "Weight", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "parameter", .Name = "Weight Parameter", .Default = std::string()},
};
const std::array LayeredProperties{
    FGraphPropertyDescriptor{.Key = "joint", .Name = "Mask Joint", .Default = std::string("spine_01")},
    FGraphPropertyDescriptor{.Key = "weight", .Name = "Weight", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "parameter", .Name = "Weight Parameter", .Default = std::string()},
};
const std::array PoseRotationProperties{
    FGraphPropertyDescriptor{.Key = "joint", .Name = "Joint", .Default = std::string("spine_01")},
    FGraphPropertyDescriptor{.Key = "axis", .Name = "Axis", .Default = std::array{1.f, 0.f, 0.f, 0.f}},
    FGraphPropertyDescriptor{.Key = "parameter", .Name = "Parameter", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "scale", .Name = "Radians", .Default = 1.f},
};

const std::array AnimationNodes{
    FGraphNodeDescriptor{.Type = "State", .Name = "State", .Category = "States", .Inputs = StateInputs, .Outputs = StateOutputs, .Properties = StateProperties},
    FGraphNodeDescriptor{.Type = "Transition", .Name = "Transition", .Category = "States", .Inputs = TransitionInputs, .Properties = TransitionProperties},
    FGraphNodeDescriptor{.Type = "Entry", .Name = "Entry", .Category = "States", .Inputs = EntryInputs, .Properties = EntryProperties},
    FGraphNodeDescriptor{.Type = "Parameter", .Name = "Parameter", .Category = "Parameters", .Properties = ParameterProperties},
    FGraphNodeDescriptor{.Type = "Clip", .Name = "Clip", .Category = "Blend", .Outputs = PoseOutputs, .Properties = ClipProperties},
    FGraphNodeDescriptor{.Type = "Blend1D", .Name = "Blend 1D", .Category = "Blend", .Inputs = BlendInputs, .Outputs = PoseOutputs, .Properties = Blend1DProperties},
    FGraphNodeDescriptor{.Type = "Blend2D", .Name = "Blend 2D", .Category = "Blend", .Inputs = BlendInputs, .Outputs = PoseOutputs, .Properties = Blend2DProperties},
    FGraphNodeDescriptor{.Type = "Additive", .Name = "Additive", .Category = "Blend", .Inputs = AdditiveInputs, .Outputs = PoseOutputs, .Properties = AdditiveProperties},
    FGraphNodeDescriptor{.Type = "Layered", .Name = "Layered", .Category = "Blend", .Inputs = LayeredInputs, .Outputs = PoseOutputs, .Properties = LayeredProperties},
    FGraphNodeDescriptor{.Type = "PoseRotation", .Name = "Pose Rotation", .Category = "Pose", .Properties = PoseRotationProperties},
};

bool CanConnectAnimationPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To;
}

// Version 2 added blend trees, nested machines, and pose rotations as new node types and properties, so version 1 graphs need nothing more.
std::expected<void, FGraphError> UpgradeAnimationGraph(FGraph&, const std::uint32_t FromVersion)
{
	return FromVersion >= 1 ? std::expected<void, FGraphError>() : std::unexpected(FGraphError{"Animation graphs start at version 1"});
}

const FGraphSchema AnimationSchema{.Domain = AnimationGraphDomain, .Version = AnimationGraphVersion, .Nodes = AnimationNodes, .CanConnect = CanConnectAnimationPins, .Upgrade = UpgradeAnimationGraph};

[[nodiscard]] const FGraphValue& Property(const FGraphNode& Node, const std::string_view Key)
{
	return std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value;
}

[[nodiscard]] const std::string& Text(const FGraphNode& Node, const std::string_view Key)
{
	return std::get<std::string>(Property(Node, Key));
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

[[nodiscard]] std::vector<std::string_view> SplitList(const std::string_view Text, const char Separator = ',')
{
	std::vector<std::string_view> Items;
	std::size_t Start = 0;
	while (Start <= Text.size())
	{
		const std::size_t End = std::min(Text.find(Separator, Start), Text.size());
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

// The nodes of one machine; the unnamed machine is the outermost.
struct FMachineNodes
{
	std::vector<const FGraphNode*> States{};
	std::vector<const FGraphNode*> Transitions{};
	std::vector<const FGraphNode*> Entries{};
};

class FAnimationGraphCompiler
{
public:
	explicit FAnimationGraphCompiler(const FGraph& InGraph)
	    : Graph(InGraph)
	{
	}

	[[nodiscard]] std::expected<FAnimationGraphCompilation, std::vector<FGraphDiagnostic>> Compile();

private:
	void Fail(FGraphNodeId Node, std::string Message, std::string Pin = {});
	[[nodiscard]] std::optional<FGraphNodeId> Source(FGraphNodeId Node, std::string_view Pin) const;
	[[nodiscard]] std::optional<std::size_t> FindParameter(std::string_view Name) const;
	// A float parameter by name, or empty for an empty name; a name that is not a float parameter fails on Node.
	[[nodiscard]] std::optional<std::size_t> FloatParameter(const FGraphNode& Node, std::string_view Name);
	// Appends the tree under Node in preorder, so every child comes after its parent, and returns Node's index.
	std::size_t AppendTree(const FGraphNode& Node, std::vector<FAnimationBlendNodeDesc>& Tree, std::size_t Depth);
	[[nodiscard]] FAnimationStateMachineDesc BuildMachine(const std::string& Name, std::vector<std::string>& Path, bool bOutermost);

	const FGraph& Graph;
	std::vector<FGraphDiagnostic> Errors{};
	FAnimationGraphCompilation Result{};
	std::map<std::string, FMachineNodes> Machines{};
	std::set<std::string> Played{};
};

void FAnimationGraphCompiler::Fail(const FGraphNodeId Node, std::string Message, std::string Pin)
{
	Errors.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Pin = std::move(Pin), .Message = std::move(Message)});
}

std::optional<FGraphNodeId> FAnimationGraphCompiler::Source(const FGraphNodeId Node, const std::string_view Pin) const
{
	const auto Link = std::ranges::find_if(Graph.Links, [&](const FGraphLink& Candidate)
	{
		return Candidate.To.Node == Node && Candidate.To.Key == Pin;
	});

	return Link == Graph.Links.end() ? std::nullopt : std::optional(Link->From.Node);
}

std::optional<std::size_t> FAnimationGraphCompiler::FindParameter(const std::string_view Name) const
{
	const auto& Parameters = Result.StateMachine.Parameters;
	const auto Found = std::ranges::find(Parameters, Name, &FAnimationParameterDesc::Name);
	return Found == Parameters.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Parameters.begin()));
}

std::optional<std::size_t> FAnimationGraphCompiler::FloatParameter(const FGraphNode& Node, const std::string_view Name)
{
	if (Name.empty())
	{
		return std::nullopt;
	}

	const auto Parameter = FindParameter(Name);
	if (!Parameter || Result.StateMachine.Parameters[*Parameter].Type != EAnimationParameterType::Float)
	{
		Fail(Node.Id, std::format("'{}' must be a float parameter", Name));
		return std::nullopt;
	}

	return Parameter;
}

std::size_t FAnimationGraphCompiler::AppendTree(const FGraphNode& Node, std::vector<FAnimationBlendNodeDesc>& Tree, const std::size_t Depth)
{
	const std::size_t Index = Tree.size();
	Tree.emplace_back();
	FAnimationBlendNodeDesc Desc;
	// The graph is acyclic, but a pose shared by many blends is copied into each, so the depth still bounds the work.
	if (Depth > 32)
	{
		Fail(Node.Id, "A blend tree nests too deep");
		return Index;
	}

	std::vector<std::string_view> Pins;
	if (Node.Type == "Clip")
	{
		Desc.Type = EAnimationBlendNodeType::Clip;
		Desc.Clip = Text(Node, "clip");
		if (Desc.Clip.empty())
		{
			Fail(Node.Id, "A clip node names no clip");
		}
	}
	else if (Node.Type == "Blend1D" || Node.Type == "Blend2D")
	{
		const bool b2D = Node.Type == "Blend2D";
		Desc.Type = b2D ? EAnimationBlendNodeType::Blend2D : EAnimationBlendNodeType::Blend1D;
		Desc.ParameterX = FloatParameter(Node, Text(Node, b2D ? "parameterX" : "parameter"));
		Desc.ParameterY = b2D ? FloatParameter(Node, Text(Node, "parameterY")) : std::nullopt;
		if (!Desc.ParameterX || (b2D && !Desc.ParameterY))
		{
			Fail(Node.Id, "A blend needs a float parameter for each axis it blends along");
		}

		for (const FGraphPinDescriptor& Pin : BlendInputs)
		{
			if (Source(Node.Id, Pin.Key))
			{
				Pins.push_back(Pin.Key);
			}
		}

		const auto Places = SplitList(Text(Node, b2D ? "points" : "positions"));
		if (Places.size() != Pins.size() || Pins.empty())
		{
			Fail(Node.Id, std::format("A blend lists {} {} for {} linked poses", Places.size(), b2D ? "points" : "positions", Pins.size()));
		}

		for (std::size_t Child = 0; Child < std::min(Places.size(), Pins.size()); ++Child)
		{
			const auto Values = SplitList(Places[Child], ' ');
			const auto X = Values.empty() ? std::nullopt : ParseNumber(Values[0]);
			const auto Y = Values.size() > 1 ? ParseNumber(Values[1]) : std::optional(0.f);
			if (!X || !Y || Values.size() != (b2D ? 2u : 1u))
			{
				Fail(Node.Id, std::format("'{}' must be {}", Places[Child], b2D ? "two numbers, x and y" : "a number"));
			}

			Desc.Children.push_back({.X = X.value_or(0.f), .Y = Y.value_or(0.f)});
		}
	}
	else if (Node.Type == "Additive" || Node.Type == "Layered")
	{
		const bool bLayered = Node.Type == "Layered";
		Desc.Type = bLayered ? EAnimationBlendNodeType::Layered : EAnimationBlendNodeType::Additive;
		Desc.Weight = std::get<float>(Property(Node, "weight"));
		Desc.ParameterX = FloatParameter(Node, Text(Node, "parameter"));
		Desc.MaskJoint = bLayered ? Text(Node, "joint") : std::string();
		Pins = {"Base", bLayered ? "Layer" : "Additive"};
		for (const std::string_view Pin : Pins)
		{
			if (!Source(Node.Id, Pin))
			{
				Fail(Node.Id, std::format("Link a pose to {}", Pin), std::string(Pin));
			}
		}

		Desc.Children.resize(2);
	}

	for (std::size_t Child = 0; Child < Pins.size() && Child < Desc.Children.size(); ++Child)
	{
		if (const auto From = Source(Node.Id, Pins[Child]))
		{
			Desc.Children[Child].Node = AppendTree(*FindGraphNode(Graph, *From), Tree, Depth + 1);
		}
	}

	Tree[Index] = std::move(Desc);
	return Index;
}

FAnimationStateMachineDesc FAnimationGraphCompiler::BuildMachine(const std::string& Name, std::vector<std::string>& Path, const bool bOutermost)
{
	FAnimationStateMachineDesc Machine;
	const FMachineNodes& Nodes = Machines[Name];
	std::map<FGraphNodeId, std::size_t> StateIndices;
	std::map<std::string, std::size_t> Nested;
	for (const FGraphNode* Node : Nodes.States)
	{
		FAnimationStateDesc State{.Name = Text(*Node, "name"), .bLoop = std::get<bool>(Property(*Node, "loop")), .Speed = std::get<float>(Property(*Node, "speed")), .bReverse = std::get<bool>(Property(*Node, "reverse")), .bUpperBody = std::get<bool>(Property(*Node, "upperBody"))};
		if (State.Name.empty() || std::ranges::contains(Machine.States, State.Name, &FAnimationStateDesc::Name))
		{
			Fail(Node->Id, std::format("State names must be unique within their machine and not empty ('{}')", State.Name));
		}

		if (!std::isfinite(State.Speed) || State.Speed < 0.f)
		{
			Fail(Node->Id, "A state's speed must be zero or more");
		}

		for (const std::string_view Item : SplitList(Text(*Node, "clips")))
		{
			const auto Sample = ParseNamedNumber(Item);
			if (!Sample)
			{
				Fail(Node->Id, std::format("Clip '{}' must be a clip name, optionally followed by @ and its blend position", Item));
				continue;
			}

			State.Samples.push_back({.Clip = Sample->first, .Position = Sample->second});
		}

		if (const auto Pose = Source(Node->Id, "Pose"))
		{
			AppendTree(*FindGraphNode(Graph, *Pose), State.Tree, 0);
		}

		const std::string& Plays = Text(*Node, "plays");
		if (!Plays.empty())
		{
			if (std::ranges::contains(Path, Plays) || !Machines.contains(Plays) || Path.size() > 4)
			{
				Fail(Node->Id, std::format("State '{}' plays machine '{}', which has no states, contains this state, or nests too deep", State.Name, Plays));
			}
			else
			{
				Played.insert(Plays);
				if (!Nested.contains(Plays))
				{
					Path.push_back(Plays);
					Nested[Plays] = Machine.SubMachines.size();
					Machine.SubMachines.push_back(BuildMachine(Plays, Path, false));
					Path.pop_back();
				}

				State.SubMachine = Nested[Plays];
			}
		}

		const std::size_t Sources = (State.Samples.empty() ? 0 : 1) + (State.Tree.empty() ? 0 : 1) + (State.SubMachine ? 1 : 0);
		if (Sources != 1)
		{
			Fail(Node->Id, std::format("State '{}' needs exactly one of clips, a linked pose, or a machine to play", State.Name));
		}

		const std::string& Blend = Text(*Node, "blend");
		if (!Blend.empty())
		{
			State.BlendParameter = FindParameter(Blend);
			if (!State.BlendParameter || Result.StateMachine.Parameters[*State.BlendParameter].Type != EAnimationParameterType::Float)
			{
				Fail(Node->Id, std::format("Blend parameter '{}' must be a float parameter", Blend));
			}
		}
		else if (State.Samples.size() > 1)
		{
			Fail(Node->Id, std::format("State '{}' blends several clips, so it needs a blend parameter", State.Name));
		}

		for (const std::string_view Item : SplitList(Text(*Node, "events")))
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
		if (bOutermost)
		{
			Result.StateNodes.push_back(Node->Id);
		}

		Machine.States.push_back(std::move(State));
	}

	const std::string Label = Name.empty() ? std::string("The animation graph") : std::format("Machine '{}'", Name);
	if (Machine.States.empty())
	{
		Fail({}, std::format("{} needs at least one state", Label));
	}

	if (Nodes.Entries.size() != 1)
	{
		Fail(Nodes.Entries.empty() ? FGraphNodeId{} : Nodes.Entries[1]->Id, std::format("{} needs exactly one Entry node", Label));
	}
	else if (const auto EntryState = Source(Nodes.Entries.front()->Id, "State"); !EntryState || !StateIndices.contains(*EntryState))
	{
		Fail(Nodes.Entries.front()->Id, "Link the Entry node to the state its machine starts in", "State");
	}
	else
	{
		Machine.Entry = StateIndices.at(*EntryState);
	}

	std::vector<std::pair<std::int32_t, const FGraphNode*>> Transitions;
	for (const FGraphNode* Node : Nodes.Transitions)
	{
		Transitions.emplace_back(std::get<std::int32_t>(Property(*Node, "priority")), Node);
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

		if (!StateIndices.contains(*To) || (From && !StateIndices.contains(*From)))
		{
			Fail(Node->Id, "A transition links states of another machine than its own");
			continue;
		}

		FAnimationTransitionDesc Transition{.From = From ? StateIndices.at(*From) : AnyAnimationState, .To = StateIndices.at(*To), .DurationSeconds = std::get<float>(Property(*Node, "duration")), .bAtEnd = std::get<bool>(Property(*Node, "atEnd"))};
		if (!std::isfinite(Transition.DurationSeconds) || Transition.DurationSeconds < 0.f)
		{
			Fail(Node->Id, "A transition's duration must be zero or more");
		}

		for (const std::string_view Item : SplitList(Text(*Node, "conditions")))
		{
			const std::size_t Operator = Item.find_first_of("<>");
			const bool bNot = Item.starts_with('!');
			const std::string_view ParameterName = Trim(bNot ? Item.substr(1) : Item.substr(0, Operator));
			const auto Parameter = FindParameter(ParameterName);
			if (!Parameter)
			{
				Fail(Node->Id, std::format("Condition '{}' tests '{}', which is not a parameter", Item, ParameterName));
				continue;
			}

			const EAnimationParameterType Type = Result.StateMachine.Parameters[*Parameter].Type;
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

		if (bOutermost)
		{
			Result.TransitionNodes.push_back(Node->Id);
		}

		Machine.Transitions.push_back(std::move(Transition));
	}

	return Machine;
}

std::expected<FAnimationGraphCompilation, std::vector<FGraphDiagnostic>> FAnimationGraphCompiler::Compile()
{
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

	std::vector<const FGraphNode*> Nodes;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Nodes.push_back(&Node);
	}

	std::ranges::sort(Nodes, {}, [](const FGraphNode* Node)
	{
		return Node->Id;
	});

	FAnimationStateMachineDesc& Outer = Result.StateMachine;
	for (const FGraphNode* Node : Nodes)
	{
		if (Node->Type != "Parameter")
		{
			continue;
		}

		const std::string& Name = Text(*Node, "name");
		const std::string& Type = Text(*Node, "type");
		const auto Kind = Type == "Float" ? std::optional(EAnimationParameterType::Float) : Type == "Bool" ? std::optional(EAnimationParameterType::Bool) : Type == "Trigger" ? std::optional(EAnimationParameterType::Trigger) : std::nullopt;
		if (Name.empty() || std::ranges::contains(Outer.Parameters, Name, &FAnimationParameterDesc::Name))
		{
			Fail(Node->Id, std::format("Parameter names must be unique and not empty ('{}')", Name));
		}

		if (!Kind)
		{
			Fail(Node->Id, std::format("Parameter type '{}' must be Float, Bool, or Trigger", Type));
		}

		Outer.Parameters.push_back({.Name = Name, .Type = Kind.value_or(EAnimationParameterType::Float), .Default = std::get<float>(Property(*Node, "default"))});
	}

	Machines[""];
	for (const FGraphNode* Node : Nodes)
	{
		FMachineNodes* Machine = Node->Type == "State" || Node->Type == "Transition" || Node->Type == "Entry" ? &Machines[Text(*Node, "machine")] : nullptr;
		if (Machine)
		{
			(Node->Type == "State" ? Machine->States : Node->Type == "Transition" ? Machine->Transitions : Machine->Entries).push_back(Node);
		}
	}

	std::vector<std::string> Path{std::string()};
	auto Built = BuildMachine("", Path, true);
	Built.Parameters = std::move(Outer.Parameters);
	Outer = std::move(Built);
	for (const auto& [Name, Machine] : Machines)
	{
		if (!Name.empty() && !Played.contains(Name))
		{
			Fail(Machine.States.empty() ? FGraphNodeId{} : Machine.States.front()->Id, std::format("No state plays machine '{}'", Name));
		}
	}

	for (const FGraphNode* Node : Nodes)
	{
		if (Node->Type != "PoseRotation")
		{
			continue;
		}

		const auto Axis = std::get<std::array<float, 4>>(Property(*Node, "axis"));
		FAnimationPoseOperationDesc Operation{.Joint = Text(*Node, "joint"), .Axis = {Axis[0], Axis[1], Axis[2]}, .Parameter = FloatParameter(*Node, Text(*Node, "parameter")), .Scale = std::get<float>(Property(*Node, "scale"))};
		if (Operation.Joint.empty() || std::hypot(Axis[0], Axis[1], Axis[2]) < 1.e-6f || !std::isfinite(Operation.Scale))
		{
			Fail(Node->Id, "A pose rotation needs a joint, an axis, and a finite angle");
		}

		Outer.PoseOperations.push_back(std::move(Operation));
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	return std::move(Result);
}

// Lays a machine out on the canvas, nested machines in bands below, and returns the next free row.
class FAnimationGraphWriter
{
public:
	explicit FAnimationGraphWriter(FGraph& InGraph)
	    : Graph(InGraph)
	{
	}

	void WriteParameters(const FAnimationStateMachineDesc& Machine);
	void WritePoseOperations(const FAnimationStateMachineDesc& Machine);
	float WriteMachine(const FAnimationStateMachineDesc& Machine, const std::vector<FAnimationParameterDesc>& Parameters, const std::string& Name, float Top);

private:
	FGraphNode& Add(std::string_view Type, std::array<float, 2> Position);
	static void Set(FGraphNode& Node, std::string_view Key, FGraphValue Value);
	FGraphNodeId WriteTree(const FAnimationStateDesc& State, std::size_t Node, const std::vector<FAnimationParameterDesc>& Parameters, std::array<float, 2> Position, float& Row);

	FGraph& Graph;
	std::uint32_t NextId = 1;
	std::set<std::string> UsedNames{};
};

FGraphNode& FAnimationGraphWriter::Add(const std::string_view Type, const std::array<float, 2> Position)
{
	Graph.Nodes.push_back(*MakeGraphNode(AnimationSchema, Type, FGraphNodeId{NextId++}, Position));
	return Graph.Nodes.back();
}

void FAnimationGraphWriter::Set(FGraphNode& Node, const std::string_view Key, FGraphValue Value)
{
	std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value = std::move(Value);
}

void FAnimationGraphWriter::WriteParameters(const FAnimationStateMachineDesc& Machine)
{
	for (std::size_t Index = 0; Index < Machine.Parameters.size(); ++Index)
	{
		const FAnimationParameterDesc& Parameter = Machine.Parameters[Index];
		FGraphNode& Node = Add("Parameter", {-640.f, 110.f * static_cast<float>(Index)});
		Set(Node, "name", Parameter.Name);
		Set(Node, "type", ParameterTypeName(Parameter.Type));
		Set(Node, "default", Parameter.Default);
	}
}

void FAnimationGraphWriter::WritePoseOperations(const FAnimationStateMachineDesc& Machine)
{
	for (std::size_t Index = 0; Index < Machine.PoseOperations.size(); ++Index)
	{
		const FAnimationPoseOperationDesc& Operation = Machine.PoseOperations[Index];
		FGraphNode& Node = Add("PoseRotation", {-1040.f, 220.f * static_cast<float>(Index)});
		Set(Node, "joint", Operation.Joint);
		Set(Node, "axis", std::array{Operation.Axis[0], Operation.Axis[1], Operation.Axis[2], 0.f});
		Set(Node, "parameter", Operation.Parameter ? Machine.Parameters[*Operation.Parameter].Name : std::string());
		Set(Node, "scale", Operation.Scale);
	}
}

FGraphNodeId FAnimationGraphWriter::WriteTree(const FAnimationStateDesc& State, const std::size_t Node, const std::vector<FAnimationParameterDesc>& Parameters, const std::array<float, 2> Position, float& Row)
{
	const FAnimationBlendNodeDesc& Desc = State.Tree[Node];
	const auto Name = [&Parameters](const std::optional<std::size_t>& Parameter)
	{
		return Parameter ? Parameters[*Parameter].Name : std::string();
	};

	constexpr std::array Types{std::string_view("Clip"), std::string_view("Blend1D"), std::string_view("Blend2D"), std::string_view("Additive"), std::string_view("Layered")};
	FGraphNode& Written = Add(Types[static_cast<std::size_t>(Desc.Type)], Position);
	const FGraphNodeId Id = Written.Id;
	std::string Places;
	for (const FAnimationBlendChild& Child : Desc.Children)
	{
		Places += (Places.empty() ? "" : ", ") + (Desc.Type == EAnimationBlendNodeType::Blend2D ? FormatNumber(Child.X) + " " + FormatNumber(Child.Y) : FormatNumber(Child.X));
	}

	switch (Desc.Type)
	{
		case EAnimationBlendNodeType::Clip:
			Set(Written, "clip", Desc.Clip);
			break;
		case EAnimationBlendNodeType::Blend1D:
			Set(Written, "parameter", Name(Desc.ParameterX));
			Set(Written, "positions", Places);
			break;
		case EAnimationBlendNodeType::Blend2D:
			Set(Written, "parameterX", Name(Desc.ParameterX));
			Set(Written, "parameterY", Name(Desc.ParameterY));
			Set(Written, "points", Places);
			break;
		case EAnimationBlendNodeType::Additive:
		case EAnimationBlendNodeType::Layered:
			Set(Written, "weight", Desc.Weight);
			Set(Written, "parameter", Name(Desc.ParameterX));
			if (Desc.Type == EAnimationBlendNodeType::Layered)
			{
				Set(Written, "joint", Desc.MaskJoint);
			}

			break;
	}

	// Children sit one column further left, each on its own row.
	const bool bPair = Desc.Type == EAnimationBlendNodeType::Additive || Desc.Type == EAnimationBlendNodeType::Layered;
	for (std::size_t Child = 0; Child < Desc.Children.size(); ++Child)
	{
		const FGraphNodeId From = WriteTree(State, Desc.Children[Child].Node, Parameters, {Position[0] - 420.f, Row}, Row);
		const std::string_view Pin = bPair ? (Child == 0 ? std::string_view("Base") : Desc.Type == EAnimationBlendNodeType::Layered ? std::string_view("Layer") : std::string_view("Additive")) : BlendInputs[Child].Key;
		Graph.Links.push_back({.From = {.Node = From, .Key = "Pose"}, .To = {.Node = Id, .Key = std::string(Pin)}});
	}

	Row = std::max(Row, Position[1] + 200.f);
	return Id;
}

float FAnimationGraphWriter::WriteMachine(const FAnimationStateMachineDesc& Machine, const std::vector<FAnimationParameterDesc>& Parameters, const std::string& Name, float Top)
{
	// States sit on a grid three wide with the transitions that leave each stacked beside it, and any-state transitions in a column of their
	// own, so no node covers another; rows grow to fit their tallest stack. Spacing allows for the canvas's node sizes: a state is about 180
	// units tall and a transition 160.
	constexpr std::size_t Columns = 3;
	constexpr float StateX = 400.f;
	constexpr float ColumnWidth = 820.f;
	constexpr float TransitionOffset = 300.f;
	constexpr float TransitionPitch = 190.f;
	constexpr float RowGap = 140.f;
	FGraphNode& EntryNode = Add("Entry", {0.f, Top - 260.f});
	Set(EntryNode, "machine", Name);
	const FGraphNodeId Entry = EntryNode.Id;
	std::vector<std::size_t> Outgoing(Machine.States.size(), 0);
	for (const FAnimationTransitionDesc& Transition : Machine.Transitions)
	{
		if (Transition.From < Outgoing.size())
		{
			++Outgoing[Transition.From];
		}
	}

	// A nested machine takes the name of the first state that plays it, made unique.
	std::vector<std::string> NestedNames(Machine.SubMachines.size());
	for (const FAnimationStateDesc& State : Machine.States)
	{
		if (State.SubMachine && *State.SubMachine < NestedNames.size() && NestedNames[*State.SubMachine].empty())
		{
			std::string Unique = State.Name;
			for (int Suffix = 2; UsedNames.contains(Unique); ++Suffix)
			{
				Unique = std::format("{} {}", State.Name, Suffix);
			}

			UsedNames.insert(Unique);
			NestedNames[*State.SubMachine] = Unique;
		}
	}

	std::vector<FGraphNodeId> States;
	std::vector<std::array<float, 2>> Positions;
	float RowTop = Top;
	float TreeRow = Top;
	for (std::size_t Index = 0; Index < Machine.States.size(); ++Index)
	{
		const FAnimationStateDesc& State = Machine.States[Index];
		Positions.push_back({StateX + ColumnWidth * static_cast<float>(Index % Columns), RowTop});
		if (Index % Columns == Columns - 1 || Index + 1 == Machine.States.size())
		{
			std::size_t Tallest = 1;
			for (std::size_t Other = Index - Index % Columns; Other <= Index; ++Other)
			{
				Tallest = std::max(Tallest, Outgoing[Other]);
			}

			RowTop += TransitionPitch * static_cast<float>(Tallest) + RowGap;
		}

		FGraphNode& Node = Add("State", Positions.back());
		const FGraphNodeId StateId = Node.Id;
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
		Set(Node, "blend", State.BlendParameter ? Parameters[*State.BlendParameter].Name : std::string());
		Set(Node, "loop", State.bLoop);
		Set(Node, "speed", State.Speed);
		Set(Node, "reverse", State.bReverse);
		Set(Node, "upperBody", State.bUpperBody);
		Set(Node, "events", Events);
		Set(Node, "machine", Name);
		Set(Node, "plays", State.SubMachine && *State.SubMachine < NestedNames.size() ? NestedNames[*State.SubMachine] : std::string());
		States.push_back(StateId);
		if (!State.Tree.empty())
		{
			// Trees hang off to the left of the grid, out of the states' way.
			const FGraphNodeId Root = WriteTree(State, 0, Parameters, {-1500.f, TreeRow}, TreeRow);
			TreeRow += 120.f;
			Graph.Links.push_back({.From = {.Node = Root, .Key = "Pose"}, .To = {.Node = StateId, .Key = "Pose"}});
		}
	}

	if (Machine.Entry < States.size())
	{
		Graph.Links.push_back({.From = {.Node = States[Machine.Entry], .Key = "State"}, .To = {.Node = Entry, .Key = "State"}});
	}

	std::vector<std::size_t> Stacked(Machine.States.size(), 0);
	std::size_t AnyStacked = 0;
	for (std::size_t Index = 0; Index < Machine.Transitions.size(); ++Index)
	{
		const FAnimationTransitionDesc& Transition = Machine.Transitions[Index];
		const bool bAny = Transition.From == AnyAnimationState || Transition.From >= States.size();
		if (Transition.To >= States.size())
		{
			continue;
		}

		const std::array<float, 2> Position = bAny ? std::array{0.f, Top + TransitionPitch * static_cast<float>(AnyStacked++)}
		                                           : std::array{Positions[Transition.From][0] + TransitionOffset, Positions[Transition.From][1] + TransitionPitch * static_cast<float>(Stacked[Transition.From]++)};
		FGraphNode& Node = Add("Transition", Position);
		std::string Conditions;
		for (const FAnimationConditionDesc& Condition : Transition.Conditions)
		{
			const std::string& ParameterName = Parameters[Condition.Parameter].Name;
			const std::string Written = Condition.Test == EAnimationCondition::Greater ? ParameterName + " > " + FormatNumber(Condition.Value)
			                            : Condition.Test == EAnimationCondition::Less  ? ParameterName + " < " + FormatNumber(Condition.Value)
			                            : Condition.Test == EAnimationCondition::IsFalse ? "!" + ParameterName
			                                                                             : ParameterName;
			Conditions += (Conditions.empty() ? "" : ", ") + Written;
		}

		Set(Node, "priority", static_cast<std::int32_t>(Index));
		Set(Node, "duration", Transition.DurationSeconds);
		Set(Node, "atEnd", Transition.bAtEnd);
		Set(Node, "conditions", Conditions);
		Set(Node, "machine", Name);
		if (!bAny)
		{
			Graph.Links.push_back({.From = {.Node = States[Transition.From], .Key = "State"}, .To = {.Node = Node.Id, .Key = "From"}});
		}

		Graph.Links.push_back({.From = {.Node = States[Transition.To], .Key = "State"}, .To = {.Node = Node.Id, .Key = "To"}});
	}

	float Bottom = std::max(RowTop, TreeRow) + 600.f;
	for (std::size_t Index = 0; Index < Machine.SubMachines.size(); ++Index)
	{
		Bottom = WriteMachine(Machine.SubMachines[Index], Parameters, NestedNames[Index], Bottom);
	}

	return Bottom;
}
}

const FGraphSchema& GetAnimationGraphSchema()
{
	return AnimationSchema;
}

std::expected<FAnimationGraphCompilation, std::vector<FGraphDiagnostic>> CompileAnimationGraph(const FGraph& Graph)
{
	return FAnimationGraphCompiler(Graph).Compile();
}

FGraph MakeAnimationGraph(const FAnimationStateMachineDesc& StateMachine)
{
	FGraph Graph{.Domain = std::string(AnimationGraphDomain), .DomainVersion = AnimationGraphVersion};
	FAnimationGraphWriter Writer(Graph);
	Writer.WriteParameters(StateMachine);
	Writer.WritePoseOperations(StateMachine);
	Writer.WriteMachine(StateMachine, StateMachine.Parameters, std::string(), 0.f);
	return Graph;
}
}
