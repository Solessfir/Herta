#include "Herta/Level/ScalingLevel.h"

#include <algorithm>
#include <format>

namespace Herta
{
namespace
{
constexpr FAssetId EngineCubeAsset{0x59f13694df4844e5, 0x863555194493f242};
constexpr std::size_t DynamicStackHeight = 10;
constexpr double RenderingSpacing = 1.25;
constexpr double DynamicSpacing = 1.05;
constexpr double DynamicVerticalSpacing = 1.02;

[[nodiscard]] bool IsSupportedCubeCount(const std::size_t CubeCount) noexcept
{
	return CubeCount == 1000 || CubeCount == 5000 || CubeCount == 10000;
}

[[nodiscard]] std::size_t SquareGridWidth(const std::size_t Count) noexcept
{
	std::size_t Width = 1;
	while (Width * Width < Count)
	{
		++Width;
	}

	return Width;
}

[[nodiscard]] double GridCoordinate(const std::size_t Index, const std::size_t Extent, const double Spacing) noexcept
{
	return static_cast<double>(Index) * Spacing - static_cast<double>(Extent - 1) * Spacing * 0.5;
}

[[nodiscard]] FObjectId MakeLevelId(const FScalingLevelOptions& Options) noexcept
{
	const std::uint64_t Workload = Options.Workload == EScalingLevelWorkload::Rendering ? 1 : 2;
	return FObjectId{0x485254415343414c, (Workload << 32) | Options.CubeCount};
}

[[nodiscard]] FObjectId MakeEntityId(const EScalingLevelWorkload Workload, const std::size_t Index) noexcept
{
	const std::uint64_t Namespace = Workload == EScalingLevelWorkload::Rendering ? 0x4852544152454e44 : 0x4852544150485953;
	return FObjectId{Namespace, Index + 1};
}
}

std::expected<FLevelDocument, FLevelError> GenerateScalingLevel(const FScalingLevelOptions Options)
{
	if (!IsSupportedCubeCount(Options.CubeCount))
	{
		return std::unexpected(FLevelError{"Scaling levels support exactly 1000, 5000, or 10000 cubes"});
	}

	if (Options.Workload != EScalingLevelWorkload::Rendering && Options.Workload != EScalingLevelWorkload::DynamicBodies)
	{
		return std::unexpected(FLevelError{"Unknown scaling level workload"});
	}

	const bool bDynamic = Options.Workload == EScalingLevelWorkload::DynamicBodies;
	const std::size_t StackHeight = bDynamic ? DynamicStackHeight : 1;
	const std::size_t ColumnCount = (Options.CubeCount + StackHeight - 1) / StackHeight;
	const std::size_t GridWidth = SquareGridWidth(ColumnCount);
	const std::size_t GridDepth = (ColumnCount + GridWidth - 1) / GridWidth;
	const double Spacing = bDynamic ? DynamicSpacing : RenderingSpacing;

	FLevelDocument Document{
	    .Id = MakeLevelId(Options),
	    .Name = std::format("Scaling {} {}", bDynamic ? "Dynamic" : "Rendering", Options.CubeCount),
	};
	Document.Entities.reserve(Options.CubeCount + 1);
	Document.Entities.push_back(FLevelEntity{
	    .Id = MakeEntityId(Options.Workload, 0),
	    .Name = "Floor",
	    .Transform = {
	        .Translation = FWorldPosition{0., -0.25, 0.},
	        .Scale = {
	            static_cast<float>(std::max(10., static_cast<double>(GridWidth - 1) * Spacing + 2.)),
	            0.5f,
	            static_cast<float>(std::max(10., static_cast<double>(GridDepth - 1) * Spacing + 2.)),
	        },
	    },
	    .Mesh = FStaticMeshComponent{.Asset = EngineCubeAsset},
	    .BodyType = bDynamic ? ELevelBodyType::Static : ELevelBodyType::None,
	});

	for (std::size_t Index = 0; Index < Options.CubeCount; ++Index)
	{
		const std::size_t Column = Index / StackHeight;
		const std::size_t Layer = Index % StackHeight;
		const std::size_t X = Column % GridWidth;
		const std::size_t Z = Column / GridWidth;
		Document.Entities.push_back(FLevelEntity{
		    .Id = MakeEntityId(Options.Workload, Index + 1),
		    .Name = std::format("Cube {:05}", Index + 1),
		    .Transform = {.Translation = FWorldPosition{
		                      GridCoordinate(X, GridWidth, Spacing),
		                      0.5 + static_cast<double>(Layer) * DynamicVerticalSpacing,
		                      GridCoordinate(Z, GridDepth, Spacing),
		                  }},
		    .Mesh = FStaticMeshComponent{.Asset = EngineCubeAsset},
		    .BodyType = bDynamic ? ELevelBodyType::Dynamic : ELevelBodyType::None,
		});
	}

	if (auto Result = ValidateLevelEntities(Document.Entities); !Result)
	{
		return std::unexpected(Result.error());
	}

	return Document;
}
}
