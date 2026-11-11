#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

namespace Herta
{
// Terminal output as lines of text. Escape sequences are dropped rather than emulated: shells and line-oriented tools read correctly,
// while full-screen programs that address the cursor, such as editors, do not.
// ponytail: no screen buffer or cursor addressing; adopt a VT emulator such as libvterm when full-screen programs need to run here.
class FTerminalText final
{
public:
	explicit FTerminalText(std::size_t InMaximumLines = 5000);

	void Append(std::string_view Bytes);
	void Clear();
	[[nodiscard]] const std::deque<std::string>& GetLines() const noexcept;

private:
	enum class EState : std::uint8_t
	{
		Text,
		Escape,
		Control,
		Command,
		CommandEscape
	};

	void NewLine();

	std::deque<std::string> Lines{std::string{}};
	std::size_t MaximumLines = 5000;
	// Where the next character lands in the last line, which a carriage return moves back to its start.
	std::size_t Column = 0;
	EState State = EState::Text;
	std::string Parameters{};
};

inline FTerminalText::FTerminalText(const std::size_t InMaximumLines)
    : MaximumLines(InMaximumLines > 0 ? InMaximumLines : 1)
{
}

inline void FTerminalText::Append(const std::string_view Bytes)
{
	for (const char Character : Bytes)
	{
		const auto Byte = static_cast<unsigned char>(Character);
		switch (State)
		{
			case EState::Text:
				if (Byte == 0x1b)
				{
					State = EState::Escape;
				}
				else if (Character == '\n')
				{
					NewLine();
				}
				else if (Character == '\r')
				{
					Column = 0;
				}
				else if (Character == '\b')
				{
					// Back over a whole UTF-8 character, not one of its continuation bytes.
					while (Column > 0 && (static_cast<unsigned char>(Lines.back()[--Column]) & 0xc0) == 0x80)
					{
					}
				}
				else if (Byte >= 0x20 || Character == '\t')
				{
					// A character that starts a new code point replaces the whole one under the cursor; its continuation bytes follow it in.
					std::string& Line = Lines.back();
					if ((Byte & 0xc0) != 0x80 && Column < Line.size())
					{
						std::size_t End = Column + 1;
						while (End < Line.size() && (static_cast<unsigned char>(Line[End]) & 0xc0) == 0x80)
						{
							++End;
						}

						Line.erase(Column, End - Column);
					}

					Line.insert(Line.begin() + static_cast<std::ptrdiff_t>(std::min(Column, Line.size())), Character);
					++Column;
				}

				break;

			case EState::Escape:
				// CSI and OSC carry parameters; any other escape is a single character.
				State = Character == '[' ? EState::Control : Character == ']' ? EState::Command
				                                                              : EState::Text;
				Parameters.clear();
				break;

			case EState::Control:
				if (Byte >= 0x40 && Byte <= 0x7e)
				{
					// Erase in line from the cursor and erase the whole display are the two a shell prompt relies on.
					if (Character == 'K' && (Parameters.empty() || Parameters == "0"))
					{
						Lines.back().resize(std::min(Column, Lines.back().size()));
					}
					else if (Character == 'J' && Parameters == "2")
					{
						Clear();
					}
					else if (Character == 'H' || Character == 'G')
					{
						Column = 0;
					}

					State = EState::Text;
				}
				else if (Parameters.size() < 32)
				{
					Parameters.push_back(Character);
				}

				break;

			case EState::Command:
				State = Byte == 0x07 ? EState::Text : Byte == 0x1b ? EState::CommandEscape
				                                                   : EState::Command;
				break;

			case EState::CommandEscape:
				State = Character == '\\' ? EState::Text : EState::Command;
				break;
		}
	}
}

inline void FTerminalText::Clear()
{
	Lines.assign(1, std::string{});
	Column = 0;
}

inline const std::deque<std::string>& FTerminalText::GetLines() const noexcept
{
	return Lines;
}

inline void FTerminalText::NewLine()
{
	Lines.emplace_back();
	Column = 0;
	while (Lines.size() > MaximumLines)
	{
		Lines.pop_front();
	}
}
}
