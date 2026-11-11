#include "Herta/GameFramework/GameplayProgram.h"

#include <algorithm>

namespace Herta
{
namespace
{
using FStep = std::uint32_t (*)(FGameplayContext&, const FGameplayInstruction&, std::uint32_t);

FVector3 ReadVector(const FGameplayContext& Context, const std::uint16_t Slot)
{
	return {Context.Slots[Slot], Context.Slots[Slot + 1u], Context.Slots[Slot + 2u]};
}

template <EGameplayOp Op> void Apply(FGameplayContext& Context, const FGameplayInstruction& Instruction)
{
	float* const Slots = Context.Slots.data();
	if constexpr (Op == EGameplayOp::Add)
	{
		Slots[Instruction.Out] = Slots[Instruction.A] + Slots[Instruction.B];
	}
	else if constexpr (Op == EGameplayOp::Subtract)
	{
		Slots[Instruction.Out] = Slots[Instruction.A] - Slots[Instruction.B];
	}
	else if constexpr (Op == EGameplayOp::Multiply)
	{
		Slots[Instruction.Out] = Slots[Instruction.A] * Slots[Instruction.B];
	}
	else if constexpr (Op == EGameplayOp::Greater)
	{
		Slots[Instruction.Out] = Slots[Instruction.A] > Slots[Instruction.B] ? 1.f : 0.f;
	}
	else if constexpr (Op == EGameplayOp::Not)
	{
		Slots[Instruction.Out] = Slots[Instruction.A] == 0.f ? 1.f : 0.f;
	}
	else if constexpr (Op == EGameplayOp::Copy)
	{
		Slots[Instruction.Out] = Slots[Instruction.A];
	}
	else if constexpr (Op == EGameplayOp::SelfPosition)
	{
		Slots[Instruction.Out] = Context.SelfPosition.X;
		Slots[Instruction.Out + 1u] = Context.SelfPosition.Y;
		Slots[Instruction.Out + 2u] = Context.SelfPosition.Z;
	}
	else if constexpr (Op == EGameplayOp::DeltaSeconds)
	{
		Slots[Instruction.Out] = Context.DeltaSeconds;
	}
	else if constexpr (Op == EGameplayOp::PlayerDistance)
	{
		Slots[Instruction.Out] = Context.PlayerDistance;
	}
	else if constexpr (Op == EGameplayOp::PlaySound)
	{
		Context.Host->PlaySound(Context.Strings[Instruction.String], ReadVector(Context, Instruction.A));
	}
	else if constexpr (Op == EGameplayOp::SpawnEffect)
	{
		Context.Host->SpawnEffect(Context.Strings[Instruction.String], ReadVector(Context, Instruction.A), Slots[Instruction.B]);
	}
	else if constexpr (Op == EGameplayOp::Log)
	{
		Context.Host->Log(Context.Strings[Instruction.String], Slots[Instruction.A]);
	}
}

// Each step returns the next instruction; End returns past the code, which stops the run.
template <EGameplayOp Op> std::uint32_t Step(FGameplayContext& Context, const FGameplayInstruction& Instruction, const std::uint32_t Pc)
{
	if constexpr (Op == EGameplayOp::Branch)
	{
		return Context.Slots[Instruction.A] != 0.f ? Pc + 1 : Instruction.Jump;
	}
	else if constexpr (Op == EGameplayOp::Jump)
	{
		return Instruction.Jump;
	}
	else if constexpr (Op == EGameplayOp::End)
	{
		return UINT32_MAX;
	}
	else
	{
		Apply<Op>(Context, Instruction);
		return Pc + 1;
	}
}

// Indexed by EGameplayOp, so binding a program is one lookup per instruction.
constexpr std::array<FStep, static_cast<std::size_t>(EGameplayOp::Log) + 1> Steps{
    Step<EGameplayOp::Add>,
    Step<EGameplayOp::Subtract>,
    Step<EGameplayOp::Multiply>,
    Step<EGameplayOp::Greater>,
    Step<EGameplayOp::Not>,
    Step<EGameplayOp::Copy>,
    Step<EGameplayOp::SelfPosition>,
    Step<EGameplayOp::DeltaSeconds>,
    Step<EGameplayOp::PlayerDistance>,
    Step<EGameplayOp::Branch>,
    Step<EGameplayOp::Jump>,
    Step<EGameplayOp::End>,
    Step<EGameplayOp::PlaySound>,
    Step<EGameplayOp::SpawnEffect>,
    Step<EGameplayOp::Log>,
};
}

FGameplayProgram FGameplayProgram::Create(std::shared_ptr<const FCookedGameplayGraph> Graph, const EGameplayBackend Preferred)
{
	FGameplayProgram Program;
	if (Preferred == EGameplayBackend::Native)
	{
		const std::uint64_t Hash = HashGameplayGraph(*Graph);
		const auto Generated = std::ranges::find(GetGeneratedGameplayGraphs(), Hash, &FGeneratedGameplayGraph::Hash);
		Program.Native = Generated == GetGeneratedGameplayGraphs().end() ? nullptr : &*Generated;
	}

	Program.Steps.reserve(Graph->Code.size());
	for (const FGameplayInstruction& Instruction : Graph->Code)
	{
		Program.Steps.push_back(Herta::Steps[static_cast<std::size_t>(Instruction.Op)]);
	}

	Program.Graph = std::move(Graph);
	return Program;
}

std::vector<float> FGameplayProgram::MakeFrame() const
{
	return Graph->Slots;
}

bool FGameplayProgram::Handles(const EGameplayEvent Event) const
{
	return Graph->Entries[static_cast<std::size_t>(Event)] != GameplayNoEntry;
}

void FGameplayProgram::Run(const EGameplayEvent Event, FGameplayContext& Context) const
{
	const std::uint32_t Entry = Graph->Entries[static_cast<std::size_t>(Event)];
	if (Entry == GameplayNoEntry)
	{
		return;
	}

	Context.Strings = Graph->Strings;
	if (Native)
	{
		Native->Events[static_cast<std::size_t>(Event)](Context);
		return;
	}

	// Validation proved every jump goes forward and the code ends with End, so this finishes within the program's length.
	for (std::uint32_t Pc = Entry; Pc < Steps.size();)
	{
		if (Context.Trace)
		{
			Context.Trace->push_back(Graph->Code[Pc].Node);
		}

		Pc = Steps[Pc](Context, Graph->Code[Pc], Pc);
	}
}

EGameplayBackend FGameplayProgram::GetBackend() const
{
	return Native ? EGameplayBackend::Native : EGameplayBackend::CallList;
}

const FCookedGameplayGraph& FGameplayProgram::GetGraph() const
{
	return *Graph;
}
}
