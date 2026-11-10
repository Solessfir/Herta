#include "Herta/GameFramework/GameSession.h"

#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/Animation/MeshSlice.h"
#include "Herta/Math/AffineTransform.h"
#include "CharacterPiece.h"
#include "GameAudio.h"
#include "PlayerCharacter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <numbers>
#include <utility>

namespace Herta
{
namespace
{
constexpr float MaximumPitch = 1.53f;
constexpr float CameraNear = 0.05f;
// Impacts softer than this make no sound, and impulses are capped where the impact graph is already at its loudest.
constexpr float MinimumImpactImpulse = 1.5f;
constexpr float MaximumImpactImpulse = 60.f;
constexpr std::size_t MaximumImpactsPerUpdate = 4;
// A thrown ball: a 3 kg, 24 cm ball at 24 m/s carries about 86 N*s into a wall, enough to break the playground's walls but not its footings.
// The sword in grip space: the blade runs along +Y from the guard to the tip, its edge leading along +X.
constexpr float SwordBladeStart = 0.1f;
constexpr float SwordBladeEnd = 1.f;

// Boxes for a plain arming sword: a dark grip through the fist, a steel guard, and a steel blade, in grip space.
[[nodiscard]] FCookedModel BuildSwordModel()
{
	FCookedModel Model;
	Model.Materials.push_back({.Name = "Steel"});
	Model.Materials.back().Parameters.BaseColor = {0.78f, 0.78f, 0.8f, 1.f};
	Model.Materials.back().Parameters.Metallic = 1.f;
	Model.Materials.back().Parameters.Roughness = 0.28f;
	Model.Materials.push_back({.Name = "Grip"});
	Model.Materials.back().Parameters.BaseColor = {0.12f, 0.07f, 0.04f, 1.f};
	Model.Materials.back().Parameters.Roughness = 0.7f;
	const auto Box = [&Model](const FVector3& Minimum, const FVector3& Maximum, const std::uint32_t Material)
	{
		const auto First = static_cast<std::uint32_t>(Model.Indices.size());
		for (std::size_t Axis = 0; Axis < 3; ++Axis)
		{
			for (const float Sign : {-1.f, 1.f})
			{
				// A face across the axis, wound counter-clockwise seen from outside.
				const std::size_t U = (Axis + 1) % 3;
				const std::size_t V = (Axis + 2) % 3;
				const auto Base = static_cast<std::uint32_t>(Model.Vertices.size());
				for (const auto [CornerU, CornerV] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}})
				{
					std::array<float, 3> Position{};
					Position[Axis] = Sign > 0.f ? std::array{Maximum.X, Maximum.Y, Maximum.Z}[Axis] : std::array{Minimum.X, Minimum.Y, Minimum.Z}[Axis];
					Position[U] = (CornerU != 0 ? std::array{Maximum.X, Maximum.Y, Maximum.Z} : std::array{Minimum.X, Minimum.Y, Minimum.Z})[U];
					Position[V] = (CornerV != 0 ? std::array{Maximum.X, Maximum.Y, Maximum.Z} : std::array{Minimum.X, Minimum.Y, Minimum.Z})[V];
					std::array<float, 3> Normal{};
					Normal[Axis] = Sign;
					std::array<float, 4> Tangent{0.f, 0.f, 0.f, 1.f};
					Tangent[U] = 1.f;
					Model.Vertices.push_back({.Position = Position, .UV = {static_cast<float>(CornerU), static_cast<float>(CornerV)}, .Normal = Normal, .Tangent = Tangent});
				}

				const std::array<std::uint32_t, 6> Front{Base, Base + 1, Base + 2, Base, Base + 2, Base + 3};
				const std::array<std::uint32_t, 6> Backward{Base, Base + 2, Base + 1, Base, Base + 3, Base + 2};
				const auto& Corners = Sign > 0.f ? Front : Backward;
				Model.Indices.insert(Model.Indices.end(), Corners.begin(), Corners.end());
			}
		}

		Model.Sections.push_back({.FirstIndex = First, .IndexCount = static_cast<std::uint32_t>(Model.Indices.size()) - First, .Material = Material});
	};

	Box({-0.016f, -0.1f, -0.016f}, {0.016f, 0.06f, 0.016f}, 1);
	Box({-0.022f, 0.06f, -0.11f}, {0.022f, SwordBladeStart, 0.11f}, 0);
	Box({-0.026f, SwordBladeStart, -0.006f}, {0.026f, SwordBladeEnd, 0.006f}, 0);
	return Model;
}

constexpr float ProjectileRadius = 0.12f;
constexpr float ProjectileMassKg = 3.f;
constexpr float ProjectileSpeed = 24.f;
constexpr float ProjectileLifetimeSeconds = 10.f;
constexpr std::size_t MaximumProjectiles = 24;

// A UV sphere for thrown balls, so they need no content.
FCookedModel BuildProjectileModel()
{
	constexpr std::uint32_t Columns = 16;
	constexpr std::uint32_t Rows = 10;
	FCookedModel Model;
	for (std::uint32_t Row = 0; Row <= Rows; ++Row)
	{
		const float Polar = std::numbers::pi_v<float> * static_cast<float>(Row) / static_cast<float>(Rows);
		for (std::uint32_t Column = 0; Column <= Columns; ++Column)
		{
			const float Azimuth = 2.f * std::numbers::pi_v<float> * static_cast<float>(Column) / static_cast<float>(Columns);
			const FVector3 Normal{std::sin(Polar) * std::cos(Azimuth), std::cos(Polar), std::sin(Polar) * std::sin(Azimuth)};
			const FVector3 Tangent{-std::sin(Azimuth), 0.f, std::cos(Azimuth)};
			const FVector3 Position = Normal * ProjectileRadius;
			Model.Vertices.push_back({.Position = {Position.X, Position.Y, Position.Z}, .UV = {static_cast<float>(Column) / Columns, static_cast<float>(Row) / Rows}, .Normal = {Normal.X, Normal.Y, Normal.Z}, .Tangent = {Tangent.X, Tangent.Y, Tangent.Z, 1.f}});
		}
	}

	for (std::uint32_t Row = 0; Row < Rows; ++Row)
	{
		for (std::uint32_t Column = 0; Column < Columns; ++Column)
		{
			const std::uint32_t A = Row * (Columns + 1) + Column;
			const std::uint32_t B = A + Columns + 1;
			Model.Indices.insert(Model.Indices.end(), {A, A + 1, B, A + 1, B + 1, B});
		}
	}

	Model.Sections.push_back({.FirstIndex = 0, .IndexCount = static_cast<std::uint32_t>(Model.Indices.size()), .Material = 0});
	FCookedMaterial Material{.Name = "Thrown ball"};
	Material.Parameters.BaseColor = {0.95f, 0.38f, 0.08f, 1.f};
	Material.Parameters.Roughness = 0.35f;
	Model.Materials.push_back(Material);
	return Model;
}

[[nodiscard]] FMatrix4 ToFloatMatrix(const TMatrix4<double>& Matrix)
{
	FMatrix4 Result;
	for (std::size_t Row = 0; Row < 4; ++Row)
	{
		for (std::size_t Column = 0; Column < 4; ++Column)
		{
			Result(Row, Column) = static_cast<float>(Matrix(Row, Column));
		}
	}

	return Result;
}

[[nodiscard]] FVector3 TransformPoint(const FMatrix4& Matrix, const FVector3& Point)
{
	const FVector4 Result = Matrix * FVector4{Point.X, Point.Y, Point.Z, 1.f};
	return {Result.X, Result.Y, Result.Z};
}

[[nodiscard]] FVector3 GetTranslation(const FMatrix4& Matrix)
{
	return {Matrix(0, 3), Matrix(1, 3), Matrix(2, 3)};
}

[[nodiscard]] FQuaternion GetLookOrientation(const float Yaw, const float Pitch)
{
	return FQuaternion::FromAxisAngle(FVector3::Up(), Yaw) * FQuaternion::FromAxisAngle(FVector3::Left(), -Pitch);
}

struct FSoftBodyMesh
{
	std::size_t Entity = 0;
	FSoftBodyTopology Topology{};
	std::shared_ptr<const FRenderMesh> Mesh{};
	std::vector<FCookedVertex> Vertices{};
	std::uint64_t Revision = 0;
};

struct FTriggerZone
{
	std::size_t Entity = 0;
	FMatrix4 WorldToLocal{};
	FVector3 HalfExtents{};
};

}

struct FGameSession::FImplementation
{
	[[nodiscard]] std::expected<void, FGameError> Start();
	[[nodiscard]] std::expected<void, FPhysicsError> StepPlayer(FPhysicsWorld& World, float StepSeconds);
	void UpdateObjective(float DeltaSeconds);
	[[nodiscard]] bool IsInside(const std::optional<FTriggerZone>& Zone, const FVector3& Point) const;
	void BuildModels();
	void BuildSoftBodies();
	[[nodiscard]] std::expected<void, FPhysicsError> Throw();
	[[nodiscard]] std::expected<void, FPhysicsError> AgeProjectiles(float DeltaSeconds);
	[[nodiscard]] FQuaternion GetBodyRotation() const;
	[[nodiscard]] FQuaternion GetViewOrientation() const;
	void UpdateBody(float DeltaSeconds);
	[[nodiscard]] std::expected<void, FPhysicsError> StartCharacters(FPhysicsWorld& World);
	// The body's model matrix at the mover's current feet, as drawn.
	[[nodiscard]] FMatrix4 GetBodyModel() const;
	// Traces the blade between updates during a swing and slices characters it passes through along its plane.
	[[nodiscard]] std::expected<void, FGameError> SweepBlade();
	[[nodiscard]] std::expected<void, FPhysicsError> StepCharacters(FPhysicsWorld& World, float StepSeconds);
	// Adds a piece to the world, pushing out the oldest of its character's pieces past the character's budget.
	[[nodiscard]] std::expected<void, FGameError> AddPiece(std::size_t Character, std::shared_ptr<const FCookedModel> Model, std::shared_ptr<const FAnimationLibrary> Library, std::span<const FMatrix4> Pose, FSkinnedMeshPiece Mesh, const FMatrix4& ModelToWorld, const FVector3& Velocity);
	[[nodiscard]] std::expected<bool, FGameError> CutCharacter(std::size_t Character, const std::function<std::expected<std::optional<FSkinnedMeshPiece>, FGameError>(FPlayerCharacter&)>& Cut, const FVector3& Away);
	// Cuts a piece along a world plane into two pieces; false when the plane misses it or would leave a sliver.
	[[nodiscard]] std::expected<bool, FGameError> CutPiece(std::size_t Piece, const FVector3& Point, const FVector3& Normal);
	[[nodiscard]] std::expected<void, FPhysicsError> StartRagdoll();
	[[nodiscard]] std::expected<void, FPhysicsError> EndRagdoll();
	[[nodiscard]] std::expected<void, FPhysicsError> ReadRagdoll(const FPhysicsWorld& World);
	[[nodiscard]] std::expected<void, FPhysicsError> Respawn(FPhysicsWorld& World);
	// Foot is the capsule's; BodyFoot is where the body is drawn, which differs while a climb clip lifts its own root.
	[[nodiscard]] FVector3 GetCameraPosition(const FVector3& Foot, const FVector3& BodyFoot);
	// The player's sounds, moving emitters, and the listener, after everything else in the update.
	void UpdateAudio(bool bJumped);

