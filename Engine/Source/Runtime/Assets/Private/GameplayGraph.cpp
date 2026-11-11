#include "GameplayGraphSerialization.h"

#include <cmath>
#include <format>

namespace Herta
{
namespace
{
[[nodiscard]] std::unexpected<FAssetError> Invalid(const std::size_t Index, const std::string_view Message)
{
	return std::unexpected(FAssetError{std::format("Invalid gameplay graph: instruction {} {}", Index, Message)});
}

[[nodiscard]] std::size_t ReadCount(FBinaryReader& Reader, const std::size_t Maximum)
{
	const auto Count = Reader.Read<std::uint32_t>();
	if (Count > Maximum)
	{
		Reader.Invalidate();
		return 0;
	}

	return Count;
}

struct FOperands
{
	std::uint8_t A = 0;
	std::uint8_t B = 0;
	std::uint8_t Out = 0;
	bool bString = false;
	bool bJump = false;
};

// How many slots each operand reads or writes, so validation proves every access stays inside the frame.
[[nodiscard]] FOperands GetOperands(const EGameplayOp Op)
{
	switch (Op)
	{
		case EGameplayOp::Add:
		case EGameplayOp::Subtract:
		case EGameplayOp::Multiply:
		case EGameplayOp::Greater:
			return {.A = 1, .B = 1, .Out = 1};
		case EGameplayOp::Not:
		case EGameplayOp::Copy:
			return {.A = 1, .Out = 1};
		case EGameplayOp::SelfPosition:
			return {.Out = 3};
		case EGameplayOp::DeltaSeconds:
		case EGameplayOp::PlayerDistance:
			return {.Out = 1};
		case EGameplayOp::Branch:
			return {.A = 1, .bJump = true};
		case EGameplayOp::Jump:
			return {.bJump = true};
		case EGameplayOp::End:
			return {};
		case EGameplayOp::PlaySound:
			return {.A = 3, .bString = true};
		case EGameplayOp::SpawnEffect:
			return {.A = 3, .B = 1, .bString = true};
		case EGameplayOp::Log:
			return {.A = 1, .bString = true};
	}

	return {};
}
}

std::expected<void, FAssetError> ValidateGameplayGraph(const FCookedGameplayGraph& Graph)
{
	if (Graph.Slots.size() > MaximumGameplaySlots || Graph.Code.empty() || Graph.Code.size() > MaximumGameplayInstructions || Graph.Strings.size() > MaximumGameplayStrings)
	{
		return std::unexpected(FAssetError{"Invalid gameplay graph: too many slots, instructions, or strings, or no code"});
	}

	for (const float Value : Graph.Slots)
	{
		if (!std::isfinite(Value))
		{
			return std::unexpected(FAssetError{"Invalid gameplay graph: a slot starts at a nonfinite value"});
		}
	}

	for (const std::uint32_t Entry : Graph.Entries)
	{
		if (Entry != GameplayNoEntry && Entry >= Graph.Code.size())
		{
			return std::unexpected(FAssetError{"Invalid gameplay graph: an event enters past the code"});
		}
	}

	for (std::size_t Index = 0; Index < Graph.Code.size(); ++Index)
	{
		const FGameplayInstruction& Instruction = Graph.Code[Index];
		if (Instruction.Op > EGameplayOp::Log)
		{
			return Invalid(Index, "has an unknown operation");
		}

		const FOperands Operands = GetOperands(Instruction.Op);
		const auto Fits = [&Graph](const std::uint16_t Slot, const std::uint8_t Width)
		{
			return Width == 0 || static_cast<std::size_t>(Slot) + Width <= Graph.Slots.size();
		};

		if (!Fits(Instruction.A, Operands.A) || !Fits(Instruction.B, Operands.B) || !Fits(Instruction.Out, Operands.Out))
		{
			return Invalid(Index, "reaches past the frame");
		}

		if (Operands.bString && Instruction.String >= Graph.Strings.size())
		{
			return Invalid(Index, "names a missing string");
		}

		// Forward jumps only, and the last instruction ends, so every run finishes within the program's length.
		if (Operands.bJump && (Instruction.Jump <= Index || Instruction.Jump >= Graph.Code.size()))
		{
			return Invalid(Index, "jumps backward or past the code");
		}
	}

	if (Graph.Code.back().Op != EGameplayOp::End)
	{
		return std::unexpected(FAssetError{"Invalid gameplay graph: the code must finish with End"});
	}

	return {};
}

std::uint64_t HashGameplayGraph(const FCookedGameplayGraph& Graph)
{
	FBinaryWriter Writer;
	WriteGameplayGraph(Writer, Graph);
	// FNV-1a: stable across platforms and compilers, which the generated code's identity needs, and not a security boundary.
	std::uint64_t Hash = 14695981039346656037ull;
	for (const std::byte Byte : Writer.GetBytes())
	{
		Hash = (Hash ^ static_cast<std::uint64_t>(Byte)) * 1099511628211ull;
	}

	return Hash;
}

void WriteGameplayGraph(FBinaryWriter& Writer, const FCookedGameplayGraph& Graph)
{
	Writer.Write(static_cast<std::uint32_t>(Graph.Slots.size()));
	for (const float Value : Graph.Slots)
	{
		Writer.WriteFloat(Value);
	}

	for (const std::uint32_t Entry : Graph.Entries)
	{
		Writer.Write(Entry);
	}

	Writer.Write(static_cast<std::uint32_t>(Graph.Code.size()));
	for (const FGameplayInstruction& Instruction : Graph.Code)
	{
		Writer.Write(static_cast<std::uint8_t>(Instruction.Op));
		Writer.Write(Instruction.A);
		Writer.Write(Instruction.B);
		Writer.Write(Instruction.Out);
		Writer.Write(Instruction.String);
		Writer.Write(Instruction.Jump);
		Writer.Write(Instruction.Node);
	}

	Writer.Write(static_cast<std::uint32_t>(Graph.Strings.size()));
	for (const std::string& String : Graph.Strings)
	{
		Writer.WriteString(String);
	}
}

FCookedGameplayGraph ReadGameplayGraph(FBinaryReader& Reader)
{
	FCookedGameplayGraph Graph;
	Graph.Slots.resize(ReadCount(Reader, MaximumGameplaySlots));
	for (float& Value : Graph.Slots)
	{
		Value = Reader.ReadFloat();
	}

	for (std::uint32_t& Entry : Graph.Entries)
	{
		Entry = Reader.Read<std::uint32_t>();
	}

	Graph.Code.resize(ReadCount(Reader, MaximumGameplayInstructions));
	for (FGameplayInstruction& Instruction : Graph.Code)
	{
		Instruction.Op = static_cast<EGameplayOp>(Reader.Read<std::uint8_t>());
		Instruction.A = Reader.Read<std::uint16_t>();
		Instruction.B = Reader.Read<std::uint16_t>();
		Instruction.Out = Reader.Read<std::uint16_t>();
		Instruction.String = Reader.Read<std::uint16_t>();
		Instruction.Jump = Reader.Read<std::uint32_t>();
		Instruction.Node = Reader.Read<std::uint32_t>();
	}

	Graph.Strings.resize(ReadCount(Reader, MaximumGameplayStrings));
	for (std::string& String : Graph.Strings)
	{
		String = Reader.ReadString(256);
	}

	return Graph;
}
}
