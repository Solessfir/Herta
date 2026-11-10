#include "Herta/Localization/MessageFormat.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <utility>

namespace Herta
{
namespace
{
enum class ENodeKind : std::uint8_t
{
	Text,
	Argument,
	Number,
	Date,
	Time,
	DateTime,
	Plural,
	Select,
	// '#' inside a plural: its number less the offset.
	Pound,
};

constexpr std::size_t MaximumPatternBytes = 16 * 1024;
constexpr std::size_t MaximumDepth = 8;
constexpr std::array<std::string_view, 6> PluralKeywords{"zero", "one", "two", "few", "many", "other"};
constexpr std::array<std::string_view, 4> DateStyles{"full", "long", "medium", "short"};
}

struct FMessage::FNode
{
	struct FCase
	{
		std::string Key{};
		std::optional<double> Exact{};
		std::vector<FNode> Message{};
	};

	ENodeKind Kind = ENodeKind::Text;
	// The literal text, or the argument's name.
	std::string Text{};
	std::string Style{};
	double Offset = 0.;
	std::vector<FCase> Cases{};
};

namespace
{
using FNode = FMessage::FNode;

class FParser
{
public:
	explicit FParser(const std::string_view InPattern)
	    : Pattern(InPattern)
	{
	}

	[[nodiscard]] std::expected<std::vector<FNode>, FLocalizationError> ParseMessage(const std::size_t Depth, const bool bInPlural)
	{
		if (Depth > MaximumDepth)
		{
			return Error("nests more than eight arguments deep");
		}

		std::vector<FNode> Nodes;
		std::string Text;
		const auto FlushText = [&Nodes, &Text]
		{
			if (!Text.empty())
			{
				Nodes.push_back({.Kind = ENodeKind::Text, .Text = std::exchange(Text, {})});
			}
		};

		while (Cursor < Pattern.size())
		{
			const char Character = Pattern[Cursor];
			if (Character == '}')
			{
				break;
			}

			if (Character == '\'')
			{
				// '' is a quote; a quote before a syntax character starts quoted text; any other quote is itself.
				if (Cursor + 1 < Pattern.size() && Pattern[Cursor + 1] == '\'')
				{
					Text.push_back('\'');
					Cursor += 2;
				}
				else if (Cursor + 1 < Pattern.size() && (Pattern[Cursor + 1] == '{' || Pattern[Cursor + 1] == '}' || (bInPlural && Pattern[Cursor + 1] == '#')))
				{
					++Cursor;
					while (Cursor < Pattern.size())
					{
						if (Pattern[Cursor] == '\'' && (Cursor + 1 == Pattern.size() || Pattern[Cursor + 1] != '\''))
						{
							++Cursor;
							break;
						}

						Cursor += Pattern[Cursor] == '\'' ? 1 : 0;
						Text.push_back(Pattern[Cursor++]);
					}
				}
				else
				{
					Text.push_back('\'');
					++Cursor;
				}

				continue;
			}

			if (Character == '#' && bInPlural)
			{
				FlushText();
				Nodes.push_back({.Kind = ENodeKind::Pound});
				++Cursor;
				continue;
			}

			if (Character == '{')
			{
				FlushText();
				++Cursor;
				auto Argument = ParseArgument(Depth);
				if (!Argument)
				{
					return std::unexpected(Argument.error());
				}

				Nodes.push_back(std::move(*Argument));
				continue;
			}

			Text.push_back(Character);
			++Cursor;
		}

		FlushText();
		return Nodes;
	}

	[[nodiscard]] bool AtEnd() const noexcept
	{
		return Cursor >= Pattern.size();
	}

	[[nodiscard]] std::unexpected<FLocalizationError> Error(const std::string_view Message) const
	{
		return std::unexpected(FLocalizationError{std::format("Message '{}' {} at character {}", Pattern, Message, Cursor)});
	}

private:
	void SkipSpace()
	{
		while (Cursor < Pattern.size() && (Pattern[Cursor] == ' ' || Pattern[Cursor] == '\t' || Pattern[Cursor] == '\n' || Pattern[Cursor] == '\r'))
		{
			++Cursor;
		}
	}

