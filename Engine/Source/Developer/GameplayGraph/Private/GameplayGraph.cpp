#include "Herta/GameplayGraph/GameplayGraph.h"

#include "Herta/Level/LevelSerialization.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

namespace Herta
{
namespace
{
enum EGameplayPin : std::uint32_t
{
	ExecPin,
	FloatPin,
	BoolPin,
	VectorPin,
};

constexpr std::array EventOutputs{FGraphPinDescriptor{.Key = "Then", .Name = "", .Type = ExecPin}};
constexpr std::array ActionOutputs{FGraphPinDescriptor{.Key = "Then", .Name = "", .Type = ExecPin}};
constexpr std::array BranchInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Condition", .Name = "Condition", .Type = BoolPin},
};
constexpr std::array BranchOutputs{
    FGraphPinDescriptor{.Key = "True", .Name = "True", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "False", .Name = "False", .Type = ExecPin},
};
constexpr std::array SequenceInputs{FGraphPinDescriptor{.Key = "In", .Name = "", .Type = ExecPin}};
constexpr std::array SequenceOutputs{
    FGraphPinDescriptor{.Key = "Then1", .Name = "Then 1", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Then2", .Name = "Then 2", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Then3", .Name = "Then 3", .Type = ExecPin},
};
constexpr std::array PlaySoundInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Position", .Name = "Position", .Type = VectorPin},
};
constexpr std::array SpawnEffectInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Position", .Name = "Position", .Type = VectorPin},
    FGraphPinDescriptor{.Key = "Scale", .Name = "Scale", .Type = FloatPin},
};
constexpr std::array ValueActionInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "", .Type = ExecPin},
    FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = FloatPin},
};
constexpr std::array FloatOutputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = FloatPin}};
constexpr std::array BoolOutputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = BoolPin}};
constexpr std::array VectorOutputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = VectorPin}};
constexpr std::array BinaryInputs{
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = FloatPin},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = FloatPin},
};
constexpr std::array NotInputs{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = BoolPin}};

const std::array PlaySoundProperties{FGraphPropertyDescriptor{.Key = "cue", .Name = "Sound", .Default = std::string("objective")}};
const std::array SpawnEffectProperties{
    FGraphPropertyDescriptor{.Key = "cue", .Name = "Effect", .Default = std::string("sparks")},
    FGraphPropertyDescriptor{.Key = "scale", .Name = "Scale", .Default = 1.f},
};
const std::array SetVariableProperties{
    FGraphPropertyDescriptor{.Key = "variable", .Name = "Variable", .Default = std::string("Count")},
    FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = 0.f},
};
const std::array GetVariableProperties{FGraphPropertyDescriptor{.Key = "variable", .Name = "Variable", .Default = std::string("Count")}};
const std::array LogProperties{FGraphPropertyDescriptor{.Key = "message", .Name = "Message", .Default = std::string("Value")}};
const std::array ConstantProperties{FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = 0.f}};
const std::array BinaryProperties{
    FGraphPropertyDescriptor{.Key = "a", .Name = "A", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "b", .Name = "B", .Default = 0.f},
};
const std::array MultiplyProperties{
    FGraphPropertyDescriptor{.Key = "a", .Name = "A", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "b", .Name = "B", .Default = 1.f},
};

