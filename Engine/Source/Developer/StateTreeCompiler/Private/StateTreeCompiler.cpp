#include "Herta/StateTreeCompiler/StateTreeCompiler.h"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace Herta
{
namespace
{
enum EStateTreePin : std::uint8_t
{
	HierarchyPin = 1,
	ItemsPin,
	ConditionsPin,
	// A condition attaches to a state's items or to a transition's conditions.
	ConditionOwnerPin,
};

constexpr std::array ParentInputs{FGraphPinDescriptor{.Key = "Parent", .Name = "Parent", .Type = HierarchyPin}};
constexpr std::array StateOutputs{FGraphPinDescriptor{.Key = "Children", .Name = "Children", .Type = HierarchyPin}, FGraphPinDescriptor{.Key = "Items", .Name = "Items", .Type = ItemsPin}};
constexpr std::array ItemInputs{FGraphPinDescriptor{.Key = "State", .Name = "State", .Type = ItemsPin}};
constexpr std::array ConditionInputs{FGraphPinDescriptor{.Key = "Owner", .Name = "Owner", .Type = ConditionOwnerPin}};
constexpr std::array TransitionOutputs{FGraphPinDescriptor{.Key = "Conditions", .Name = "Conditions", .Type = ConditionsPin}};

const std::array RootProperties{
    FGraphPropertyDescriptor{.Key = "selection", .Name = "Selection", .Default = std::string("first")},
};
const std::array StateProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("State")},
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "selection", .Name = "Selection", .Default = std::string("first")},
    FGraphPropertyDescriptor{.Key = "score", .Name = "Score", .Default = 0.f},
};
const std::array OrderProperty{FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}}};
const std::array WaitProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "minimum", .Name = "Minimum (s)", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "maximum", .Name = "Maximum (s)", .Default = 1.f},
};
const std::array MoveToProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "target", .Name = "Target", .Default = std::string("enemy")},
    FGraphPropertyDescriptor{.Key = "run", .Name = "Run", .Default = false},
    FGraphPropertyDescriptor{.Key = "acceptance", .Name = "Acceptance (m)", .Default = 0.6f},
};
const std::array WanderProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "radius", .Name = "Radius (m)", .Default = 5.f},
};
const std::array FleeProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "target", .Name = "Target", .Default = std::string("enemy")},
    FGraphPropertyDescriptor{.Key = "distance", .Name = "Distance (m)", .Default = 6.f},
};
const std::array FaceProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "target", .Name = "Target", .Default = std::string("enemy")},
};
const std::array AttackProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "target", .Name = "Target", .Default = std::string("enemy")},
    FGraphPropertyDescriptor{.Key = "reach", .Name = "Reach (m)", .Default = 1.6f},
};
const std::array SurroundProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "radius", .Name = "Radius (m)", .Default = 3.f},
};
const std::array AnimateProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "trigger", .Name = "Trigger", .Default = std::string("Hit")},
};
const std::array SetFlagProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "flag", .Name = "Flag", .Default = std::string("Alerted")},
    FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = true},
};
const std::array ConditionProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "type", .Name = "Type", .Default = std::string("sees")},
    FGraphPropertyDescriptor{.Key = "compare", .Name = "Compare", .Default = std::string("less")},
    FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "flag", .Name = "Flag", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "invert", .Name = "Invert", .Default = false},
};
const std::array TransitionProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "trigger", .Name = "Trigger", .Default = std::string("completed")},
    FGraphPropertyDescriptor{.Key = "event", .Name = "Event", .Default = std::string()},
    FGraphPropertyDescriptor{.Key = "kind", .Name = "Kind", .Default = std::string("goto")},
    FGraphPropertyDescriptor{.Key = "target", .Name = "Target State", .Default = std::string()},
};
const std::array ConsiderationProperties{
    FGraphPropertyDescriptor{.Key = "order", .Name = "Order", .Default = std::int32_t{0}},
    FGraphPropertyDescriptor{.Key = "input", .Name = "Input", .Default = std::string("distance")},
    FGraphPropertyDescriptor{.Key = "minimum", .Name = "Minimum", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "maximum", .Name = "Maximum", .Default = 10.f},
    FGraphPropertyDescriptor{.Key = "weight", .Name = "Weight", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "invert", .Name = "Invert", .Default = false},
};

