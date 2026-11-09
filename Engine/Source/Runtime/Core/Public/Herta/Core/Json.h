#pragma once

#include <string>
#include <string_view>

namespace Herta
{
// Appends Text as a quoted JSON string, escaping quotes, backslashes, and control characters. UTF-8 passes through unchanged.
void AppendJsonString(std::string& Output, std::string_view Text);

[[nodiscard]] std::string ToJsonString(std::string_view Text);
}
