#include "Herta/Assets/AssetSearch.h"

#include <algorithm>

namespace Herta
{
namespace
{
constexpr int MatchScore = 16;
constexpr int ConsecutiveBonus = 16;
constexpr int WordStartBonus = 12;
constexpr int FileNameBonus = 8;
constexpr std::size_t CancellationStride = 256;

[[nodiscard]] constexpr char ToLowerAscii(const char Character) noexcept
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
}

[[nodiscard]] constexpr bool IsSeparator(const char Character) noexcept
{
	return Character == '/' || Character == '\\' || Character == '_' || Character == '-' || Character == '.' || Character == ' ';
}

[[nodiscard]] constexpr bool IsWordStart(const std::string_view Text, const std::size_t Index) noexcept
{
	if (Index == 0)
	{
		return true;
	}

	const char Previous = Text[Index - 1];
	const char Current = Text[Index];
	return IsSeparator(Previous) || (Previous >= 'a' && Previous <= 'z' && Current >= 'A' && Current <= 'Z');
}

// Greedy matching from one start position; nullopt when the rest of the query does not fit.
[[nodiscard]] std::optional<int> ScoreFrom(const std::string_view Candidate, const std::string_view Query, const std::size_t Start, const std::size_t FileNameStart)
{
	int Score = 0;
	std::size_t Position = Start;
	std::size_t Previous = std::string_view::npos;
	for (const char QueryCharacter : Query)
	{
		const char Wanted = ToLowerAscii(QueryCharacter);
		while (Position < Candidate.size() && ToLowerAscii(Candidate[Position]) != Wanted)
		{
			++Position;
		}

		if (Position == Candidate.size())
		{
			return std::nullopt;
		}

		Score += MatchScore;
		if (Previous != std::string_view::npos)
		{
			Score += Position == Previous + 1 ? ConsecutiveBonus : -static_cast<int>(std::min<std::size_t>(Position - Previous - 1, 8));
		}

		if (IsWordStart(Candidate, Position))
		{
			Score += WordStartBonus;
		}

		Previous = Position++;
	}

	return Start == FileNameStart ? Score + FileNameBonus : Score;
}

[[nodiscard]] std::optional<int> ScoreCandidate(const std::string_view Candidate, const std::string_view Query)
{
	const std::size_t LastSeparator = Candidate.find_last_of("/\\");
	const std::size_t FileNameStart = LastSeparator == std::string_view::npos ? 0 : LastSeparator + 1;
	std::optional<int> Best;
	const char First = ToLowerAscii(Query.front());
	for (std::size_t Start = 0; Start < Candidate.size(); ++Start)
	{
		if (ToLowerAscii(Candidate[Start]) != First)
		{
			continue;
		}

		const std::optional<int> Score = ScoreFrom(Candidate, Query, Start, FileNameStart);
		if (!Score)
		{
			// Later starts only see a shorter tail, so they cannot fit either.
			break;
		}

		Best = std::max(Best.value_or(*Score), *Score);
	}

	return Best;
}
}

std::optional<std::vector<FAssetSearchMatch>> SearchAssets(const std::span<const std::string_view> Candidates, const std::string_view Query, const std::function<bool()>& ShouldCancel)
{
	std::vector<FAssetSearchMatch> Matches;
	for (std::size_t Index = 0; Index < Candidates.size(); ++Index)
	{
		if (Index % CancellationStride == 0 && ShouldCancel && ShouldCancel())
		{
			return std::nullopt;
		}

		if (Query.empty())
		{
			Matches.push_back({.Index = Index, .Score = 0});
		}
		else if (const std::optional<int> Score = ScoreCandidate(Candidates[Index], Query))
		{
			Matches.push_back({.Index = Index, .Score = *Score});
		}
	}

	std::ranges::stable_sort(Matches, std::ranges::greater{}, &FAssetSearchMatch::Score);
	return Matches;
}
}