const std::array StateTreeNodes{
    FGraphNodeDescriptor{.Type = "Root", .Name = "Root", .Category = "Hierarchy", .Outputs = StateOutputs, .Properties = RootProperties, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "State", .Name = "State", .Category = "Hierarchy", .Inputs = ParentInputs, .Outputs = StateOutputs, .Properties = StateProperties},
    FGraphNodeDescriptor{.Type = "Wait", .Name = "Wait", .Category = "Tasks", .Inputs = ItemInputs, .Properties = WaitProperties},
    FGraphNodeDescriptor{.Type = "MoveTo", .Name = "Move To", .Category = "Tasks", .Inputs = ItemInputs, .Properties = MoveToProperties},
    FGraphNodeDescriptor{.Type = "Wander", .Name = "Wander", .Category = "Tasks", .Inputs = ItemInputs, .Properties = WanderProperties},
    FGraphNodeDescriptor{.Type = "Flee", .Name = "Flee", .Category = "Tasks", .Inputs = ItemInputs, .Properties = FleeProperties},
    FGraphNodeDescriptor{.Type = "Face", .Name = "Face", .Category = "Tasks", .Inputs = ItemInputs, .Properties = FaceProperties},
    FGraphNodeDescriptor{.Type = "Attack", .Name = "Attack", .Category = "Tasks", .Inputs = ItemInputs, .Properties = AttackProperties},
    FGraphNodeDescriptor{.Type = "Animate", .Name = "Animate", .Category = "Tasks", .Inputs = ItemInputs, .Properties = AnimateProperties},
    FGraphNodeDescriptor{.Type = "SetFlag", .Name = "Set Flag", .Category = "Tasks", .Inputs = ItemInputs, .Properties = SetFlagProperties},
    FGraphNodeDescriptor{.Type = "Forget", .Name = "Forget", .Category = "Tasks", .Inputs = ItemInputs, .Properties = OrderProperty},
    FGraphNodeDescriptor{.Type = "Surround", .Name = "Surround", .Category = "Tasks", .Inputs = ItemInputs, .Properties = SurroundProperties},
    FGraphNodeDescriptor{.Type = "Condition", .Name = "Condition", .Category = "Logic", .Inputs = ConditionInputs, .Properties = ConditionProperties},
    FGraphNodeDescriptor{.Type = "Transition", .Name = "Transition", .Category = "Logic", .Inputs = ItemInputs, .Outputs = TransitionOutputs, .Properties = TransitionProperties},
    FGraphNodeDescriptor{.Type = "Consideration", .Name = "Consideration", .Category = "Logic", .Inputs = ItemInputs, .Properties = ConsiderationProperties},
};

bool CanConnectStateTreePins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To || (To == ConditionOwnerPin && (From == ItemsPin || From == ConditionsPin));
}

// Trees, not graphs: the hierarchy cannot loop, and transitions loop through names rather than links.
const FGraphSchema StateTreeSchema{.Domain = StateTreeDomain, .Version = StateTreeGraphVersion, .Nodes = StateTreeNodes, .CanConnect = CanConnectStateTreePins};

constexpr std::array<std::string_view, 2> SelectionChoices{"first", "utility"};
// "player" is the name the enemy had before agents fought each other, kept so older trees still compile.
constexpr std::array<std::string_view, 5> TargetChoices{"enemy", "lastSeen", "noise", "home", "player"};
constexpr std::array<std::string_view, 8> ConditionChoices{"sees", "remembers", "heard", "distance", "health", "random", "time", "flag"};
constexpr std::array<std::string_view, 2> CompareChoices{"less", "greater"};
constexpr std::array<std::string_view, 5> TriggerChoices{"completed", "succeeded", "failed", "tick", "event"};
constexpr std::array<std::string_view, 3> KindChoices{"goto", "succeed", "fail"};
constexpr std::array<std::string_view, 4> InputChoices{"distance", "health", "seen", "heard"};
constexpr std::array<std::string_view, 10> TaskTypes{"Wait", "MoveTo", "Wander", "Flee", "Face", "Attack", "Animate", "SetFlag", "Forget", "Surround"};

[[nodiscard]] const FGraphValue* FindProperty(const FGraphNode& Node, const std::string_view Key)
{
	const auto Found = std::ranges::find(Node.Properties, Key, &FGraphProperty::Key);
	return Found != Node.Properties.end() ? &Found->Value : nullptr;
}