	// cppcheck-suppress uninitMemberVarNoCtor ; set by aggregate initialization in Create
	const IGameAssets& Assets;
	IGraphicsDevice* Device = nullptr;
	FGamePlayerSettings Player{};
	FTaskSystem* Tasks = nullptr;

	// Authored state, fixed for the session.
	std::vector<FLevelEntity> Entities{};
	std::vector<FMatrix4> AuthoredWorld{};
	// Parent-relative matrices, so unsimulated children follow a simulated parent.
	std::vector<FMatrix4> ParentRelative{};
	std::vector<std::optional<std::size_t>> Parents{};
	// Parents come before their children.
	std::vector<std::size_t> Order{};
	std::vector<const FRenderMesh*> BoundsMeshes{};
	std::vector<FSoftBodyMesh> SoftBodies{};
	std::optional<FTriggerZone> StartZone{};
	std::optional<FTriggerZone> FinishZone{};
	FVector3 SpawnPosition{};
	float SpawnYaw = 0.f;
	std::vector<std::string> Warnings{};

	FLevelSimulation Simulation{};
	// Chunk meshes per destructible, built once: fractures are deterministic, so every restart produces the same chunks.
	std::map<std::size_t, std::vector<std::shared_ptr<const FRenderMesh>>> ChunkMeshes{};
	std::shared_ptr<const FRenderMesh> ProjectileMesh{};

	struct FProjectile
	{
		FLevelSimulationHandle Handle{};
		float AgeSeconds = 0.f;
		// A ball hits a character once, so bouncing on it does not sever a second limb.
		bool bSpent = false;
	};

	// A Character entity: its animated body, which stays across restarts and restores itself, and its collider in the current physics world.
	struct FCharacterActor
	{
		std::size_t Entity = 0;
		std::unique_ptr<FPlayerCharacter> Body{};
		FCharacterComponent Settings{};
		FPhysicsBodyId Collider{};
	};

	std::vector<FCharacterActor> Characters{};
	// The sword the player's body holds, and its blade last update, from guard to tip in world space, while a swing is cutting.
	std::shared_ptr<const FRenderMesh> SwordMesh{};
	std::optional<std::array<FVector3, 2>> LastBlade{};
	bool bSwinging = false;
	// Characters and pieces the current swing has already cut, so one swing cuts each once.
	std::vector<std::size_t> SwingCuts{};
	std::vector<const FCharacterPiece*> SwingPieceCuts{};
	// Pieces cut off characters, oldest first, falling until restart or newer pieces from the same character push them out.
	std::vector<std::unique_ptr<FCharacterPiece>> Pieces{};
	std::size_t CutCount = 0;

	std::vector<FProjectile> Projectiles{};
	// Rebuilt on every start, because the physics world and its character are.
	std::optional<FCharacterMover> Mover{};
	// Eases the body's lean away from a wall it runs on, and remembers which wall, so the lean fades out after the run.
	float WallLean = 0.f;
	FVector3 WallLeanNormal{};
	// Eases between standing and crouched so the view does not snap.
	float EyeHeight = 0.f;
	FGameInput Input{};
	float Yaw = 0.f;
	float Pitch = 0.f;
	float LastDeltaSeconds = 0.f;
	// Distance walked since the capsule's last footstep, when there is no body to time them.
	float StrideDistance = 0.f;

	FGameObjective Objective{};
	std::vector<std::string> Events{};
	FGameAudio Audio{};

	// The animated body, its facing, and the third-person camera.
	std::unique_ptr<FPlayerCharacter> Body{};
	bool bThirdPerson = true;
	bool bJumpedThisUpdate = false;
	bool bWallJumpedThisUpdate = false;
	bool bMantledThisUpdate = false;
	float BodyYaw = 0.f;
	float BoomLength = 0.f;
	// The first-person eye eases after the head, so the camera follows the body's lean without every bob of the walk.
	std::optional<FVector3> SmoothedEye{};

	// While ragdolled, the body draws where it collapsed and its driven joints follow the parts; the mover waits where it was. Getting up
	// eases the body from the parts' last pose back to the animation.
	std::optional<FPhysicsRagdoll> Ragdoll{};
	FMatrix4 RagdollModel{};
	std::vector<FPhysicsBodyTransform> RagdollParts{};
	float GetUpBlend = 0.f;

	// Render view storage referenced by View.
	std::vector<FMatrix4> Models{};
	// The models the previous view rendered, for motion vectors.
	std::vector<FMatrix4> PreviousModels{};
	std::vector<const FRenderMesh*> Meshes{};
	std::vector<std::vector<const FRenderMaterial*>> MaterialSlots{};
	std::vector<std::span<const FRenderMaterial* const>> MaterialSpans{};
	std::vector<FRenderLight> Lights{};
	std::vector<FRenderMeshVertexUpdate> VertexUpdates{};
	std::vector<EMeshVisibility> Visibility{};
	FMeshRenderView View{};
};

FGameSession::FGameSession(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameSession::~FGameSession() = default;

std::expected<std::unique_ptr<FGameSession>, FGameError> FGameSession::Create(const std::span<const FLevelEntity> Entities, const IGameAssets& Assets, IGraphicsDevice* const Device, const FGamePlayerSettings& Player, FTaskSystem* const Tasks, std::shared_ptr<const FCookedModel> Character)
{
	// Built in place: the session holds references and a running simulation that must not move.
	std::unique_ptr<FImplementation> Implementation(new FImplementation{.Assets = Assets, .Device = Device, .Player = Player, .Tasks = Tasks});
	FImplementation& Session = *Implementation;
	Session.Entities.assign(Entities.begin(), Entities.end());

	// The world validates the hierarchy and composes authored world matrices once.
	FWorld World;
	if (auto Replaced = World.ReplaceEntities(Session.Entities); !Replaced)
	{
		return std::unexpected(FGameError{std::format("The level is invalid: {}", Replaced.error().Message)});
	}

	const std::size_t Count = Session.Entities.size();
	std::map<FObjectId, std::size_t> Indices;
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Indices.emplace(Session.Entities[Index].Id, Index);
	}

	Session.AuthoredWorld.resize(Count);
	Session.Parents.resize(Count);
	Session.BoundsMeshes.resize(Count);
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Session.Entities[Index];
		const auto Handle = World.FindEntity(Entity.Id);
		if (!Handle)
		{
			return std::unexpected(FGameError{"Level entity is missing from its world"});
		}

		const auto Matrix = World.GetWorldMatrix(*Handle);
		if (!Matrix)
		{
			return std::unexpected(FGameError{Matrix.error().Message});
		}

		Session.AuthoredWorld[Index] = ToFloatMatrix(*Matrix);
		if (const auto Parent = Indices.find(Entity.Parent); Entity.Parent.IsValid() && Parent != Indices.end())
		{
			Session.Parents[Index] = Parent->second;
		}

		Session.BoundsMeshes[Index] = Entity.Mesh ? Assets.FindMesh(Entity.Mesh->Asset) : nullptr;
	}

	Session.ParentRelative.resize(Count);
	std::vector<bool> Placed(Count, false);
	while (Session.Order.size() < Count)
	{
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			const std::optional<std::size_t>& ParentIndex = Session.Parents[Index];
			if (Placed[Index] || (ParentIndex && !Placed[*ParentIndex]))
			{
				continue;
			}

			Placed[Index] = true;
			Session.Order.push_back(Index);
			const auto ParentInverse = ParentIndex ? TryInverseAffine(Session.AuthoredWorld[*ParentIndex]) : std::optional{FMatrix4{}};
			Session.ParentRelative[Index] = ParentInverse ? *ParentInverse * Session.AuthoredWorld[Index] : Session.AuthoredWorld[Index];
		}
	}

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Session.Entities[Index];
		if (Entity.Trigger && (Entity.Trigger->Role == ETriggerRole::Start || Entity.Trigger->Role == ETriggerRole::Finish))
		{
			const auto Inverse = TryInverseAffine(Session.AuthoredWorld[Index]);
			std::optional<FTriggerZone>& Zone = Entity.Trigger->Role == ETriggerRole::Start ? Session.StartZone : Session.FinishZone;
			if (Inverse && !Zone)
			{
				Zone = FTriggerZone{.Entity = Index, .WorldToLocal = *Inverse, .HalfExtents = Entity.Trigger->Size * 0.5f};
			}
		}

		if (Entity.SoftBody && !Entity.Mesh)
		{
			FSoftBodyMesh& SoftBody = Session.SoftBodies.emplace_back(FSoftBodyMesh{.Entity = Index, .Topology = BuildSoftBodyTopology(*Entity.SoftBody)});
			if (Device)
			{
				auto Mesh = FRenderMesh::Create(*Device, BuildSoftBodyModel(*Entity.SoftBody, SoftBody.Topology, SoftBody.Topology.Vertices), Entity.Name);
				if (!Mesh)
				{
					return std::unexpected(FGameError{std::format("Cannot build the soft body mesh of {}: {}", Entity.Name, Mesh.error().Message)});
				}

				SoftBody.Mesh = std::move(*Mesh);
				Session.BoundsMeshes[Index] = SoftBody.Mesh.get();
			}
		}
	}

	// The player stands on the bottom of the Start zone, facing along its forward axis.
	if (Session.StartZone)
	{
		const FMatrix4& Start = Session.AuthoredWorld[Session.StartZone->Entity];
		const FVector3 Bottom = TransformPoint(Start, {0.f, -Session.StartZone->HalfExtents.Y, 0.f});
		const FVector3 Forward = TransformPoint(Start, FVector3::Forward()) - GetTranslation(Start);
		Session.SpawnPosition = Bottom + FVector3::Up() * 0.05f;
		Session.SpawnYaw = std::atan2(Forward.X, Forward.Z);
	}
	else
	{
		Session.Warnings.emplace_back("The level has no Start trigger; the player starts at the world origin");
		Session.SpawnPosition = FVector3::Up();
	}

	if (!Session.StartZone || !Session.FinishZone)
	{
		Session.Warnings.emplace_back("The course objective needs a Start and a Finish trigger");
	}

	// Characters whose model cannot animate stay as their static mesh.
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Session.Entities[Index];
		std::shared_ptr<const FCookedModel> Model = Entity.Character && Entity.Mesh ? Assets.FindSkinnedModel(Entity.Mesh->Asset) : nullptr;
		if (!Model)
		{
			if (Entity.Character)
			{
				Session.Warnings.push_back(std::format("{} is a Character without a skinned model; it stays a static mesh", Entity.Name));
			}

			continue;
		}

		auto Body = FPlayerCharacter::Create(std::move(Model), Device);
		if (!Body)
		{
			Session.Warnings.push_back(std::format("{} cannot animate: {}", Entity.Name, Body.error().Message));
			continue;
		}

		Session.Characters.push_back({.Entity = Index, .Body = std::move(*Body), .Settings = *Entity.Character});
	}

	if (Device && Character)
	{
		auto Sword = FRenderMesh::Create(*Device, BuildSwordModel(), "Sword");
		if (!Sword)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the sword: {}", Sword.error().Message)});
		}

		Session.SwordMesh = std::move(*Sword);
	}

	// A character that cannot animate leaves the capsule playable rather than failing the level.
	if (Character)
	{
		auto Created = FPlayerCharacter::Create(std::move(Character), Device);
		if (Created)
		{
			Session.Body = std::move(*Created);
		}
		else
		{
			Session.Warnings.push_back(Created.error().Message);
		}
	}

	if (auto Started = Session.Start(); !Started)
	{
		return std::unexpected(Started.error());
	}

	return std::unique_ptr<FGameSession>(new FGameSession(std::move(Implementation)));
}

