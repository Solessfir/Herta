#include "Herta/GameFramework/GameSession.h"

#include "Herta/Core/Hash.h"
#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/Animation/MeshSlice.h"
#include "Herta/Math/AffineTransform.h"
#include "CharacterPiece.h"
#include "GameAudio.h"
#include "GameNavigation.h"
#include "GameNpc.h"
#include "Herta/AI/AiWorld.h"
#include "PlayerCharacter.h"
#include "WorldStreamer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <numbers>
#include <string_view>
#include <tuple>
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
				for (const auto& [CornerU, CornerV] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{1, 1}, std::pair{0, 1}})
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

// Line of sight through physics and reachable points from navigation, the world as agents sense it.
class FSessionAiEnvironment final : public IAiEnvironment
{
public:
	FSessionAiEnvironment(const FPhysicsWorld* const InPhysics, const FNavigationWorld* const InNavigation)
	    : Physics(InPhysics)
	    , Navigation(InNavigation)
	{
	}

	bool HasLineOfSight(const FVector3& From, const FVector3& To) const override
	{
		const FVector3 Offset = To - From;
		const float Length = Offset.Length();
		// The ray starts outside the looker's own capsule, which would otherwise hide everything.
		constexpr float CapsuleClearance = 0.45f;
		if (!Physics || Length <= CapsuleClearance + 0.1f)
		{
			return true;
		}

		const FVector3 Start = From + Offset * (CapsuleClearance / Length);
		return !Physics->CastSphereHit(Physics->ToWorld(Start), Physics->ToWorld(To), 0.05f).has_value();
	}

	std::optional<FVector3> FindReachablePoint(const FVector3& Center, const float Radius, std::uint64_t& Random) const override
	{
		if (!Navigation)
		{
			return std::nullopt;
		}

		for (int Attempt = 0; Attempt < 6; ++Attempt)
		{
			const auto Unit = [&Random]
			{
				Random += 0x9e3779b97f4a7c15ull;
				std::uint64_t Value = Random;
				Value = (Value ^ (Value >> 30)) * 0xbf58476d1ce4e5b9ull;
				Value = (Value ^ (Value >> 27)) * 0x94d049bb133111ebull;
				return static_cast<float>((Value ^ (Value >> 31)) >> 40) / static_cast<float>(1ull << 24);
			};

			const float Angle = Unit() * 2.f * std::numbers::pi_v<float>;
			const float Distance = Radius * std::sqrt(Unit());
			const FVector3 Point = Center + FVector3{std::sin(Angle), 0.f, std::cos(Angle)} * Distance;
			if (const auto Nearest = Navigation->FindNearestPoint(Point, {1.f, 3.f, 1.f}))
			{
				return Nearest;
			}
		}

		return std::nullopt;
	}

private:
	const FPhysicsWorld* Physics = nullptr;
	const FNavigationWorld* Navigation = nullptr;
};

struct FTriggerZone
{
	std::size_t Entity = 0;
	FMatrix4 WorldToLocal{};
	FVector3 HalfExtents{};
};

// Everything one player owns. The session processes players one at a time by swapping each one's slot into its own base, so the code
// that moves, animates, and fights a player reads the same members whichever player it serves.
struct FPlayerSlot
{
	// A remote player on a client is not simulated: replicas place it instead.
	bool bSimulated = true;
	FGameInput Input{};
	std::uint32_t Attacks = 0;
	// Rebuilt on every start, because the physics world and its character are.
	std::optional<FCharacterMover> Mover{};
	std::optional<FNavigationAgentId> PlayerAgent{};
	float SprintNoiseSeconds = 0.f;
	// A strike that knocks the player down waits for the step to finish, and the player gets back up on their own after a while.
	std::optional<FVector3> PendingKnockdown{};
	std::optional<float> KnockdownSeconds{};

	// The blade last update, from guard to tip in world space, while a swing is cutting.
	std::optional<std::array<FVector3, 2>> LastBlade{};
	bool bSwinging = false;
	// Characters and pieces the current swing has already cut, so one swing cuts each once.
	std::vector<std::size_t> SwingCuts{};
	std::vector<const FCharacterPiece*> SwingPieceCuts{};

	// Eases the body's lean away from a wall it runs on, and remembers which wall, so the lean fades out after the run.
	float WallLean = 0.f;
	FVector3 WallLeanNormal{};
	// Eases between standing and crouched so the view does not snap.
	float EyeHeight = 0.f;
	float Yaw = 0.f;
	float Pitch = 0.f;
	// Distance walked since the capsule's last footstep, when there is no body to time them.
	float StrideDistance = 0.f;
	FGameObjective Objective{};

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
};

}

struct FGameSession::FImplementation : FPlayerSlot
{
	struct FCharacterActor;

	FImplementation(const IGameAssets& InAssets, IGraphicsDevice* InDevice, const FGamePlayerSettings& InPlayer, FTaskSystem* InTasks);

	[[nodiscard]] std::expected<void, FGameError> Start();
	// Before the physics step: the active player's view, camera, ragdoll, knockdown, and throws.
	[[nodiscard]] std::expected<void, FGameError> BeginPlayerUpdate(FGameInput InInput, float DeltaSeconds);
	// After the physics step: the active player's objective, body, blade, and sounds.
	[[nodiscard]] std::expected<void, FGameError> FinishPlayerUpdate(float DeltaSeconds);
	// After the physics step: projectiles, physics events, every player, effects, and pieces.
	[[nodiscard]] std::expected<void, FGameError> FinishUpdate(float DeltaSeconds);
	[[nodiscard]] std::expected<void, FPhysicsError> StepPlayer(FPhysicsWorld& World, float StepSeconds);
	void UpdateObjective(float DeltaSeconds);
	[[nodiscard]] bool IsInside(const std::optional<FTriggerZone>& Zone, const FVector3& Point) const;
	void BuildModels();
	void BuildSoftBodies();
	[[nodiscard]] std::expected<void, FPhysicsError> Throw();
	[[nodiscard]] std::expected<FLevelSimulationHandle, FPhysicsError> SpawnBall(const FPhysicsBodyTransform& Transform, const FVector3& Velocity);
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
	// Drops what is left of a character as its ragdoll in its current pose, replacing the one it had, and removes its capsule.
	[[nodiscard]] std::expected<void, FGameError> RagdollCharacter(FCharacterActor& Actor, const FVector3& Velocity);
	[[nodiscard]] std::expected<bool, FGameError> CutCharacter(std::size_t Character, const std::function<std::expected<std::optional<FSkinnedMeshPiece>, FGameError>(FPlayerCharacter&)>& Cut, const FVector3& Away);
	// Cuts a piece along a world plane into two pieces; false when the plane misses it or would leave a sliver.
	[[nodiscard]] std::expected<bool, FGameError> CutPiece(std::size_t Piece, const FVector3& Point, const FVector3& Normal);
	[[nodiscard]] std::expected<void, FPhysicsError> StartRagdoll();
	[[nodiscard]] std::expected<void, FPhysicsError> EndRagdoll();
	[[nodiscard]] std::expected<void, FPhysicsError> ReadRagdoll(const FPhysicsWorld& World);
	[[nodiscard]] std::expected<void, FPhysicsError> Respawn(FPhysicsWorld& World);
	// Foot is the capsule's; BodyFoot is where the body is drawn, which differs while a climb clip lifts its own root.
	[[nodiscard]] FVector3 GetCameraPosition(const FVector3& Foot, const FVector3& BodyFoot);
	void RequestEffect(ELevelEffect Cue, const FVector3& Position, const FVector3& Up = {0.f, 1.f, 0.f}, float Scale = 1.f);
	// Starts the sorted requests, moves emitters with their entities, and steps every particle against the physics world.
	void UpdateEffects(float DeltaSeconds);
	void StartEmitters();
	// The active player's sounds, after everything else in the update.
	void UpdateAudio(bool bJumped);
	// Moving emitters and the listener, at the view player.
	void UpdateListener();
	// Takes finished region loads, unloads regions the player left, and activates regions whose assets are ready.
	[[nodiscard]] std::expected<void, FGameError> UpdateRegions();
	[[nodiscard]] std::expected<void, FGameError> ActivateRegion(std::size_t Streamed);
	void UnloadRegion(std::size_t Region);
	// Gives every agent a fresh mind and a crowd agent in the current navigation, and the player a crowd agent others steer around.
	void StartAgents();
	// After the player and the characters stepped: the crowd, then the agents' bodies, then what they sense and decide.
	void StepAgents(FPhysicsWorld& World, float StepSeconds);
	// A swing whose blade starts moving with the player in reach and in front knocks the player down.
	void StrikePlayer(const FCharacterActor& Actor);
	void AddPlayerAgent(FNavigationWorld* World);
	// Where the active player's feet are drawn between the last two fixed steps.
	[[nodiscard]] FVector3 GetViewFoot(float Alpha) const;
	// Poses and skins the active player's body into the view, in first person only for the viewer, and returns where its feet are drawn.
	FVector3 AddBodyToView(float Alpha, bool bViewer);
	// Swaps Player's slot into the base, putting the active one back first.
	void Activate(std::size_t Player) noexcept;
	[[nodiscard]] const FPlayerSlot& GetSlot(std::size_t Player) const noexcept;
	[[nodiscard]] FPlayerSlot& GetSlot(std::size_t Player) noexcept;
	// Runs Visit with Player active, then makes the view player active again.
	template <typename FVisit>
	auto VisitPlayer(std::size_t Player, FVisit&& Visit);
	// The movement half of an update for the active player, without camera, ragdoll, or throw presses, which a replay must not repeat.
	[[nodiscard]] std::expected<void, FPhysicsError> ReplayInput(FGameInput InInput, FPhysicsWorld& World);
	// Runs Visit with each player active in turn, then makes the view player active again, even when Visit stops early by throwing.
	template <typename FVisit>
	void ForEachPlayer(FVisit&& Visit);
	// Runs Visit with each player active until one fails, and returns that failure.
	template <typename FError, typename FVisit>
	[[nodiscard]] std::expected<void, FError> ForEachPlayerUntilError(FVisit&& Visit);
	// Places the active player at the spawn in the current physics world, as a start does.
	[[nodiscard]] std::expected<void, FGameError> StartPlayer();
	[[nodiscard]] FVector3 GetPlayerSpawn() const;
	// Everything but the mover and the ragdoll back to how a start leaves it.
	void ResetPlayerState();
	[[nodiscard]] std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> CreateBody() const;

	const IGameAssets& Assets;
	IGraphicsDevice* Device = nullptr;
	FGamePlayerSettings Player{};
	FTaskSystem* Tasks = nullptr;
	// Every player's slot; the active player's lives in the base, and its entry here holds whatever the swap left.
	std::vector<FPlayerSlot> Players = std::vector<FPlayerSlot>(1);
	std::size_t ActivePlayer = 0;
	// Whose camera the view and the listener follow. Outside per-player loops the view player is the active one.
	std::size_t ViewPlayer = 0;
	std::shared_ptr<const FCookedModel> CharacterModel{};
	// A client leaves agents, destruction, cuts, and throws to the server.
	bool bAuthority = true;
	std::vector<FGameOutcome> Outcomes{};
	std::uint32_t NextProjectileId = 1;

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
	// The setup the simulation last started from, which navigation rebuilds its obstacles from.
	FLevelSimulationSetup Setup{};
	FGameNavigation Navigation{};
	// Recreated with every start, so agents begin each run with nothing remembered.
	std::unique_ptr<FAiWorld> Ai{};
	std::map<FAssetId, std::shared_ptr<const FCookedStateTree>> StateTrees{};
	std::uint64_t AgentsGeneration = 0;
	// Chunk meshes per destructible, built once: fractures are deterministic, so every restart produces the same chunks.
	std::map<std::size_t, std::vector<std::shared_ptr<const FRenderMesh>>> ChunkMeshes{};
	std::shared_ptr<const FRenderMesh> ProjectileMesh{};