[[nodiscard]] float Number(const FGraphNode& Node, const std::string_view Key)
{
	const FGraphValue* const Value = FindProperty(Node, Key);
	return Value && std::holds_alternative<float>(*Value) ? std::get<float>(*Value) : 0.f;
}

[[nodiscard]] std::int32_t Integer(const FGraphNode& Node, const std::string_view Key)
{
	const FGraphValue* const Value = FindProperty(Node, Key);
	return Value && std::holds_alternative<std::int32_t>(*Value) ? std::get<std::int32_t>(*Value) : 0;
}

[[nodiscard]] bool Flag(const FGraphNode& Node, const std::string_view Key)
{
	const FGraphValue* const Value = FindProperty(Node, Key);
	return Value && std::holds_alternative<bool>(*Value) && std::get<bool>(*Value);
}

[[nodiscard]] std::string_view Text(const FGraphNode& Node, const std::string_view Key)
{
	const FGraphValue* const Value = FindProperty(Node, Key);
	return Value && std::holds_alternative<std::string>(*Value) ? std::string_view(std::get<std::string>(*Value)) : std::string_view{};
}

// The index of Value among Choices, or empty with a diagnostic on Node.
template <std::size_t Count> [[nodiscard]] std::optional<std::uint8_t> Choose(const FGraphNode& Node, const std::string_view Key, const std::array<std::string_view, Count>& Choices, std::vector<FGraphDiagnostic>& Diagnostics)
{
	const std::string_view Value = Text(Node, Key);
	const auto Found = std::ranges::find(Choices, Value);
	if (Found == Choices.end())
	{
		Diagnostics.push_back({.Node = Node.Id, .Message = std::format("{} '{}' is not one of the choices", Key, Value)});
		return std::nullopt;
	}

	return static_cast<std::uint8_t>(Found - Choices.begin());
}

struct FCompiler
{
	[[nodiscard]] std::uint16_t Name(const std::string_view Text)
	{
		const auto Found = std::ranges::find(Tree.Names, Text);
		if (Found != Tree.Names.end())
		{
			return static_cast<std::uint16_t>(Found - Tree.Names.begin());
		}

		Tree.Names.emplace_back(Text);
		return static_cast<std::uint16_t>(Tree.Names.size() - 1);
	}

	[[nodiscard]] std::vector<const FGraphNode*> Attached(const FGraphNodeId Owner, const std::string_view Pin, const std::function<bool(const FGraphNode&)>& Accept) const
	{
		std::vector<const FGraphNode*> Result;
		for (const FGraphLink& Link : Graph.Links)
		{
			if (Link.From.Node == Owner && Link.From.Key == Pin)
			{
				const FGraphNode* const Node = FindGraphNode(Graph, Link.To.Node);
				if (Node && Accept(*Node))
				{
					Result.push_back(Node);
				}
			}
		}

		// Siblings and items run in their Order, then by identity, so moving nodes on the canvas changes nothing.
		std::ranges::sort(Result, [](const FGraphNode* const Left, const FGraphNode* const Right)
		{
			return std::pair{Integer(*Left, "order"), Left->Id} < std::pair{Integer(*Right, "order"), Right->Id};
		});

		return Result;
	}

	void AddCondition(const FGraphNode& Node)
	{
		const auto Type = Choose(Node, "type", ConditionChoices, Errors);
		const auto Compare = Choose(Node, "compare", CompareChoices, Errors);
		FStateTreeConditionDesc Condition{.Type = static_cast<EStateTreeConditionType>(Type.value_or(0)), .Compare = static_cast<EStateTreeCompare>(Compare.value_or(0)), .bInvert = Flag(Node, "invert"), .Value = Number(Node, "value")};
		if (Condition.Type == EStateTreeConditionType::Flag)
		{
			if (Text(Node, "flag").empty())
			{
				Errors.push_back({.Node = Node.Id, .Message = "A flag condition needs the flag's name"});
			}

			Condition.Key = Name(Text(Node, "flag"));
		}

		Tree.Conditions.push_back(Condition);
	}

