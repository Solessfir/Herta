#include "Herta/Assets/AssetRegistry.h"

#include <algorithm>
#include <format>
#include <ranges>

namespace Herta
{
namespace
{
inline constexpr std::size_t MaximumAssetPathLength = 1024;

[[nodiscard]] constexpr bool IsPortableSegment(const std::string_view Segment) noexcept
{
	if (Segment.empty() || Segment == "." || Segment == ".." || Segment.back() == '.' || Segment.back() == ' ')
	{
		return false;
	}

	return std::ranges::none_of(Segment, [](const char Character)
	{
		const auto Byte = static_cast<unsigned char>(Character);
		return Byte < 0x20 || Byte == 0x7f || std::string_view(R"(\:*?"<>|)").contains(Character);
	});
}

[[nodiscard]] constexpr char FoldAsciiCase(const char Character) noexcept
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
}

[[nodiscard]] bool EqualsIgnoringAsciiCase(const std::string_view Left, const std::string_view Right) noexcept
{
	return std::ranges::equal(Left, Right, {}, FoldAsciiCase, FoldAsciiCase);
}
}

bool IsValidAssetPath(const std::string_view Path) noexcept
{
	if (Path.empty() || Path.size() > MaximumAssetPathLength)
	{
		return false;
	}

	for (const auto Segment : std::views::split(Path, '/'))
	{
		if (!IsPortableSegment(std::string_view(Segment.begin(), Segment.end())))
		{
			return false;
		}
	}

	return true;
}

std::expected<FAssetRegistry, FAssetError> FAssetRegistry::Create(std::vector<FAssetRecord> Records)
{
	for (const FAssetRecord& Record : Records)
	{
		if (!Record.Id.IsValid())
		{
			return std::unexpected(FAssetError{std::format("Asset '{}' has no valid ID", Record.SourcePath)});
		}

		if (!IsValidAssetPath(Record.SourcePath))
		{
			return std::unexpected(FAssetError{std::format("Asset {} has an invalid source path '{}'", Record.Id.ToString(), Record.SourcePath)});
		}
	}

	const auto FoldedLess = [](const FAssetRecord& Left, const FAssetRecord& Right)
	{
		return std::ranges::lexicographical_compare(Left.SourcePath, Right.SourcePath, {}, FoldAsciiCase, FoldAsciiCase);
	};

	std::ranges::sort(Records, FoldedLess);
	for (std::size_t Index = 1; Index < Records.size(); ++Index)
	{
		if (EqualsIgnoringAsciiCase(Records[Index - 1].SourcePath, Records[Index].SourcePath))
		{
			return std::unexpected(FAssetError{std::format("Asset paths '{}' and '{}' collide on case-insensitive file systems", Records[Index - 1].SourcePath, Records[Index].SourcePath)});
		}
	}

	// Paths are unique even after case folding, so a byte-wise sort is a strict, deterministic order.
	std::ranges::sort(Records, {}, &FAssetRecord::SourcePath);

	FAssetRegistry Registry;
	Registry.IdOrder.resize(Records.size());
	for (std::size_t Index = 0; Index < Records.size(); ++Index)
	{
		Registry.IdOrder[Index] = Index;
	}

	std::ranges::sort(Registry.IdOrder, {}, [&Records](const std::size_t Index)
	{
		return Records[Index].Id;
	});

	for (std::size_t Index = 1; Index < Registry.IdOrder.size(); ++Index)
	{
		const FAssetRecord& Previous = Records[Registry.IdOrder[Index - 1]];
		const FAssetRecord& Current = Records[Registry.IdOrder[Index]];
		if (Previous.Id == Current.Id)
		{
			return std::unexpected(FAssetError{std::format("Assets '{}' and '{}' share ID {}", Previous.SourcePath, Current.SourcePath, Current.Id.ToString())});
		}
	}

	Registry.Records = std::move(Records);
	return Registry;
}

const FAssetRecord* FAssetRegistry::Find(const FAssetId& Id) const noexcept
{
	const auto Iterator = std::ranges::lower_bound(IdOrder, Id, {}, [this](const std::size_t Index)
	{
		return Records[Index].Id;
	});

	return Iterator != IdOrder.end() && Records[*Iterator].Id == Id ? &Records[*Iterator] : nullptr;
}

const FAssetRecord* FAssetRegistry::FindBySourcePath(const std::string_view SourcePath) const noexcept
{
	const auto Iterator = std::ranges::lower_bound(Records, SourcePath, {}, &FAssetRecord::SourcePath);
	return Iterator != Records.end() && Iterator->SourcePath == SourcePath ? &*Iterator : nullptr;
}
}