	struct FProjectile
	{
		FLevelSimulationHandle Handle{};
		// Which player threw it, so the character it hits knows where the throw came from.
		std::size_t Thrower = 0;
		// The server's id for it, which clients know it by.
		std::uint32_t Id = 0;
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
		// Where it stands now, which an AI agent walks away from where it was authored.
		FMatrix4 Model{};
		std::unique_ptr<FGameNpc> Npc{};
		std::optional<FAiAgentId> Agent{};
		// A character without a leg to stand on lies alive as its own ragdoll, posed in Model's space, and its capsule is gone.
		bool bDowned = false;
		std::optional<FPhysicsRagdoll> Ragdoll{};
		std::vector<FPhysicsBodyTransform> RagdollParts{};
	};

	std::vector<FCharacterActor> Characters{};
	// The sword every player's body holds.
	std::shared_ptr<const FRenderMesh> SwordMesh{};
	// Pieces cut off characters, oldest first, falling until restart or newer pieces from the same character push them out.
	std::vector<std::unique_ptr<FCharacterPiece>> Pieces{};
	std::size_t CutCount = 0;

	std::vector<FProjectile> Projectiles{};
	float LastDeltaSeconds = 0.f;
	std::vector<std::string> Events{};
	FGameAudio Audio{};

	// Effects spawn from requests gathered through the update and sorted before they start, so their order, and the particles' random
	// streams, never depend on which system asked first.
	struct FEffectRequest
	{
		ELevelEffect Cue = ELevelEffect::Impact;
		FVector3 Position{};
		FVector3 Up{0.f, 1.f, 0.f};
		float Scale = 1.f;
	};

	FParticleWorld Particles{};
	FLevelEffects Effects{};
	std::vector<FEffectRequest> EffectRequests{};
	// Auto-play Particle Emitters and the effect each started, which follows its entity.
	std::vector<std::pair<std::size_t, FParticleEffectId>> EmitterEffects{};

	// A loaded region's scenery, placed at its offset. It draws and collides once active, after its assets are ready.
	struct FStreamedRegion
	{
		std::size_t Region = 0;
		FLoadedRegion Loaded{};
		bool bActive = false;
		std::vector<FMatrix4> World{};
		std::vector<const FRenderMesh*> Meshes{};
		std::vector<std::vector<const FRenderMaterial*>> Materials{};
		std::vector<FRenderLight> Lights{};
		std::vector<FLevelSimulationBody> Bodies{};
		std::optional<FLevelStaticGroup> Group{};
	};

	// Destroyed before the regions it fills, so no finished load arrives into a region list that is going away.
	std::vector<FStreamedRegion> StreamedRegions{};
	FWorldStreamer Streamer{};

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

FGameSession::FImplementation::FImplementation(const IGameAssets& InAssets, IGraphicsDevice* const InDevice, const FGamePlayerSettings& InPlayer, FTaskSystem* const InTasks)
    : Assets(InAssets)
    , Device(InDevice)
    , Player(InPlayer)
    , Tasks(InTasks)
{
}

void FGameSession::FImplementation::Activate(const std::size_t InPlayer) noexcept
{
	if (InPlayer == ActivePlayer)
	{
		return;
	}

	// The active player goes back to its entry and the new one moves in, which leaves the new one's entry as the placeholder. Moving through
	// std::exchange rather than std::swap avoids a GCC false positive about the swap temporary's ragdoll.
	FPlayerSlot& Base = *this;
	Players[ActivePlayer] = std::exchange(Base, std::move(Players[InPlayer]));
	ActivePlayer = InPlayer;
}

const FPlayerSlot& FGameSession::FImplementation::GetSlot(const std::size_t InPlayer) const noexcept
{
	return InPlayer == ActivePlayer ? static_cast<const FPlayerSlot&>(*this) : Players[InPlayer];
}

FPlayerSlot& FGameSession::FImplementation::GetSlot(const std::size_t InPlayer) noexcept
{
	return InPlayer == ActivePlayer ? static_cast<FPlayerSlot&>(*this) : Players[InPlayer];
}

namespace
{
// Makes the view player active again however the scope ends.
template <typename FSession>
class FViewRestore final
{
public:
	explicit FViewRestore(FSession& InSession) noexcept
	    : Session(InSession)
	{
	}

	~FViewRestore()
	{
		Session.Activate(Session.ViewPlayer);
	}

	FViewRestore(const FViewRestore&) = delete;
	FViewRestore& operator=(const FViewRestore&) = delete;
	FViewRestore(FViewRestore&&) = delete;
	FViewRestore& operator=(FViewRestore&&) = delete;

private:
	FSession& Session;
};
}

template <typename FVisit>
void FGameSession::FImplementation::ForEachPlayer(FVisit&& Visit)
{
	const FViewRestore Restore(*this);
	for (std::size_t Index = 0; Index < Players.size(); ++Index)
	{
		Activate(Index);
		Visit();
	}
}

template <typename FVisit>
auto FGameSession::FImplementation::VisitPlayer(const std::size_t InPlayer, FVisit&& Visit)
{
	const FViewRestore Restore(*this);
	Activate(InPlayer);
	return Visit();
}

template <typename FError, typename FVisit>
std::expected<void, FError> FGameSession::FImplementation::ForEachPlayerUntilError(FVisit&& Visit)
{
	std::expected<void, FError> Result;
	ForEachPlayer([&Result, &Visit]
	{
		if (Result)
		{
			Result = Visit();
		}
	});

	return Result;
}

std::expected<std::unique_ptr<FPlayerCharacter>, FGameError> FGameSession::FImplementation::CreateBody() const
{
	if (!CharacterModel)
	{
		return nullptr;
	}

	auto Created = FPlayerCharacter::Create(CharacterModel, Device);
	if (!Created)
	{
		return std::unexpected(FGameError{Created.error().Message});
	}

	return std::move(*Created);
}

FGameSession::FGameSession(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameSession::~FGameSession() = default;

std::expected<std::unique_ptr<FGameSession>, FGameError> FGameSession::Create(const std::span<const FLevelEntity> Entities, const IGameAssets& Assets, IGraphicsDevice* const Device, const FGamePlayerSettings& Player, FTaskSystem* const Tasks, std::shared_ptr<const FCookedModel> Character)
{
	// Built in place: the session holds references and a running simulation that must not move.
	auto Implementation = std::make_unique<FImplementation>(Assets, Device, Player, Tasks);
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

		FImplementation::FCharacterActor& Actor = Session.Characters.emplace_back(FImplementation::FCharacterActor{.Entity = Index, .Body = std::move(*Body), .Settings = *Entity.Character, .Model = Session.AuthoredWorld[Index]});
		if (Entity.AiAgent)
		{
			Actor.Npc = std::make_unique<FGameNpc>(*Actor.Body, *Entity.AiAgent);
		}
	}

	// The player and every guard carry the same sword.
	if (Device && (Character || std::ranges::any_of(Session.Characters, [](const FImplementation::FCharacterActor& Actor)
	{
		return Actor.Npc != nullptr;
	})))
	{
		auto Sword = FRenderMesh::Create(*Device, BuildSwordModel(), "Sword");
		if (!Sword)
		{
			return std::unexpected(FGameError{std::format("Cannot upload the sword: {}", Sword.error().Message)});
		}

		Session.SwordMesh = std::move(*Sword);
	}

	// A character that cannot animate leaves the capsule playable rather than failing the level.
	Session.CharacterModel = std::move(Character);
	if (auto Created = Session.CreateBody())
	{
		Session.Body = std::move(*Created);
	}
	else
	{
		Session.CharacterModel.reset();
		Session.Warnings.push_back(Created.error().Message);
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
	Setup = BuildLevelSimulation(Entities, AuthoredWorld, BoundsMeshes);
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

	Simulation.SetBreakable(bAuthority);

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

	if (auto Placed = ForEachPlayerUntilError<FGameError>([this]
	{
		return StartPlayer();
	}); !Placed)
	{
		Simulation.Stop();
		return Placed;
	}

	if (auto Started = StartCharacters(*Simulation.GetPhysicsWorld()); !Started)
	{
		Simulation.Stop();
		return std::unexpected(FGameError{std::format("Cannot place the characters: {}", Started.error().Message)});
	}

	for (FStreamedRegion& Region : StreamedRegions)
	{
		Region.Group.reset();
		if (Region.bActive && !Region.Bodies.empty())
		{
			auto Group = Simulation.AddStaticBodies(Region.Bodies);
			if (!Group)
			{
				return std::unexpected(FGameError{std::format("Cannot restore region bodies: {}", Group.error().Message)});
			}

			Region.Group = *Group;
		}
	}

	if (auto Navigated = Navigation.Start(Entities, AuthoredWorld, Setup); !Navigated)
	{
		return Navigated;
	}

	StartAgents();
	Audio.Start(Entities, AuthoredWorld);
	StartEmitters();
	for (FSoftBodyMesh& SoftBody : SoftBodies)
	{
		SoftBody.Revision = 0;
	}

	return {};
}

std::expected<void, FGameError> FGameSession::FImplementation::StartPlayer()
{
	constexpr float MantleSecondsWithoutBody = 0.65f;
	const float MantleSeconds = Body ? Body->GetStateSeconds("Mantle").value_or(MantleSecondsWithoutBody) : MantleSecondsWithoutBody;
	auto Created = FCharacterMover::Create(*Simulation.GetPhysicsWorld(), GetPlayerSpawn(), Player.Movement, Player.Modes, Player.CustomModes, MantleSeconds);
	if (!Created)
	{
		return std::unexpected(FGameError{std::format("Cannot create the player: {}", Created.error().Message)});
	}

	Mover = std::move(*Created);

	// The new physics world has none of the old one's ragdoll bodies.
	Ragdoll.reset();
	ResetPlayerState();
	return {};
}

FVector3 FGameSession::FImplementation::GetPlayerSpawn() const
{
	// Later players line up beside the first, alternating sides, so nobody spawns inside anyone else.
	constexpr float Spacing = 0.8f;
	const float Side = static_cast<float>((ActivePlayer + 1) / 2) * (ActivePlayer % 2 == 1 ? 1.f : -1.f);
	return SpawnPosition + Movement::Right(SpawnYaw) * (Side * Spacing);
}

void FGameSession::FImplementation::ResetPlayerState()
{
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
	LastBlade.reset();
	bSwinging = false;
	SwingCuts.clear();
	SwingPieceCuts.clear();
	PendingKnockdown.reset();
	KnockdownSeconds.reset();
	Input = {};
	if (Body)
	{
		Body->Restart();
	}

	Objective.State = StartZone && FinishZone ? EGameObjectiveState::Ready : EGameObjectiveState::Exploring;
	Objective.ElapsedSeconds = 0.;
}

std::expected<std::size_t, FGameError> FGameSession::AddPlayer()
{
	FImplementation& Session = *Implementation;
	if (Session.Players.size() >= MaximumPlayers)
	{
		return std::unexpected(FGameError{std::format("A session holds at most {} players", MaximumPlayers)});
	}

	auto Body = Session.CreateBody();
	if (!Body)
	{
		return std::unexpected(Body.error());
	}

	const std::size_t Added = Session.Players.size();
	Session.Players.emplace_back().Body = std::move(*Body);
	Session.Activate(Added);
	auto Started = Session.StartPlayer();
	if (Started)
	{
		Session.AddPlayerAgent(Session.Navigation.GetWorld());
	}

	Session.Activate(Session.ViewPlayer);
	if (!Started)
	{
		Session.Players.pop_back();
		return std::unexpected(Started.error());
	}

	return Added;
}

std::expected<void, FGameError> FGameSession::RemovePlayer(const std::size_t InPlayer)
{
	FImplementation& Session = *Implementation;
	if (InPlayer >= Session.Players.size() || Session.Players.size() == 1)
	{
		return std::unexpected(FGameError{"Only an existing player can leave, and the last one stays"});
	}

	Session.Activate(InPlayer);
	if (FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld())
	{
		if (Session.Ragdoll)
		{
			(void)World->DestroyRagdoll(*Session.Ragdoll);
		}

		(void)World->DestroyCharacter(Session.Mover->GetCharacter());
	}

	if (FNavigationWorld* const Crowd = Session.Navigation.GetWorld(); Crowd && Session.PlayerAgent)
	{
		Crowd->RemoveAgent(*Session.PlayerAgent);
	}

	// Another player's slot replaces the leaving one's in the base, which leaves that player's entry as the active placeholder.
	const std::size_t NextView = Session.ViewPlayer != InPlayer ? Session.ViewPlayer : (InPlayer == 0 ? 1 : 0);
	static_cast<FPlayerSlot&>(Session) = std::move(Session.Players[NextView]);
	Session.Players.erase(Session.Players.begin() + static_cast<std::ptrdiff_t>(InPlayer));
	const auto Shift = [InPlayer](const std::size_t Index)
	{
		return Index > InPlayer ? Index - 1 : Index;
	};

	Session.ActivePlayer = Shift(NextView);
	Session.ViewPlayer = Session.ActivePlayer;
	for (FImplementation::FProjectile& Projectile : Session.Projectiles)
	{
		Projectile.Thrower = Projectile.Thrower == InPlayer ? Session.ViewPlayer : Shift(Projectile.Thrower);
	}

	return {};
}

std::expected<void, FGameError> FGameSession::ResetPlayer(const std::size_t InPlayer)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (InPlayer >= Session.Players.size() || !World)
	{
		return std::unexpected(FGameError{"No such player"});
	}

	return Session.VisitPlayer(InPlayer, [&Session, World]() -> std::expected<void, FGameError>
	{
		if (Session.Ragdoll)
		{
			if (auto Risen = Session.EndRagdoll(); !Risen)
			{
				return std::unexpected(FGameError{Risen.error().Message});
			}
		}

		if (auto Placed = Session.Mover->Teleport(*World, Session.GetPlayerSpawn()); !Placed)
		{
			return std::unexpected(FGameError{Placed.error().Message});
		}

		const std::optional<double> Best = Session.Objective.BestSeconds;
		Session.ResetPlayerState();
		Session.Objective = {.State = Session.Objective.State, .BestSeconds = Best};
		return {};
	});
}

std::expected<void, FGameError> FGameSession::PlacePlayer(const std::size_t InPlayer, const FVector3& Foot)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (InPlayer >= Session.Players.size() || !World || !std::isfinite(Foot.X) || !std::isfinite(Foot.Y) || !std::isfinite(Foot.Z))
	{
		return std::unexpected(FGameError{"Placing a player needs an existing player and a finite position"});
	}