	void AddTask(const FGraphNode& Node)
	{
		const auto Type = static_cast<EStateTreeTaskType>(std::ranges::find(TaskTypes, Node.Type) - TaskTypes.begin());
		FStateTreeTaskDesc Task{.Type = Type};
		if (Type == EStateTreeTaskType::MoveTo || Type == EStateTreeTaskType::Flee || Type == EStateTreeTaskType::Face || Type == EStateTreeTaskType::Attack)
		{
			const std::uint8_t Target = Choose(Node, "target", TargetChoices, Errors).value_or(0);
			Task.Target = Target == TargetChoices.size() - 1 ? EStateTreeTarget::Enemy : static_cast<EStateTreeTarget>(Target);
		}

		switch (Type)
		{
			case EStateTreeTaskType::Wait:
				Task.Values = {Number(Node, "minimum"), Number(Node, "maximum")};
				if (Task.Values[0] < 0.f || Task.Values[1] < Task.Values[0])
				{
					Errors.push_back({.Node = Node.Id, .Message = "A wait lasts from a minimum of zero or more to a maximum no shorter"});
				}

				break;
			case EStateTreeTaskType::MoveTo:
				Task.Values = {Flag(Node, "run") ? 1.f : 0.f, Number(Node, "acceptance")};
				break;
			case EStateTreeTaskType::Wander:
				Task.Values = {Number(Node, "radius"), 0.f};
				break;
			case EStateTreeTaskType::Flee:
				Task.Values = {Number(Node, "distance"), 0.f};
				break;
			case EStateTreeTaskType::Attack:
				Task.Values = {Number(Node, "reach"), 0.f};
				break;
			case EStateTreeTaskType::Surround:
				Task.Values = {Number(Node, "radius"), 0.f};
				if (Task.Values[0] < 1.f)
				{
					Errors.push_back({.Node = Node.Id, .Message = "A surround ring is at least 1 m across its radius"});
				}

				break;
			case EStateTreeTaskType::Animate:
				Task.Key = Name(Text(Node, "trigger"));
				break;
			case EStateTreeTaskType::SetFlag:
				Task.Key = Name(Text(Node, "flag"));
				Task.Values = {Flag(Node, "value") ? 1.f : 0.f, 0.f};
				break;
			default:
				break;
		}

		Tree.Tasks.push_back(Task);
	}

	// Lays out State and its subtree depth first, its items first so they stay contiguous per state.
	void AddState(const FGraphNode& Node, const std::uint16_t Parent, const std::uint16_t Depth)
	{
		if (Tree.States.size() >= MaximumStateTreeStates || Depth >= MaximumStateTreeDepth)
		{
			Errors.push_back({.Node = Node.Id, .Message = std::format("A StateTree holds at most {} states, {} deep", MaximumStateTreeStates, MaximumStateTreeDepth)});
			return;
		}

		const auto Index = static_cast<std::uint16_t>(Tree.States.size());
		const bool bRoot = Node.Type == "Root";
		FStateTreeStateDesc State{
		    .Name = bRoot ? std::string("Root") : std::string(Text(Node, "name")),
		    .Node = Node.Id.Value,
		    .Parent = Parent,
		    .Depth = Depth,
		    .Selection = static_cast<EStateTreeSelection>(Choose(Node, "selection", SelectionChoices, Errors).value_or(0)),
		    .Score = Number(Node, "score"),
		};

		if (!bRoot && State.Name.empty())
		{
			Errors.push_back({.Node = Node.Id, .Message = "A state needs a name"});
		}

		if (Parent != StateTreeNone)
		{
			std::uint16_t& Link = Tree.States[Parent].FirstChild == StateTreeNone ? Tree.States[Parent].FirstChild : Tree.States[LastChild[Parent]].NextSibling;
			Link = Index;
			LastChild[Parent] = Index;
		}

		State.FirstTask = static_cast<std::uint16_t>(Tree.Tasks.size());
		for (const FGraphNode* const Task : Attached(Node.Id, "Items", [](const FGraphNode& Item)
		{
			return IsStateTreeTask(Item.Type);
		}))
		{
			AddTask(*Task);
		}

		State.TaskCount = static_cast<std::uint16_t>(Tree.Tasks.size() - State.FirstTask);
		State.FirstCondition = static_cast<std::uint16_t>(Tree.Conditions.size());
		for (const FGraphNode* const Condition : Attached(Node.Id, "Items", [](const FGraphNode& Item)
		{
			return Item.Type == "Condition";
		}))
		{
			AddCondition(*Condition);
		}

		State.ConditionCount = static_cast<std::uint16_t>(Tree.Conditions.size() - State.FirstCondition);
		State.FirstConsideration = static_cast<std::uint16_t>(Tree.Considerations.size());
		for (const FGraphNode* const Consideration : Attached(Node.Id, "Items", [](const FGraphNode& Item)
		{
			return Item.Type == "Consideration";
		}))
		{
			const auto Input = Choose(*Consideration, "input", InputChoices, Errors);
			const FStateTreeConsiderationDesc Desc{.Input = static_cast<EStateTreeInput>(Input.value_or(0)), .Minimum = Number(*Consideration, "minimum"), .Maximum = Number(*Consideration, "maximum"), .Weight = Number(*Consideration, "weight"), .bInvert = Flag(*Consideration, "invert")};
			if (Desc.Maximum <= Desc.Minimum)
			{
				Errors.push_back({.Node = Consideration->Id, .Message = "A consideration's maximum must be above its minimum"});
			}

			Tree.Considerations.push_back(Desc);
		}

		State.ConsiderationCount = static_cast<std::uint16_t>(Tree.Considerations.size() - State.FirstConsideration);
		if (State.ConsiderationCount > 0 && (Parent == StateTreeNone || Tree.States[Parent].Selection != EStateTreeSelection::Utility))
		{
			Warnings.push_back({.Severity = EGraphSeverity::Warning, .Node = Node.Id, .Message = "Considerations only score children of a utility state; these are ignored"});
		}

		Tree.States.push_back(State);
		StateNodes.push_back(&Node);
		LastChild.push_back(StateTreeNone);
		for (const FGraphNode* const Child : Attached(Node.Id, "Children", [](const FGraphNode& Item)
		{
			return Item.Type == "State";
		}))
		{
			AddState(*Child, Index, static_cast<std::uint16_t>(Depth + 1));
		}
	}

