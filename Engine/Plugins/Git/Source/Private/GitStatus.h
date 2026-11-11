#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FGitChange
{
	std::string Path{};
	// The source of a rename or copy.
	std::string OriginalPath{};
	// Porcelain status letters for the index and the work tree, such as 'M', 'A', 'D', 'R', '?' for untracked, or 'U' for a conflict.
	char Staged = '.';
	char Unstaged = '.';
};

struct FGitStatus
{
	std::string Branch{};
	std::string Upstream{};
	int Ahead = 0;
	int Behind = 0;
	std::vector<FGitChange> Changes{};
};

// Parses `git status --porcelain=v2 --branch -z`, whose fields Git keeps stable across versions and locales.
[[nodiscard]] inline FGitStatus ParseGitStatus(const std::string_view Output)
{
	FGitStatus Status;
	// Skips Count space-separated fields and returns the rest, which is the path and may itself hold spaces.
	const auto Rest = [](std::string_view Entry, const int Count)
	{
		for (int Field = 0; Field < Count && !Entry.empty(); ++Field)
		{
			const std::size_t Space = Entry.find(' ');
			Entry = Space == std::string_view::npos ? std::string_view{} : Entry.substr(Space + 1);
		}

		return Entry;
	};

	std::size_t Start = 0;
	while (Start < Output.size())
	{
		const std::size_t End = std::min(Output.find('\0', Start), Output.size());
		const std::string_view Entry = Output.substr(Start, End - Start);
		Start = End + 1;
		if (Entry.size() < 2)
		{
			continue;
		}

		if (Entry.starts_with("# branch.head "))
		{
			Status.Branch = std::string(Entry.substr(14));
		}
		else if (Entry.starts_with("# branch.upstream "))
		{
			Status.Upstream = std::string(Entry.substr(18));
		}
		else if (Entry.starts_with("# branch.ab +"))
		{
			const std::string_view Counts = Entry.substr(13);
			const std::size_t Minus = Counts.find(" -");
			std::from_chars(Counts.data(), Counts.data() + Counts.size(), Status.Ahead);
			if (Minus != std::string_view::npos)
			{
				std::from_chars(Counts.data() + Minus + 2, Counts.data() + Counts.size(), Status.Behind);
			}
		}
		else if ((Entry[0] == '1' || Entry[0] == '2' || Entry[0] == 'u') && Entry.size() > 4)
		{
			FGitChange Change{.Path = {}, .OriginalPath = {}, .Staged = Entry[2], .Unstaged = Entry[3]};
			Change.Path = std::string(Rest(Entry, Entry[0] == '1' ? 8 : Entry[0] == '2' ? 9
			                                                                            : 10));
			if (Entry[0] == '2' && Start < Output.size())
			{
				// A rename's original path is the next entry.
				const std::size_t OriginalEnd = std::min(Output.find('\0', Start), Output.size());
				Change.OriginalPath = std::string(Output.substr(Start, OriginalEnd - Start));
				Start = OriginalEnd + 1;
			}

			Status.Changes.push_back(std::move(Change));
		}
		else if (Entry[0] == '?')
		{
			Status.Changes.push_back({.Path = std::string(Entry.substr(2)), .OriginalPath = {}, .Staged = '.', .Unstaged = '?'});
		}
	}

	return Status;
}
}