const std::array GameplayNodes{
    FGraphNodeDescriptor{.Type = "OnStart", .Name = "On Start", .Category = "Events", .Outputs = EventOutputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "OnPlayerEnter", .Name = "On Player Enter", .Category = "Events", .Outputs = EventOutputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "OnPlayerExit", .Name = "On Player Exit", .Category = "Events", .Outputs = EventOutputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "OnTick", .Name = "On Tick", .Category = "Events", .Outputs = EventOutputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "Branch", .Name = "Branch", .Category = "Flow", .Inputs = BranchInputs, .Outputs = BranchOutputs},
    FGraphNodeDescriptor{.Type = "Sequence", .Name = "Sequence", .Category = "Flow", .Inputs = SequenceInputs, .Outputs = SequenceOutputs},
    FGraphNodeDescriptor{.Type = "PlaySound", .Name = "Play Sound", .Category = "Actions", .Inputs = PlaySoundInputs, .Outputs = ActionOutputs, .Properties = PlaySoundProperties},
    FGraphNodeDescriptor{.Type = "SpawnEffect", .Name = "Spawn Effect", .Category = "Actions", .Inputs = SpawnEffectInputs, .Outputs = ActionOutputs, .Properties = SpawnEffectProperties},
    FGraphNodeDescriptor{.Type = "SetVariable", .Name = "Set Variable", .Category = "Actions", .Inputs = ValueActionInputs, .Outputs = ActionOutputs, .Properties = SetVariableProperties},
    FGraphNodeDescriptor{.Type = "Log", .Name = "Log", .Category = "Actions", .Inputs = ValueActionInputs, .Outputs = ActionOutputs, .Properties = LogProperties},
    FGraphNodeDescriptor{.Type = "GetVariable", .Name = "Get Variable", .Category = "Values", .Outputs = FloatOutputs, .Properties = GetVariableProperties},
    FGraphNodeDescriptor{.Type = "Constant", .Name = "Constant", .Category = "Values", .Outputs = FloatOutputs, .Properties = ConstantProperties},
    FGraphNodeDescriptor{.Type = "SelfPosition", .Name = "Self Position", .Category = "Values", .Outputs = VectorOutputs},
    FGraphNodeDescriptor{.Type = "DeltaSeconds", .Name = "Delta Seconds", .Category = "Values", .Outputs = FloatOutputs},
    FGraphNodeDescriptor{.Type = "PlayerDistance", .Name = "Player Distance", .Category = "Values", .Outputs = FloatOutputs},
    FGraphNodeDescriptor{.Type = "Add", .Name = "Add", .Category = "Math", .Inputs = BinaryInputs, .Outputs = FloatOutputs, .Properties = BinaryProperties},
    FGraphNodeDescriptor{.Type = "Subtract", .Name = "Subtract", .Category = "Math", .Inputs = BinaryInputs, .Outputs = FloatOutputs, .Properties = BinaryProperties},
    FGraphNodeDescriptor{.Type = "Multiply", .Name = "Multiply", .Category = "Math", .Inputs = BinaryInputs, .Outputs = FloatOutputs, .Properties = MultiplyProperties},
    FGraphNodeDescriptor{.Type = "Greater", .Name = "Greater", .Category = "Math", .Inputs = BinaryInputs, .Outputs = BoolOutputs, .Properties = BinaryProperties},
    FGraphNodeDescriptor{.Type = "Not", .Name = "Not", .Category = "Math", .Inputs = NotInputs, .Outputs = BoolOutputs},
};

constexpr std::array<std::pair<std::string_view, EGameplayEvent>, GameplayEventCount> EventNodes{{
    {"OnStart", EGameplayEvent::Start},
    {"OnPlayerEnter", EGameplayEvent::PlayerEnter},
    {"OnPlayerExit", EGameplayEvent::PlayerExit},
    {"OnTick", EGameplayEvent::Tick},
}};

bool CanConnectGameplayPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To;
}

const FGraphSchema GameplaySchema{.Domain = GameplayGraphDomain, .Version = GameplayGraphVersion, .Nodes = GameplayNodes, .CanConnect = CanConnectGameplayPins};

[[nodiscard]] const FGraphValue& Property(const FGraphNode& Node, const std::string_view Key)
{
	return std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value;
}

// Deep enough for any graph a person lays out, and short of overflowing the compiler's stack on a generated one.
constexpr std::size_t MaximumGameplayDepth = 256;

class FGameplayCompiler
{
public:
	explicit FGameplayCompiler(const FGraph& InGraph)
	    : Graph(InGraph)
	{
		for (const FGraphLink& Link : Graph.Links)
		{
			Sources[{Link.To.Node.Value, Link.To.Key}] = Link.From;
			Targets[{Link.From.Node.Value, Link.From.Key}].push_back(Link.To);
		}
	}