	// Transitions come after every state, since they name states anywhere in the tree.
	void AddTransitions()
	{
		std::map<std::string, std::uint16_t, std::less<>> Indices;
		for (std::size_t Index = 0; Index < Tree.States.size(); ++Index)
		{
			if (!Indices.emplace(Tree.States[Index].Name, static_cast<std::uint16_t>(Index)).second)
			{
				Errors.push_back({.Node = StateNodes[Index]->Id, .Message = std::format("Another state is already named '{}'", Tree.States[Index].Name)});
			}
		}

		for (std::size_t Index = 0; Index < Tree.States.size(); ++Index)
		{
			Tree.States[Index].FirstTransition = static_cast<std::uint16_t>(Tree.Transitions.size());
			for (const FGraphNode* const Node : Attached(StateNodes[Index]->Id, "Items", [](const FGraphNode& Item)
			{
				return Item.Type == "Transition";
			}))
			{
				FStateTreeTransitionDesc Transition{
				    .Trigger = static_cast<EStateTreeTrigger>(Choose(*Node, "trigger", TriggerChoices, Errors).value_or(0)),
				    .Kind = static_cast<EStateTreeTransitionKind>(Choose(*Node, "kind", KindChoices, Errors).value_or(0)),
				};

				if (Transition.Kind == EStateTreeTransitionKind::GotoState)
				{
					const auto Target = Indices.find(Text(*Node, "target"));
					if (Target == Indices.end())
					{
						Errors.push_back({.Node = Node->Id, .Message = std::format("No state is named '{}'", Text(*Node, "target"))});
					}
					else
					{
						Transition.Target = Target->second;
					}
				}

				if (Transition.Trigger == EStateTreeTrigger::OnEvent)
				{
					if (Text(*Node, "event").empty())
					{
						Errors.push_back({.Node = Node->Id, .Message = "An event transition needs the event's name"});
					}

					Transition.Event = Name(Text(*Node, "event"));
				}

				Transition.FirstCondition = static_cast<std::uint16_t>(Tree.Conditions.size());
				for (const FGraphNode* const Condition : Attached(Node->Id, "Conditions", [](const FGraphNode& Item)
				{
					return Item.Type == "Condition";
				}))
				{
					AddCondition(*Condition);
				}

				Transition.ConditionCount = static_cast<std::uint16_t>(Tree.Conditions.size() - Transition.FirstCondition);
				Tree.Transitions.push_back(Transition);
				TransitionNodes.push_back(Node);
			}

			Tree.States[Index].TransitionCount = static_cast<std::uint16_t>(Tree.Transitions.size() - Tree.States[Index].FirstTransition);
		}
	}