	[[nodiscard]] std::string_view Word()
	{
		SkipSpace();
		const std::size_t Start = Cursor;
		while (Cursor < Pattern.size() && std::string_view(" \t\r\n,{}").find(Pattern[Cursor]) == std::string_view::npos)
		{
			++Cursor;
		}

		return Pattern.substr(Start, Cursor - Start);
	}

	[[nodiscard]] bool Expect(const char Character)
	{
		SkipSpace();
		if (Cursor < Pattern.size() && Pattern[Cursor] == Character)
		{
			++Cursor;
			return true;
		}

		return false;
	}

	[[nodiscard]] std::expected<FNode, FLocalizationError> ParseArgument(const std::size_t Depth)
	{
		const std::string_view Name = Word();
		const bool bNameValid = !Name.empty() && std::ranges::all_of(Name, [](const char Character)
		{
			return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') || (Character >= '0' && Character <= '9') || Character == '_';
		});

		if (!bNameValid)
		{
			return Error("names an argument with something other than letters, digits, and underscores");
		}

		FNode Node{.Kind = ENodeKind::Argument, .Text = std::string(Name)};
		if (Expect('}'))
		{
			return Node;
		}

		if (!Expect(','))
		{
			return Error("expects ',' or '}' after an argument's name");
		}

		const std::string_view Type = Word();
		if (Type == "plural" || Type == "select")
		{
			Node.Kind = Type == "plural" ? ENodeKind::Plural : ENodeKind::Select;
			if (!Expect(','))
			{
				return Error("expects ',' before the cases");
			}

			return ParseCases(std::move(Node), Depth);
		}

		Node.Kind = Type == "number" ? ENodeKind::Number : Type == "date" ? ENodeKind::Date : Type == "time" ? ENodeKind::Time : Type == "datetime" ? ENodeKind::DateTime : ENodeKind::Text;
		if (Node.Kind == ENodeKind::Text)
		{
			return Error(std::format("has an unknown argument type '{}'", Type));
		}

		if (Expect(','))
		{
			Node.Style = Word();
		}

		const bool bStyleValid = Node.Kind == ENodeKind::Number ? (Node.Style.empty() || Node.Style == "integer" || Node.Style == "percent") : (Node.Style.empty() || std::ranges::contains(DateStyles, Node.Style));
		if (!bStyleValid || !Expect('}'))
		{
			return Error(bStyleValid ? "expects '}' after an argument's style" : "has an unknown argument style");
		}

		return Node;
	}

	[[nodiscard]] std::expected<FNode, FLocalizationError> ParseCases(FNode Node, const std::size_t Depth)
	{
		const bool bPlural = Node.Kind == ENodeKind::Plural;
		while (true)
		{
			SkipSpace();
			if (Expect('}'))
			{
				break;
			}

			std::string_view Key = Word();
			if (bPlural && Key.starts_with("offset:"))
			{
				const std::string_view Value = Key.substr(7);
				const auto [End, Failed] = std::from_chars(Value.data(), Value.data() + Value.size(), Node.Offset);
				if (Failed != std::errc{} || End != Value.data() + Value.size() || !Node.Cases.empty())
				{
					return Error("has an offset that is not a number before the cases");
				}

				continue;
			}

			FNode::FCase Case{.Key = std::string(Key)};
			if (bPlural && Key.starts_with('='))
			{
				double Exact = 0.;
				const auto [End, Failed] = std::from_chars(Key.data() + 1, Key.data() + Key.size(), Exact);
				if (Failed != std::errc{} || End != Key.data() + Key.size())
				{
					return Error("has an exact case that is not a number");
				}

				Case.Exact = Exact;
			}
			else if (Key.empty() || (bPlural && !std::ranges::contains(PluralKeywords, Key)))
			{
				return Error(bPlural ? "has a plural case other than =n, zero, one, two, few, many, or other" : "has an empty select case");
			}

			if (std::ranges::any_of(Node.Cases, [&Case](const FNode::FCase& Other)
			{
				return Other.Key == Case.Key;
			}))
			{
				return Error(std::format("repeats the case '{}'", Key));
			}

			if (!Expect('{'))
			{
				return Error("expects '{' to start a case's message");
			}

			auto Message = ParseMessage(Depth + 1, bPlural);
			if (!Message)
			{
				return std::unexpected(Message.error());
			}

			if (!Expect('}'))
			{
				return Error("expects '}' to end a case's message");
			}

			Case.Message = std::move(*Message);
			Node.Cases.push_back(std::move(Case));
		}

		if (!std::ranges::contains(Node.Cases, std::string_view("other"), [](const FNode::FCase& Case)
		{
			return std::string_view(Case.Key);
		}))
		{
			return Error("needs an 'other' case");
		}

		return Node;
	}