	std::vector<FGraphDiagnostic> Compile(FCookedGameplayGraph& Out, std::vector<std::uint32_t>& OutReached)
	{
		for (const auto& [Type, Event] : EventNodes)
		{
			const auto Node = std::ranges::find(Graph.Nodes, Type, &FGraphNode::Type);
			if (Node == Graph.Nodes.end())
			{
				continue;
			}

			Program.Entries[static_cast<std::size_t>(Event)] = static_cast<std::uint32_t>(Program.Code.size());
			Follow(*Node, "Then", 0);
			Emit({.Op = EGameplayOp::End, .Node = Node->Id.Value});
		}

		if (Program.Code.empty())
		{
			Fail({}, "Add an event node, such as On Player Enter, to start the graph");
		}

		if (Program.Slots.size() > MaximumGameplaySlots || Program.Code.size() > MaximumGameplayInstructions || Program.Strings.size() > MaximumGameplayStrings)
		{
			Fail({}, std::format("The graph needs more than {} values, {} steps, or {} names", MaximumGameplaySlots, MaximumGameplayInstructions, MaximumGameplayStrings));
		}

		Out = std::move(Program);
		OutReached.assign(Reached.begin(), Reached.end());
		return std::move(Errors);
	}

private:
	void Fail(const FGraphNodeId Node, std::string Message)
	{
		// One diagnostic per node, since a bad node reached from two places would otherwise repeat itself.
		if (!std::ranges::contains(Errors, Node, &FGraphDiagnostic::Node))
		{
			Errors.push_back({.Node = Node, .Message = std::move(Message)});
		}
	}

	void Emit(const FGameplayInstruction& Instruction)
	{
		Program.Code.push_back(Instruction);
	}

	[[nodiscard]] std::uint16_t Slot(const std::size_t Width)
	{
		const std::size_t First = Program.Slots.size();
		Program.Slots.resize(First + Width, 0.f);
		return static_cast<std::uint16_t>(std::min<std::size_t>(First, UINT16_MAX - 2));
	}

	[[nodiscard]] std::uint16_t Constant(const float Value)
	{
		auto [Found, bAdded] = Constants.try_emplace(std::bit_cast<std::uint32_t>(Value), 0);
		if (bAdded)
		{
			Found->second = Slot(1);
			Program.Slots[Found->second] = Value;
		}

		return Found->second;
	}

	[[nodiscard]] std::uint16_t Variable(const std::string& Name)
	{
		auto [Found, bAdded] = Variables.try_emplace(Name, 0);
		if (bAdded)
		{
			Found->second = Slot(1);
		}

		return Found->second;
	}

	[[nodiscard]] std::uint16_t String(const std::string& Value)
	{
		const auto Found = std::ranges::find(Program.Strings, Value);
		if (Found != Program.Strings.end())
		{
			return static_cast<std::uint16_t>(Found - Program.Strings.begin());
		}

		Program.Strings.push_back(Value);
		return static_cast<std::uint16_t>(Program.Strings.size() - 1);
	}

	// Each pure node keeps one slot for its output, computed again wherever an action reads it.
	[[nodiscard]] std::uint16_t Output(const FGraphNode& Node, const std::size_t Width)
	{
		auto [Found, bAdded] = Outputs.try_emplace(Node.Id.Value, 0);
		if (bAdded)
		{
			Found->second = Slot(Width);
		}

		return Found->second;
	}

	[[nodiscard]] const FGraphNode* Source(const FGraphNode& Node, const std::string_view Key) const
	{
		const auto Found = Sources.find({Node.Id.Value, std::string(Key)});
		return Found == Sources.end() ? nullptr : FindGraphNode(Graph, Found->second.Node);
	}

	// The input's linked value, or the node's own property, or zero, whichever comes first.
	[[nodiscard]] std::uint16_t Input(const FGraphNode& Node, const std::string_view Key, const std::string_view Fallback, const std::size_t Depth)
	{
		if (const FGraphNode* const From = Source(Node, Key))
		{
			return Evaluate(*From, Depth + 1);
		}

		const auto Found = std::ranges::find(Node.Properties, Fallback, &FGraphProperty::Key);
		return Constant(Found != Node.Properties.end() ? std::get<float>(Found->Value) : 0.f);
	}

