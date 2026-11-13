#include "CharacterPiece.h"

#include "Herta/Math/AffineTransform.h"
#include "PlayerCharacter.h"

#include <algorithm>
#include <format>
#include <numeric>

namespace Herta
{
namespace
{
// A hull of at most this many of the piece's vertices, picked evenly, keeps its shape while staying cheap for Jolt to build.
constexpr std::size_t MaximumHullPoints = 128;

void ApplyPose(FAnimationPose& Pose, const std::span<const FMatrix4> Transforms)
{
	std::vector<std::size_t> All(Transforms.size());
	std::iota(All.begin(), All.end(), std::size_t{0});
	(void)Pose.Evaluate({});
	Pose.OverrideJoints(All, Transforms, 1.f);
}
}

std::expected<std::unique_ptr<FCharacterPiece>, FGameError> FCharacterPiece::Create(FPhysicsWorld& World, IGraphicsDevice* const Device, std::shared_ptr<const FCookedModel> Model, std::shared_ptr<const FAnimationLibrary> Library, const std::span<const FMatrix4> PoseTransforms, FSkinnedMeshPiece Mesh, const FMatrix4& ModelToWorld, const FVector3& Velocity, const std::size_t Owner)
{
	std::unique_ptr<FCharacterPiece> Piece(new FCharacterPiece);
	Piece->Model = std::move(Model);
	Piece->Library = std::move(Library);
	Piece->Bind = std::move(Mesh);
	Piece->Owner = Owner;
	Piece->ModelToWorld = ModelToWorld;
	Piece->BasePose.assign(PoseTransforms.begin(), PoseTransforms.end());
	Piece->Pose = std::make_unique<FAnimationPose>(Piece->Library);
	ApplyPose(*Piece->Pose, Piece->BasePose);
	Piece->Vertices.resize(Piece->Bind.Vertices.size());
	SkinVertices(Piece->Pose->GetSkinningMatrices(), Piece->Bind.Vertices, Piece->Bind.Influences, Piece->Vertices);
	std::ranges::for_each(Piece->Vertices, OrthonormalizeTangent);

	const std::vector<bool> Carried = FindCarriedJoints(Piece->Bind, Piece->Library->GetJointCount());
	FRagdollFit Fit = FitRagdoll(*Piece->Library, Piece->BasePose, ModelToWorld, [&Carried](const std::size_t Joint)
	{
		return Joint < Carried.size() && Carried[Joint];
	});

	FCookedModel Drawn = MakeCutModel(*Piece->Model, Piece->Bind);
	if (Fit.Parts.size() >= 2)
	{
		auto Created = World.CreateRagdoll({.Parts = Fit.Parts, .LinearVelocity = Velocity});
		if (!Created)
		{
			return std::unexpected(FGameError{Created.error().Message});
		}

		Piece->Ragdoll = std::move(*Created);
		Piece->Joints = std::move(Fit.Joints);
		Piece->Offsets = std::move(Fit.Offsets);
		Piece->WorldToModel = TryInverseAffine(ModelToWorld).value_or(FMatrix4{});
	}
	else
	{
		// Its model space rides on one body at the posed vertices' centroid.
		FVector3 Centroid{};
		for (const FCookedVertex& Vertex : Piece->Vertices)
		{
			Centroid = Centroid + ModelToWorld.TransformPosition({Vertex.Position[0], Vertex.Position[1], Vertex.Position[2]});
		}

		Centroid = Centroid * (1.f / static_cast<float>(std::max<std::size_t>(Piece->Vertices.size(), 1)));
		std::vector<FVector3> Hull;
		const std::size_t Stride = std::max<std::size_t>(Piece->Vertices.size() / MaximumHullPoints, 1);
		for (std::size_t Index = 0; Index < Piece->Vertices.size(); Index += Stride)
		{
			const auto& Position = Piece->Vertices[Index].Position;
			Hull.push_back(ModelToWorld.TransformPosition({Position[0], Position[1], Position[2]}) - Centroid);
		}

		auto Created = World.CreateBody({
		    .Shape = EPhysicsShape::ConvexHull,
		    .Position = Centroid,
		    .MotionType = EPhysicsMotionType::Dynamic,
		    .Properties = {.MassKg = 3.f, .Friction = 0.7f, .Restitution = 0.05f, .LinearDamping = 0.1f, .AngularDamping = 0.3f},
		    .Layer = PhysicsLayerRagdoll,
		    .HullPoints = Hull,
		});
		if (!Created)
		{
			return std::unexpected(FGameError{Created.error().Message});
		}

		Piece->Body = *Created;
		Piece->BodyToModel = FMatrix4::Translation(-Centroid) * ModelToWorld;
		if (auto Moved = World.SetVelocity(Piece->Body, Velocity); !Moved)
		{
			return std::unexpected(FGameError{Moved.error().Message});
		}

		// A rigid piece never deforms, so it draws its cut pose as it was skinned.
		Drawn.Vertices = Piece->Vertices;
	}

	if (auto Valid = ValidateCookedModel(Drawn); !Valid)
	{
		return std::unexpected(FGameError{std::format("A cut piece is invalid: {}", Valid.error().Message)});
	}

	if (Device)
	{
		auto Uploaded = FRenderMesh::Create(*Device, Drawn, "Cut piece");
		if (!Uploaded)
		{
			return std::unexpected(FGameError{std::format("Cannot upload a cut piece: {}", Uploaded.error().Message)});
		}

		Piece->Mesh = std::move(*Uploaded);
	}

	return Piece;
}

std::expected<void, FPhysicsError> FCharacterPiece::Destroy(FPhysicsWorld& World)
{
	if (Ragdoll)
	{
		auto Destroyed = World.DestroyRagdoll(*Ragdoll);
		Ragdoll.reset();
		return Destroyed;
	}

	return World.DestroyBody(std::exchange(Body, FPhysicsBodyId{}));
}

std::expected<void, FPhysicsError> FCharacterPiece::Update(const FPhysicsWorld& World)
{
	if (!Ragdoll)
	{
		const auto Transform = World.GetBodyTransform(Body);
		if (!Transform)
		{
			return std::unexpected(Transform.error());
		}

		ModelToWorld = FMatrix4::Translation(Transform->Position) * FMatrix4::Rotation(Transform->Rotation) * BodyToModel;
		return {};
	}

	Parts.clear();
	for (const FPhysicsBodyId Part : Ragdoll->Parts)
	{
		auto Transform = World.GetBodyTransform(Part);
		if (!Transform)
		{
			return std::unexpected(Transform.error());
		}

		Parts.push_back(*Transform);
	}

	ApplyPose(*Pose, BasePose);
	JointPose.clear();
	for (std::size_t Index = 0; Index < Parts.size() && Index < Offsets.size(); ++Index)
	{
		JointPose.push_back(WorldToModel * FMatrix4::Translation(Parts[Index].Position) * FMatrix4::Rotation(Parts[Index].Rotation) * Offsets[Index]);
	}

	Pose->OverrideJoints(Joints, JointPose, 1.f);
	SkinVertices(Pose->GetSkinningMatrices(), Bind.Vertices, Bind.Influences, Vertices);
	return {};
}

const FRenderMesh* FCharacterPiece::GetMesh() const noexcept
{
	return Mesh.get();
}

std::span<const FCookedVertex> FCharacterPiece::GetVertices() const noexcept
{
	return Vertices;
}

bool FCharacterPiece::IsDeforming() const noexcept
{
	return Ragdoll.has_value();
}

const FMatrix4& FCharacterPiece::GetModel() const noexcept
{
	return ModelToWorld;
}

std::span<const FMatrix4> FCharacterPiece::GetSkinningMatrices() const noexcept
{
	return Pose->GetSkinningMatrices();
}

std::span<const FMatrix4> FCharacterPiece::GetModelTransforms() const noexcept
{
	return Pose->GetModelTransforms();
}

const FSkinnedMeshPiece& FCharacterPiece::GetBind() const noexcept
{
	return Bind;
}

const std::shared_ptr<const FCookedModel>& FCharacterPiece::GetCookedModel() const noexcept
{
	return Model;
}

const std::shared_ptr<const FAnimationLibrary>& FCharacterPiece::GetLibrary() const noexcept
{
	return Library;
}

FVector3 FCharacterPiece::GetVelocity(const FPhysicsWorld& World) const
{
	return World.GetLinearVelocity(Ragdoll ? Ragdoll->Parts.front() : Body).value_or(FVector3{});
}

std::size_t FCharacterPiece::GetOwner() const noexcept
{
	return Owner;
}

bool FCharacterPiece::IsRagdoll() const noexcept
{
	return Ragdoll.has_value();
}
}
