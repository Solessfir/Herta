#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Herta
{
struct FLogTextPosition
{
	std::size_t Line = 0;
	std::size_t Byte = 0;

	[[nodiscard]] constexpr bool operator==(const FLogTextPosition&) const noexcept = default;
};

class FLogTextSelection final
{
public:
	void Begin(FLogTextPosition Position, bool bExtend) noexcept;
	void Update(FLogTextPosition Position) noexcept;
	void Clear() noexcept;
	void SelectAll(std::span<const std::string> Lines) noexcept;
	void ClampTo(std::span<const std::string> Lines) noexcept;

	[[nodiscard]] bool HasSelection() const noexcept;
	[[nodiscard]] std::pair<FLogTextPosition, FLogTextPosition> GetOrderedRange() const noexcept;
	[[nodiscard]] std::string Copy(std::span<const std::string> Lines) const;

private:
	[[nodiscard]] static constexpr bool IsBefore(const FLogTextPosition Left, const FLogTextPosition Right) noexcept
	{
		return Left.Line < Right.Line || (Left.Line == Right.Line && Left.Byte < Right.Byte);
	}

	std::optional<FLogTextPosition> Anchor;
	std::optional<FLogTextPosition> Caret;
};
}
