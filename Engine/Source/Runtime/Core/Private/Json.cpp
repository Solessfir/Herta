#include "Herta/Core/Json.h"

#include <format>

namespace Herta
{
void AppendJsonString(std::string& Output, const std::string_view Text)
{
	Output += '"';

	for (const char Character : Text)
	{
		switch (Character)
		{
			case '"':
				Output += "\\\"";
				break;
			case '\\':
				Output += "\\\\";
				break;
			case '\n':
				Output += "\\n";
				break;
			case '\r':
				Output += "\\r";
				break;
			case '\t':
				Output += "\\t";
				break;
			default:
				if (static_cast<unsigned char>(Character) < 0x20)
				{
					Output += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(Character)));
				}
				else
				{
					Output += Character;
				}
		}
	}

	Output += '"';
}

std::string ToJsonString(const std::string_view Text)
{
	std::string Output;
	Output.reserve(Text.size() + 2);
	AppendJsonString(Output, Text);
	return Output;
}
}
