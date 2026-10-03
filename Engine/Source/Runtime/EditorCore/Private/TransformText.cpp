#include "Herta/EditorCore/TransformText.h"

#include <array>
#include <charconv>
#include <cmath>
#include <format>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumExpressionLength = 256;
constexpr std::size_t MaximumNesting = 32;
constexpr std::size_t MaximumClipboardLength = 3 * MaximumExpressionLength + 24;

class FNumericExpressionParser
{
public:
	explicit FNumericExpressionParser(const std::string_view InExpression)
	    : Expression(InExpression)
	{
	}

	[[nodiscard]] std::optional<float> Parse() noexcept
	{
		if (Expression.empty() || Expression.size() > MaximumExpressionLength)
		{
			return std::nullopt;
		}

		const auto Value = ParseExpression();
		SkipWhitespace();
		return Value && Position == Expression.size() && std::isfinite(*Value) ? Value : std::nullopt;
	}

private:
	void SkipWhitespace() noexcept
	{
		while (Position < Expression.size() && (Expression[Position] == ' ' || Expression[Position] == '\t' || Expression[Position] == '\n' || Expression[Position] == '\r'))
		{
			++Position;
		}
	}

	[[nodiscard]] std::optional<float> ParseExpression() noexcept
	{
		auto Left = ParseTerm();
		while (Left)
		{
			SkipWhitespace();
			if (Position >= Expression.size() || (Expression[Position] != '+' && Expression[Position] != '-'))
			{
				return Left;
			}

			const char Operation = Expression[Position++];
			const auto Right = ParseTerm();
			if (!Right)
			{
				return std::nullopt;
			}

			*Left = Operation == '+' ? *Left + *Right : *Left - *Right;
			if (!std::isfinite(*Left))
			{
				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	[[nodiscard]] std::optional<float> ParseTerm() noexcept
	{
		auto Left = ParseUnary();
		while (Left)
		{
			SkipWhitespace();
			if (Position >= Expression.size() || (Expression[Position] != '*' && Expression[Position] != '/'))
			{
				return Left;
			}

			const char Operation = Expression[Position++];
			const auto Right = ParseUnary();
			if (!Right || (Operation == '/' && *Right == 0.f))
			{
				return std::nullopt;
			}

			*Left = Operation == '*' ? *Left * *Right : *Left / *Right;
			if (!std::isfinite(*Left))
			{
				return std::nullopt;
			}
		}

		return std::nullopt;
	}

	[[nodiscard]] std::optional<float> ParseUnary() noexcept
	{
		SkipWhitespace();
		bool bNegative = false;
		while (Position < Expression.size() && (Expression[Position] == '+' || Expression[Position] == '-'))
		{
			bNegative = bNegative != (Expression[Position] == '-');
			++Position;
			SkipWhitespace();
		}

		auto Value = ParsePrimary();
		if (Value && bNegative)
		{
			*Value = -*Value;
		}

		return Value;
	}

	[[nodiscard]] std::optional<float> ParsePrimary() noexcept
	{
		SkipWhitespace();
		if (Position >= Expression.size())
		{
			return std::nullopt;
		}

		if (Expression[Position] == '(')
		{
			if (++Nesting > MaximumNesting)
			{
				return std::nullopt;
			}

			++Position;
			auto Value = ParseExpression();
			SkipWhitespace();
			--Nesting;
			if (!Value || Position >= Expression.size() || Expression[Position++] != ')')
			{
				return std::nullopt;
			}

			return Value;
		}

		float Value = 0.f;
		const char* const Begin = Expression.data() + Position;
		const char* const End = Expression.data() + Expression.size();
		const auto Result = std::from_chars(Begin, End, Value, std::chars_format::general);
		if (Result.ec != std::errc{} || Result.ptr == Begin)
		{
			return std::nullopt;
		}

		Position += static_cast<std::size_t>(Result.ptr - Begin);
		return std::isfinite(Value) ? std::optional{Value} : std::nullopt;
	}

	std::string_view Expression;
	std::size_t Position = 0;
	std::size_t Nesting = 0;
};

void SkipWhitespace(const std::string_view Text, std::size_t& Position) noexcept
{
	while (Position < Text.size() && (Text[Position] == ' ' || Text[Position] == '\t' || Text[Position] == '\n' || Text[Position] == '\r'))
	{
		++Position;
	}
}

bool Consume(const std::string_view Text, std::size_t& Position, const char Character) noexcept
{
	SkipWhitespace(Text, Position);
	if (Position >= Text.size() || Text[Position] != Character)
	{
		return false;
	}

	++Position;
	return true;
}

std::optional<float> ParseVectorComponent(const std::string_view Text, std::size_t& Position) noexcept
{
	SkipWhitespace(Text, Position);
	const std::size_t Start = Position;
	std::size_t Nesting = 0;
	while (Position < Text.size())
	{
		if (Text[Position] == '(')
		{
			++Nesting;
		}
		else if (Text[Position] == ')')
		{
			if (Nesting == 0)
			{
				break;
			}

			--Nesting;
		}
		else if (Text[Position] == ',' && Nesting == 0)
		{
			break;
		}

		++Position;
	}

	return EvaluateNumericExpression(Text.substr(Start, Position - Start));
}

std::string FormatVectorClipboard(const FVector3& Value, const std::array<std::string_view, 3>& Labels)
{
	return std::format("({}={:.9g},{}={:.9g},{}={:.9g})", Labels[0], Value.X, Labels[1], Value.Y, Labels[2], Value.Z);
}

std::optional<FVector3> ParseVectorClipboard(const std::string_view Text, const std::array<std::string_view, 3>& Labels) noexcept
{
	if (Text.empty() || Text.size() > MaximumClipboardLength)
	{
		return std::nullopt;
	}

	std::size_t Position = 0;
	if (!Consume(Text, Position, '('))
	{
		return std::nullopt;
	}

	std::array<float, 3> Components{};
	for (std::size_t Index = 0; Index < Labels.size(); ++Index)
	{
		SkipWhitespace(Text, Position);
		if (Text.substr(Position, Labels[Index].size()) != Labels[Index])
		{
			return std::nullopt;
		}

		Position += Labels[Index].size();
		if (!Consume(Text, Position, '='))
		{
			return std::nullopt;
		}

		const auto Component = ParseVectorComponent(Text, Position);
		if (!Component)
		{
			return std::nullopt;
		}

		Components[Index] = *Component;
		if (Index + 1 < Labels.size() && !Consume(Text, Position, ','))
		{
			return std::nullopt;
		}
	}

	if (!Consume(Text, Position, ')'))
	{
		return std::nullopt;
	}

	SkipWhitespace(Text, Position);
	return Position == Text.size() ? std::optional{FVector3{Components[0], Components[1], Components[2]}} : std::nullopt;
}
}

std::optional<float> EvaluateNumericExpression(const std::string_view Expression) noexcept
{
	return FNumericExpressionParser(Expression).Parse();
}

std::string FormatTransformVectorClipboard(const FVector3& Value)
{
	return FormatVectorClipboard(Value, {"X", "Y", "Z"});
}

std::optional<FVector3> ParseTransformVectorClipboard(const std::string_view Text) noexcept
{
	return ParseVectorClipboard(Text, {"X", "Y", "Z"});
}

std::string FormatTransformRotationClipboard(const FVector3& Value)
{
	return FormatVectorClipboard(Value, {"Pitch", "Yaw", "Roll"});
}

std::optional<FVector3> ParseTransformRotationClipboard(const std::string_view Text) noexcept
{
	return ParseVectorClipboard(Text, {"Pitch", "Yaw", "Roll"});
}
}