	return Session.VisitPlayer(InPlayer, [&Session, World, &Foot]() -> std::expected<void, FGameError>
	{
		if (auto Placed = Session.Mover->Teleport(*World, Foot); !Placed)
		{
			return std::unexpected(FGameError{Placed.error().Message});
		}

		return {};
	});
}

std::size_t FGameSession::GetPlayerCount() const noexcept
{
	return Implementation->Players.size();
}

std::expected<void, FGameError> FGameSession::SetViewPlayer(const std::size_t InPlayer)
{
	FImplementation& Session = *Implementation;
	if (InPlayer >= Session.Players.size())
	{
		return std::unexpected(FGameError{"No such player"});
	}

	Session.ViewPlayer = InPlayer;
	Session.Activate(InPlayer);
	return {};
}

std::size_t FGameSession::GetViewPlayer() const noexcept
{
	return Implementation->ViewPlayer;
}

struct FGameSessionSnapshotState
{
	struct FPlayer
	{
		FMoverSnapshot Mover{};
		float Yaw = 0.f;
		float Pitch = 0.f;
		float EyeHeight = 0.f;
		float SprintNoiseSeconds = 0.f;
		FGameObjective Objective{};
		std::optional<FVector3> PendingKnockdown{};
		std::optional<float> KnockdownSeconds{};
	};

	FLevelSimulationSnapshot Simulation{};
	std::vector<FPlayer> Players{};
	std::vector<std::pair<FLevelSimulationHandle, float>> Projectiles{};
	std::vector<std::size_t> Throwers{};
	std::vector<bool> Spent{};
};

FGameSessionSnapshot FGameSession::SaveSnapshot() const
{
	const FImplementation& Session = *Implementation;
	auto State = std::make_shared<FGameSessionSnapshotState>();
	State->Simulation = Session.Simulation.SaveSnapshot();
	for (std::size_t Index = 0; Index < Session.Players.size(); ++Index)
	{
		const FPlayerSlot& Slot = Session.GetSlot(Index);
		State->Players.push_back({
		    .Mover = Slot.Mover->SaveSnapshot(),
		    .Yaw = Slot.Yaw,
		    .Pitch = Slot.Pitch,
		    .EyeHeight = Slot.EyeHeight,
		    .SprintNoiseSeconds = Slot.SprintNoiseSeconds,
		    .Objective = Slot.Objective,
		    .PendingKnockdown = Slot.PendingKnockdown,
		    .KnockdownSeconds = Slot.KnockdownSeconds,
		});
	}

	for (const FImplementation::FProjectile& Projectile : Session.Projectiles)
	{
		State->Projectiles.emplace_back(Projectile.Handle, Projectile.AgeSeconds);
		State->Throwers.push_back(Projectile.Thrower);
		State->Spent.push_back(Projectile.bSpent);
	}

	return {.State = std::move(State)};
}

std::expected<void, FGameError> FGameSession::RestoreSnapshot(const FGameSessionSnapshot& Snapshot)
{
	FImplementation& Session = *Implementation;
	if (!Snapshot.State || Snapshot.State->Players.size() != Session.Players.size())
	{
		return std::unexpected(FGameError{"The snapshot was saved with another set of players"});
	}

	if (auto Restored = Session.Simulation.RestoreSnapshot(Snapshot.State->Simulation); !Restored)
	{
		return std::unexpected(FGameError{std::format("Cannot restore the snapshot: {}", Restored.error().Message)});
	}

	FPhysicsWorld& World = *Session.Simulation.GetPhysicsWorld();
	if (auto Placed = Session.ForEachPlayerUntilError<FGameError>([&Session, &World, &Snapshot]() -> std::expected<void, FGameError>
	{
		const FGameSessionSnapshotState::FPlayer& Saved = Snapshot.State->Players[Session.ActivePlayer];
		if (auto Moved = Session.Mover->RestoreSnapshot(World, Saved.Mover); !Moved)
		{
			return std::unexpected(FGameError{std::format("Cannot restore a player: {}", Moved.error().Message)});
		}

		Session.Yaw = Saved.Yaw;
		Session.Pitch = Saved.Pitch;
		Session.EyeHeight = Saved.EyeHeight;
		Session.SprintNoiseSeconds = Saved.SprintNoiseSeconds;
		Session.Objective = Saved.Objective;
		Session.PendingKnockdown = Saved.PendingKnockdown;
		Session.KnockdownSeconds = Saved.KnockdownSeconds;
		return {};
	}); !Placed)
	{
		return Placed;
	}

	Session.Projectiles.clear();
	for (std::size_t Index = 0; Index < Snapshot.State->Projectiles.size(); ++Index)
	{
		const auto& [Handle, Age] = Snapshot.State->Projectiles[Index];
		Session.Projectiles.push_back({.Handle = Handle, .Thrower = Snapshot.State->Throwers[Index], .AgeSeconds = Age, .bSpent = Snapshot.State->Spent[Index]});
	}

	Session.Events.clear();
	return {};
}

std::uint64_t FGameSession::ComputeStateHash() const
{
	const FImplementation& Session = *Implementation;
	const FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	std::vector<float> Values;
	for (std::size_t Index = 0; Index < Session.Players.size(); ++Index)
	{
		const FPlayerSlot& Slot = Session.GetSlot(Index);
		const FMoverState& State = Slot.Mover->GetState();
		Values.insert(Values.end(), {State.Foot.X, State.Foot.Y, State.Foot.Z, State.Velocity.X, State.Velocity.Y, State.Velocity.Z, State.bGrounded ? 1.f : 0.f, State.bCrouched ? 1.f : 0.f});
		Values.insert(Values.end(), {Slot.Yaw, Slot.Pitch, static_cast<float>(Slot.Objective.State), static_cast<float>(Slot.Objective.ElapsedSeconds)});
	}

	const FHash128 Players = HashBytes(std::as_bytes(std::span(Values)));
	return (World ? World->ComputeStateHash() : 0) ^ Players.Low ^ (Players.High << 1);
}

void FGameSession::SetAuthority(const bool bInAuthority)
{
	Implementation->bAuthority = bInAuthority;
	Implementation->Simulation.SetBreakable(bInAuthority);
}

std::expected<void, FGameError> FGameSession::SetPlayerSimulated(const std::size_t InPlayer, const bool bSimulated)
{
	if (InPlayer >= Implementation->Players.size())
	{
		return std::unexpected(FGameError{"No such player"});
	}

	Implementation->GetSlot(InPlayer).bSimulated = bSimulated;
	return {};
}

FGamePlayerReplica FGameSession::CapturePlayer(const std::size_t InPlayer) const
{
	const FImplementation& Session = *Implementation;
	if (InPlayer >= Session.Players.size())
	{
		return {};
	}

	const FPlayerSlot& Slot = Session.GetSlot(InPlayer);
	return {
	    .Mover = Slot.Mover->SaveSnapshot(),
	    .Yaw = Slot.Yaw,
	    .Pitch = Slot.Pitch,
	    .Objective = Slot.Objective,
	    .bRagdolled = Slot.Ragdoll.has_value(),
	    .bBlocking = Slot.Input.bBlock,
	    .Attacks = Slot.Attacks,
	};
}

std::expected<void, FGameError> FGameSession::ApplyPlayer(const std::size_t InPlayer, const FGamePlayerReplica& Replica)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (InPlayer >= Session.Players.size() || !World || !std::isfinite(Replica.Yaw) || !std::isfinite(Replica.Pitch))
	{
		return std::unexpected(FGameError{"A player replica needs an existing player, a running session, and a finite view"});
	}

	return Session.VisitPlayer(InPlayer, [&Session, World, &Replica]() -> std::expected<void, FGameError>
	{
		if (Replica.bRagdolled != Session.Ragdoll.has_value())
		{
			Session.KnockdownSeconds.reset();
			if (auto Toggled = Replica.bRagdolled ? Session.StartRagdoll() : Session.EndRagdoll(); !Toggled)
			{
				return std::unexpected(FGameError{std::format("Cannot ragdoll a replicated player: {}", Toggled.error().Message)});
			}
		}

		if (auto Moved = Session.Mover->RestoreSnapshot(*World, Replica.Mover); !Moved)
		{
			return std::unexpected(FGameError{std::format("Cannot place a replicated player: {}", Moved.error().Message)});
		}

		Session.Yaw = Replica.Yaw;
		Session.Pitch = std::clamp(Replica.Pitch, -MaximumPitch, MaximumPitch);
		Session.Objective = Replica.Objective;
		if (Session.Body && Replica.Attacks > Session.Attacks && !Session.Ragdoll)
		{
			Session.Body->FireTrigger("Attack");
		}

		Session.Attacks = std::max(Session.Attacks, Replica.Attacks);
		if (Session.Body && !Session.bSimulated)
		{
			Session.Body->SetBool("Blocking", Replica.bBlocking);
		}

		return {};
	});
}

std::expected<void, FPhysicsError> FGameSession::FImplementation::ReplayInput(FGameInput InInput, FPhysicsWorld& World)
{
	const float Deflection = std::hypot(InInput.Move.X, InInput.Move.Y);
	if (Deflection > 1.f)
	{
		InInput.Move = {InInput.Move.X / Deflection, InInput.Move.Y / Deflection};
	}

	Input = InInput;
	Yaw = std::remainder(Yaw + Input.YawDelta, 2.f * std::numbers::pi_v<float>);
	Pitch = std::clamp(Pitch + Input.PitchDelta, -MaximumPitch, MaximumPitch);
	if (Input.bJump && !Ragdoll)
	{
		Mover->PressJump();
	}

	return StepPlayer(World, static_cast<float>(LevelFixedStep));
}