	// Leaves without tasks finish the moment they start, so unconditional finishing transitions between them would loop until the limit
	// every tick.
	void CheckInstantLoops()
	{
		const auto IsInstant = [this](const std::uint16_t State)
		{
			return Tree.States[State].TaskCount == 0 && Tree.States[State].FirstChild == StateTreeNone;
		};

		const auto NextInstant = [&](const std::uint16_t State) -> std::optional<std::pair<std::uint16_t, std::size_t>>
		{
			const FStateTreeStateDesc& Desc = Tree.States[State];
			for (std::uint16_t Index = Desc.FirstTransition; Index < Desc.FirstTransition + Desc.TransitionCount; ++Index)
			{
				const FStateTreeTransitionDesc& Transition = Tree.Transitions[Index];
				const bool bFinishing = Transition.Trigger == EStateTreeTrigger::OnCompleted || Transition.Trigger == EStateTreeTrigger::OnSucceeded;
				if (bFinishing && Transition.ConditionCount == 0 && Transition.Kind == EStateTreeTransitionKind::GotoState && Transition.Target != StateTreeNone && IsInstant(Transition.Target))
				{
					return std::pair{Transition.Target, static_cast<std::size_t>(Index)};
				}
			}

			return std::nullopt;
		};

		for (std::size_t StateIndex = 0; StateIndex < Tree.States.size(); ++StateIndex)
		{
			const auto Start = static_cast<std::uint16_t>(StateIndex);
			if (!IsInstant(Start))
			{
				continue;
			}

			std::uint16_t State = Start;
			for (std::size_t Step = 0; Step <= Tree.States.size(); ++Step)
			{
				const auto Next = NextInstant(State);
				if (!Next)
				{
					break;
				}

				if (Next->first == Start)
				{
					Errors.push_back({.Node = TransitionNodes[Next->second]->Id, .Message = std::format("'{}' and the states it passes to finish at once and hand over to each other forever", Tree.States[Start].Name)});
					return;
				}

				State = Next->first;
			}
		}
	}

	const FGraph& Graph;
	FCookedStateTree Tree{};
	std::vector<const FGraphNode*> StateNodes{};
	std::vector<const FGraphNode*> TransitionNodes{};
	std::vector<std::uint16_t> LastChild{};
	std::vector<FGraphDiagnostic> Errors{};
	std::vector<FGraphDiagnostic> Warnings{};
};
}

const FGraphSchema& GetStateTreeSchema()
{
	return StateTreeSchema;
}

std::span<const std::string_view> GetStateTreeChoices(const std::string_view NodeType, const std::string_view Property)
{
	if (Property == "selection")
	{
		return SelectionChoices;
	}

	if (Property == "target" && NodeType != "Transition")
	{
		return TargetChoices;
	}

	if (NodeType == "Condition")
	{
		return Property == "type" ? std::span<const std::string_view>(ConditionChoices) : Property == "compare" ? std::span<const std::string_view>(CompareChoices)
		                                                                                                        : std::span<const std::string_view>{};
	}

	if (NodeType == "Transition")
	{
		return Property == "trigger" ? std::span<const std::string_view>(TriggerChoices) : Property == "kind" ? std::span<const std::string_view>(KindChoices)
		                                                                                                      : std::span<const std::string_view>{};
	}

	return NodeType == "Consideration" && Property == "input" ? std::span<const std::string_view>(InputChoices) : std::span<const std::string_view>{};
}

bool IsStateTreeTask(const std::string_view NodeType)
{
	return std::ranges::contains(TaskTypes, NodeType);
}