	[[nodiscard]] std::uint16_t Evaluate(const FGraphNode& Node, const std::size_t Depth)
	{
		if (Depth > MaximumGameplayDepth)
		{
			Fail(Node.Id, "The values feeding this node nest too deeply");
			return Constant(0.f);
		}

		if (const auto Found = Evaluated.find(Node.Id.Value); Found != Evaluated.end())
		{
			return Found->second;
		}

		Reached.insert(Node.Id.Value);
		const std::uint32_t Id = Node.Id.Value;
		std::uint16_t Result = 0;
		if (Node.Type == "GetVariable")
		{
			Result = Variable(std::get<std::string>(Property(Node, "variable")));
		}
		else if (Node.Type == "Constant")
		{
			Result = Constant(std::get<float>(Property(Node, "value")));
		}
		else if (Node.Type == "SelfPosition" || Node.Type == "DeltaSeconds" || Node.Type == "PlayerDistance")
		{
			const EGameplayOp Op = Node.Type == "SelfPosition" ? EGameplayOp::SelfPosition : Node.Type == "DeltaSeconds" ? EGameplayOp::DeltaSeconds
			                                                                                                             : EGameplayOp::PlayerDistance;
			Result = Output(Node, Op == EGameplayOp::SelfPosition ? 3 : 1);
			Emit({.Op = Op, .Out = Result, .Node = Id});
		}
		else if (Node.Type == "Not")
		{
			const std::uint16_t Value = Input(Node, "Value", {}, Depth);
			Result = Output(Node, 1);
			Emit({.Op = EGameplayOp::Not, .A = Value, .Out = Result, .Node = Id});
		}
		else
		{
			const EGameplayOp Op = Node.Type == "Add" ? EGameplayOp::Add : Node.Type == "Subtract" ? EGameplayOp::Subtract
			                                                           : Node.Type == "Multiply"   ? EGameplayOp::Multiply
			                                                                                       : EGameplayOp::Greater;
			const std::uint16_t A = Input(Node, "A", "a", Depth);
			const std::uint16_t B = Input(Node, "B", "b", Depth);
			Result = Output(Node, 1);
			Emit({.Op = Op, .A = A, .B = B, .Out = Result, .Node = Id});
		}

		Evaluated[Id] = Result;
		return Result;
	}

	// Runs whatever the execution output links to: nothing, or exactly one node.
	void Follow(const FGraphNode& Node, const std::string_view Key, const std::size_t Depth)
	{
		const auto Found = Targets.find({Node.Id.Value, std::string(Key)});
		if (Found == Targets.end())
		{
			return;
		}

		if (Found->second.size() > 1)
		{
			Fail(Node.Id, "An execution output runs one node; use a Sequence to run several in order");
			return;
		}

		if (const FGraphNode* const Next = FindGraphNode(Graph, Found->second.front().Node))
		{
			Run(*Next, Depth + 1);
		}
	}

	void Run(const FGraphNode& Node, const std::size_t Depth)
	{
		if (Depth > MaximumGameplayDepth || Program.Code.size() > MaximumGameplayInstructions)
		{
			Fail(Node.Id, "The graph runs too many steps from one event");
			return;
		}

		// Values are computed again for each action, so they see what earlier actions set.
		Evaluated.clear();
		Reached.insert(Node.Id.Value);
		const std::uint32_t Id = Node.Id.Value;
		if (Node.Type == "Branch")
		{
			const std::uint16_t Condition = Input(Node, "Condition", {}, Depth);
			const std::size_t Branch = Program.Code.size();
			Emit({.Op = EGameplayOp::Branch, .A = Condition, .Node = Id});
			Follow(Node, "True", Depth);
			// The true side always ends by jumping over the false side, which code generation relies on to rebuild the if and else.
			const std::size_t Skip = Program.Code.size();
			Emit({.Op = EGameplayOp::Jump, .Node = Id});
			Program.Code[Branch].Jump = static_cast<std::uint32_t>(Program.Code.size());
			Follow(Node, "False", Depth);
			Program.Code[Skip].Jump = static_cast<std::uint32_t>(Program.Code.size());
			return;
		}

		if (Node.Type == "Sequence")
		{
			for (const std::string_view Key : {"Then1", "Then2", "Then3"})
			{
				Follow(Node, Key, Depth);
			}

			return;
		}

		if (Node.Type == "PlaySound" || Node.Type == "SpawnEffect")
		{
			const bool bSound = Node.Type == "PlaySound";
			const std::string& Cue = std::get<std::string>(Property(Node, "cue"));
			const bool bKnown = bSound ? std::ranges::contains(LevelSoundCues, Cue, &FLevelSoundCue::Key) : std::ranges::contains(LevelEffectCues, Cue, &FLevelSoundCue::Key);
			if (!bKnown)
			{
				Fail(Node.Id, std::format("'{}' is not a level {}", Cue, bSound ? "sound" : "effect"));
			}

			std::uint16_t Position = 0;
			if (const FGraphNode* const From = Source(Node, "Position"))
			{
				Position = Evaluate(*From, Depth + 1);
			}
			else
			{
				// Every action placed at the entity shares one vector, refreshed before each use.
				SelfSlot = SelfSlot ? SelfSlot : Slot(3);
				Position = *SelfSlot;
				Emit({.Op = EGameplayOp::SelfPosition, .Out = Position, .Node = Id});
			}

			const std::uint16_t Scale = bSound ? 0 : Input(Node, "Scale", "scale", Depth);
			Emit({.Op = bSound ? EGameplayOp::PlaySound : EGameplayOp::SpawnEffect, .A = Position, .B = Scale, .String = String(Cue), .Node = Id});
		}
		else if (Node.Type == "SetVariable")
		{
			const std::uint16_t Value = Input(Node, "Value", "value", Depth);
			Emit({.Op = EGameplayOp::Copy, .A = Value, .Out = Variable(std::get<std::string>(Property(Node, "variable"))), .Node = Id});
		}
		else if (Node.Type == "Log")
		{
			const std::uint16_t Value = Input(Node, "Value", {}, Depth);
			Emit({.Op = EGameplayOp::Log, .A = Value, .String = String(std::get<std::string>(Property(Node, "message"))), .Node = Id});
		}

		Follow(Node, "Then", Depth);
	}