std::expected<void, FGameError> FGameSession::ReplayPlayer(const std::size_t InPlayer, const std::span<const FGameInput> Inputs)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (InPlayer >= Session.Players.size() || !World)
	{
		return std::unexpected(FGameError{"Replaying needs an existing player and a running session"});
	}

	return Session.VisitPlayer(InPlayer, [&Session, World, Inputs]() -> std::expected<void, FGameError>
	{
		// The replayed steps already played their sounds and animation the first time.
		const std::array Flags{Session.bJumpedThisUpdate, Session.bWallJumpedThisUpdate, Session.bMantledThisUpdate};
		for (const FGameInput& Replayed : Inputs)
		{
			if (auto Stepped = Session.ReplayInput(Replayed, *World); !Stepped)
			{
				return std::unexpected(FGameError{std::format("Cannot replay the player: {}", Stepped.error().Message)});
			}
		}

		Session.bJumpedThisUpdate = Flags[0];
		Session.bWallJumpedThisUpdate = Flags[1];
		Session.bMantledThisUpdate = Flags[2];
		return {};
	});
}

std::vector<FLevelBodyReplica> FGameSession::CaptureBodies() const
{
	return Implementation->Simulation.CaptureDynamicBodies();
}

std::expected<void, FGameError> FGameSession::ApplyBodies(const std::span<const FLevelBodyReplica> Bodies)
{
	for (const FLevelBodyReplica& Body : Bodies)
	{
		if (auto Applied = Implementation->Simulation.ApplyDynamicBody(Body); !Applied)
		{
			return std::unexpected(FGameError{std::format("Cannot place a replicated body: {}", Applied.error().Message)});
		}
	}

	return {};
}

std::vector<FGameProjectileReplica> FGameSession::CaptureProjectiles() const
{
	const FImplementation& Session = *Implementation;
	const FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	std::vector<FGameProjectileReplica> Replicas;
	for (const FImplementation::FProjectile& Projectile : Session.Projectiles)
	{
		const auto Ball = Session.Simulation.FindSpawnedBody(Projectile.Handle);
		const auto Transform = Ball && World ? World->GetBodyTransform(*Ball) : std::unexpected(FPhysicsError{});
		const auto Velocity = Ball && World ? World->GetLinearVelocity(*Ball) : std::unexpected(FPhysicsError{});
		if (Transform && Velocity)
		{
			Replicas.push_back({.Id = Projectile.Id, .Transform = *Transform, .LinearVelocity = *Velocity});
		}
	}

	return Replicas;
}

std::expected<void, FGameError> FGameSession::ApplyProjectiles(const std::span<const FGameProjectileReplica> Replicas)
{
	FImplementation& Session = *Implementation;
	FPhysicsWorld* const World = Session.Simulation.GetPhysicsWorld();
	if (!World)
	{
		return std::unexpected(FGameError{"Replicated balls need a running session"});
	}

	for (std::size_t Index = 0; Index < Session.Projectiles.size();)
	{
		const std::uint32_t Id = Session.Projectiles[Index].Id;
		if (std::ranges::any_of(Replicas, [Id](const FGameProjectileReplica& Replica)
		{
			return Replica.Id == Id;
		}))
		{
			++Index;
			continue;
		}

		if (auto Despawned = Session.Simulation.Despawn(Session.Projectiles[Index].Handle); !Despawned)
		{
			return std::unexpected(FGameError{Despawned.error().Message});
		}

		Session.Projectiles.erase(Session.Projectiles.begin() + static_cast<std::ptrdiff_t>(Index));
	}

	for (const FGameProjectileReplica& Replica : Replicas)
	{
		const auto Known = std::ranges::find(Session.Projectiles, Replica.Id, &FImplementation::FProjectile::Id);
		if (Known == Session.Projectiles.end())
		{
			auto Ball = Session.SpawnBall(Replica.Transform, Replica.LinearVelocity);
			if (!Ball)
			{
				return std::unexpected(FGameError{std::format("Cannot spawn a replicated ball: {}", Ball.error().Message)});
			}

			Session.Projectiles.push_back({.Handle = *Ball, .Thrower = Session.ViewPlayer, .Id = Replica.Id});
			continue;
		}

		const auto Ball = Session.Simulation.FindSpawnedBody(Known->Handle);
		if (Ball)
		{
			if (auto Placed = World->SetBodyState(*Ball, Replica.Transform, Replica.LinearVelocity, FVector3::Zero()); !Placed)
			{
				return std::unexpected(FGameError{Placed.error().Message});
			}
		}
	}

	return {};
}

std::vector<FGameNpcReplica> FGameSession::CaptureNpcs() const
{
	std::vector<FGameNpcReplica> Replicas;
	const auto& Characters = Implementation->Characters;
	for (std::size_t Index = 0; Index < Characters.size(); ++Index)
	{
		if (const FGameNpc* const Npc = Characters[Index].Npc.get())
		{
			Replicas.push_back({
			    .Character = static_cast<std::uint32_t>(Index),
			    .Foot = Npc->GetFootPosition(),
			    .Yaw = Npc->GetYaw(),
			    .GroundSpeed = Npc->GetGroundSpeed(),
			    .Health = Npc->GetHealth(),
			    .Attacks = Npc->GetAttackCount(),
			});
		}
	}

	return Replicas;
}

void FGameSession::ApplyNpcs(const std::span<const FGameNpcReplica> Replicas)
{
	auto& Characters = Implementation->Characters;
	for (const FGameNpcReplica& Replica : Replicas)
	{
		const bool bFinite = std::isfinite(Replica.Foot.X) && std::isfinite(Replica.Foot.Y) && std::isfinite(Replica.Foot.Z) && std::isfinite(Replica.Yaw) && std::isfinite(Replica.GroundSpeed) && std::isfinite(Replica.Health);
		if (Replica.Character < Characters.size() && Characters[Replica.Character].Npc && bFinite)
		{
			Characters[Replica.Character].Npc->SetPuppet(Replica.Foot, Replica.Yaw, Replica.GroundSpeed, std::clamp(Replica.Health, 0.f, 1.f), Replica.Attacks);
		}
	}
}

std::vector<FGameOutcome> FGameSession::TakeOutcomes()
{
	return std::exchange(Implementation->Outcomes, {});
}

std::expected<void, FGameError> FGameSession::ApplyOutcome(const FGameOutcome& Outcome)
{
	FImplementation& Session = *Implementation;
	switch (Outcome.Kind)
	{
		case EGameOutcome::Break:
		{
			if (Outcome.Object >= Session.Entities.size())
			{
				return std::unexpected(FGameError{"A break names no level object"});
			}

			if (auto Broken = Session.Simulation.RestoreBonds(Outcome.Object, Outcome.IntactBonds); !Broken)
			{
				return std::unexpected(FGameError{std::format("Cannot break a replicated destructible: {}", Broken.error().Message)});
			}

			return {};
		}
		case EGameOutcome::Slice:
		{
			if (auto Cut = SliceCharacter(Outcome.Object, Outcome.Point, Outcome.Normal); !Cut)
			{
				return std::unexpected(Cut.error());
			}

			return {};
		}
		case EGameOutcome::Sever:
		{
			if (auto Cut = SeverCharacter(Outcome.Object, Outcome.Joint); !Cut)
			{
				return std::unexpected(Cut.error());
			}

			return {};
		}
	}

	return std::unexpected(FGameError{"Unknown outcome"});
}

std::expected<void, FGameError> FGameSession::Restart()
{
	Implementation->ForEachPlayer([&Session = *Implementation]
	{
		Session.Objective.Respawns = 0;
	});

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
	if (Implementation->Players.size() != 1)
	{
		return std::unexpected(FGameError{"A session with several players needs an input for each"});
	}

	return UpdatePlayers(DeltaSeconds, std::span(&Input, 1));
}

std::expected<void, FGameError> FGameSession::UpdatePlayers(const float DeltaSeconds, const std::span<const FGameInput> Inputs)
{
	FImplementation& Session = *Implementation;
	if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.f)
	{
		return std::unexpected(FGameError{"Game updates need a finite, nonnegative frame time"});
	}

	if (Inputs.size() != Session.Players.size())
	{
		return std::unexpected(FGameError{std::format("The session has {} players but got {} inputs", Session.Players.size(), Inputs.size())});
	}

	if (std::ranges::any_of(Inputs, [](const FGameInput& Input)
	{
		return !std::isfinite(Input.YawDelta) || !std::isfinite(Input.PitchDelta) || !std::isfinite(Input.Move.X) || !std::isfinite(Input.Move.Y);
	}))
	{
		return std::unexpected(FGameError{"Game input must be finite"});
	}

	Session.LastDeltaSeconds = DeltaSeconds;
	if (auto Begun = Session.ForEachPlayerUntilError<FGameError>([&Session, Inputs, DeltaSeconds]
	{
		return Session.BeginPlayerUpdate(Inputs[Session.ActivePlayer], DeltaSeconds);
	}); !Begun)
	{
		return Begun;
	}

	if (auto Streamed = Session.UpdateRegions(); !Streamed)
	{
		return Streamed;
	}

	const auto Updated = Session.Simulation.Update(DeltaSeconds, [&Session](FPhysicsWorld& World, const float StepSeconds)
	{
		if (auto Stepped = Session.ForEachPlayerUntilError<FPhysicsError>([&Session, &World, StepSeconds]
		{
			return Session.bSimulated ? Session.StepPlayer(World, StepSeconds) : std::expected<void, FPhysicsError>{};
		}); !Stepped)
		{
			return Stepped;
		}

		if (auto Stepped = Session.StepCharacters(World, StepSeconds); !Stepped)
		{
			return Stepped;
		}

		Session.StepAgents(World, StepSeconds);
		return std::expected<void, FPhysicsError>{};
	});

	if (!Updated)
	{
		return std::unexpected(FGameError{std::format("Physics failed: {}", Updated.error().Message)});
	}

	return Session.FinishUpdate(DeltaSeconds);
}

std::expected<void, FGameError> FGameSession::FImplementation::BeginPlayerUpdate(FGameInput InInput, const float DeltaSeconds)
{
	if (!bSimulated)
	{
		Input = {};
		return {};
	}

	// Network input is untrusted, so a stick past full deflection counts as full deflection.
	const float Deflection = std::hypot(InInput.Move.X, InInput.Move.Y);
	if (Deflection > 1.f)
	{
		InInput.Move = {InInput.Move.X / Deflection, InInput.Move.Y / Deflection};
	}

	Input = InInput;
	Yaw = std::remainder(Yaw + Input.YawDelta, 2.f * std::numbers::pi_v<float>);
	Pitch = std::clamp(Pitch + Input.PitchDelta, -MaximumPitch, MaximumPitch);
	if (Input.bJump && !Ragdoll)
	{
		Mover->PressJump();
	}

	if (Input.bToggleCamera)
	{
		bThirdPerson = !bThirdPerson;
	}

	if (Input.bToggleRagdoll)
	{
		KnockdownSeconds.reset();
		if (auto Toggled = Ragdoll ? EndRagdoll() : StartRagdoll(); !Toggled)
		{
			return std::unexpected(FGameError{std::format("Cannot ragdoll: {}", Toggled.error().Message)});
		}
	}

	if (const auto Push = std::exchange(PendingKnockdown, std::nullopt); Push && !Ragdoll && Body)
	{
		if (auto Fallen = StartRagdoll(); !Fallen)
		{
			return std::unexpected(FGameError{std::format("Cannot knock the player down: {}", Fallen.error().Message)});
		}

		FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
		for (std::size_t Part = 0; World && Ragdoll && Part < std::min<std::size_t>(Ragdoll->Parts.size(), 3); ++Part)
		{
			(void)World->AddImpulse(Ragdoll->Parts[Part], *Push, RagdollParts[Part].Position);
		}

		KnockdownSeconds = 0.f;
	}

	constexpr float KnockdownGetUpSeconds = 2.5f;
	if (KnockdownSeconds && (*KnockdownSeconds += DeltaSeconds) >= KnockdownGetUpSeconds)
	{
		KnockdownSeconds.reset();
		if (auto Risen = EndRagdoll(); !Risen)
		{
			return std::unexpected(FGameError{std::format("Cannot get up: {}", Risen.error().Message)});
		}
	}

	if (Input.bThrow && bAuthority)
	{
		if (auto Thrown = Throw(); !Thrown)
		{
			return std::unexpected(FGameError{std::format("Cannot throw: {}", Thrown.error().Message)});
		}
	}

	return {};
}