std::expected<void, FGameError> FGameSession::FImplementation::Start()
{
	Simulation.Stop();
	const FLevelSimulationSetup Setup = BuildLevelSimulation(Entities, AuthoredWorld, BoundsMeshes);
	for (const std::string& Warning : Setup.Warnings)
	{
		if (!std::ranges::contains(Warnings, Warning))
		{
			Warnings.push_back(Warning);
		}
	}

	// A character needs a physics world even when the level has nothing else to simulate.
	const std::array Ground{FLevelSimulationBody{.ObjectIndex = Entities.size(), .Transform = {{0.f, -1000.f, 0.f}, {}, {1.f, 1.f, 1.f}}, .Shape = {.HalfExtents = {0.5f, 0.5f, 0.5f}}}};
	const std::span<const FLevelSimulationBody> Bodies = Setup.Bodies.empty() && Setup.SoftBodies.empty() && Setup.Destructibles.empty() ? std::span<const FLevelSimulationBody>(Ground) : std::span<const FLevelSimulationBody>(Setup.Bodies);
	FPhysicsWorldSettings Settings = LevelPhysicsSettings;
	Settings.Tasks = Tasks;
	Projectiles.clear();
	if (auto Started = Simulation.Start(Bodies, Setup.SoftBodies, Setup.Constraints, Settings, Setup.Destructibles); !Started)
	{
		return std::unexpected(FGameError{std::format("Cannot start physics: {}", Started.error().Message)});
	}

	for (const FLevelSimulationDestructible& Destructible : Setup.Destructibles)
	{
		if (!Device || ChunkMeshes.contains(Destructible.ObjectIndex))
		{
			continue;
		}

		std::vector<std::shared_ptr<const FRenderMesh>>& Chunks = ChunkMeshes[Destructible.ObjectIndex];
		for (std::size_t Chunk = 0; Chunk < Destructible.Fracture->Chunks.size(); ++Chunk)
		{
			auto Mesh = FRenderMesh::Create(*Device, BuildFractureChunkModel(*Destructible.Fracture, Chunk), std::format("{} chunk {}", Entities[Destructible.ObjectIndex].Name, Chunk));
			if (!Mesh)
			{
				return std::unexpected(FGameError{std::format("Cannot upload a chunk of {}: {}", Entities[Destructible.ObjectIndex].Name, Mesh.error().Message)});
			}

			Chunks.push_back(std::move(*Mesh));
		}
	}

	if (Device && !ProjectileMesh)
	{
		auto Mesh = FRenderMesh::Create(*Device, BuildProjectileModel(), "Thrown ball");
		if (!Mesh)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the thrown ball: {}", Mesh.error().Message)});
		}

		ProjectileMesh = std::move(*Mesh);
	}

	constexpr float MantleSecondsWithoutBody = 0.65f;
	const float MantleSeconds = Body ? Body->GetStateSeconds("Mantle").value_or(MantleSecondsWithoutBody) : MantleSecondsWithoutBody;
	auto Created = FCharacterMover::Create(*Simulation.GetPhysicsWorld(), SpawnPosition, Player.Movement, Player.Modes, Player.CustomModes, MantleSeconds);
	if (!Created)
	{
		Simulation.Stop();
		return std::unexpected(FGameError{std::format("Cannot create the player: {}", Created.error().Message)});
	}

	Mover = std::move(*Created);
	if (auto Started = StartCharacters(*Simulation.GetPhysicsWorld()); !Started)
	{
		Simulation.Stop();
		return std::unexpected(FGameError{std::format("Cannot place the characters: {}", Started.error().Message)});
	}

	// The new physics world has none of the old one's ragdoll bodies.
	Ragdoll.reset();
	GetUpBlend = 0.f;
	WallLean = 0.f;
	EyeHeight = Player.EyeHeight;
	Yaw = SpawnYaw;
	Pitch = 0.f;
	BodyYaw = SpawnYaw;
	BoomLength = Player.ThirdPersonDistance;
	SmoothedEye.reset();
	bJumpedThisUpdate = false;
	bWallJumpedThisUpdate = false;
	bMantledThisUpdate = false;
	if (Body)
	{
		Body->Restart();
	}

	Objective.State = StartZone && FinishZone ? EGameObjectiveState::Ready : EGameObjectiveState::Exploring;
	Objective.ElapsedSeconds = 0.;
	Audio.Start(Entities, AuthoredWorld);
	for (FSoftBodyMesh& SoftBody : SoftBodies)
	{
		SoftBody.Revision = 0;
	}

	return {};
}