	const FGraph& Graph;
	std::map<std::pair<std::uint32_t, std::string>, FGraphPin> Sources{};
	std::map<std::pair<std::uint32_t, std::string>, std::vector<FGraphPin>> Targets{};

	FCookedGameplayGraph Program{};
	std::map<std::uint32_t, std::uint16_t> Constants{};
	std::map<std::string, std::uint16_t> Variables{};
	std::map<std::uint32_t, std::uint16_t> Outputs{};
	std::optional<std::uint16_t> SelfSlot{};
	std::map<std::uint32_t, std::uint16_t> Evaluated{};
	std::set<std::uint32_t> Reached{};
	std::vector<FGraphDiagnostic> Errors{};
};

[[nodiscard]] std::string Slot(const std::uint16_t Index, const std::uint16_t Offset = 0)
{
	return std::format("S[{}]", Index + Offset);
}

// Rebuilds the if and else of each Branch from the shape the compiler emits, so the generated code needs no labels or jumps.
class FGameplayGenerator
{
public:
	FGameplayGenerator(const FCookedGameplayGraph& InProgram, std::string& InOut)
	    : Program(InProgram)
	    , Out(InOut)
	{
	}

	[[nodiscard]] std::expected<void, std::string> Block(std::uint32_t Pc, const std::uint32_t End, const std::size_t Indent)
	{
		const std::string Tabs(Indent, '\t');
		while (Pc < End)
		{
			const FGameplayInstruction& Instruction = Program.Code[Pc];
			switch (Instruction.Op)
			{
				case EGameplayOp::Branch:
				{
					const std::uint32_t Else = Instruction.Jump;
					const FGameplayInstruction& Skip = Program.Code[Else - 1];
					if (Else <= Pc + 1 || Skip.Op != EGameplayOp::Jump || Skip.Jump < Else || Skip.Jump > End)
					{
						return std::unexpected(std::format("instruction {} is a branch the generator cannot rebuild", Pc));
					}

					const bool bThen = Else - 1 > Pc + 1;
					const bool bElse = Skip.Jump > Else;
					if (bThen || bElse)
					{
						Out += std::format("{}if ({} {} 0.f)\n{}{{\n", Tabs, Slot(Instruction.A), bThen ? "!=" : "==", Tabs);
						if (auto Inner = bThen ? Block(Pc + 1, Else - 1, Indent + 1) : Block(Else, Skip.Jump, Indent + 1); !Inner)
						{
							return Inner;
						}

						Out += std::format("{}}}\n", Tabs);
						if (bThen && bElse)
						{
							Out += std::format("{}else\n{}{{\n", Tabs, Tabs);
							if (auto Inner = Block(Else, Skip.Jump, Indent + 1); !Inner)
							{
								return Inner;
							}

							Out += std::format("{}}}\n", Tabs);
						}

						if (Skip.Jump < End)
						{
							Out += '\n';
						}
					}

					Pc = Skip.Jump;
					continue;
				}
				case EGameplayOp::Jump:
				case EGameplayOp::End:
					return std::unexpected(std::format("instruction {} jumps or ends inside a block", Pc));
				default:
					Out += Tabs + Statement(Instruction, Tabs) + '\n';
					break;
			}

			++Pc;
		}

		return {};
	}

private:
	[[nodiscard]] static std::string Statement(const FGameplayInstruction& Instruction, const std::string& Tabs)
	{
		const std::string A = Slot(Instruction.A);
		const std::string B = Slot(Instruction.B);
		const std::string Out = Slot(Instruction.Out);
		const std::string Vector = std::format("FVector3{{{}, {}, {}}}", A, Slot(Instruction.A, 1), Slot(Instruction.A, 2));
		switch (Instruction.Op)
		{
			case EGameplayOp::Add:
				return std::format("{} = {} + {};", Out, A, B);
			case EGameplayOp::Subtract:
				return std::format("{} = {} - {};", Out, A, B);
			case EGameplayOp::Multiply:
				return std::format("{} = {} * {};", Out, A, B);
			case EGameplayOp::Greater:
				return std::format("{} = {} > {} ? 1.f : 0.f;", Out, A, B);
			case EGameplayOp::Not:
				return std::format("{} = {} == 0.f ? 1.f : 0.f;", Out, A);
			case EGameplayOp::Copy:
				return std::format("{} = {};", Out, A);
			case EGameplayOp::SelfPosition:
				return std::format("{} = Context.SelfPosition.X;\n{}{} = Context.SelfPosition.Y;\n{}{} = Context.SelfPosition.Z;", Out, Tabs, Slot(Instruction.Out, 1), Tabs, Slot(Instruction.Out, 2));
			case EGameplayOp::DeltaSeconds:
				return std::format("{} = Context.DeltaSeconds;", Out);
			case EGameplayOp::PlayerDistance:
				return std::format("{} = Context.PlayerDistance;", Out);
			case EGameplayOp::PlaySound:
				return std::format("Context.Host->PlaySound(Context.Strings[{}], {});", Instruction.String, Vector);
			case EGameplayOp::SpawnEffect:
				return std::format("Context.Host->SpawnEffect(Context.Strings[{}], {}, {});", Instruction.String, Vector, B);
			case EGameplayOp::Log:
				return std::format("Context.Host->Log(Context.Strings[{}], {});", Instruction.String, A);
			default:
				return {};
		}
	}