	std::string_view Pattern;
	std::size_t Cursor = 0;
};

[[nodiscard]] const FMessageValue* FindValue(const std::span<const FMessageArgument> Arguments, const std::string_view Name)
{
	const auto Found = std::ranges::find(Arguments, Name, &FMessageArgument::Name);
	return Found != Arguments.end() ? &Found->Value : nullptr;
}

[[nodiscard]] std::optional<double> AsNumber(const FMessageValue* const Value)
{
	if (const auto* const Integer = Value ? std::get_if<std::int64_t>(Value) : nullptr)
	{
		return static_cast<double>(*Integer);
	}

	if (const auto* const Real = Value ? std::get_if<double>(Value) : nullptr)
	{
		return *Real;
	}

	if (const auto* const Fixed = Value ? std::get_if<FFixedNumber>(Value) : nullptr)
	{
		return Fixed->Value;
	}

	return std::nullopt;
}

[[nodiscard]] FNumberFormat NumberFormatFor(const std::string_view Style, const FMessageValue* const Value)
{
	if (const auto* const Fixed = Value ? std::get_if<FFixedNumber>(Value) : nullptr; Fixed && Style.empty())
	{
		return {.MinimumFractionDigits = Fixed->FractionDigits, .MaximumFractionDigits = Fixed->FractionDigits};
	}

	if (Style == "integer")
	{
		return {.MaximumFractionDigits = 0};
	}

	if (Style == "percent")
	{
		return {.MaximumFractionDigits = 0, .bPercent = true};
	}

	// Whole numbers show without a fraction; others with up to three digits.
	return {.MaximumFractionDigits = Value && std::holds_alternative<std::int64_t>(*Value) ? 0u : 3u};
}

void FormatNodes(const std::span<const FNode> Nodes, const FCulture& Culture, const std::span<const FMessageArgument> Arguments, const std::optional<std::pair<double, FNumberFormat>> Pound, std::string& Output)
{
	for (const FNode& Node : Nodes)
	{
		const FMessageValue* const Value = Node.Kind == ENodeKind::Text || Node.Kind == ENodeKind::Pound ? nullptr : FindValue(Arguments, Node.Text);
		const std::string Missing = std::format("{{{}}}", Node.Text);
		switch (Node.Kind)
		{
			case ENodeKind::Text:
				Output += Node.Text;
				break;
			case ENodeKind::Pound:
				Output += Pound ? Culture.FormatNumber(Pound->first, Pound->second) : "#";
				break;
			case ENodeKind::Argument:
				if (const auto* const String = Value ? std::get_if<std::string>(Value) : nullptr)
				{
					Output += *String;
				}
				else if (const auto Number = AsNumber(Value))
				{
					Output += Culture.FormatNumber(*Number, NumberFormatFor({}, Value));
				}
				else if (const auto* const Time = Value ? std::get_if<FCivilTime>(Value) : nullptr)
				{
					Output += Culture.FormatDateTime(*Time, EDateTimeStyle::Short);
				}
				else
				{
					Output += Missing;
				}

				break;
			case ENodeKind::Number:
			{
				const auto Number = AsNumber(Value);
				Output += Number ? Culture.FormatNumber(*Number, NumberFormatFor(Node.Style, Value)) : Missing;
				break;
			}
			case ENodeKind::Date:
			case ENodeKind::Time:
			case ENodeKind::DateTime:
			{
				const auto* const Time = Value ? std::get_if<FCivilTime>(Value) : nullptr;
				const auto Style = Node.Style.empty() ? EDateTimeStyle::Medium : static_cast<EDateTimeStyle>(std::ranges::find(DateStyles, Node.Style) - DateStyles.begin());
				Output += !Time ? Missing : Node.Kind == ENodeKind::Date ? Culture.FormatDate(*Time, Style) : Node.Kind == ENodeKind::Time ? Culture.FormatTime(*Time, Style) : Culture.FormatDateTime(*Time, Style);
				break;
			}
			case ENodeKind::Plural:
			{
				const auto Number = AsNumber(Value);
				if (!Number)
				{
					Output += Missing;
					break;
				}

				// Exact cases match the value itself; keywords match the number shown after the offset.
				const FNumberFormat Format = NumberFormatFor({}, Value);
				const double Shown = *Number - Node.Offset;
				const std::string_view Keyword = PluralKeywords[static_cast<std::size_t>(Culture.SelectPlural(Shown, Format))];
				const auto Exact = std::ranges::find_if(Node.Cases, [&Number](const FNode::FCase& Case)
				{
					return Case.Exact && *Case.Exact == *Number;
				});

				const auto Chosen = Exact != Node.Cases.end() ? Exact : std::ranges::find_if(Node.Cases, [Keyword](const FNode::FCase& Case)
				{
					return Case.Key == Keyword;
				});

				const auto& Case = Chosen != Node.Cases.end() ? *Chosen : *std::ranges::find(Node.Cases, std::string_view("other"), [](const FNode::FCase& Other)
				{
					return std::string_view(Other.Key);
				});

				FormatNodes(Case.Message, Culture, Arguments, std::pair{Shown, Format}, Output);
				break;
			}
			case ENodeKind::Select:
			{
				const auto* const String = Value ? std::get_if<std::string>(Value) : nullptr;
				const std::string_view Key = String ? std::string_view(*String) : std::string_view("other");
				const auto Chosen = std::ranges::find_if(Node.Cases, [Key](const FNode::FCase& Case)
				{
					return Case.Key == Key;
				});

				const auto& Case = Chosen != Node.Cases.end() ? *Chosen : *std::ranges::find(Node.Cases, std::string_view("other"), [](const FNode::FCase& Other)
				{
					return std::string_view(Other.Key);
				});

				FormatNodes(Case.Message, Culture, Arguments, Pound, Output);
				break;
			}
		}
	}
}

void CollectNames(const std::span<const FNode> Nodes, std::vector<std::string>& Names)
{
	for (const FNode& Node : Nodes)
	{
		if (Node.Kind != ENodeKind::Text && Node.Kind != ENodeKind::Pound)
		{
			Names.push_back(Node.Text);
		}

		for (const FNode::FCase& Case : Node.Cases)
		{
			CollectNames(Case.Message, Names);
		}
	}
}

void TransformNodes(std::vector<FNode>& Nodes, std::string (*Transform)(std::string_view))
{
	for (FNode& Node : Nodes)
	{
		if (Node.Kind == ENodeKind::Text)
		{
			Node.Text = Transform(Node.Text);
		}

		for (FNode::FCase& Case : Node.Cases)
		{
			TransformNodes(Case.Message, Transform);
		}
	}
}
}

std::expected<FMessage, FLocalizationError> FMessage::Compile(const std::string_view Pattern)
{
	if (Pattern.size() > MaximumPatternBytes)
	{
		return std::unexpected(FLocalizationError{"A message is longer than 16 KiB"});
	}

	FParser Parser(Pattern);
	auto Nodes = Parser.ParseMessage(0, false);
	if (!Nodes)
	{
		return std::unexpected(Nodes.error());
	}

	if (!Parser.AtEnd())
	{
		return Parser.Error("has a '}' that closes nothing");
	}

	FMessage Message;
	Message.Nodes = std::move(*Nodes);
	return Message;
}

FMessage::FMessage() = default;
FMessage::~FMessage() = default;
FMessage::FMessage(const FMessage& Other) = default;
FMessage& FMessage::operator=(const FMessage& Other) = default;
FMessage::FMessage(FMessage&&) noexcept = default;
FMessage& FMessage::operator=(FMessage&&) noexcept = default;

std::string FMessage::Format(const FCulture& Culture, const std::span<const FMessageArgument> Arguments) const
{
	std::string Output;
	FormatNodes(Nodes, Culture, Arguments, std::nullopt, Output);
	return Output;
}

std::vector<std::string> FMessage::GetArgumentNames() const
{
	std::vector<std::string> Names;
	CollectNames(Nodes, Names);
	std::ranges::sort(Names);
	const auto [First, Last] = std::ranges::unique(Names);
	Names.erase(First, Last);
	return Names;
}

FMessage FMessage::TransformText(std::string (*Transform)(std::string_view)) const
{
	FMessage Result = *this;
	TransformNodes(Result.Nodes, Transform);
	return Result;
}
}