std::expected<void, FGameError> FGameSession::FImplementation::FinishUpdate(const float DeltaSeconds)
{
	if (auto Aged = bAuthority ? AgeProjectiles(DeltaSeconds) : std::expected<void, FPhysicsError>{}; !Aged)
	{
		return std::unexpected(FGameError{std::format("Physics failed: {}", Aged.error().Message)});
	}

	for (const FLevelBreakEvent& Break : Simulation.TakeBreakEvents())
	{
		Outcomes.push_back({.Kind = EGameOutcome::Break, .Object = static_cast<std::uint32_t>(Break.ObjectIndex), .IntactBonds = Simulation.GetIntactBonds(Break.ObjectIndex)});
		Events.push_back(std::format("{} broke ({} bonds)", Entities[Break.ObjectIndex].Name, Break.BrokenBonds));
		Audio.Play(ELevelSound::Impact, Break.Point, {{.Name = "Impulse", .Value = MaximumImpactImpulse}});
		RequestEffect(ELevelEffect::Break, Break.Point);
		if (Ai)
		{
			Ai->PostNoise(Break.Point, 30.f);
		}
	}

	// The hardest few impacts of the frame sound; the rest would be masked by them anyway.
	std::vector<FLevelImpactEvent> Impacts = Simulation.TakeImpactEvents();
	std::erase_if(Impacts, [](const FLevelImpactEvent& Impact)
	{
		return Impact.Impulse < MinimumImpactImpulse;
	});

	std::ranges::stable_sort(Impacts, std::ranges::greater{}, &FLevelImpactEvent::Impulse);
	Impacts.resize(std::min<std::size_t>(Impacts.size(), MaximumImpactsPerUpdate));
	for (const FLevelImpactEvent& Impact : Impacts)
	{
		Audio.Play(ELevelSound::Impact, Impact.Point, {{.Name = "Impulse", .Value = std::min(Impact.Impulse, MaximumImpactImpulse)}});
		// Harder hits throw a bigger cloud, up to twice the authored size.
		const FVector3 Up = Impact.Normal.Y < 0.f ? -Impact.Normal : Impact.Normal;
		RequestEffect(ELevelEffect::Impact, Impact.Point, Up, std::clamp(Impact.Impulse / 20.f, 0.5f, 2.f));
		// Harder impacts carry further: a thrown ball's thud is heard about fifteen meters away.
		if (Ai)
		{
			Ai->PostNoise(Impact.Point, std::clamp(Impact.Impulse * 0.6f, 4.f, 25.f));
		}
	}

	for (const FLevelTriggerEvent& Event : Simulation.TakeTriggerEvents())
	{
		Events.push_back(std::format("{} {} {}", Entities[Event.ObjectIndex].Name, Event.bEntered ? "entered" : "left", Entities[Event.TriggerObjectIndex].Name));
	}

	if (auto Finished = ForEachPlayerUntilError<FGameError>([this, DeltaSeconds]
	{
		return FinishPlayerUpdate(DeltaSeconds);
	}); !Finished)
	{
		return Finished;
	}

	UpdateListener();
	UpdateEffects(DeltaSeconds);

	if (FPhysicsWorld* const World = Simulation.GetPhysicsWorld())
	{
		for (const std::unique_ptr<FCharacterPiece>& Piece : Pieces)
		{
			if (auto Posed = Piece->Update(*World); !Posed)
			{
				return std::unexpected(FGameError{std::format("Physics failed: {}", Posed.error().Message)});
			}
		}
	}

	return {};
}

std::expected<void, FGameError> FGameSession::FImplementation::FinishPlayerUpdate(const float DeltaSeconds)
{
	if (bSimulated)
	{
		UpdateObjective(DeltaSeconds);
	}

	if (Body && !Ragdoll)
	{
		if (Input.bAttack)
		{
			Body->FireTrigger("Attack");
			++Attacks;
		}

		Body->SetBool("Blocking", Input.bBlock);
	}

	const bool bJumped = bJumpedThisUpdate || bWallJumpedThisUpdate;
	UpdateBody(DeltaSeconds);
	if (Body && !Ragdoll && bAuthority)
	{
		if (auto Swept = SweepBlade(); !Swept)
		{
			return std::unexpected(Swept.error());
		}
	}

	UpdateAudio(bJumped);
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
	const FVector3 Velocity = Direction * ProjectileSpeed + FVector3{Mover->GetState().Velocity.X, 0.f, Mover->GetState().Velocity.Z};
	const auto Ball = SpawnBall({.Position = Eye + Direction * (Player.Movement.Radius + ProjectileRadius + 0.1f), .Rotation = FQuaternion::Identity()}, Velocity);
	if (!Ball)
	{
		return std::unexpected(Ball.error());
	}

	Projectiles.push_back({.Handle = *Ball, .Thrower = ActivePlayer, .Id = NextProjectileId++});
	return {};
}

