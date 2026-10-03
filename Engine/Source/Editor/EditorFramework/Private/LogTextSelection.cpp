#include "Herta/EditorFramework/LogTextSelection.h"

namespace Herta
{
void FLogTextSelection::Begin(const FLogTextPosition Position, const bool bExtend) noexcept
{
	if (!bExtend || !Anchor)
	{
		Anchor = Position;
	}

	Caret = Position;
}

void FLogTextSelection::Update(const FLogTextPosition Position) noexcept
{
	if (!Anchor)
	{
		Anchor = Position;
	}

	Caret = Position;
}

void FLogTextSelection::Clear() noexcept
{
	Anchor.reset();
	Caret.reset();
}

void FLogTextSelection::SelectAll(const std::span<const std::string> Lines) noexcept
{
	if (Lines.empty())
	{
		Clear();
		return;
	}

	Anchor = FLogTextPosition{};
	Caret = FLogTextPosition{.Line = Lines.size() - 1, .Byte = Lines.back().size()};
}

void FLogTextSelection::ClampTo(const std::span<const std::string> Lines) noexcept
{
	if (Lines.empty())
	{
		Clear();
		return;
	}

	const auto ClampPosition = [Lines](FLogTextPosition& Position)
	{
		Position.Line = std::min(Position.Line, Lines.size() - 1);
		Position.Byte = std::min(Position.Byte, Lines[Position.Line].size());
	};

	if (Anchor)
	{
		ClampPosition(*Anchor);
	}

	if (Caret)
	{
		ClampPosition(*Caret);
	}
}

bool FLogTextSelection::HasSelection() const noexcept
{
	return Anchor && Caret && *Anchor != *Caret;
}

std::pair<FLogTextPosition, FLogTextPosition> FLogTextSelection::GetOrderedRange() const noexcept
{
	if (!Anchor || !Caret)
	{
		return {};
	}

	return IsBefore(*Caret, *Anchor) ? std::pair{*Caret, *Anchor} : std::pair{*Anchor, *Caret};
}

std::string FLogTextSelection::Copy(const std::span<const std::string> Lines) const
{
	if (!HasSelection() || Lines.empty())
	{
		return {};
	}

	const auto [First, Last] = GetOrderedRange();
	if (First.Line >= Lines.size() || Last.Line >= Lines.size())
	{
		return {};
	}

	if (First.Line == Last.Line)
	{
		return Lines[First.Line].substr(First.Byte, Last.Byte - First.Byte);
	}

	std::string Text = Lines[First.Line].substr(First.Byte);
	for (std::size_t LineIndex = First.Line + 1; LineIndex <= Last.Line; ++LineIndex)
	{
		Text.push_back('\n');
		const std::size_t ByteCount = LineIndex == Last.Line ? Last.Byte : Lines[LineIndex].size();
		Text.append(Lines[LineIndex], 0, ByteCount);
	}

	return Text;
}
}
