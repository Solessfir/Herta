#include "Herta/TextLayout/TextLayout.h"

#include <UnicodeData.h>

#include <algorithm>

namespace Herta
{
namespace
{
using Unicode::EGraphemeBreak;
using Unicode::EIndicConjunct;

constexpr Unicode::FGraphemeRange OtherRange{0, 0, EGraphemeBreak::Other, false, EIndicConjunct::None};

[[nodiscard]] const Unicode::FGraphemeRange& GetProperties(const char32_t CodePoint) noexcept
{
	const auto Found = std::ranges::upper_bound(Unicode::GraphemeRanges, CodePoint, {}, &Unicode::FGraphemeRange::First);
	if (Found == Unicode::GraphemeRanges.begin() || std::prev(Found)->Last < CodePoint)
	{
		return OtherRange;
	}

	return *std::prev(Found);
}

// The rules' memory of what came before the current boundary: emoji ZWJ sequences, regional indicator pairs, and Indic conjuncts.
struct FBreakState
{
	EGraphemeBreak Previous = EGraphemeBreak::Other;
	// 1 after an Extended_Pictographic and any Extend, 2 after a ZWJ that follows that.
	int Emoji = 0;
	int RegionalIndicators = 0;
	// 1 after an InCB consonant and any InCB extends, 2 once an InCB linker follows it.
	int Conjunct = 0;
};

// Whether there is a cluster boundary before a code point with Properties, following GB3 to GB999.
[[nodiscard]] bool IsBoundary(const FBreakState& State, const Unicode::FGraphemeRange& Properties) noexcept
{
	const EGraphemeBreak Previous = State.Previous;
	const EGraphemeBreak Current = Properties.Break;
	if (Previous == EGraphemeBreak::CR && Current == EGraphemeBreak::LF)
	{
		return false;
	}

	const auto IsControl = [](const EGraphemeBreak Break)
	{
		return Break == EGraphemeBreak::Control || Break == EGraphemeBreak::CR || Break == EGraphemeBreak::LF;
	};

	if (IsControl(Previous) || IsControl(Current))
	{
		return true;
	}

	if (Previous == EGraphemeBreak::L && (Current == EGraphemeBreak::L || Current == EGraphemeBreak::V || Current == EGraphemeBreak::LV || Current == EGraphemeBreak::LVT))
	{
		return false;
	}

	if ((Previous == EGraphemeBreak::LV || Previous == EGraphemeBreak::V) && (Current == EGraphemeBreak::V || Current == EGraphemeBreak::T))
	{
		return false;
	}

	if ((Previous == EGraphemeBreak::LVT || Previous == EGraphemeBreak::T) && Current == EGraphemeBreak::T)
	{
		return false;
	}

	if (Current == EGraphemeBreak::Extend || Current == EGraphemeBreak::ZWJ || Current == EGraphemeBreak::SpacingMark || Previous == EGraphemeBreak::Prepend)
	{
		return false;
	}

	if (State.Conjunct == 2 && Properties.Conjunct == EIndicConjunct::Consonant)
	{
		return false;
	}

	if (State.Emoji == 2 && Properties.bExtendedPictographic)
	{
		return false;
	}

	return !(Previous == EGraphemeBreak::Regional_Indicator && Current == EGraphemeBreak::Regional_Indicator && State.RegionalIndicators % 2 == 1);
}

void Advance(FBreakState& State, const Unicode::FGraphemeRange& Properties) noexcept
{
	State.Emoji = Properties.bExtendedPictographic                                       ? 1
	            : State.Emoji == 1 && Properties.Break == EGraphemeBreak::Extend ? 1
	            : State.Emoji == 1 && Properties.Break == EGraphemeBreak::ZWJ    ? 2
	                                                                             : 0;
	State.RegionalIndicators = Properties.Break == EGraphemeBreak::Regional_Indicator ? State.RegionalIndicators + 1 : 0;
	State.Conjunct = Properties.Conjunct == EIndicConjunct::Consonant                 ? 1
	               : State.Conjunct != 0 && Properties.Conjunct == EIndicConjunct::Linker ? 2
	               : State.Conjunct != 0 && Properties.Conjunct == EIndicConjunct::Extend ? State.Conjunct
	                                                                                      : 0;
	State.Previous = Properties.Break;
}
}

FUtf8CodePoint DecodeUtf8(const std::string_view Text, const std::size_t Offset) noexcept
{
	constexpr FUtf8CodePoint Invalid{0xFFFD, 1};
	if (Offset >= Text.size())
	{
		return Invalid;
	}

	const auto Byte = [&Text, Offset](const std::size_t Index)
	{
		return Offset + Index < Text.size() ? static_cast<std::uint8_t>(Text[Offset + Index]) : std::uint8_t{0};
	};

	const std::uint8_t Lead = Byte(0);
	const std::uint32_t Length = Lead < 0x80 ? 1 : (Lead & 0xE0) == 0xC0 ? 2 : (Lead & 0xF0) == 0xE0 ? 3 : (Lead & 0xF8) == 0xF0 ? 4 : 0;
	if (Length == 0)
	{
		return Invalid;
	}

	char32_t Value = Length == 1 ? Lead : Lead & (0x7F >> Length);
	for (std::uint32_t Index = 1; Index < Length; ++Index)
	{
		if ((Byte(Index) & 0xC0) != 0x80)
		{
			return Invalid;
		}

		Value = Value << 6 | (Byte(Index) & 0x3F);
	}

	// Overlong forms, surrogates, and values past Unicode are malformed.
	constexpr char32_t Minimums[]{0, 0, 0x80, 0x800, 0x10000};
	if (Value < Minimums[Length] || Value > 0x10FFFF || (Value >= 0xD800 && Value <= 0xDFFF))
	{
		return Invalid;
	}

	return {Value, Length};
}

void AppendUtf8(std::string& Text, char32_t CodePoint)
{
	if (CodePoint > 0x10FFFF || (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
	{
		CodePoint = 0xFFFD;
	}

	if (CodePoint < 0x80)
	{
		Text.push_back(static_cast<char>(CodePoint));
	}
	else if (CodePoint < 0x800)
	{
		Text.push_back(static_cast<char>(0xC0 | CodePoint >> 6));
		Text.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
	else if (CodePoint < 0x10000)
	{
		Text.push_back(static_cast<char>(0xE0 | CodePoint >> 12));
		Text.push_back(static_cast<char>(0x80 | (CodePoint >> 6 & 0x3F)));
		Text.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
	else
	{
		Text.push_back(static_cast<char>(0xF0 | CodePoint >> 18));
		Text.push_back(static_cast<char>(0x80 | (CodePoint >> 12 & 0x3F)));
		Text.push_back(static_cast<char>(0x80 | (CodePoint >> 6 & 0x3F)));
		Text.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
}

std::size_t GetNextGrapheme(const std::string_view Text, const std::size_t Offset) noexcept
{
	if (Offset >= Text.size())
	{
		return Text.size();
	}

	FBreakState State;
	const FUtf8CodePoint First = DecodeUtf8(Text, Offset);
	Advance(State, GetProperties(First.Value));
	std::size_t Cursor = Offset + First.Length;
	while (Cursor < Text.size())
	{
		const FUtf8CodePoint CodePoint = DecodeUtf8(Text, Cursor);
		const auto& Properties = GetProperties(CodePoint.Value);
		if (IsBoundary(State, Properties))
		{
			break;
		}

		Advance(State, Properties);
		Cursor += CodePoint.Length;
	}

	return Cursor;
}

std::size_t GetPreviousGrapheme(const std::string_view Text, const std::size_t Offset) noexcept
{
	// Boundaries depend on everything since the last one, so walk forward from the start; a field's text is short.
	std::size_t Previous = 0;
	for (std::size_t Cursor = 0; Cursor < std::min(Offset, Text.size());)
	{
		Previous = Cursor;
		Cursor = GetNextGrapheme(Text, Cursor);
	}

	return Previous;
}

std::size_t CountGraphemes(const std::string_view Text) noexcept
{
	std::size_t Count = 0;
	for (std::size_t Cursor = 0; Cursor < Text.size(); Cursor = GetNextGrapheme(Text, Cursor))
	{
		++Count;
	}

	return Count;
}

void InsertText(FTextEdit& Edit, const std::string_view Inserted, const std::size_t MaximumBytes)
{
	std::string Clean;
	for (std::size_t Offset = 0; Offset < Inserted.size();)
	{
		const FUtf8CodePoint CodePoint = DecodeUtf8(Inserted, Offset);
		Offset += CodePoint.Length;
		if (CodePoint.Value >= 0x20 && CodePoint.Value != 0x7F && !(CodePoint.Value >= 0x80 && CodePoint.Value < 0xA0))
		{
			AppendUtf8(Clean, CodePoint.Value);
		}
	}

	Edit.Cursor = std::min(Edit.Cursor, Edit.Text.size());
	Edit.Text.insert(Edit.Cursor, Clean);
	std::size_t End = Edit.Cursor + Clean.size();
	if (Edit.Text.size() > MaximumBytes)
	{
		// Keep whole graphemes of the inserted text that still fit.
		const std::size_t Room = MaximumBytes - std::min(MaximumBytes, Edit.Text.size() - Clean.size());
		std::size_t Kept = 0;
		for (std::size_t Next = GetNextGrapheme(Clean, 0); Kept < Clean.size() && Next <= Room; Next = GetNextGrapheme(Clean, Next))
		{
			Kept = Next;
		}

		Edit.Text.erase(Edit.Cursor + Kept, Clean.size() - Kept);
		End = Edit.Cursor + Kept;
	}

	// An insertion can join the text around it into one cluster, such as a combining mark after a letter.
	Edit.Cursor = End;
	if (Edit.Cursor < Edit.Text.size() && GetNextGrapheme(Edit.Text, GetPreviousGrapheme(Edit.Text, Edit.Cursor)) != Edit.Cursor)
	{
		Edit.Cursor = GetNextGrapheme(Edit.Text, GetPreviousGrapheme(Edit.Text, Edit.Cursor));
	}
}

void DeleteBackward(FTextEdit& Edit)
{
	Edit.Cursor = std::min(Edit.Cursor, Edit.Text.size());
	const std::size_t Start = GetPreviousGrapheme(Edit.Text, Edit.Cursor);
	Edit.Text.erase(Start, Edit.Cursor - Start);
	Edit.Cursor = Start;
}

void DeleteForward(FTextEdit& Edit)
{
	Edit.Cursor = std::min(Edit.Cursor, Edit.Text.size());
	Edit.Text.erase(Edit.Cursor, GetNextGrapheme(Edit.Text, Edit.Cursor) - Edit.Cursor);
}

void MoveCursor(FTextEdit& Edit, const int Graphemes)
{
	Edit.Cursor = std::min(Edit.Cursor, Edit.Text.size());
	for (int Step = 0; Step < Graphemes; ++Step)
	{
		Edit.Cursor = GetNextGrapheme(Edit.Text, Edit.Cursor);
	}

	for (int Step = 0; Step > Graphemes; --Step)
	{
		Edit.Cursor = GetPreviousGrapheme(Edit.Text, Edit.Cursor);
	}
}

std::size_t SnapToGrapheme(const std::string_view Text, const std::size_t Offset, const bool bForward) noexcept
{
	if (Offset == 0 || Offset >= Text.size())
	{
		return std::min(Offset, Text.size());
	}

	const std::size_t Start = GetPreviousGrapheme(Text, Offset);
	const std::size_t End = GetNextGrapheme(Text, Start);
	return End == Offset ? Offset : bForward ? End : Start;
}

std::optional<FTextEdit> RepairGraphemeEdit(const std::string_view Before, const std::string_view After)
{
	// The edit is what lies between the longest common prefix and suffix.
	const std::size_t Shorter = std::min(Before.size(), After.size());
	std::size_t Prefix = 0;
	while (Prefix < Shorter && Before[Prefix] == After[Prefix])
	{
		++Prefix;
	}

	std::size_t Suffix = 0;
	while (Suffix < Shorter - Prefix && Before[Before.size() - 1 - Suffix] == After[After.size() - 1 - Suffix])
	{
		++Suffix;
	}

	// A common prefix can end inside a multibyte character; the edit starts at that character.
	while (Prefix > 0 && (static_cast<std::uint8_t>(Before[Prefix]) & 0xC0) == 0x80)
	{
		--Prefix;
	}

	const std::size_t RemovedEnd = Before.size() - Suffix;
	const std::size_t Start = SnapToGrapheme(Before, Prefix, false);
	const std::size_t End = SnapToGrapheme(Before, std::max(RemovedEnd, Prefix), true);
	if (Start == Prefix && End == RemovedEnd)
	{
		return std::nullopt;
	}

	// Only deletions and replacements cut clusters; an insertion inside one, such as a combining mark, is meant to join it.
	if (RemovedEnd == Prefix)
	{
		return std::nullopt;
	}

	const std::string_view Inserted = After.substr(Prefix, After.size() - Suffix - Prefix);
	FTextEdit Repaired{.Text = std::string(Before.substr(0, Start)), .Cursor = 0};
	Repaired.Text += Inserted;
	Repaired.Cursor = Repaired.Text.size();
	Repaired.Text += Before.substr(End);
	return Repaired;
}
}