std::expected<FLevelSimulationHandle, FPhysicsError> FGameSession::FImplementation::SpawnBall(const FPhysicsBodyTransform& Transform, const FVector3& Velocity)
{
	const auto Ball = Simulation.Spawn({
	    .Shape = EPhysicsShape::Sphere,
	    .Radius = ProjectileRadius,
	    .Position = Transform.Position,
	    .Rotation = Transform.Rotation,
	    .MotionType = EPhysicsMotionType::Dynamic,
	    .Properties = {.MassKg = ProjectileMassKg, .Friction = 0.5f, .Restitution = 0.35f, .LinearDamping = 0.01f, .AngularDamping = 0.2f},
	    .bReportContacts = true,
	    .bContinuousCollision = true,
	});

	if (!Ball)
	{
		return Ball;
	}

	const auto BallBody = Simulation.FindSpawnedBody(*Ball);
	if (!BallBody)
	{
		return std::unexpected(FPhysicsError{"Spawned projectile has no physics body"});
	}

	if (auto Launched = Simulation.GetPhysicsWorld()->SetVelocity(*BallBody, Velocity); !Launched)
	{
		return std::unexpected(Launched.error());
	}

	return Ball;
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
		if (std::ranges::contains(SwingCuts, Index) || Character.IsEmpty() || !Crossed(Character.GetVertices(), Characters[Index].Model))
		{
			continue;
		}

		SwingCuts.push_back(Index);
		const auto Inverse = TryInverseAffine(Characters[Index].Model);
		const FVector3 LocalPoint = Inverse ? TransformPoint(*Inverse, Blade[0]) : Blade[0];
		const FVector3 LocalNormal = Inverse ? Inverse->TransformVector(Unit).Normalized() : Unit;
		auto Cut = CutCharacter(Index, [&](FPlayerCharacter& Victim)
		{
			return Victim.Slice({.Normal = LocalNormal, .Distance = LocalNormal.Dot(LocalPoint)});
		}, Swept.Normalized());

		if (!Cut)
		{
			return std::unexpected(Cut.error());
		}

		if (*Cut)
		{
			Outcomes.push_back({.Kind = EGameOutcome::Slice, .Object = static_cast<std::uint32_t>(Index), .Point = Blade[0], .Normal = Unit});
			Audio.Play(ELevelSound::Slice, (Blade[0] + Blade[1]) * 0.5f);
			RequestEffect(ELevelEffect::Slice, (Blade[0] + Blade[1]) * 0.5f, Unit);
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
		RequestEffect(ELevelEffect::Slice, (Blade[0] + Blade[1]) * 0.5f, Unit);
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
	for (FCharacterActor& Actor : Characters)
	{
		Actor.Body->Restart();
		Actor.Model = AuthoredWorld[Actor.Entity];
		Actor.bDowned = false;
		Actor.Ragdoll.reset();
		Actor.RagdollParts.clear();
		// A standing capsule the player and thrown balls bump into; cuts and hits are found against the body itself.
		auto Collider = World.CreateBody({
		    .Shape = EPhysicsShape::Capsule,
		    .Radius = 0.3f,
		    .HalfHeight = 0.6f,
		    .Position = GetTranslation(Actor.Model) + FVector3::Up() * 0.9f,
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
		if (Actor.Ragdoll)
		{
			Actor.RagdollParts.clear();
			for (const FPhysicsBodyId Part : Actor.Ragdoll->Parts)
			{
				auto Transform = World.GetBodyTransform(Part);
				if (!Transform)
				{
					return std::unexpected(Transform.error());
				}

				Actor.RagdollParts.push_back(*Transform);
			}

			Actor.Body->SetRagdollPose(Actor.RagdollParts, TryInverseAffine(Actor.Model).value_or(FMatrix4{}), 1.f);
		}

		Actor.Body->Update(StepSeconds, {.GroundSpeed = Actor.Npc ? Actor.Npc->GetGroundSpeed() : 0.f, .bGrounded = true});
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
		if (!bAuthority || Projectile.bSpent || !Transform || !Velocity)
		{
			continue;
		}

		for (std::size_t Index = 0; Index < Characters.size(); ++Index)
		{
			FCharacterActor& Actor = Characters[Index];
			const FMatrix4& Model = Actor.Model;
			const FVector3 Foot = GetTranslation(Model);
			const FVector3 Offset = Transform->Position - Foot;
			// Touching the capsule: within its radius plus the ball's, between the feet and the top of the head. A downed body lies within about
			// its height of its pelvis.
			const bool bTouching = Actor.RagdollParts.empty()
			    ? std::hypot(Offset.X, Offset.Z) <= 0.3f + ProjectileRadius + 0.05f && Offset.Y >= 0.f && Offset.Y <= 1.9f
			    : (Transform->Position - Actor.RagdollParts.front().Position).Length() <= 0.9f + ProjectileRadius;
			if (!bTouching)
			{
				continue;
			}

			if (Actor.Body->IsEmpty())
			{
				continue;
			}

			Projectile.bSpent = true;
			Actor.Body->FireTrigger("Hit");
			if (Ai && Actor.Agent)
			{
				Ai->ReportHit(*Actor.Agent, GetSlot(Projectile.Thrower).Mover->GetState().Foot);
			}
			const float Impulse = Velocity->Length() * ProjectileMassKg;
			const auto Inverse = TryInverseAffine(Model);
			if (Impulse < Actor.Settings.SeverImpulse || !Inverse)
			{
				break;
			}

			const auto Joint = Actor.Body->FindSeverableJoint(TransformPoint(*Inverse, Transform->Position), 0.45f);
			if (Joint)
			{
				auto Severed = CutCharacter(Index, [Joint = *Joint](FPlayerCharacter& Victim)
				{
					return Victim.Sever(Joint);
				}, Velocity->Normalized());
				if (!Severed)
				{
					return std::unexpected(FPhysicsError{Severed.error().Message});
				}

				if (*Severed)
				{
					Outcomes.push_back({.Kind = EGameOutcome::Sever, .Object = static_cast<std::uint32_t>(Index), .Joint = std::string(Actor.Body->GetJointName(*Joint))});
					Audio.Play(ELevelSound::Sever, Transform->Position);
					RequestEffect(ELevelEffect::Sever, Transform->Position, -Velocity->Normalized());
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
	// Every cut costs an agent a quarter of its health and tells it who did it.
	if (Actor.Npc)
	{
		Actor.Npc->Wound(0.25f);
		if (Ai && Actor.Agent)
		{
			Ai->ReportHit(*Actor.Agent, Mover->GetState().Foot);
		}
	}

	FPlayerCharacter& Victim = *Actor.Body;
	const FMatrix4 Model = Actor.Model;
	if (auto Added = AddPiece(Character, Victim.GetCookedModel(), Victim.GetLibrary(), Victim.GetModelTransforms(), std::move(**Severed), Model, Away * 1.5f + FVector3::Up() * 0.5f); !Added)
	{
		return std::unexpected(Added.error());
	}

	const auto Lost = [&Victim](const std::string_view Name)
	{
		const auto Joint = Victim.FindJoint(Name);
		return Joint && !Victim.Carries(*Joint);
	};

	// Without its head or its chest a character dies: what is left of it collapses as a ragdoll piece, and its capsule goes with it.
	if (Lost("Head") || Lost("spine_02"))
	{
		Victim.SetBool("Dead", true);
		if (Actor.Npc)
		{
			Actor.Npc->Wound(1.f);
			if (Ai && Actor.Agent)
			{
				Ai->RemoveAgent(*Actor.Agent);
			}

			Actor.Agent.reset();
		}

		FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
		if (auto Rest = Victim.DetachAll(); Rest && World)
		{
			if (auto Added = AddPiece(Character, Victim.GetCookedModel(), Victim.GetLibrary(), Victim.GetModelTransforms(), std::move(*Rest), Model, Away * 0.3f); !Added)
			{
				return std::unexpected(Added.error());
			}

			if (Actor.Ragdoll)
			{
				auto Destroyed = World->DestroyRagdoll(*Actor.Ragdoll);
				Actor.Ragdoll.reset();
				Actor.RagdollParts.clear();
				if (!Destroyed)
				{
					return std::unexpected(FGameError{Destroyed.error().Message});
				}
			}

			if (Actor.Collider != FPhysicsBodyId{})
			{
				if (auto Destroyed = World->DestroyBody(std::exchange(Actor.Collider, FPhysicsBodyId{})); !Destroyed)
				{
					return std::unexpected(FGameError{Destroyed.error().Message});
				}
			}
		}

		return true;
	}

	// Without a leg to stand on it falls and lives on the ground, where it can still be cut; a cut while down drops it again without what was
	// cut off. A thigh cut takes the calf with it, so the calves cover both.
	if (Actor.bDowned || Lost("calf_l") || Lost("calf_r"))
	{
		if (!Actor.bDowned)
		{
			Actor.bDowned = true;
			Events.push_back(std::format("{} went down", Entities[Actor.Entity].Name));
			if (Actor.Npc)
			{
				Actor.Npc->Down();
			}
		}

		if (auto Fallen = RagdollCharacter(Actor, Away * 0.5f); !Fallen)
		{
			return std::unexpected(Fallen.error());
		}
	}

	return true;
}

std::expected<void, FGameError> FGameSession::FImplementation::RagdollCharacter(FCharacterActor& Actor, const FVector3& Velocity)
{
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (!World)
	{
		return {};
	}

	if (Actor.Ragdoll)
	{
		auto Destroyed = World->DestroyRagdoll(*Actor.Ragdoll);
		Actor.Ragdoll.reset();
		if (!Destroyed)
		{
			return std::unexpected(FGameError{Destroyed.error().Message});
		}
	}

	if (Actor.Collider != FPhysicsBodyId{})
	{
		if (auto Destroyed = World->DestroyBody(std::exchange(Actor.Collider, FPhysicsBodyId{})); !Destroyed)
		{
			return std::unexpected(FGameError{Destroyed.error().Message});
		}
	}

	Actor.RagdollParts.clear();
	const std::vector<FPhysicsRagdollPart> Parts = Actor.Body->BuildRagdoll(Actor.Model);
	if (Parts.size() < 2)
	{
		Actor.Body->SetRagdollPose({}, FMatrix4{}, 0.f);
		return {};
	}

	auto Created = World->CreateRagdoll({.Parts = Parts, .LinearVelocity = Velocity});
	if (!Created)
	{
		return std::unexpected(FGameError{Created.error().Message});
	}

	Actor.Ragdoll = std::move(*Created);
	// Posed from the new parts at once, so the body never draws a frame in its animation between the old ragdoll and the next step.
	for (const FPhysicsBodyId Part : Actor.Ragdoll->Parts)
	{
		auto Transform = World->GetBodyTransform(Part);
		if (!Transform)
		{
			return std::unexpected(FGameError{Transform.error().Message});
		}

		Actor.RagdollParts.push_back(*Transform);
	}

	Actor.Body->SetRagdollPose(Actor.RagdollParts, TryInverseAffine(Actor.Model).value_or(FMatrix4{}), 1.f);
	return {};
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

	for (const FImplementation::FStreamedRegion& Region : Session.StreamedRegions)
	{
		if (!Region.bActive)
		{
			continue;
		}

		for (std::size_t Index = 0; Index < Region.Meshes.size(); ++Index)
		{
			if (Region.Meshes[Index])
			{
				Session.Models.push_back(Region.World[Index]);
				Session.Meshes.push_back(Region.Meshes[Index]);
				Session.MaterialSpans.emplace_back(Region.Materials[Index]);
			}
		}

		Session.Lights.insert(Session.Lights.end(), Region.Lights.begin(), Region.Lights.end());
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
	for (FImplementation::FCharacterActor& Actor : Session.Characters)
	{
		if (Actor.Body->Skin(false) && Actor.Body->GetMesh())
		{
			Session.VertexUpdates.push_back({.Mesh = Actor.Body->GetMesh(), .Vertices = Actor.Body->GetVertices()});
			const FMatrix4 ActorModel = Actor.Npc && !Actor.Npc->IsDead() && !Actor.bDowned ? Actor.Npc->GetModel(Alpha) : Actor.Model;
			Session.Models.push_back(ActorModel);
			Session.Meshes.push_back(Actor.Body->GetMesh());
			Session.MaterialSpans.emplace_back();
			if (const auto Grip = Actor.Npc && !Actor.Npc->IsDead() && Session.SwordMesh ? Actor.Body->GetGripFrame() : std::nullopt)
			{
				Session.Models.push_back(ActorModel * *Grip);
				Session.Meshes.push_back(Session.SwordMesh.get());
				Session.MaterialSpans.emplace_back();
			}
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
	// Everyone else first, so the view player's body keeps its place at the end of the list for motion vectors.
	for (std::size_t Other = 0; Other < Session.Players.size(); ++Other)
	{
		if (Other != Session.ViewPlayer)
		{
			Session.Activate(Other);
			Session.AddBodyToView(Alpha, false);
		}
	}

	Session.Activate(Session.ViewPlayer);
	const FVector3 Foot = Session.GetViewFoot(Alpha);
	const FVector3 BodyFoot = Session.AddBodyToView(Alpha, true);
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
	    .Particles = Session.Particles.GetRenderParticles(),
	};

	return Session.View;
}

FVector3 FGameSession::FImplementation::GetViewFoot(const float Alpha) const
{
	// A ragdoll's camera follows the pelvis as it would the capsule, from where the capsule's feet would be.
	const FMoverState& State = Mover->GetState();
	return Ragdoll ? RagdollParts.front().Position - FVector3::Up() * 0.6f : State.PreviousFoot * (1.f - Alpha) + State.Foot * Alpha;
}

FVector3 FGameSession::FImplementation::AddBodyToView(const float Alpha, const bool bViewer)
{
	const FVector3 Foot = GetViewFoot(Alpha);
	FVector3 BodyFoot = Foot;
	if (Body)
	{
		// Headless sessions still pose the body, because the first-person camera follows its head.
		const bool bFirstPerson = bViewer && !bThirdPerson && !Ragdoll && Body->GetCameraMesh();
		if (Ragdoll)
		{
			Body->SetRagdollPose(RagdollParts, TryInverseAffine(RagdollModel).value_or(FMatrix4{}), 1.f);
		}
		else
		{
			const FMatrix4 Standing = FMatrix4::Translation(Foot - FVector3::Up() * Body->GetRootHeight()) * FMatrix4::Rotation(GetBodyRotation());
			Body->SetRagdollPose(RagdollParts, TryInverseAffine(Standing).value_or(FMatrix4{}), GetUpBlend);
		}

		const auto Skinned = Body->Skin(bFirstPerson);
		if (Skinned)
		{
			BodyFoot = Foot - FVector3::Up() * Body->GetRootHeight();
		}

		if (Skinned && Body->GetMesh())
		{
			const FMatrix4 BodyModel = Ragdoll ? RagdollModel : FMatrix4::Translation(BodyFoot) * FMatrix4::Rotation(GetBodyRotation());
			VertexUpdates.push_back({.Mesh = Body->GetMesh(), .Vertices = Body->GetVertices()});
			Models.push_back(BodyModel);
			Meshes.push_back(Body->GetMesh());
			MaterialSpans.emplace_back();
			Visibility.push_back(bFirstPerson ? EMeshVisibility::HiddenFromCamera : EMeshVisibility::Everywhere);
			if (bFirstPerson)
			{
				VertexUpdates.push_back({.Mesh = Body->GetCameraMesh(), .Vertices = Body->GetCameraVertices()});
				Models.push_back(BodyModel);
				Meshes.push_back(Body->GetCameraMesh());
				MaterialSpans.emplace_back();
				Visibility.push_back(EMeshVisibility::CameraOnly);
			}

			if (const auto Grip = SwordMesh ? Body->GetGripFrame() : std::nullopt)
			{
				Models.push_back(BodyModel * *Grip);
				Meshes.push_back(SwordMesh.get());
				MaterialSpans.emplace_back();
				Visibility.push_back(EMeshVisibility::Everywhere);
			}
		}
	}

	return BodyFoot;
}

void FGameSession::FImplementation::RequestEffect(const ELevelEffect Cue, const FVector3& Position, const FVector3& Up, const float Scale)
{
	if (Effects[static_cast<std::size_t>(Cue)].IsValid())
	{
		EffectRequests.push_back({.Cue = Cue, .Position = Position, .Up = Up, .Scale = Scale});
	}
}

void FGameSession::FImplementation::StartEmitters()
{
	Particles.Clear();
	EffectRequests.clear();
	EmitterEffects.clear();
	for (std::size_t Index = 0; Index < Entities.size(); ++Index)
	{
		const auto& Emitter = Entities[Index].ParticleEmitter;
		if (!Emitter || !Emitter->bAutoPlay || !Emitter->System.IsValid())
		{
			continue;
		}

		const FMatrix4& Model = AuthoredWorld[Index];
		if (const auto Effect = Particles.Spawn(Emitter->System, GetTranslation(Model), Model.TransformVector(FVector3::Up()).Normalized(), Emitter->Scale))
		{
			EmitterEffects.emplace_back(Index, *Effect);
		}
	}
}

void FGameSession::FImplementation::UpdateEffects(const float DeltaSeconds)
{
	// Emitters whose systems arrived after the session started begin now.
	if (EmitterEffects.size() != static_cast<std::size_t>(std::ranges::count_if(Entities, [this](const FLevelEntity& Entity)
	    {
		    return Entity.ParticleEmitter && Entity.ParticleEmitter->bAutoPlay && Particles.HasSystem(Entity.ParticleEmitter->System);
	    })))
	{
		for (std::size_t Index = 0; Index < Entities.size(); ++Index)
		{
			const auto& Emitter = Entities[Index].ParticleEmitter;
			if (Emitter && Emitter->bAutoPlay && !std::ranges::contains(EmitterEffects, Index, &std::pair<std::size_t, FParticleEffectId>::first))
			{
				const FMatrix4& Model = AuthoredWorld[Index];
				if (const auto Effect = Particles.Spawn(Emitter->System, GetTranslation(Model), Model.TransformVector(FVector3::Up()).Normalized(), Emitter->Scale))
				{
					EmitterEffects.emplace_back(Index, *Effect);
				}
			}
		}
	}

	std::ranges::stable_sort(EffectRequests, [](const FEffectRequest& Left, const FEffectRequest& Right)
	{
		return std::tuple(Left.Cue, Left.Position.X, Left.Position.Y, Left.Position.Z) < std::tuple(Right.Cue, Right.Position.X, Right.Position.Y, Right.Position.Z);
	});

	for (const FEffectRequest& Request : EffectRequests)
	{
		(void)Particles.Spawn(Effects[static_cast<std::size_t>(Request.Cue)], Request.Position, Request.Up, Request.Scale);
	}

	EffectRequests.clear();
	if (!EmitterEffects.empty())
	{
		BuildModels();
		for (const auto& [Entity, Effect] : EmitterEffects)
		{
			Particles.Move(Effect, GetTranslation(Models[Entity]));
		}
	}

	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	Particles.Update(DeltaSeconds, [World](const FVector3& From, const FVector3& To) -> std::optional<FParticleHit>
	{
		if (!World)
		{
			return std::nullopt;
		}

		const auto Hit = World->CastSphereHit(World->ToWorld(From), World->ToWorld(To), 0.01f);
		if (!Hit)
		{
			return std::nullopt;
		}

		return FParticleHit{.Position = From + (To - From) * Hit->Fraction, .Normal = Hit->Normal};
	});
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
			RequestEffect(ELevelEffect::Land, Foot);
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

	// The level has one slide loop, which follows whoever the view follows.
	if (ActivePlayer == ViewPlayer)
	{
		Audio.SetLoop(ELevelSound::Slide, Mover->GetActiveMode() == "Slide" && !Ragdoll, Foot);
	}
}

void FGameSession::FImplementation::UpdateListener()
{
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
		Audio.SetListener({.Position = Mover->GetState().Foot + FVector3::Up() * EyeHeight, .Forward = Look.RotateVector(FVector3::Forward()), .Up = Look.RotateVector(FVector3::Up())});
	}
}

std::expected<void, FGameError> FGameSession::FImplementation::UpdateRegions()
{
	if (Streamer.GetRegions().empty())
	{
		return {};
	}

	std::vector<FVector3> Feet;
	ForEachPlayer([this, &Feet]
	{
		Feet.push_back(Mover->GetState().Foot);
	});

	Streamer.Update(
	    Feet,
	    [this](const std::size_t Region, FLoadedRegion&& Loaded)
	{
		StreamedRegions.push_back({.Region = Region, .Loaded = std::move(Loaded)});
	},
	    [this](const std::size_t Region)
	{
		UnloadRegion(Region);
	});

	for (std::size_t Index = 0; Index < StreamedRegions.size(); ++Index)
	{
		FStreamedRegion& Region = StreamedRegions[Index];
		if (Region.bActive)
		{
			continue;
		}

		if (Region.Loaded.Prepare)
		{
			auto Ready = Region.Loaded.Prepare();
			if (!Ready)
			{
				Warnings.push_back(std::format("Region '{}' cannot load its assets: {}", Streamer.GetRegions()[Region.Region].Name, Ready.error().Message));
				Region.Loaded.Prepare = {};
				continue;
			}

			if (!*Ready)
			{
				continue;
			}
		}

		if (auto Activated = ActivateRegion(Index); !Activated)
		{
			return Activated;
		}
	}

	return {};
}

std::expected<void, FGameError> FGameSession::FImplementation::ActivateRegion(const std::size_t Streamed)
{
	FStreamedRegion& Region = StreamedRegions[Streamed];
	const FLevelRegion& Placement = Streamer.GetRegions()[Region.Region];
	const std::vector<FLevelEntity>& RegionEntities = Region.Loaded.Document.Entities;

	auto Matrices = ComposeLevelRegionMatrices(Placement, RegionEntities);
	if (!Matrices)
	{
		return std::unexpected(FGameError{std::format("Region '{}' is invalid: {}", Placement.Name, Matrices.error().Message)});
	}

	const std::size_t Count = RegionEntities.size();
	Region.World = std::move(*Matrices);
	Region.Meshes.assign(Count, nullptr);
	Region.Materials.assign(Count, {});
	std::size_t Skipped = 0;
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const FLevelEntity& Entity = RegionEntities[Index];
		if (!IsLevelRegionScenery(Entity))
		{
			++Skipped;
			continue;
		}

		if (Entity.Mesh)
		{
			Region.Meshes[Index] = Assets.FindMesh(Entity.Mesh->Asset);
			for (const FAssetId& Material : Entity.Mesh->Materials)
			{
				Region.Materials[Index].push_back(Material.IsValid() ? Assets.FindMaterial(Material) : nullptr);
			}
		}

		if (Entity.Light && Entity.Light->bEnabled)
		{
			Region.Lights.push_back({.Settings = *Entity.Light, .Transform = Region.World[Index], .Id = GetLevelRegionInstanceId(Placement, Entity.Id)});
		}
	}

	if (Skipped > 0)
	{
		Warnings.push_back(std::format("Region '{}' streams only static meshes, local lights, and static bodies; {} entities left out", Placement.Name, Skipped));
	}

	const FLevelSimulationSetup RegionSetup = BuildLevelSimulation(RegionEntities, Region.World, Region.Meshes);
	Region.Bodies.clear();
	for (const FLevelSimulationBody& RegionBody : RegionSetup.Bodies)
	{
		if (RegionBody.ObjectIndex < Count && IsLevelRegionScenery(RegionEntities[RegionBody.ObjectIndex]) && RegionBody.MotionType == EPhysicsMotionType::Static && !RegionBody.Mover && !RegionBody.bTrigger)
		{
			Region.Bodies.push_back(RegionBody);
		}
	}

	if (!Region.Bodies.empty() && Simulation.IsRunning())
	{
		auto Group = Simulation.AddStaticBodies(Region.Bodies);
		if (!Group)
		{
			return std::unexpected(FGameError{std::format("Region '{}' cannot add its bodies: {}", Placement.Name, Group.error().Message)});
		}

		Region.Group = *Group;
	}

	Region.bActive = true;
	Navigation.ActivateRegion(Placement.Id);
	Events.push_back(std::format("Region '{}' streamed in", Placement.Name));
	return {};
}

void FGameSession::FImplementation::UnloadRegion(const std::size_t Region)
{
	const auto Found = std::ranges::find(StreamedRegions, Region, &FStreamedRegion::Region);
	if (Found == StreamedRegions.end())
	{
		return;
	}

	if (Found->Group)
	{
		(void)Simulation.RemoveStaticBodies(*Found->Group);
	}

	if (Found->Loaded.Release)
	{
		Found->Loaded.Release();
	}

	Navigation.UnloadRegion(Streamer.GetRegions()[Region].Id);
	Events.push_back(std::format("Region '{}' streamed out", Streamer.GetRegions()[Region].Name));
	StreamedRegions.erase(Found);
}

void FGameSession::SetRegions(const std::span<const FLevelRegion> Regions, FRegionLoader Loader)
{
	FImplementation& Session = *Implementation;
	for (FImplementation::FStreamedRegion& Region : Session.StreamedRegions)
	{
		if (Region.Group)
		{
			(void)Session.Simulation.RemoveStaticBodies(*Region.Group);
		}

		if (Region.Loaded.Release)
		{
			Region.Loaded.Release();
		}

		Session.Navigation.UnloadRegion(Session.Streamer.GetRegions()[Region.Region].Id);
	}

	Session.StreamedRegions.clear();
	Session.Streamer.Configure({Regions.begin(), Regions.end()}, std::move(Loader), Session.Tasks);
}

std::expected<void, FGameError> FGameSession::SetNavigation(std::shared_ptr<const FNavigationFile> Navigation)
{
	FImplementation& Session = *Implementation;
	Session.Navigation.SetData(std::move(Navigation));
	if (auto Started = Session.Navigation.Start(Session.Entities, Session.AuthoredWorld, Session.Setup); !Started)
	{
		return Started;
	}

	Session.StartAgents();
	return {};
}

void FGameSession::FImplementation::StartAgents()
{
	// Agents that joined this same navigation leave it before joining again; a navigation that restarted took them with it, so their old
	// agents are forgotten before stopping their tasks could reach for them.
	FNavigationWorld* const World = Navigation.GetWorld();
	const bool bSameNavigation = World && AgentsGeneration == Navigation.GetGeneration();
	if (!bSameNavigation)
	{
		ForEachPlayer([this]
		{
			PlayerAgent.reset();
		});

		for (FCharacterActor& Actor : Characters)
		{
			if (Actor.Npc)
			{
				Actor.Npc->Abandon();
			}
		}
	}

	if (Ai)
	{
		Ai->Stop();
	}

	Ai = std::make_unique<FAiWorld>();
	if (bSameNavigation)
	{
		ForEachPlayer([this, World]
		{
			if (PlayerAgent)
			{
				World->RemoveAgent(*PlayerAgent);
			}
		});

		for (FCharacterActor& Actor : Characters)
		{
			if (Actor.Npc)
			{
				Actor.Npc->Leave();
			}
		}
	}

	AgentsGeneration = Navigation.GetGeneration();
	ForEachPlayer([this, World]
	{
		AddPlayerAgent(World);
	});

	for (FCharacterActor& Actor : Characters)
	{
		Actor.Agent.reset();
		if (!Actor.Npc)
		{
			continue;
		}

		Actor.Npc->Start(World, AuthoredWorld[Actor.Entity]);
		Actor.Model = AuthoredWorld[Actor.Entity];
		if (Actor.Body->IsEmpty())
		{
			Actor.Npc->Wound(1.f);
			continue;
		}

		const FAiAgentComponent& Settings = Actor.Npc->GetSettings();
		const auto Tree = StateTrees.find(Settings.StateTree);
		if (Tree == StateTrees.end())
		{
			continue;
		}

		const FLevelEntity& Entity = Entities[Actor.Entity];
		Actor.Agent = Ai->AddAgent({
		    .Name = Entity.Name,
		    .Body = Actor.Npc.get(),
		    .Tree = Tree->second,
		    .Perception = {.SightRange = Settings.SightRange, .SightHalfAngle = Settings.SightHalfAngle, .HearingRange = Settings.HearingRange},
		    .WalkSpeed = Settings.WalkSpeed,
		    .RunSpeed = Settings.RunSpeed,
		    .Home = Actor.Npc->GetFootPosition(),
		    .Seed = Entity.Id.GetHigh() ^ Entity.Id.GetLow(),
		});
	}
}

void FGameSession::FImplementation::AddPlayerAgent(FNavigationWorld* const World)
{
	PlayerAgent.reset();
	if (World && Mover)
	{
		if (auto Added = World->AddAgent(Mover->GetState().Foot, {.Radius = 0.3f, .Height = 1.8f, .MaximumSpeed = 10.f, .MaximumAcceleration = 40.f, .bExternal = true}))
		{
			PlayerAgent = *Added;
		}
	}
}

void FGameSession::FImplementation::StrikePlayer(const FCharacterActor& Actor)
{
	constexpr float Reach = 1.9f;
	const FVector3 Target = Mover->GetState().Foot;
	const FVector3 Foot = Actor.Npc->GetFootPosition();
	const FVector3 Offset{Target.X - Foot.X, 0.f, Target.Z - Foot.Z};
	const float Distance = Offset.Length();
	const FVector3 Facing{std::sin(Actor.Npc->GetYaw()), 0.f, std::cos(Actor.Npc->GetYaw())};
	if (Ragdoll || Distance > Reach || std::abs(Target.Y - Foot.Y) > 1.5f || (Distance > 0.3f && Offset.Dot(Facing) < 0.3f * Distance))
	{
		return;
	}

	PendingKnockdown = Facing * 90.f + FVector3::Up() * 30.f;
	Events.push_back(std::format("{} struck the player", Entities[Actor.Entity].Name));
	Audio.Play(ELevelSound::Impact, Target + FVector3::Up(), {{.Name = "Impulse", .Value = 40.f}});
}

void FGameSession::FImplementation::StepAgents(FPhysicsWorld& World, const float StepSeconds)
{
	// A client's characters stand where the server's agents put them.
	if (!bAuthority)
	{
		for (FCharacterActor& Actor : Characters)
		{
			if (Actor.Npc && !Actor.Npc->IsDead() && !Actor.bDowned)
			{
				Actor.Model = Actor.Npc->GetModel(1.f);
				if (Actor.Collider != FPhysicsBodyId{})
				{
					(void)World.MoveKinematicBody(Actor.Collider, {.Position = Actor.Npc->GetFootPosition() + FVector3::Up() * 0.9f, .Rotation = FQuaternion::Identity()}, StepSeconds);
				}
			}
		}

		return;
	}

	FNavigationWorld* const Crowd = Navigation.GetWorld();
	ForEachPlayer([this, Crowd]
	{
		if (Crowd && PlayerAgent)
		{
			Crowd->SetAgentPosition(*PlayerAgent, Mover->GetState().Foot, Mover->GetState().Velocity);
		}
	});

	Navigation.Step(Simulation, StepSeconds);
	for (FCharacterActor& Actor : Characters)
	{
		if (!Actor.Npc)
		{
			continue;
		}

		Actor.Npc->Step(StepSeconds, Actor.Body->GetEvents());
		if (Actor.Npc->IsDead() || Actor.bDowned)
		{
			continue;
		}

		Actor.Model = Actor.Npc->GetModel(1.f);
		if (Actor.Collider != FPhysicsBodyId{})
		{
			(void)World.MoveKinematicBody(Actor.Collider, {.Position = Actor.Npc->GetFootPosition() + FVector3::Up() * 0.9f, .Rotation = FQuaternion::Identity()}, StepSeconds);
		}

		if (Actor.Npc->IsSwingStarting())
		{
			ForEachPlayer([this, &Actor]
			{
				StrikePlayer(Actor);
			});
		}
	}

	if (!Ai)
	{
		return;
	}

	std::vector<FAiTarget> Targets;
	ForEachPlayer([this, StepSeconds, &Targets]
	{
		// Sprinting footfalls carry; walking is quiet.
		const FMoverState& PlayerState = Mover->GetState();
		const float Speed = std::hypot(PlayerState.Velocity.X, PlayerState.Velocity.Z);
		SprintNoiseSeconds = PlayerState.bGrounded && Speed > 5.5f ? SprintNoiseSeconds + StepSeconds : 0.f;
		if (SprintNoiseSeconds >= 0.4f)
		{
			SprintNoiseSeconds = 0.f;
			Ai->PostNoise(PlayerState.Foot, 9.f);
		}

		const FVector3 Foot = Ragdoll ? RagdollParts.front().Position - FVector3::Up() * 0.6f : PlayerState.Foot;
		Targets.push_back(FAiTarget{.Foot = Foot, .Head = Foot + FVector3::Up() * 1.6f});
	});

	Ai->SetTargets(Targets);
	const FSessionAiEnvironment Environment(&World, Crowd);
	Ai->Step(StepSeconds, Environment);
}

std::vector<FAssetId> FGameSession::GetStateTrees() const
{
	std::vector<FAssetId> Trees;
	for (const FImplementation::FCharacterActor& Actor : Implementation->Characters)
	{
		const FAssetId Tree = Actor.Npc ? Actor.Npc->GetSettings().StateTree : FAssetId{};
		if (Tree.IsValid() && std::ranges::none_of(Trees, [&Tree](const FAssetId& Known)
		{
			return Known == Tree;
		}))
		{
			Trees.push_back(Tree);
		}
	}

	return Trees;
}

std::expected<void, FGameError> FGameSession::AddStateTree(const FAssetId& Id, std::shared_ptr<const FCookedStateTree> Tree)
{
	if (!Tree)
	{
		return std::unexpected(FGameError{"A StateTree is needed"});
	}

	Implementation->StateTrees[Id] = std::move(Tree);
	Implementation->StartAgents();
	return {};
}

std::vector<FGameAgentState> FGameSession::GetAgentStates() const
{
	const FImplementation& Session = *Implementation;
	std::vector<FGameAgentState> States;
	for (const FImplementation::FCharacterActor& Actor : Session.Characters)
	{
		if (!Actor.Npc)
		{
			continue;
		}

		FGameAgentState& State = States.emplace_back(FGameAgentState{
		    .Name = Session.Entities[Actor.Entity].Name,
		    .Foot = Actor.Npc->GetFootPosition(),
		    .Yaw = Actor.Npc->GetYaw(),
		    .Health = Actor.Npc->GetHealth(),
		    .bOnNavigation = Actor.Npc->IsOnNavigation(),
		    .bAttacking = Actor.Npc->IsAttacking(),
		});

		if (Session.Ai && Actor.Agent)
		{
			State.State = std::string(Session.Ai->GetStateName(*Actor.Agent));
			if (const FAiMemory* const Memory = Session.Ai->GetMemory(*Actor.Agent))
			{
				State.bSeesPlayer = Memory->bSeesTarget;
				State.bRemembersPlayer = Memory->LastSeen.has_value();
			}
		}
	}

	return States;
}

std::optional<FGameAgentDebug> FGameSession::GetAgentDebug(const std::size_t Agent) const
{
	const FImplementation& Session = *Implementation;
	std::size_t Index = 0;
	for (const FImplementation::FCharacterActor& Actor : Session.Characters)
	{
		if (!Actor.Npc)
		{
			continue;
		}

		if (Index++ == Agent)
		{
			if (!Session.Ai || !Actor.Agent)
			{
				return std::nullopt;
			}

			return FGameAgentDebug{.Tree = Actor.Npc->GetSettings().StateTree, .Program = Session.Ai->GetTree(*Actor.Agent), .Active = Session.Ai->GetActiveStates(*Actor.Agent)};
		}
	}

	return std::nullopt;
}

const FNavigationWorld* FGameSession::GetNavigation() const noexcept
{
	return Implementation->Navigation.GetWorld();
}

FRegionStats FGameSession::GetRegionStats() const
{
	FRegionStats Stats = Implementation->Streamer.GetStats();
	Stats.Preparing = static_cast<std::uint32_t>(std::ranges::count(Implementation->StreamedRegions, false, &FImplementation::FStreamedRegion::bActive));
	return Stats;
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

bool FGameSession::IsCharacterDowned(const std::size_t Character) const noexcept
{
	return Character < Implementation->Characters.size() && Implementation->Characters[Character].bDowned;
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
	const FMatrix4& Model = Session.Characters[Character].Model;
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

FGameSave FGameSession::CaptureSave(const FObjectId& Level) const
{
	const FImplementation& Session = *Implementation;
	FGameSave Save{
	    .Level = Level,
	    .ObjectiveState = static_cast<std::uint32_t>(Session.Objective.State),
	    .ElapsedSeconds = Session.Objective.ElapsedSeconds,
	    .BestSeconds = Session.Objective.BestSeconds,
	    .Respawns = Session.Objective.Respawns,
	    .PlayerFoot = Session.Mover->GetState().Foot,
	    .PlayerYaw = Session.Yaw,
	};

	for (std::size_t Index = 0; Index < Session.Entities.size(); ++Index)
	{
		if (Session.Entities[Index].Destructible && !Session.Simulation.IsIntact(Index))
		{
			Save.Destructibles.push_back({.Object = Session.Entities[Index].Id, .IntactBonds = Session.Simulation.GetIntactBonds(Index)});
		}
	}

	for (const FImplementation::FCharacterActor& Actor : Session.Characters)
	{
		if (Actor.Body->IsEmpty())
		{
			Save.DeadCharacters.push_back(Session.Entities[Actor.Entity].Id);
		}
	}

	return Save;
}

std::expected<std::vector<std::string>, FGameError> FGameSession::ApplySave(const FGameSave& Save)
{
	FImplementation& Session = *Implementation;
	if (Save.ObjectiveState > static_cast<std::uint32_t>(EGameObjectiveState::Finished))
	{
		return std::unexpected(FGameError{"The save's objective state is unknown"});
	}

	if (auto Restarted = Session.Start(); !Restarted)
	{
		return std::unexpected(Restarted.error());
	}

	std::vector<std::string> Skipped;
	const auto FindEntity = [&Session](const FObjectId& Object) -> std::optional<std::size_t>
	{
		const auto Found = std::ranges::find(Session.Entities, Object, &FLevelEntity::Id);
		return Found == Session.Entities.end() ? std::nullopt : std::optional(static_cast<std::size_t>(Found - Session.Entities.begin()));
	};

	for (const FGameSaveDestructible& Destructible : Save.Destructibles)
	{
		const auto Index = FindEntity(Destructible.Object);
		if (!Index)
		{
			Skipped.push_back(std::format("Destructible {} is no longer in the level", Destructible.Object.ToString()));
			continue;
		}

		if (auto Restored = Session.Simulation.RestoreBonds(*Index, Destructible.IntactBonds); !Restored)
		{
			Skipped.push_back(std::format("{}: {}", Session.Entities[*Index].Name, Restored.error().Message));
		}
	}

	for (const FObjectId& Dead : Save.DeadCharacters)
	{
		const auto Found = std::ranges::find_if(Session.Characters, [&](const FImplementation::FCharacterActor& Actor)
		{
			return Session.Entities[Actor.Entity].Id == Dead;
		});

		if (Found == Session.Characters.end())
		{
			Skipped.push_back(std::format("Character {} is no longer in the level", Dead.ToString()));
			continue;
		}

		// Losing its head kills a character, which then collapses as it did before the save.
		auto Severed = Session.CutCharacter(static_cast<std::size_t>(Found - Session.Characters.begin()), [](FPlayerCharacter& Body) -> std::expected<std::optional<FSkinnedMeshPiece>, FGameError>
		{
			if (const auto Neck = Body.FindJoint("neck_01"))
			{
				return Body.Sever(*Neck);
			}

			return std::optional<FSkinnedMeshPiece>{};
		}, FVector3::Up());

		if (!Severed || !*Severed)
		{
			Skipped.push_back(std::format("{} could not be killed again", Session.Entities[Found->Entity].Name));
		}
	}

	if (auto Placed = TeleportPlayer(Save.PlayerFoot, Save.PlayerYaw); !Placed)
	{
		Skipped.push_back(Placed.error().Message);
	}

	// A run saved before it started, with no zones to time it, stays exploring.
	Session.Objective.State = Session.Objective.State == EGameObjectiveState::Exploring ? EGameObjectiveState::Exploring : static_cast<EGameObjectiveState>(Save.ObjectiveState);
	Session.Objective.ElapsedSeconds = Save.ElapsedSeconds;
	Session.Objective.BestSeconds = Save.BestSeconds;
	Session.Objective.Respawns = Save.Respawns;
	Session.Events.push_back("Save loaded");
	return Skipped;
}

void FGameSession::SetLevelEffects(const FLevelEffects& Effects)
{
	Implementation->Effects = Effects;
}

std::vector<FAssetId> FGameSession::GetParticleSystems() const
{
	std::vector<FAssetId> Systems;
	for (const FAssetId& Effect : Implementation->Effects)
	{
		if (Effect.IsValid())
		{
			Systems.push_back(Effect);
		}
	}

	for (const FLevelEntity& Entity : Implementation->Entities)
	{
		if (Entity.ParticleEmitter && Entity.ParticleEmitter->System.IsValid())
		{
			Systems.push_back(Entity.ParticleEmitter->System);
		}
	}

	std::ranges::sort(Systems);
	const auto Duplicates = std::ranges::unique(Systems);
	Systems.erase(Duplicates.begin(), Duplicates.end());
	return Systems;
}

std::expected<void, FGameError> FGameSession::AddParticleSystem(const FAssetId& Id, std::shared_ptr<const FCookedParticleSystem> System)
{
	if (auto Added = Implementation->Particles.AddSystem(Id, std::move(System)); !Added)
	{
		return std::unexpected(FGameError{Added.error().Message});
	}

	return {};
}

FParticleStats FGameSession::GetParticleStats() const
{
	return Implementation->Particles.GetStats();
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