std::expected<void, FGameError> FGameSession::Restart()
{
	Implementation->Objective.Respawns = 0;
	return Implementation->Start();
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::StartRagdoll()
{
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (!Body || !World)
	{
		return {};
	}

	const FMoverState& State = Mover->GetState();
	RagdollModel = FMatrix4::Translation(State.Foot - FVector3::Up() * Body->GetRootHeight()) * FMatrix4::Rotation(GetBodyRotation());
	const std::vector<FPhysicsRagdollPart> Parts = Body->BuildRagdoll(RagdollModel);
	if (Parts.empty())
	{
		Events.push_back("The player character's skeleton has no ragdoll");
		return {};
	}

	auto Created = World->CreateRagdoll({.Parts = Parts, .LinearVelocity = State.Velocity});
	if (!Created)
	{
		return std::unexpected(Created.error());
	}

	Ragdoll = std::move(*Created);
	GetUpBlend = 0.f;
	return ReadRagdoll(*World);
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::ReadRagdoll(const FPhysicsWorld& World)
{
	RagdollParts.clear();
	for (const FPhysicsBodyId Part : Ragdoll->Parts)
	{
		auto Transform = World.GetBodyTransform(Part);
		if (!Transform)
		{
			return std::unexpected(Transform.error());
		}

		RagdollParts.push_back(*Transform);
	}

	return {};
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::EndRagdoll()
{
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (!Ragdoll || !World)
	{
		return {};
	}

	if (auto Read = ReadRagdoll(*World); !Read)
	{
		return Read;
	}

	// The player stands up over the pelvis, whose capsule lies about its radius above whatever it rests on.
	const FVector3 Pelvis = RagdollParts.front().Position;
	auto Destroyed = World->DestroyRagdoll(*Ragdoll);
	Ragdoll.reset();
	if (!Destroyed)
	{
		return Destroyed;
	}

	GetUpBlend = 1.f;
	return Mover->Teleport(*World, Pelvis - FVector3::Up() * 0.1f);
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::StepPlayer(FPhysicsWorld& World, const float StepSeconds)
{
	if (Ragdoll)
	{
		if (auto Read = ReadRagdoll(World); !Read)
		{
			return Read;
		}

		if (RagdollParts.front().Position.Y >= Player.RespawnHeight)
		{
			return {};
		}

		auto Destroyed = World.DestroyRagdoll(*Ragdoll);
		Ragdoll.reset();
		if (!Destroyed)
		{
			return Destroyed;
		}

		return Respawn(World);
	}

	// The frame's view turn, spread over its fixed steps, brakes the sprint as the game does.
	const float TurnRadians = LastDeltaSeconds > 0.f ? std::abs(Input.YawDelta) * std::min(StepSeconds / LastDeltaSeconds, 1.f) : 0.f;
	const FMoverInput MoverInput{.Move = {std::clamp(Input.Move.X, -1.f, 1.f), std::clamp(Input.Move.Y, -1.f, 1.f)}, .Yaw = Yaw, .TurnRadians = TurnRadians, .bSprint = Input.bSprint, .bCrouch = Input.bCrouch};
	if (auto Stepped = Mover->Step(World, MoverInput, StepSeconds); !Stepped)
	{
		return Stepped;
	}

	for (const EMoverEvent Event : Mover->TakeEvents())
	{
		bJumpedThisUpdate |= Event == EMoverEvent::Jumped;
		bWallJumpedThisUpdate |= Event == EMoverEvent::WallJumped;
		bMantledThisUpdate |= Event == EMoverEvent::Mantled;
	}

	const FMoverState& State = Mover->GetState();
	constexpr float EyeSpeed = 6.f;
	const float TargetEye = State.bCrouched ? Player.CrouchEyeHeight : Player.EyeHeight;
	EyeHeight += std::clamp(TargetEye - EyeHeight, -EyeSpeed * StepSeconds, EyeSpeed * StepSeconds);
	if (State.Foot.Y >= Player.RespawnHeight)
	{
		return {};
	}

	return Respawn(World);
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::Respawn(FPhysicsWorld& World)
{
	++Objective.Respawns;
	Events.push_back("The player fell and returned to the start");
	if (Objective.State != EGameObjectiveState::Exploring)
	{
		Objective.State = EGameObjectiveState::Ready;
		Objective.ElapsedSeconds = 0.;
	}

	Yaw = SpawnYaw;
	return Mover->Teleport(World, SpawnPosition);
}

bool FGameSession::FImplementation::IsInside(const std::optional<FTriggerZone>& Zone, const FVector3& Point) const
{
	if (!Zone)
	{
		return false;
	}

	const FVector3 Local = TransformPoint(Zone->WorldToLocal, Point);
	return std::abs(Local.X) <= Zone->HalfExtents.X && std::abs(Local.Y) <= Zone->HalfExtents.Y && std::abs(Local.Z) <= Zone->HalfExtents.Z;
}

void FGameSession::FImplementation::UpdateObjective(const float DeltaSeconds)
{
	if (Objective.State == EGameObjectiveState::Exploring)
	{
		return;
	}

	// The middle of the body decides which zone the player is in, so a hand over the line does not count.
	const FVector3 Center = Mover->GetState().Foot + FVector3::Up() * (Player.Movement.Height * 0.5f);
	const bool bInStart = IsInside(StartZone, Center);
	if (Objective.State == EGameObjectiveState::Running)
	{
		Objective.ElapsedSeconds += DeltaSeconds;
	}

	if (Objective.State == EGameObjectiveState::Ready && !bInStart)
	{
		Objective.State = EGameObjectiveState::Running;
		Objective.ElapsedSeconds = 0.;
		Events.push_back("Run started");
		Audio.PlayFlat(ELevelSound::Objective, {{.Name = "Finished", .Value = 0.f}});
	}
	else if (Objective.State != EGameObjectiveState::Ready && bInStart)
	{
		Objective.State = EGameObjectiveState::Ready;
		Objective.ElapsedSeconds = 0.;
	}
	else if (Objective.State == EGameObjectiveState::Running && IsInside(FinishZone, Center))
	{
		Objective.State = EGameObjectiveState::Finished;
		const bool bBest = !Objective.BestSeconds || Objective.ElapsedSeconds < *Objective.BestSeconds;
		Objective.BestSeconds = bBest ? Objective.ElapsedSeconds : Objective.BestSeconds;
		Events.push_back(std::format("Course finished in {:.2f} s{}", Objective.ElapsedSeconds, bBest ? ", a new best" : ""));
		Audio.PlayFlat(ELevelSound::Objective, {{.Name = "Finished", .Value = 1.f}});
	}
}

std::expected<void, FGameError> FGameSession::Update(const float DeltaSeconds, const FGameInput& Input)
{
	FImplementation& Session = *Implementation;
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.f || !std::isfinite(Input.YawDelta) || !std::isfinite(Input.PitchDelta) || !std::isfinite(Input.Move.X) || !std::isfinite(Input.Move.Y))
	{
		return std::unexpected(FGameError{"Game updates need a finite, nonnegative frame time and finite input"});
	}

	Session.Input = Input;
	Session.LastDeltaSeconds = DeltaSeconds;
	Session.Yaw = std::remainder(Session.Yaw + Input.YawDelta, 2.f * std::numbers::pi_v<float>);
	Session.Pitch = std::clamp(Session.Pitch + Input.PitchDelta, -MaximumPitch, MaximumPitch);
	if (Input.bJump && !Session.Ragdoll)
	{
		Session.Mover->PressJump();
	}

	if (Input.bToggleCamera)
	{
		Session.bThirdPerson = !Session.bThirdPerson;
	}

	if (Input.bToggleRagdoll)
	{
		if (auto Toggled = Session.Ragdoll ? Session.EndRagdoll() : Session.StartRagdoll(); !Toggled)
		{
			return std::unexpected(FGameError{std::format("Cannot ragdoll: {}", Toggled.error().Message)});
		}
	}

	if (Input.bThrow)
	{
		if (auto Thrown = Session.Throw(); !Thrown)
		{
			return std::unexpected(FGameError{std::format("Cannot throw: {}", Thrown.error().Message)});
		}
	}

	const auto Updated = Session.Simulation.Update(DeltaSeconds, [&Session](FPhysicsWorld& World, const float StepSeconds)
	{
		if (auto Stepped = Session.StepPlayer(World, StepSeconds); !Stepped)
		{
			return Stepped;
		}

		return Session.StepCharacters(World, StepSeconds);
	});

	if (!Updated)
	{
		return std::unexpected(FGameError{std::format("Physics failed: {}", Updated.error().Message)});
	}

	if (auto Aged = Session.AgeProjectiles(DeltaSeconds); !Aged)
	{
		return std::unexpected(FGameError{std::format("Physics failed: {}", Aged.error().Message)});
	}

	for (const FLevelBreakEvent& Break : Session.Simulation.TakeBreakEvents())
	{
		Session.Events.push_back(std::format("{} broke ({} bonds)", Session.Entities[Break.ObjectIndex].Name, Break.BrokenBonds));
		Session.Audio.Play(ELevelSound::Impact, Break.Point, {{.Name = "Impulse", .Value = MaximumImpactImpulse}});
	}

	// The hardest few impacts of the frame sound; the rest would be masked by them anyway.
	std::vector<FLevelImpactEvent> Impacts = Session.Simulation.TakeImpactEvents();
	std::erase_if(Impacts, [](const FLevelImpactEvent& Impact)
	{
		return Impact.Impulse < MinimumImpactImpulse;
	});

	std::ranges::stable_sort(Impacts, std::ranges::greater{}, &FLevelImpactEvent::Impulse);
	Impacts.resize(std::min<std::size_t>(Impacts.size(), MaximumImpactsPerUpdate));
	for (const FLevelImpactEvent& Impact : Impacts)
	{
		Session.Audio.Play(ELevelSound::Impact, Impact.Point, {{.Name = "Impulse", .Value = std::min(Impact.Impulse, MaximumImpactImpulse)}});
	}

	for (const FLevelTriggerEvent& Event : Session.Simulation.TakeTriggerEvents())
	{
		Session.Events.push_back(std::format("{} {} {}", Session.Entities[Event.ObjectIndex].Name, Event.bEntered ? "entered" : "left", Session.Entities[Event.TriggerObjectIndex].Name));
	}

	Session.UpdateObjective(DeltaSeconds);
	if (Session.Body && !Session.Ragdoll)
	{
		if (Input.bAttack)
		{
			Session.Body->FireTrigger("Attack");
		}

		Session.Body->SetBool("Blocking", Input.bBlock);
	}

	const bool bJumped = Session.bJumpedThisUpdate || Session.bWallJumpedThisUpdate;
	Session.UpdateBody(DeltaSeconds);
	if (Session.Body && !Session.Ragdoll)
	{
		if (auto Swept = Session.SweepBlade(); !Swept)
		{
			return std::unexpected(Swept.error());
		}
	}

	Session.UpdateAudio(bJumped);

	if (FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld())
	{
		for (const std::unique_ptr<FCharacterPiece>& Piece : Session.Pieces)
		{
			if (auto Posed = Piece->Update(*World); !Posed)
			{
				return std::unexpected(FGameError{std::format("Physics failed: {}", Posed.error().Message)});
			}
		}
	}

	return {};
}

void FGameSession::FImplementation::UpdateBody(const float DeltaSeconds)
{
	GetUpBlend = std::max(GetUpBlend - DeltaSeconds / 0.4f, 0.f);

	// The lean tilts the view too, so it eases even without a body.
	const FMoverState& State = Mover->GetState();
	constexpr float LeanRate = 6.f;
	WallLean = std::clamp(WallLean + (State.WallRunNormal ? LeanRate : -LeanRate) * DeltaSeconds, 0.f, 1.f);
	WallLeanNormal = State.WallRunNormal.value_or(WallLeanNormal);
	if (!Body)
	{
		return;
	}

	// In first person the body follows the view; in third person it turns to face where it moves and holds that facing when it stops.
	const std::string_view Mode = Mover->GetActiveMode();
	// A ragdoll leaves the mover where it collapsed, still holding the run's speed, which must not step the animation's feet.
	const float Speed = Ragdoll ? 0.f : std::hypot(State.Velocity.X, State.Velocity.Z);
	const float TargetYaw = !bThirdPerson ? Yaw : Speed > 0.5f ? std::atan2(State.Velocity.X, State.Velocity.Z)
	                                                           : BodyYaw;
	const float Turn = std::remainder(TargetYaw - BodyYaw, 2.f * std::numbers::pi_v<float>);
	const float MaximumTurn = (bThirdPerson ? 10.f : 30.f) * DeltaSeconds;
	BodyYaw = std::remainder(BodyYaw + std::clamp(Turn, -MaximumTurn, MaximumTurn), 2.f * std::numbers::pi_v<float>);

	Body->Update(DeltaSeconds, {.GroundSpeed = Speed, .bGrounded = State.bGrounded || Mode == "Mantle", .bCrouched = State.bCrouched, .bSliding = Mode == "Slide", .bWallRunning = Mode == "WallRun", .bJumped = bJumpedThisUpdate, .bWallJumped = bWallJumpedThisUpdate, .bMantled = bMantledThisUpdate});
	bJumpedThisUpdate = false;
	bWallJumpedThisUpdate = false;
	bMantledThisUpdate = false;
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::Throw()
{
	// Old balls make room for new ones, so throwing never runs out.
	if (Projectiles.size() >= MaximumProjectiles)
	{
		if (auto Despawned = Simulation.Despawn(Projectiles.front().Handle); !Despawned)
		{
			return Despawned;
		}

		Projectiles.erase(Projectiles.begin());
	}

	const FVector3 Direction = GetLookOrientation(Yaw, Pitch).RotateVector(FVector3::Forward());
	const FVector3 Eye = Mover->GetState().Foot + FVector3::Up() * EyeHeight;
	const auto Ball = Simulation.Spawn({
	    .Shape = EPhysicsShape::Sphere,
	    .Radius = ProjectileRadius,
	    .Position = Eye + Direction * (Player.Movement.Radius + ProjectileRadius + 0.1f),
	    .MotionType = EPhysicsMotionType::Dynamic,
	    .Properties = {.MassKg = ProjectileMassKg, .Friction = 0.5f, .Restitution = 0.35f, .LinearDamping = 0.01f, .AngularDamping = 0.2f},
	    .bReportContacts = true,
	    .bContinuousCollision = true,
	});

	if (!Ball)
	{
		return std::unexpected(Ball.error());
	}

	const auto BallBody = Simulation.FindSpawnedBody(*Ball);
	if (!BallBody)
	{
		return std::unexpected(FPhysicsError{"Spawned projectile has no physics body"});
	}

	if (auto Launched = Simulation.GetPhysicsWorld()->SetVelocity(*BallBody, Direction * ProjectileSpeed + FVector3{Mover->GetState().Velocity.X, 0.f, Mover->GetState().Velocity.Z}); !Launched)
	{
		return Launched;
	}

	Projectiles.push_back({.Handle = *Ball});
	return {};
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::AgeProjectiles(const float DeltaSeconds)
{
	for (FProjectile& Projectile : Projectiles)
	{
		Projectile.AgeSeconds += DeltaSeconds;
	}

	while (!Projectiles.empty() && Projectiles.front().AgeSeconds > ProjectileLifetimeSeconds)
	{
		if (auto Despawned = Simulation.Despawn(Projectiles.front().Handle); !Despawned)
		{
			return Despawned;
		}

		Projectiles.erase(Projectiles.begin());
	}

	return {};
}

FMatrix4 FGameSession::FImplementation::GetBodyModel() const
{
	return FMatrix4::Translation(Mover->GetState().Foot - FVector3::Up() * Body->GetRootHeight()) * FMatrix4::Rotation(GetBodyRotation());
}

std::expected<void, FGameError> FGameSession::FImplementation::SweepBlade()
{
	for (const std::string& Event : Body->GetEvents())
	{
		if (Event == "SwingStart")
		{
			bSwinging = true;
			SwingCuts.clear();
			SwingPieceCuts.clear();
		}
		else if (Event == "SwingEnd")
		{
			bSwinging = false;
		}
	}

	const std::optional<FMatrix4> Grip = Body->EvaluatePose() ? Body->GetGripFrame() : std::nullopt;
	if (!bSwinging || !Grip)
	{
		LastBlade.reset();
		return {};
	}

	const FMatrix4 Hand = GetBodyModel() * *Grip;
	const std::array Blade{TransformPoint(Hand, {0.f, SwordBladeStart, 0.f}), TransformPoint(Hand, {0.f, SwordBladeEnd, 0.f})};
	const std::optional<std::array<FVector3, 2>> Previous = std::exchange(LastBlade, Blade);
	if (!Previous)
	{
		return {};
	}

	// The blade swept a quad from its last position; its plane is the cut. A blade that barely moved has no plane to cut along.
	const FVector3 Along = Blade[1] - Blade[0];
	const FVector3 Swept = Blade[1] - (*Previous)[1];
	const FVector3 Normal = Along.Cross(Swept);
	if (Normal.LengthSquared() < 1.e-6f)
	{
		return {};
	}

	const FVector3 Unit = Normal.Normalized();
	const std::array Quad{(*Previous)[0], (*Previous)[1], Blade[1], Blade[0]};
	const auto InsideQuad = [&](const FVector3& Point)
	{
		// The point, already on the plane, is inside when it is on the inner side of all four edges.
		for (std::size_t Edge = 0; Edge < 4; ++Edge)
		{
			const FVector3 From = Quad[Edge];
			const FVector3 To = Quad[(Edge + 1) % 4];
			if ((To - From).Cross(Point - From).Dot(Unit) < 0.f)
			{
				return false;
			}
		}

		return true;
	};

	// The blade passes through a body when its vertices lie on both sides of the swept plane and one of them, near the plane, falls inside the
	// swept quad. Evenly picked vertices keep the test cheap.
	const auto Crossed = [&](const std::span<const FCookedVertex> Posed, const FMatrix4& Model)
	{
		bool bFront = false;
		bool bBack = false;
		bool bInside = false;
		const std::size_t Stride = std::max<std::size_t>(Posed.size() / 256, 1);
		for (std::size_t Vertex = 0; Vertex < Posed.size(); Vertex += Stride)
		{
			const auto& Local = Posed[Vertex].Position;
			const FVector3 Point = Model.TransformPosition({Local[0], Local[1], Local[2]});
			const float Distance = Unit.Dot(Point - Blade[0]);
			bFront |= Distance >= 0.f;
			bBack |= Distance < 0.f;
			bInside |= std::abs(Distance) < 0.12f && InsideQuad(Point - Unit * Distance);
		}

		return bFront && bBack && bInside;
	};

	for (std::size_t Index = 0; Index < Characters.size(); ++Index)
	{
		FPlayerCharacter& Character = *Characters[Index].Body;
		if (std::ranges::contains(SwingCuts, Index) || Character.IsEmpty() || !Crossed(Character.GetVertices(), AuthoredWorld[Characters[Index].Entity]))
		{
			continue;
		}

		SwingCuts.push_back(Index);
		const auto Inverse = TryInverseAffine(AuthoredWorld[Characters[Index].Entity]);
		const FVector3 LocalPoint = Inverse ? TransformPoint(*Inverse, Blade[0]) : Blade[0];
		const FVector3 LocalNormal = Inverse ? Inverse->TransformVector(Unit).Normalized() : Unit;
		auto Cut = CutCharacter(Index, [&](FPlayerCharacter& Body)
		{
			return Body.Slice({.Normal = LocalNormal, .Distance = LocalNormal.Dot(LocalPoint)});
		}, Swept.Normalized());

		if (!Cut)
		{
			return std::unexpected(Cut.error());
		}

		if (*Cut)
		{
			Audio.Play(ELevelSound::Slice, (Blade[0] + Blade[1]) * 0.5f);
		}
	}

	// Pieces cut again; the halves of a cut piece are new pieces that this swing may not cut a second time.
	for (std::size_t Index = 0; Index < Pieces.size();)
	{
		const FCharacterPiece* const Piece = Pieces[Index].get();
		if (std::ranges::contains(SwingPieceCuts, Piece) || !Crossed(Piece->GetVertices(), Piece->GetModel()))
		{
			++Index;
			continue;
		}

		auto Cut = CutPiece(Index, Blade[0], Unit);
		if (!Cut)
		{
			return std::unexpected(Cut.error());
		}

		if (!*Cut)
		{
			++Index;
			continue;
		}

		Audio.Play(ELevelSound::Slice, (Blade[0] + Blade[1]) * 0.5f);
		// The cut piece is gone and its two halves were appended; neither is cut again this swing, and Index now names the next piece.
		for (std::size_t Added = Pieces.size() - std::min<std::size_t>(Pieces.size(), 2); Added < Pieces.size(); ++Added)
		{
			SwingPieceCuts.push_back(Pieces[Added].get());
		}
	}

	return {};
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::StartCharacters(FPhysicsWorld& World)
{
	// The physics world that held the pieces is gone, so they only need dropping.
	Pieces.clear();
	CutCount = 0;
	SwingPieceCuts.clear();
	LastBlade.reset();
	bSwinging = false;
	SwingCuts.clear();
	for (FCharacterActor& Actor : Characters)
	{
		Actor.Body->Restart();
		// A standing capsule the player and thrown balls bump into; cuts and hits are found against the body itself.
		auto Collider = World.CreateBody({
		    .Shape = EPhysicsShape::Capsule,
		    .Radius = 0.3f,
		    .HalfHeight = 0.6f,
		    .Position = GetTranslation(AuthoredWorld[Actor.Entity]) + FVector3::Up() * 0.9f,
		    .MotionType = EPhysicsMotionType::Kinematic,
		});
		if (!Collider)
		{
			return std::unexpected(Collider.error());
		}

		Actor.Collider = *Collider;
	}

	return {};
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::StepCharacters(FPhysicsWorld& World, const float StepSeconds)
{
	// Characters animate in the fixed step, so the pose a ball severs against is the same on every run.
	for (FCharacterActor& Actor : Characters)
	{
		Actor.Body->Update(StepSeconds, {.GroundSpeed = 0.f, .bGrounded = true});
		if (auto Posed = Actor.Body->EvaluatePose(); !Posed)
		{
			return std::unexpected(FPhysicsError{Posed.error().Message});
		}
	}

	for (FProjectile& Projectile : Projectiles)
	{
		const auto Ball = Simulation.FindSpawnedBody(Projectile.Handle);
		const auto Transform = Ball ? World.GetBodyTransform(*Ball) : std::unexpected(FPhysicsError{});
		const auto Velocity = Ball ? World.GetLinearVelocity(*Ball) : std::unexpected(FPhysicsError{});
		if (Projectile.bSpent || !Transform || !Velocity)
		{
			continue;
		}

		for (std::size_t Index = 0; Index < Characters.size(); ++Index)
		{
			FCharacterActor& Actor = Characters[Index];
			const FMatrix4& Model = AuthoredWorld[Actor.Entity];
			const FVector3 Foot = GetTranslation(Model);
			const FVector3 Offset = Transform->Position - Foot;
			// Touching the capsule: within its radius plus the ball's, between the feet and the top of the head.
			if (std::hypot(Offset.X, Offset.Z) > 0.3f + ProjectileRadius + 0.05f || Offset.Y < 0.f || Offset.Y > 1.9f)
			{
				continue;
			}

			if (Actor.Body->IsEmpty())
			{
				continue;
			}

			Projectile.bSpent = true;
			Actor.Body->FireTrigger("Hit");
			const float Impulse = Velocity->Length() * ProjectileMassKg;
			const auto Inverse = TryInverseAffine(Model);
			if (Impulse < Actor.Settings.SeverImpulse || !Inverse)
			{
				break;
			}

			const auto Joint = Actor.Body->FindSeverableJoint(TransformPoint(*Inverse, Transform->Position), 0.45f);
			if (Joint)
			{
				auto Severed = CutCharacter(Index, [Joint = *Joint](FPlayerCharacter& Body)
				{
					return Body.Sever(Joint);
				}, Velocity->Normalized());
				if (!Severed)
				{
					return std::unexpected(FPhysicsError{Severed.error().Message});
				}

				if (*Severed)
				{
					Audio.Play(ELevelSound::Sever, Transform->Position);
				}
			}

			break;
		}
	}

	return {};
}

std::expected<bool, FGameError> FGameSession::FImplementation::CutCharacter(const std::size_t Character, const std::function<std::expected<std::optional<FSkinnedMeshPiece>, FGameError>(FPlayerCharacter&)>& Cut, const FVector3& Away)
{
	if (Character >= Characters.size() || Characters[Character].Body->IsEmpty())
	{
		return false;
	}

	FCharacterActor& Actor = Characters[Character];
	auto Severed = Cut(*Actor.Body);
	if (!Severed)
	{
		return std::unexpected(Severed.error());
	}

	if (!*Severed)
	{
		return false;
	}

	++CutCount;
	Actor.Body->FireTrigger("Hit");
	Events.push_back(std::format("{} was cut", Entities[Actor.Entity].Name));
	FPlayerCharacter& Victim = *Actor.Body;
	const FMatrix4& Model = AuthoredWorld[Actor.Entity];
	if (auto Added = AddPiece(Character, Victim.GetCookedModel(), Victim.GetLibrary(), Victim.GetModelTransforms(), std::move(**Severed), Model, Away * 1.5f + FVector3::Up() * 0.5f); !Added)
	{
		return std::unexpected(Added.error());
	}

	// Without its head or its chest a character dies: what is left of it collapses as a ragdoll, and its capsule goes with it.
	const auto Head = Victim.FindJoint("Head");
	const auto Chest = Victim.FindJoint("spine_02");
	if ((Head && !Victim.Carries(*Head)) || (Chest && !Victim.Carries(*Chest)))
	{
		Victim.SetBool("Dead", true);
		FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
		if (auto Rest = Victim.DetachAll(); Rest && World)
		{
			if (auto Added = AddPiece(Character, Victim.GetCookedModel(), Victim.GetLibrary(), Victim.GetModelTransforms(), std::move(*Rest), Model, Away * 0.3f); !Added)
			{
				return std::unexpected(Added.error());
			}

			if (auto Destroyed = World->DestroyBody(std::exchange(Actor.Collider, FPhysicsBodyId{})); !Destroyed)
			{
				return std::unexpected(FGameError{Destroyed.error().Message});
			}
		}
	}

	return true;
}

std::expected<void, FGameError> FGameSession::FImplementation::AddPiece(const std::size_t Character, std::shared_ptr<const FCookedModel> Model, std::shared_ptr<const FAnimationLibrary> Library, const std::span<const FMatrix4> Pose, FSkinnedMeshPiece Mesh, const FMatrix4& ModelToWorld, const FVector3& Velocity)
{
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (!World || Characters[Character].Settings.MaximumPieces < 1.f)
	{
		return {};
	}

	const auto Owned = [&]
	{
		return static_cast<std::size_t>(std::ranges::count_if(Pieces, [Character](const std::unique_ptr<FCharacterPiece>& Piece)
		{
			return Piece->GetOwner() == Character;
		}));
	};

	while (Owned() >= static_cast<std::size_t>(Characters[Character].Settings.MaximumPieces))
	{
		const auto Oldest = std::ranges::find_if(Pieces, [Character](const std::unique_ptr<FCharacterPiece>& Piece)
		{
			return Piece->GetOwner() == Character;
		});

		if (auto Destroyed = (*Oldest)->Destroy(*World); !Destroyed)
		{
			return std::unexpected(FGameError{Destroyed.error().Message});
		}

		Pieces.erase(Oldest);
	}

	auto Created = FCharacterPiece::Create(*World, Device, std::move(Model), std::move(Library), Pose, std::move(Mesh), ModelToWorld, Velocity, Character);
	if (!Created)
	{
		return std::unexpected(Created.error());
	}

	Pieces.push_back(std::move(*Created));
	return {};
}

std::expected<bool, FGameError> FGameSession::FImplementation::CutPiece(const std::size_t Index, const FVector3& Point, const FVector3& Normal)
{
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (Index >= Pieces.size() || !World)
	{
		return false;
	}

	const FCharacterPiece& Piece = *Pieces[Index];
	const auto Inverse = TryInverseAffine(Piece.GetModel());
	if (!Inverse)
	{
		return false;
	}

	const FVector3 LocalNormal = Inverse->TransformVector(Normal).Normalized();
	const FVector3 LocalPoint = TransformPoint(*Inverse, Point);
	auto Sliced = SliceSkinnedMesh(Piece.GetBind(), Piece.GetVertices(), Piece.GetSkinningMatrices(), {.Normal = LocalNormal, .Distance = LocalNormal.Dot(LocalPoint)}, static_cast<std::uint32_t>(Piece.GetCookedModel()->Materials.size()));
	if (!Sliced)
	{
		return std::unexpected(FGameError{Sliced.error().Message});
	}

	if (Sliced->CapLoops == 0 || IsSliver(Sliced->Back) || IsSliver(Sliced->Front))
	{
		return false;
	}

	// The halves leave the cut along its normal on top of the piece's own motion, from the pose the piece is in now.
	const std::shared_ptr<const FCookedModel> Model = Piece.GetCookedModel();
	const std::shared_ptr<const FAnimationLibrary> Library = Piece.GetLibrary();
	const std::vector<FMatrix4> Pose(Piece.GetModelTransforms().begin(), Piece.GetModelTransforms().end());
	const FMatrix4 ModelToWorld = Piece.GetModel();
	const FVector3 Velocity = Piece.GetVelocity(*World);
	const std::size_t Owner = Piece.GetOwner();
	if (auto Destroyed = Pieces[Index]->Destroy(*World); !Destroyed)
	{
		return std::unexpected(FGameError{Destroyed.error().Message});
	}

	Pieces.erase(Pieces.begin() + static_cast<std::ptrdiff_t>(Index));
	++CutCount;
	for (auto& [Half, Sign] : {std::pair{&Sliced->Back, -1.f}, std::pair{&Sliced->Front, 1.f}})
	{
		if (auto Added = AddPiece(Owner, Model, Library, Pose, std::move(*Half), ModelToWorld, Velocity + Normal * Sign); !Added)
		{
			return std::unexpected(Added.error());
		}
	}

	return true;
}

void FGameSession::FImplementation::BuildModels()
{
	const std::size_t Count = Entities.size();
	std::vector<std::optional<FMatrix4>> Simulated(Count);
	for (const FLevelSimulationTransform& Transform : Simulation.GetTransforms())
	{
		if (Transform.ObjectIndex < Count)
		{
			Simulated[Transform.ObjectIndex] = Transform.Transform.ToMatrix();
		}
	}

	Models.resize(Count);
	for (const std::size_t Index : Order)
	{
		const std::optional<FMatrix4>& Pose = Simulated[Index];
		const std::optional<std::size_t>& Parent = Parents[Index];
		Models[Index] = Pose ? *Pose : Parent ? Models[*Parent] * ParentRelative[Index]
		                                      : AuthoredWorld[Index];
	}
}

void FGameSession::FImplementation::BuildSoftBodies()
{
	VertexUpdates.clear();
	const std::uint64_t Revision = Simulation.GetSoftBodyRevision();
	for (FSoftBodyMesh& SoftBody : SoftBodies)
	{
		const std::span<const FVector3> Simulated = Simulation.GetSoftBodyPositions(SoftBody.Entity);
		const std::optional<FSoftBodyComponent>& Component = Entities[SoftBody.Entity].SoftBody;
		if (!Component || !SoftBody.Mesh || Simulated.size() != SoftBody.Topology.Vertices.size() || SoftBody.Revision == Revision)
		{
			continue;
		}

		// Simulated vertices arrive in world space, while the mesh draws with the entity's model matrix.
		const auto WorldToLocal = TryInverseAffine(Models[SoftBody.Entity]);
		if (!WorldToLocal)
		{
			continue;
		}

		std::vector<FVector3> Local(Simulated.size());
		std::ranges::transform(Simulated, Local.begin(), [&WorldToLocal](const FVector3& Position)
		{
			return TransformPoint(*WorldToLocal, Position);
		});

		SoftBody.Vertices = BuildSoftBodyModel(*Component, SoftBody.Topology, Local).Vertices;
		SoftBody.Revision = Revision;
		VertexUpdates.push_back({.Mesh = SoftBody.Mesh.get(), .Vertices = SoftBody.Vertices});
	}
}

const FMeshRenderView& FGameSession::GetRenderView(const FExtent2D Extent)
{
	FImplementation& Session = *Implementation;
	std::swap(Session.Models, Session.PreviousModels);
	Session.BuildModels();
	if (Session.PreviousModels.size() != Session.Models.size())
	{
		Session.PreviousModels = Session.Models;
	}

	Session.BuildSoftBodies();

	const std::size_t Count = Session.Entities.size();
	Session.Meshes.assign(Count, nullptr);
	Session.MaterialSlots.resize(Count);
	Session.MaterialSpans.resize(Count);
	Session.Lights.clear();
	FVisualSettings Visuals{.bStudioPreview = false, .bAutoExposure = true, .DeltaSeconds = Session.LastDeltaSeconds};
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = Session.Entities[Index];
		Session.MaterialSlots[Index].clear();
		if (Entity.Mesh)
		{
			Session.Meshes[Index] = Session.Assets.FindMesh(Entity.Mesh->Asset);
			for (const FAssetId& Material : Entity.Mesh->Materials)
			{
				Session.MaterialSlots[Index].push_back(Material.IsValid() ? Session.Assets.FindMaterial(Material) : nullptr);
			}
		}
		else if (Entity.SoftBody)
		{
			Session.Meshes[Index] = Session.BoundsMeshes[Index];
			Session.MaterialSlots[Index].push_back(Entity.SoftBody->Material.IsValid() ? Session.Assets.FindMaterial(Entity.SoftBody->Material) : nullptr);
		}

		// A broken destructible draws its pieces instead, and a character its animated body.
		if (!Session.Simulation.IsIntact(Index) || std::ranges::contains(Session.Characters, Index, &FImplementation::FCharacterActor::Entity))
		{
			Session.Meshes[Index] = nullptr;
		}

		Session.MaterialSpans[Index] = Session.MaterialSlots[Index];
		if (Entity.Light && Entity.Light->bEnabled)
		{
			Session.Lights.push_back({.Settings = *Entity.Light, .Transform = Session.Models[Index], .Id = Entity.Id});
		}

		if (!Visuals.Atmosphere && Entity.SkyAtmosphere && Entity.SkyAtmosphere->bEnabled)
		{
			Visuals.Atmosphere = Entity.SkyAtmosphere;
		}

		if (!Visuals.Fog && Entity.HeightFog && Entity.HeightFog->bEnabled)
		{
			Visuals.Fog = Entity.HeightFog;
			Visuals.FogHeight = Session.Models[Index](1, 3);
		}
	}

	// The atmosphere's linked sun goes first, where the renderer expects it.
	if (Visuals.Atmosphere && Visuals.Atmosphere->Sun.IsValid())
	{
		const auto Linked = std::ranges::find(Session.Lights, Visuals.Atmosphere->Sun, &FRenderLight::Id);
		if (Linked != Session.Lights.end() && Linked->Settings.Type == ELightType::Directional)
		{
			std::rotate(Session.Lights.begin(), Linked, Linked + 1);
		}
	}

	// Pieces and thrown balls follow the level's own objects; pieces wear their destructible's material.
	for (const FLevelSimulationPiece& Piece : Session.Simulation.GetPieces())
	{
		const auto Chunks = Session.ChunkMeshes.find(Piece.ObjectIndex);
		if (Chunks != Session.ChunkMeshes.end() && Piece.Chunk < Chunks->second.size())
		{
			Session.Models.push_back(FTransform{Piece.Transform.Position, Piece.Transform.Rotation, FVector3::One()}.ToMatrix());
			Session.Meshes.push_back(Chunks->second[Piece.Chunk].get());
			Session.MaterialSpans.push_back(Session.MaterialSpans[Piece.ObjectIndex]);
		}
	}

	for (const FLevelSimulationSpawn& Spawn : Session.Simulation.GetSpawned())
	{
		if (Session.ProjectileMesh)
		{
			Session.Models.push_back(FTransform{Spawn.Transform.Position, Spawn.Transform.Rotation, FVector3::One()}.ToMatrix());
			Session.Meshes.push_back(Session.ProjectileMesh.get());
			Session.MaterialSpans.emplace_back();
		}
	}

	const float Alpha = Session.Simulation.GetInterpolationAlpha();
	const FMoverState& PlayerState = Session.Mover->GetState();
	// A ragdoll's camera follows the pelvis as it would the capsule, from where the capsule's feet would be.
	const FVector3 Foot = Session.Ragdoll ? Session.RagdollParts.front().Position - FVector3::Up() * 0.6f : PlayerState.PreviousFoot * (1.f - Alpha) + PlayerState.Foot * Alpha;
	FVector3 BodyFoot = Foot;
	for (FImplementation::FCharacterActor& Actor : Session.Characters)
	{
		if (Actor.Body->Skin(false) && Actor.Body->GetMesh())
		{
			Session.VertexUpdates.push_back({.Mesh = Actor.Body->GetMesh(), .Vertices = Actor.Body->GetVertices()});
			Session.Models.push_back(Session.AuthoredWorld[Actor.Entity]);
			Session.Meshes.push_back(Actor.Body->GetMesh());
			Session.MaterialSpans.emplace_back();
		}
	}

	for (const std::unique_ptr<FCharacterPiece>& Piece : Session.Pieces)
	{
		if (Piece->GetMesh())
		{
			if (Piece->IsDeforming())
			{
				Session.VertexUpdates.push_back({.Mesh = Piece->GetMesh(), .Vertices = Piece->GetVertices()});
			}

			Session.Models.push_back(Piece->GetModel());
			Session.Meshes.push_back(Piece->GetMesh());
			Session.MaterialSpans.emplace_back();
		}
	}

	Session.Visibility.assign(Session.Models.size(), EMeshVisibility::Everywhere);
	if (Session.Body)
	{
		// Headless sessions still pose the body, because the first-person camera follows its head.
		const bool bFirstPerson = !Session.bThirdPerson && !Session.Ragdoll && Session.Body->GetCameraMesh();
		if (Session.Ragdoll)
		{
			Session.Body->SetRagdollPose(Session.RagdollParts, TryInverseAffine(Session.RagdollModel).value_or(FMatrix4{}), 1.f);
		}
		else
		{
			const FMatrix4 Standing = FMatrix4::Translation(Foot - FVector3::Up() * Session.Body->GetRootHeight()) * FMatrix4::Rotation(Session.GetBodyRotation());
			Session.Body->SetRagdollPose(Session.RagdollParts, TryInverseAffine(Standing).value_or(FMatrix4{}), Session.GetUpBlend);
		}

		const auto Skinned = Session.Body->Skin(bFirstPerson);
		if (Skinned)
		{
			BodyFoot = Foot - FVector3::Up() * Session.Body->GetRootHeight();
		}

		if (Skinned && Session.Body->GetMesh())
		{
			const FMatrix4 BodyModel = Session.Ragdoll ? Session.RagdollModel : FMatrix4::Translation(BodyFoot) * FMatrix4::Rotation(Session.GetBodyRotation());
			Session.VertexUpdates.push_back({.Mesh = Session.Body->GetMesh(), .Vertices = Session.Body->GetVertices()});
			Session.Models.push_back(BodyModel);
			Session.Meshes.push_back(Session.Body->GetMesh());
			Session.MaterialSpans.emplace_back();
			Session.Visibility.push_back(bFirstPerson ? EMeshVisibility::HiddenFromCamera : EMeshVisibility::Everywhere);
			if (bFirstPerson)
			{
				Session.VertexUpdates.push_back({.Mesh = Session.Body->GetCameraMesh(), .Vertices = Session.Body->GetCameraVertices()});
				Session.Models.push_back(BodyModel);
				Session.Meshes.push_back(Session.Body->GetCameraMesh());
				Session.MaterialSpans.emplace_back();
				Session.Visibility.push_back(EMeshVisibility::CameraOnly);
			}

			if (const auto Grip = Session.SwordMesh ? Session.Body->GetGripFrame() : std::nullopt)
			{
				Session.Models.push_back(BodyModel * *Grip);
				Session.Meshes.push_back(Session.SwordMesh.get());
				Session.MaterialSpans.emplace_back();
				Session.Visibility.push_back(EMeshVisibility::Everywhere);
			}
		}
	}

	if (Session.PreviousModels.size() != Session.Models.size())
	{
		Session.PreviousModels = Session.Models;
	}

	Session.Lights.resize(std::min(Session.Lights.size(), MaximumRenderLights));
	Visuals.bStudioPreview = std::ranges::none_of(Session.Entities, [](const FLevelEntity& Entity)
	{
		return Entity.Light.has_value();
	});

	const FVector3 Eye = Session.GetCameraPosition(Foot, BodyFoot);
	const float Aspect = Extent.Height > 0 ? static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height) : 1.f;
	Session.View = {
	    .View = FMatrix4::Rotation(Session.GetViewOrientation().Conjugated()) * FMatrix4::Translation(-Eye),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(Session.Player.VerticalFieldOfView, std::max(Aspect, 0.01f), CameraNear),
	    .Models = Session.Models,
	    .PreviousModels = Session.PreviousModels,
	    .Meshes = Session.Meshes,
	    .Materials = Session.MaterialSpans,
	    .Lights = Session.Lights,
	    .Visuals = Visuals,
	    .VertexUpdates = Session.VertexUpdates,
	    .Visibility = Session.Visibility,
	};

	return Session.View;
}

void FGameSession::FImplementation::UpdateAudio(const bool bJumped)
{
	const FMoverState& State = Mover->GetState();
	const FVector3 Foot = State.Foot;
	const float Speed = std::hypot(State.Velocity.X, State.Velocity.Z);
	if (bJumped)
	{
		Audio.Play(ELevelSound::Jump, Foot);
	}

	for (const std::string& Event : Body ? Body->GetEvents() : std::span<const std::string>{})
	{
		if (Event == "Footstep")
		{
			Audio.Play(ELevelSound::Footstep, Foot, {{.Name = "Speed", .Value = Speed}});
		}
		else if (Event == "Land")
		{
			Audio.Play(ELevelSound::Land, Foot, {{.Name = "Speed", .Value = Speed}});
		}
		else if (Event == "SwingStart")
		{
			Audio.Play(ELevelSound::Swing, Foot + FVector3::Up() * 1.2f);
		}
	}

	// Without a body there are no footstep events, so the capsule ticks off steps by distance walked.
	if (!Body && State.bGrounded && Speed > 0.5f)
	{
		StrideDistance += Speed * LastDeltaSeconds;
		if (StrideDistance >= 1.6f)
		{
			StrideDistance = 0.f;
			Audio.Play(ELevelSound::Footstep, Foot, {{.Name = "Speed", .Value = Speed}});
		}
	}

	Audio.SetLoop(ELevelSound::Slide, Mover->GetActiveMode() == "Slide" && !Ragdoll, Foot);

	if (!Audio.GetEmitterEntities().empty())
	{
		BuildModels();
		Audio.MoveEmitters(Models);
	}

	// The first active listener component hears the level; otherwise the player's head does, in both camera modes, so a third-person boom
	// does not pull every sound behind the player.
	const auto Listener = std::ranges::find_if(Entities, [](const FLevelEntity& Entity)
	{
		return Entity.AudioListener && Entity.AudioListener->bActive;
	});

	if (Listener != Entities.end())
	{
		if (Audio.GetEmitterEntities().empty())
		{
			BuildModels();
		}

		const FMatrix4& Model = Models[static_cast<std::size_t>(Listener - Entities.begin())];
		Audio.SetListener({.Position = GetTranslation(Model), .Forward = Model.TransformVector(FVector3::Forward()).Normalized(), .Up = Model.TransformVector(FVector3::Up()).Normalized()});
	}
	else
	{
		const FQuaternion Look = GetViewOrientation();
		Audio.SetListener({.Position = Foot + FVector3::Up() * EyeHeight, .Forward = Look.RotateVector(FVector3::Forward()), .Up = Look.RotateVector(FVector3::Up())});
	}
}

FQuaternion FGameSession::FImplementation::GetBodyRotation() const
{
	// A wall run tilts the body's head away from the wall around its feet, which stay beside the wall.
	constexpr float MaximumLean = 0.35f;
	const FQuaternion Facing = FQuaternion::FromAxisAngle(FVector3::Up(), BodyYaw);
	const FVector3 Axis = FVector3::Up().Cross(WallLeanNormal);
	if (WallLean <= 0.f || Axis.LengthSquared() < 1.e-6f)
	{
		return Facing;
	}

	return FQuaternion::FromAxisAngle(Axis.Normalized(), WallLean * MaximumLean) * Facing;
}

FQuaternion FGameSession::FImplementation::GetViewOrientation() const
{
	// The view tilts with the lean, 12 degrees at full lean, which sells a wall run in first person.
	constexpr float MaximumTilt = 0.2094f;
	const FQuaternion Look = GetLookOrientation(Yaw, Pitch);
	const FVector3 Axis = FVector3::Up().Cross(WallLeanNormal);
	if (WallLean <= 0.f || Axis.LengthSquared() < 1.e-6f)
	{
		return Look;
	}

	return FQuaternion::FromAxisAngle(Axis.Normalized(), WallLean * MaximumTilt) * Look;
}

FVector3 FGameSession::FImplementation::GetCameraPosition(const FVector3& Foot, const FVector3& BodyFoot)
{
	FVector3 Eye = Foot + FVector3::Up() * EyeHeight;
	const auto Head = Body ? Body->GetHeadPosition() : std::nullopt;
	if (!bThirdPerson && !Ragdoll && Head)
	{
		// Eyes sit in front of and above the head joint, in the body's facing, so the camera stays clear of the neck when the body leans.
		constexpr float EyeAbove = 0.07f;
		constexpr float EyeAhead = 0.24f;
		const FQuaternion Facing = FQuaternion::FromAxisAngle(FVector3::Up(), BodyYaw);
		const FVector3 Target = BodyFoot + GetBodyRotation().RotateVector(*Head) + FVector3::Up() * EyeAbove + Facing.RotateVector(FVector3::Forward()) * EyeAhead;
		const float Follow = 1.f - std::exp(-LastDeltaSeconds / 0.05f);
		SmoothedEye = SmoothedEye ? *SmoothedEye + (Target - *SmoothedEye) * Follow : Target;
		return *SmoothedEye;
	}

	SmoothedEye.reset();
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if ((!bThirdPerson && !Ragdoll) || !World)
	{
		return Eye;
	}

	// The boom snaps in when something comes between the camera and the shoulders, and eases back out so it does not pop.
	// A probe the size of Unreal's spring arm's, so passing beside low geometry does not pull the camera in.
	constexpr float CameraRadius = 0.12f;
	const FQuaternion Look = GetLookOrientation(Yaw, Pitch);
	// The boom pivots at chest height, standing or crouched: low enough to keep the character near the middle of the screen, high enough that
	// the camera clears knee-high blocks.
	const FVector3 Pivot = Foot + FVector3::Up() * (EyeHeight * 0.72f) + Look.RotateVector(-FVector3::Left()) * Player.ThirdPersonShoulderOffset;
	const FVector3 Back = Look.RotateVector(-FVector3::Forward());
	const float Free = World->CastSphere(World->ToWorld(Pivot), World->ToWorld(Pivot + Back * Player.ThirdPersonDistance), CameraRadius) * Player.ThirdPersonDistance;
	BoomLength = Free < BoomLength ? Free : BoomLength + (Free - BoomLength) * std::min(1.f, LastDeltaSeconds * 6.f);
	return Pivot + Back * BoomLength;
}

bool FGameSession::IsThirdPerson() const noexcept
{
	return Implementation->bThirdPerson;
}

std::optional<std::size_t> FGameSession::GetCharacterState() const noexcept
{
	return Implementation->Body ? std::optional(Implementation->Body->GetStateMachine().GetActiveState()) : std::nullopt;
}

std::string_view FGameSession::GetCharacterStateName() const noexcept
{
	if (!Implementation->Body)
	{
		return {};
	}

	const FAnimationStateMachine& Machine = Implementation->Body->GetStateMachine();
	return Machine.GetDesc().States[Machine.GetActiveState()].Name;
}

std::optional<std::size_t> FGameSession::GetCharacterTransition() const noexcept
{
	return Implementation->Body ? Implementation->Body->GetStateMachine().GetActiveTransition() : std::nullopt;
}

std::size_t FGameSession::GetCharacterCount() const noexcept
{
	return Implementation->Characters.size();
}

std::size_t FGameSession::GetCutPieceCount() const noexcept
{
	return Implementation->Pieces.size();
}

std::size_t FGameSession::GetCutCount() const noexcept
{
	return Implementation->CutCount;
}

std::expected<bool, FGameError> FGameSession::SliceCharacter(const std::size_t Character, const FVector3& Point, const FVector3& Normal)
{
	FImplementation& Session = *Implementation;
	if (Character >= Session.Characters.size() || Normal.LengthSquared() < 1.e-8f)
	{
		return false;
	}

	// The plane moves into the character's model space, which has no scale for a placed character.
	const FMatrix4& Model = Session.AuthoredWorld[Session.Characters[Character].Entity];
	const auto Inverse = TryInverseAffine(Model);
	if (!Inverse)
	{
		return false;
	}

	const FVector3 LocalPoint = TransformPoint(*Inverse, Point);
	const FVector3 LocalNormal = Inverse->TransformVector(Normal).Normalized();
	return Session.CutCharacter(Character, [&](FPlayerCharacter& Body)
	{
		return Body.Slice({.Normal = LocalNormal, .Distance = LocalNormal.Dot(LocalPoint)});
	}, Normal.Normalized());
}

std::expected<std::size_t, FGameError> FGameSession::SliceWorld(const FVector3& Point, const FVector3& Normal)
{
	FImplementation& Session = *Implementation;
	if (Normal.LengthSquared() < 1.e-8f)
	{
		return 0;
	}

	const FVector3 Unit = Normal.Normalized();
	std::size_t Cuts = 0;
	// Pieces first, so the halves a character's cut adds are not cut again by the same plane.
	const std::size_t Existing = Session.Pieces.size();
	for (std::size_t Index = 0, Visited = 0; Visited < Existing && Index < Session.Pieces.size(); ++Visited)
	{
		auto Cut = Session.CutPiece(Index, Point, Unit);
		if (!Cut)
		{
			return std::unexpected(Cut.error());
		}

		Cuts += *Cut ? 1 : 0;
		Index += *Cut ? 0 : 1;
	}

	for (std::size_t Character = 0; Character < Session.Characters.size(); ++Character)
	{
		auto Cut = SliceCharacter(Character, Point, Unit);
		if (!Cut)
		{
			return std::unexpected(Cut.error());
		}

		Cuts += *Cut ? 1 : 0;
	}

	return Cuts;
}

std::expected<bool, FGameError> FGameSession::SeverCharacter(const std::size_t Character, const std::string_view Joint)
{
	FImplementation& Session = *Implementation;
	const auto Found = Character < Session.Characters.size() ? Session.Characters[Character].Body->FindJoint(Joint) : std::nullopt;
	if (!Found)
	{
		return false;
	}

	return Session.CutCharacter(Character, [Index = *Found](FPlayerCharacter& Body)
	{
		return Body.Sever(Index);
	}, FVector3::Up());
}

std::span<const std::string> FGameSession::GetCharacterEvents() const noexcept
{
	return Implementation->Body ? Implementation->Body->GetEvents() : std::span<const std::string>{};
}

const FPhysicsWorld* FGameSession::GetPhysicsWorld() const noexcept
{
	return const_cast<FLevelSimulation&>(Implementation->Simulation).GetPhysicsWorld();
}

const FGameObjective& FGameSession::GetObjective() const noexcept
{
	return Implementation->Objective;
}

std::span<const std::string> FGameSession::GetWarnings() const noexcept
{
	return Implementation->Warnings;
}

std::vector<std::string> FGameSession::TakeEvents()
{
	return std::exchange(Implementation->Events, {});
}

FVector3 FGameSession::GetPlayerFootPosition() const noexcept
{
	return Implementation->Mover->GetState().Foot;
}

bool FGameSession::IsPlayerGrounded() const noexcept
{
	return Implementation->Mover->GetState().bGrounded;
}

bool FGameSession::IsPlayerCrouched() const noexcept
{
	return Implementation->Mover->GetState().bCrouched;
}

bool FGameSession::IsPlayerSliding() const noexcept
{
	return Implementation->Mover->GetActiveMode() == "Slide";
}

bool FGameSession::IsPlayerMantling() const noexcept
{
	return Implementation->Mover->GetActiveMode() == "Mantle";
}

bool FGameSession::IsPlayerRagdolled() const noexcept
{
	return Implementation->Ragdoll.has_value();
}

bool FGameSession::IsPlayerWallRunning() const noexcept
{
	return Implementation->Mover->GetActiveMode() == "WallRun";
}

float FGameSession::GetPlayerYaw() const noexcept
{
	return Implementation->Yaw;
}

std::expected<void, FGameError> FGameSession::TeleportPlayer(const FVector3& Foot, const float Yaw)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (!World || !std::isfinite(Foot.X) || !std::isfinite(Foot.Y) || !std::isfinite(Foot.Z) || !std::isfinite(Yaw))
	{
		return std::unexpected(FGameError{"Teleporting the player needs a running session and a finite position and yaw"});
	}

	if (auto Placed = Session.Mover->Teleport(*World, Foot); !Placed)
	{
		return std::unexpected(FGameError{std::format("Cannot teleport the player: {}", Placed.error().Message)});
	}

	Session.Yaw = std::remainder(Yaw, 2.f * std::numbers::pi_v<float>);
	Session.BodyYaw = Session.Yaw;
	Session.SmoothedEye.reset();
	return {};
}

const FCharacterMover& FGameSession::GetMover() const noexcept
{
	return *Implementation->Mover;
}

FVector3 FGameSession::GetPlayerVelocity() const noexcept
{
	return Implementation->Mover->GetState().Velocity;
}

void FGameSession::SetLevelSounds(const FLevelSounds& Sounds)
{
	Implementation->Audio.SetSounds(Sounds);
}

std::vector<FAssetId> FGameSession::GetSounds() const
{
	return Implementation->Audio.GetSounds(Implementation->Entities);
}

std::vector<FAudioCommand> FGameSession::TakeAudioCommands()
{
	return Implementation->Audio.TakeCommands();
}

std::shared_ptr<const FCookedTexture> FGameSession::GetEnvironment() const
{
	for (const FLevelEntity& Entity : Implementation->Entities)
	{
		if (Entity.Light && Entity.Light->Type == ELightType::Sky && Entity.Light->bEnabled && Entity.Light->Environment.IsValid())
		{
			return Implementation->Assets.FindEnvironment(Entity.Light->Environment);
		}
	}

	return nullptr;
}
}