std::expected<FStateTreeCompilation, std::vector<FGraphDiagnostic>> CompileStateTree(const FGraph& Graph)
{
	std::vector<FGraphDiagnostic> Structural = ValidateGraph(Graph, StateTreeSchema);
	if (std::ranges::any_of(Structural, [](const FGraphDiagnostic& Diagnostic)
	{
		return Diagnostic.Severity == EGraphSeverity::Error;
	}))
	{
		return std::unexpected(std::move(Structural));
	}

	const auto Root = std::ranges::find(Graph.Nodes, std::string("Root"), &FGraphNode::Type);
	if (Root == Graph.Nodes.end())
	{
		return std::unexpected(std::vector<FGraphDiagnostic>{{.Message = "A StateTree needs its Root node"}});
	}

	FCompiler Compiler{.Graph = Graph};
	Compiler.AddState(*Root, StateTreeNone, 0);
	Compiler.AddTransitions();
	Compiler.CheckInstantLoops();

	// Anything left off the tree from the root is reported once, at its node.
	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (Node.Type == "State" && !std::ranges::contains(Compiler.StateNodes, &Node))
		{
			Compiler.Warnings.push_back({.Severity = EGraphSeverity::Warning, .Node = Node.Id, .Message = "This state is not under the root, so it is left out"});
		}
	}

	if (Compiler.Tree.States.size() == 1)
	{
		Compiler.Warnings.push_back({.Severity = EGraphSeverity::Warning, .Node = Root->Id, .Message = "The root has no states to run"});
	}

	if (Compiler.Tree.Tasks.size() > MaximumStateTreeItems || Compiler.Tree.Conditions.size() > MaximumStateTreeItems || Compiler.Tree.Transitions.size() > MaximumStateTreeItems || Compiler.Tree.Names.size() > MaximumStateTreeNames)
	{
		Compiler.Errors.push_back({.Message = std::format("A StateTree holds at most {} tasks, conditions, and transitions, and {} names", MaximumStateTreeItems, MaximumStateTreeNames)});
	}

	if (!Compiler.Errors.empty())
	{
		return std::unexpected(std::move(Compiler.Errors));
	}

	std::vector<std::uint16_t> Active(Compiler.Tree.States.size(), 0);
	for (std::size_t Index = 0; Index < Compiler.Tree.States.size(); ++Index)
	{
		const FStateTreeStateDesc& State = Compiler.Tree.States[Index];
		Active[Index] = static_cast<std::uint16_t>(State.TaskCount + (State.Parent == StateTreeNone ? 0 : Active[State.Parent]));
		Compiler.Tree.MaximumActiveTasks = std::max(Compiler.Tree.MaximumActiveTasks, Active[Index]);
	}

	if (auto Valid = ValidateStateTree(Compiler.Tree); !Valid)
	{
		return std::unexpected(std::vector<FGraphDiagnostic>{{.Message = Valid.error().Message}});
	}

	Structural.insert(Structural.end(), Compiler.Warnings.begin(), Compiler.Warnings.end());
	return FStateTreeCompilation{.Tree = std::move(Compiler.Tree), .Diagnostics = std::move(Structural)};
}

FGraph MakeDefaultStateTreeGraph()
{
	FGraph Graph{.Domain = std::string(StateTreeDomain), .DomainVersion = StateTreeGraphVersion};
	const auto Add = [&Graph](const std::string_view Type, const std::array<float, 2> Position, const std::vector<FGraphProperty>& Overrides = {})
	{
		auto Node = MakeGraphNode(StateTreeSchema, Type, NextGraphNodeId(Graph), Position);
		for (const FGraphProperty& Override : Overrides)
		{
			std::ranges::find(Node->Properties, Override.Key, &FGraphProperty::Key)->Value = Override.Value;
		}

		Graph.Nodes.push_back(std::move(*Node));
		return Graph.Nodes.back().Id;
	};

	const auto Link = [&Graph](const FGraphNodeId From, const std::string_view FromKey, const FGraphNodeId To, const std::string_view ToKey)
	{
		Graph.Links.push_back({.From = {.Node = From, .Key = std::string(FromKey)}, .To = {.Node = To, .Key = std::string(ToKey)}});
	};

	const FGraphNodeId Root = Add("Root", {0.f, 0.f});
	const FGraphNodeId Idle = Add("State", {240.f, -80.f}, {{.Key = "name", .Value = std::string("Idle")}});
	const FGraphNodeId Wander = Add("State", {240.f, 120.f}, {{.Key = "name", .Value = std::string("Wander")}, {.Key = "order", .Value = std::int32_t{1}}});
	const FGraphNodeId Wait = Add("Wait", {480.f, -140.f}, {{.Key = "minimum", .Value = 1.f}, {.Key = "maximum", .Value = 3.f}});
	const FGraphNodeId ToWander = Add("Transition", {480.f, -40.f}, {{.Key = "target", .Value = std::string("Wander")}});
	const FGraphNodeId Walk = Add("Wander", {480.f, 80.f});
	const FGraphNodeId ToIdle = Add("Transition", {480.f, 180.f}, {{.Key = "target", .Value = std::string("Idle")}});
	Link(Root, "Children", Idle, "Parent");
	Link(Root, "Children", Wander, "Parent");
	Link(Idle, "Items", Wait, "State");
	Link(Idle, "Items", ToWander, "State");
	Link(Wander, "Items", Walk, "State");
	Link(Wander, "Items", ToIdle, "State");
	return Graph;
}
}
