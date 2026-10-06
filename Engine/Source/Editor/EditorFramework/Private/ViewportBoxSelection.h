#pragma once

#include "Herta/Math/Vector.h"
#include "PreviewLevel.h"

#include <span>
#include <utility>
#include <vector>

namespace Herta
{
struct FViewportBoxSelectionState
{
	void Begin(FVector2 Position, FPreviewSelection Initial, bool bAdd, bool bToggle);
	void Update(FVector2 Position, bool bPastDragThreshold) noexcept;
	void Cancel() noexcept;
	FPreviewSelection MakeSelection(std::span<const int> Hits) const;

	bool bActive = false;
	bool bDragging = false;
	FVector2 Start{};
	FVector2 Current{};
	FPreviewSelection InitialSelection;

private:
	bool bAddToSelection = false;
	bool bToggleSelection = false;
};

inline void FViewportBoxSelectionState::Begin(const FVector2 Position, FPreviewSelection Initial, const bool bAdd, const bool bToggle)
{
	Start = Current = Position;
	InitialSelection = std::move(Initial);
	bAddToSelection = bAdd;
	bToggleSelection = bToggle;
	bActive = true;
	bDragging = false;
}

inline void FViewportBoxSelectionState::Update(const FVector2 Position, const bool bPastDragThreshold) noexcept
{
	if (!bActive)
	{
		return;
	}

	Current = Position;
	bDragging |= bPastDragThreshold;
}

inline void FViewportBoxSelectionState::Cancel() noexcept
{
	bActive = false;
	bDragging = false;
}

inline FPreviewSelection FViewportBoxSelectionState::MakeSelection(const std::span<const int> Hits) const
{
	std::vector<int> UniqueHits;
	for (const int Index : Hits)
	{
		if (Index >= 0 && std::ranges::find(UniqueHits, Index) == UniqueHits.end())
		{
			UniqueHits.push_back(Index);
		}
	}

	FPreviewSelection Result = InitialSelection;
	if (!bAddToSelection && !bToggleSelection)
	{
		Result.SelectAll(UniqueHits);
		return Result;
	}

	for (const int Index : UniqueHits)
	{
		if (Result.Contains(Index))
		{
			if (bToggleSelection)
			{
				std::erase(Result.Indices, Index);
			}
		}
		else
		{
			Result.Indices.push_back(Index);
		}
	}

	if (!Result.Contains(Result.Active))
	{
		Result.Active = Result.Indices.empty() ? -1 : Result.Indices.back();
	}

	if (!Result.Contains(Result.Anchor))
	{
		Result.Anchor = Result.Indices.empty() ? -1 : Result.Indices.front();
	}

	return Result;
}
}
