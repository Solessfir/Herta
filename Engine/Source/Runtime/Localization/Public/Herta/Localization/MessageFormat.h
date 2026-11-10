#pragma once

#include "Herta/Localization/Culture.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Herta
{
struct FLocalizationError
{
	std::string Message;
};

using FMessageValue = std::variant<std::int64_t, double, std::string, FCivilTime>;

struct FMessageArgument
{
	std::string_view Name;
	FMessageValue Value;
};

// A compiled message in a subset of ICU MessageFormat:
//   {name}                                  the argument as text, or a number formatted for the culture
//   {name, number} {name, number, integer} {name, number, percent}
//   {name, date, short|medium|long|full}     and the same for time and datetime
//   {name, plural, offset:1 =0 {...} one {# item} other {# items}}   # is the number less the offset
//   {name, select, female {...} other {...}}
// An apostrophe quotes the syntax characters after it ('{' or, in a plural, '#') up to the next apostrophe, and two make one.
class FMessage final
{
public:
	[[nodiscard]] static std::expected<FMessage, FLocalizationError> Compile(std::string_view Pattern);

	FMessage();
	~FMessage();
	FMessage(const FMessage& Other);
	FMessage& operator=(const FMessage& Other);
	FMessage(FMessage&&) noexcept;
	FMessage& operator=(FMessage&&) noexcept;

	// A missing argument or one of the wrong kind shows as {name}, so the text stays readable.
	[[nodiscard]] std::string Format(const FCulture& Culture, std::span<const FMessageArgument> Arguments = {}) const;
	// The arguments it names, sorted and unique, to check a translation against its source.
	[[nodiscard]] std::vector<std::string> GetArgumentNames() const;
	// The same message with every literal run passed through Transform, such as for pseudolocalization.
	[[nodiscard]] FMessage TransformText(std::string (*Transform)(std::string_view)) const;

	struct FNode;

private:
	std::vector<FNode> Nodes;
};
}