	const FCookedGameplayGraph& Program;
	std::string& Out;
};
}

const FGraphSchema& GetGameplayGraphSchema()
{
	return GameplaySchema;
}

std::expected<FGameplayGraphCompilation, std::vector<FGraphDiagnostic>> CompileGameplayGraph(const FGraph& Graph)
{
	std::vector<FGraphDiagnostic> Errors;
	for (FGraphDiagnostic& Diagnostic : ValidateGraph(Graph, GameplaySchema))
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

	FGameplayGraphCompilation Result;
	std::vector<std::uint32_t> Reached;
	Errors = FGameplayCompiler(Graph).Compile(Result.Program, Reached);
	if (Errors.empty())
	{
		if (auto Valid = ValidateGameplayGraph(Result.Program); !Valid)
		{
			Errors.push_back({.Message = Valid.error().Message});
		}
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	// Nodes no event reaches never run, which is usually a wire someone forgot.
	for (const FGraphNode& Node : Graph.Nodes)
	{
		const bool bEvent = std::ranges::contains(EventNodes, std::string_view(Node.Type), &std::pair<std::string_view, EGameplayEvent>::first);
		if (!bEvent && !std::ranges::binary_search(Reached, Node.Id.Value))
		{
			Result.Diagnostics.push_back({.Severity = EGraphSeverity::Warning, .Node = Node.Id, .Message = "No event reaches this node, so it never runs"});
		}
	}

	return Result;
}

FGraph MakeDefaultGameplayGraph()
{
	FGraph Graph{.Domain = std::string(GameplayGraphDomain), .DomainVersion = GameplayGraphVersion};
	Graph.Nodes.push_back(*MakeGraphNode(GameplaySchema, "OnPlayerEnter", {1}, {0.f, 0.f}));
	Graph.Nodes.push_back(*MakeGraphNode(GameplaySchema, "PlaySound", {2}, {300.f, 0.f}));
	Graph.Links.push_back({.From = {.Node = {1}, .Key = "Then"}, .To = {.Node = {2}, .Key = "In"}});
	return Graph;
}

std::expected<std::vector<FGameplayGraphSource>, std::string> CollectGameplayGraphs(const std::filesystem::path& ContentRoot)
{
	std::vector<std::filesystem::path> Paths;
	std::error_code Error;
	for (auto Entry = std::filesystem::recursive_directory_iterator(ContentRoot, Error); !Error && Entry != std::filesystem::recursive_directory_iterator(); Entry.increment(Error))
	{
		if (Entry->is_regular_file(Error) && Entry->path().extension() == GameplayGraphExtension)
		{
			Paths.push_back(Entry->path().lexically_relative(ContentRoot));
		}
	}

	if (Error)
	{
		return std::unexpected(std::format("Cannot list {}: {}", ContentRoot.generic_string(), Error.message()));
	}

	std::ranges::sort(Paths);
	std::vector<FGameplayGraphSource> Sources;
	for (const std::filesystem::path& Path : Paths)
	{
		std::ifstream Stream(ContentRoot / Path, std::ios::binary);
		const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
		const std::string Name = Path.generic_string();
		const auto Graph = DeserializeGraph(Text, GameplaySchema);
		if (!Graph)
		{
			return std::unexpected(std::format("{}: {}", Name, Graph.error().Message));
		}

		auto Compiled = CompileGameplayGraph(*Graph);
		if (!Compiled)
		{
			return std::unexpected(std::format("{}: {}", Name, Compiled.error().front().Message));
		}

		Sources.push_back({.Name = Name, .Program = std::move(Compiled->Program)});
	}

	return Sources;
}

std::expected<std::string, std::string> GenerateGameplayGraphCpp(const std::span<const FGameplayGraphSource> Sources, const std::string_view Function)
{
	const bool bValidName = !Function.empty() && !std::isdigit(static_cast<unsigned char>(Function.front())) && std::ranges::all_of(Function, [](const unsigned char Character)
	{
		return std::isalnum(Character) || Character == '_';
	});

	if (!bValidName)
	{
		return std::unexpected(std::format("'{}' is not a C++ function name", Function));
	}

	std::string Out = "// Generated by HertaEditorCmd gameplay.generate from the gameplay graphs it was given. Do not edit; generate it again.\n"
	                  "#include \"Herta/GameFramework/GameplayProgram.h\"\n\n"
	                  "namespace Herta\n{\n";
	std::string Table;
	if (!Sources.empty())
	{
		Out += "namespace\n{\n";
	}

	for (std::size_t Index = 0; Index < Sources.size(); ++Index)
	{
		const FCookedGameplayGraph& Program = Sources[Index].Program;
		if (auto Valid = ValidateGameplayGraph(Program); !Valid)
		{
			return std::unexpected(std::format("{}: {}", Sources[Index].Name, Valid.error().Message));
		}

		std::array<std::string, GameplayEventCount> Functions;
		for (std::size_t Event = 0; Event < GameplayEventCount; ++Event)
		{
			const std::uint32_t Entry = Program.Entries[Event];
			if (Entry == GameplayNoEntry)
			{
				Functions[Event] = "nullptr";
				continue;
			}

			// An event's code runs from its entry to the End that closes it; branches inside it only jump within that stretch.
			std::uint32_t Close = Entry;
			while (Program.Code[Close].Op != EGameplayOp::End)
			{
				Close = Program.Code[Close].Op == EGameplayOp::Branch ? Program.Code[Program.Code[Close].Jump - 1].Jump : Close + 1;
			}

			Functions[Event] = std::format("Program{}Event{}", Index, Event);
			if (!Out.ends_with("{\n"))
			{
				Out += '\n';
			}

			Out += std::format("// {}\nvoid {}(FGameplayContext& Context)\n{{\n", Sources[Index].Name, Functions[Event]);
			if (Close > Entry)
			{
				Out += "\tfloat* const S = Context.Slots.data();\n";
			}

			if (auto Generated = FGameplayGenerator(Program, Out).Block(Entry, Close, 1); !Generated)
			{
				return std::unexpected(std::format("{}: {}", Sources[Index].Name, Generated.error()));
			}

			Out += "}\n";
		}

		Table += std::format("\t    {{.Hash = 0x{:016x}ull, .Events = {{{}, {}, {}, {}}}}},\n", HashGameplayGraph(Program), Functions[0], Functions[1], Functions[2], Functions[3]);
	}

	if (!Sources.empty())
	{
		Out += "}\n\n";
	}

	Out += std::format("std::span<const FGeneratedGameplayGraph> {}()\n{{\n\tstatic constexpr std::array<FGeneratedGameplayGraph, {}> Graphs{{", Function, Sources.size());
	Out += Sources.empty() ? "};\n" : std::format("{{\n{}\t}}}};\n", Table);
	Out += "\n\treturn Graphs;\n}\n}\n";
	return Out;
}
}
