#pragma once

#include "Herta/Assets/AssetId.h"
#include "Herta/Level/World.h"

#include <im3d_math.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Herta
{
// Engine/Content/Shapes/Cube.gltf.hmeta
inline constexpr FAssetId EngineCubeAsset{0x59f13694df4844e5, 0x863555194493f242};

inline Im3d::Mat3 FromPreviewEulerXYZ(const Im3d::Vec3& Radians)
{
	// Avoid the incorrect XYZ Euler constructor in the pinned Im3d revision.
	return Im3d::Rotation({0.f, 0.f, 1.f}, Radians.z) * Im3d::Rotation({0.f, 1.f, 0.f}, Radians.y) * Im3d::Rotation({1.f, 0.f, 0.f}, Radians.x);
}

enum class EPreviewObjectKind : std::uint8_t
{
	Mesh,
	Entity,
	DirectionalLight,
	SkyLight,
	PointLight,
	SpotLight,
	RectLight,
	SkyAtmosphere,
	HeightFog,
};

constexpr std::string_view GetPreviewObjectTypeName(const EPreviewObjectKind Kind)
{
	constexpr std::array<std::string_view, 9> Names{"Static Mesh", "Entity", "Directional Light", "Sky Light", "Point Light", "Spot Light", "Rect Light", "Sky Atmosphere", "Height Fog"};
	const auto Index = static_cast<std::size_t>(Kind);
	return Index < Names.size() ? Names[Index] : "Entity";
}

struct FPreviewObject
{
	std::string Label;
	// Gizmos and rendering consume world poses; Level stores parent-local transforms.
	Im3d::Vec3 Translation;
	Im3d::Mat3 Rotation{1.f};
	Im3d::Vec3 Scale{1.f};
	FAssetId Mesh{};
	FObjectId Id{};
	std::optional<FObjectId> Parent{};
	EPreviewObjectKind Kind = EPreviewObjectKind::Entity;
};

inline std::string_view GetPreviewObjectTypeName(const FPreviewObject& Object)
{
	return GetPreviewObjectTypeName(Object.Kind == EPreviewObjectKind::Entity && Object.Mesh.IsValid() ? EPreviewObjectKind::Mesh : Object.Kind);
}

inline constexpr int PreviewCubeIndex = 0;
inline constexpr int PreviewFloorIndex = 1;

struct FPreviewSelection
{
	std::vector<int> Indices{PreviewCubeIndex};
	int Active = PreviewCubeIndex;
	int Anchor = PreviewCubeIndex;

	[[nodiscard]] bool Contains(const int Index) const
	{
		return std::ranges::find(Indices, Index) != Indices.end();
	}

	void Select(const int Index, const bool bToggle = false)
	{
		if (Index < 0)
		{
			Indices.clear();
			Active = Anchor = -1;
			return;
		}

		Anchor = Index;
		if (!bToggle)
		{
			Indices = {Index};
		}
		else if (Contains(Index))
		{
			std::erase(Indices, Index);
		}
		else
		{
			Indices.push_back(Index);
		}

		Active = Contains(Index) ? Index : (Indices.empty() ? -1 : Indices.back());
	}

	void SelectRange(const int Index, const std::span<const int> Visible, const bool bAdd = false)
	{
		const auto Target = std::ranges::find(Visible, Index);
		if (Target == Visible.end())
		{
			return;
		}

		const auto First = std::ranges::find(Visible, Anchor);
		if (First == Visible.end())
		{
			if (!bAdd)
			{
				Select(Index);
			}
			else
			{
				if (!Contains(Index))
				{
					Indices.push_back(Index);
				}

				Active = Anchor = Index;
			}

			return;
		}

		const auto RangeBegin = std::min(First, Target);
		const auto RangeEnd = std::max(First, Target) + 1;
		std::unordered_set<int> Existing;
		Existing.reserve((bAdd ? Indices.size() : 0) + static_cast<std::size_t>(RangeEnd - RangeBegin));
		if (bAdd)
		{
			Existing.insert(Indices.begin(), Indices.end());
		}
		else
		{
			Indices.clear();
		}

		for (auto Item = RangeBegin; Item != RangeEnd; ++Item)
		{
			if (Existing.insert(*Item).second)
			{
				Indices.push_back(*Item);
			}
		}

		Active = Index;
	}

	void SelectAll(const std::span<const int> Visible)
	{
		Indices.assign(Visible.begin(), Visible.end());
		Active = Indices.empty() ? -1 : Indices.back();
		Anchor = Indices.empty() ? -1 : Indices.front();
	}

	[[nodiscard]] bool operator==(const FPreviewSelection&) const = default;
};

inline bool RenamePreviewObject(std::string& Label, const std::string_view Candidate)
{
	const auto First = Candidate.find_first_not_of(" \t\r\n");
	if (First == std::string_view::npos)
	{
		return false;
	}

	Label = Candidate.substr(First, Candidate.find_last_not_of(" \t\r\n") - First + 1);
	return true;
}

inline void ApplyPreviewTransformDelta(const std::span<FPreviewObject> Objects, const FPreviewSelection& Selection, const FPreviewObject& PreviousActive)
{
	if (Selection.Active < 0 || Selection.Indices.size() < 2)
	{
		return;
	}

	const FPreviewObject& Active = Objects[static_cast<std::size_t>(Selection.Active)];
	if (Active.Translation.x == PreviousActive.Translation.x && Active.Translation.y == PreviousActive.Translation.y && Active.Translation.z == PreviousActive.Translation.z && Active.Scale.x == PreviousActive.Scale.x && Active.Scale.y == PreviousActive.Scale.y && Active.Scale.z == PreviousActive.Scale.z && std::ranges::equal(Active.Rotation.m, PreviousActive.Rotation.m))
	{
		return;
	}

	Im3d::Mat3 InverseRotation;
	for (int Row = 0; Row < 3; ++Row)
	{
		for (int Column = 0; Column < 3; ++Column)
		{
			InverseRotation(Row, Column) = PreviousActive.Rotation(Column, Row);
		}
	}

	const Im3d::Mat3 RotationDelta = Active.Rotation * InverseRotation;
	const Im3d::Vec3 ScaleRatio = Active.Scale / PreviousActive.Scale;
	std::vector<FPreviewObject> Candidates(Objects.begin(), Objects.end());
	for (const int Index : Selection.Indices)
	{
		if (Index == Selection.Active)
		{
			continue;
		}

		FPreviewObject& Object = Candidates[static_cast<std::size_t>(Index)];
		const Im3d::Vec3 Offset = InverseRotation * (Object.Translation - PreviousActive.Translation);
		Object.Translation = Active.Translation + Active.Rotation * (Offset * ScaleRatio);
		Object.Rotation = RotationDelta * Object.Rotation;
		const Im3d::Vec3 Scale = Object.Scale * ScaleRatio;
		Object.Scale = {std::clamp(Scale.x, 0.001f, 1000.f), std::clamp(Scale.y, 0.001f, 1000.f), std::clamp(Scale.z, 0.001f, 1000.f)};
		const Im3d::Mat4 Model(Object.Translation, Object.Rotation, Object.Scale);
		if (!std::ranges::all_of(Model.m, [](const float Value)
		{
			return std::isfinite(Value);
		}))
		{
			FPreviewObject& Reverted = Objects[static_cast<std::size_t>(Selection.Active)];
			Reverted.Translation = PreviousActive.Translation;
			Reverted.Rotation = PreviousActive.Rotation;
			Reverted.Scale = PreviousActive.Scale;
			return;
		}
	}

	for (const int Index : Selection.Indices)
	{
		if (Index != Selection.Active)
		{
			Objects[static_cast<std::size_t>(Index)] = std::move(Candidates[static_cast<std::size_t>(Index)]);
		}
	}
}

[[nodiscard]] inline std::array<FPreviewObject, 2> CreatePreviewObjects()
{
	// Both use the 1 m engine cube; the floor is scaled to a 10 x 0.5 x 10 m slab with its top at Y=0.
	return {{
	    {.Label = "Preview Cube", .Translation = {0.f, 4.f, 0.f}, .Mesh = EngineCubeAsset},
	    {.Label = "Floor", .Translation = {0.f, -0.25f, 0.f}, .Scale = {10.f, 0.5f, 10.f}, .Mesh = EngineCubeAsset},
	}};
}
}
