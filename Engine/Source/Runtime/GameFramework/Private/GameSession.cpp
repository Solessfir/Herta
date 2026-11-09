#include "Herta/GameFramework/GameSession.h"

#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/Math/AffineTransform.h"
#include "PlayerCharacter.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <utility>

namespace Herta
{
namespace
{
constexpr float Gravity = 9.80665f;
// Jumping stays possible this long after walking off a ledge, and a press this long before landing still counts.
constexpr float CoyoteSeconds = 0.1f;
constexpr float JumpBufferSeconds = 0.12f;
constexpr float MaximumPitch = 1.53f;
constexpr float CameraNear = 0.05f;
// A thrown ball: a 3 kg, 24 cm ball at 24 m/s carries about 86 N*s into a wall, enough to break the playground's walls but not its footings.
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
	void UpdateBody(float DeltaSeconds);
	[[nodiscard]] FVector3 GetCameraPosition(const FVector3& Foot);

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
	};

	std::vector<FProjectile> Projectiles{};
	FPhysicsCharacterId Character{};
	FVector3 PreviousFoot{};
	FVector3 CurrentFoot{};
	// Velocity relative to the ground the player last stood on.
	FVector3 HorizontalVelocity{};
	float VerticalVelocity = 0.f;
	bool bGrounded = false;
	float AirborneSeconds = 0.f;
	float JumpBufferRemaining = 0.f;
	bool bCrouched = false;
	bool bSliding = false;
	float SlideBoostCooldown = 0.f;
	float WallJumpCooldown = 0.f;
	// Eases between standing and crouched so the view does not snap.
	float EyeHeight = 0.f;
	FGameInput Input{};
	float Yaw = 0.f;
	float Pitch = 0.f;
	float LastDeltaSeconds = 0.f;

	FGameObjective Objective{};
	std::vector<std::string> Events{};

	// The animated body, its facing, and the third-person camera.
	std::unique_ptr<FPlayerCharacter> Body{};
	bool bThirdPerson = false;
	bool bJumpedThisUpdate = false;
	float BodyYaw = 0.f;
	float BoomLength = 0.f;
	// The first-person eye eases after the head, so the camera follows the body's lean without every bob of the walk.
	std::optional<FVector3> SmoothedEye{};

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

	const auto Created = Simulation.GetPhysicsWorld()->CreateCharacter({.FootPosition = SpawnPosition, .Height = Player.Height, .Radius = Player.Radius, .StepHeight = Player.StepHeight});
	if (!Created)
	{
		Simulation.Stop();
		return std::unexpected(FGameError{std::format("Cannot create the player: {}", Created.error().Message)});
	}

	Character = *Created;
	PreviousFoot = CurrentFoot = SpawnPosition;
	HorizontalVelocity = {};
	VerticalVelocity = 0.f;
	bGrounded = false;
	AirborneSeconds = 0.f;
	JumpBufferRemaining = 0.f;
	bCrouched = false;
	bSliding = false;
	SlideBoostCooldown = 0.f;
	WallJumpCooldown = 0.f;
	EyeHeight = Player.EyeHeight;
	Yaw = SpawnYaw;
	Pitch = 0.f;
	BodyYaw = SpawnYaw;
	BoomLength = Player.ThirdPersonDistance;
	SmoothedEye.reset();
	bJumpedThisUpdate = false;
	if (Body)
	{
		Body->Restart();
	}

	Objective.State = StartZone && FinishZone ? EGameObjectiveState::Ready : EGameObjectiveState::Exploring;
	Objective.ElapsedSeconds = 0.;
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

std::expected<void, FPhysicsError> FGameSession::FImplementation::StepPlayer(FPhysicsWorld& World, const float StepSeconds)
{
	const auto Ground = World.UpdateCharacterGround(Character);
	if (!Ground)
	{
		return std::unexpected(Ground.error());
	}

	SlideBoostCooldown = std::max(0.f, SlideBoostCooldown - StepSeconds);
	WallJumpCooldown = std::max(0.f, WallJumpCooldown - StepSeconds);

	// Crouching and sliding share the short capsule; standing back up waits until there is room above.
	if (Input.bCrouch != bCrouched)
	{
		const auto Resized = World.SetCharacterHeight(Character, Input.bCrouch ? Player.CrouchHeight : Player.Height);
		if (!Resized)
		{
			return std::unexpected(Resized.error());
		}

		bCrouched = *Resized ? Input.bCrouch : bCrouched;
	}

	const float Speed = HorizontalVelocity.Length();
	if (bCrouched && Ground->bGrounded && !bSliding && Speed >= Player.SlideMinimumSpeed)
	{
		bSliding = true;
		if (SlideBoostCooldown <= 0.f && Speed > 0.f)
		{
			HorizontalVelocity *= std::max(Speed, std::min(Speed + Player.SlideBoost, Player.SlideBoostLimit)) / Speed;
			SlideBoostCooldown = Player.SlideBoostCooldownSeconds;
		}
	}
	else if (bSliding && (!bCrouched || (Ground->bGrounded && Speed <= Player.CrouchSpeed)))
	{
		bSliding = false;
	}

	const FQuaternion Facing = FQuaternion::FromAxisAngle(FVector3::Up(), Yaw);
	const FVector3 Forward = Facing.RotateVector(FVector3::Forward());
	const FVector3 Right = Forward.Cross(FVector3::Up());
	FVector2 Move = Input.Move;
	if (const float Length = std::hypot(Move.X, Move.Y); Length > 1.f)
	{
		Move = {Move.X / Length, Move.Y / Length};
	}

	const FVector3 Wish = Right * Move.X + Forward * Move.Y;
	const float WishSpeed = bCrouched ? Player.CrouchSpeed : Input.bSprint ? Player.SprintSpeed
	                                                                       : Player.WalkSpeed;
	if (Ground->bGrounded && bSliding)
	{
		// Slopes pull a slide downhill and friction is low, so input only steers it sideways.
		const FVector3 Downhill{Ground->GroundNormal.X, 0.f, Ground->GroundNormal.Z};
		HorizontalVelocity += Downhill * (Player.SlideSlopeAcceleration * StepSeconds);
		HorizontalVelocity *= std::max(0.f, 1.f - Player.SlideFriction * StepSeconds);
		const FVector3 Side = HorizontalVelocity.Normalized().Cross(FVector3::Up());
		HorizontalVelocity += Side * (Wish.Dot(Side) * Player.SlideSteerAcceleration * StepSeconds);
	}
	else if (Ground->bGrounded)
	{
		const FVector3 Change = Wish * WishSpeed - HorizontalVelocity;
		const float ChangeLength = Change.Length();
		const float Acceleration = Player.GroundAcceleration * StepSeconds;
		HorizontalVelocity += ChangeLength > Acceleration ? Change * (Acceleration / ChangeLength) : Change;
	}
	else if (const float WishLength = Wish.Length(); WishLength > 0.f)
	{
		// Air control only adds speed up to the wish speed along the wish direction, so slide and wall jump momentum carries through the air.
		const FVector3 Direction = Wish / WishLength;
		const float Missing = WishSpeed * WishLength - HorizontalVelocity.Dot(Direction);
		HorizontalVelocity += Direction * std::clamp(Missing, 0.f, Player.AirAcceleration * StepSeconds);
	}

	// Leaving the ground keeps the platform's motion, so jumping off a moving shuttle carries its speed.
	if (bGrounded && !Ground->bGrounded)
	{
		HorizontalVelocity += FVector3{Ground->GroundVelocity.X, 0.f, Ground->GroundVelocity.Z};
		VerticalVelocity += Ground->GroundVelocity.Y;
	}

	AirborneSeconds = Ground->bGrounded ? 0.f : AirborneSeconds + StepSeconds;
	JumpBufferRemaining = std::max(0.f, JumpBufferRemaining - StepSeconds);
	if (Ground->bGrounded)
	{
		VerticalVelocity = 0.f;
	}

	FVector3 Carried = Ground->bGrounded ? Ground->GroundVelocity : FVector3{};
	const bool bCanJump = AirborneSeconds <= CoyoteSeconds && VerticalVelocity <= 0.f;
	if (JumpBufferRemaining > 0.f && bCanJump)
	{
		VerticalVelocity = Player.JumpSpeed;
		JumpBufferRemaining = 0.f;
		bJumpedThisUpdate = true;
		AirborneSeconds = CoyoteSeconds + StepSeconds;
		HorizontalVelocity += FVector3{Carried.X, 0.f, Carried.Z};
		VerticalVelocity += Carried.Y;
		Carried = {};
	}
	else if (JumpBufferRemaining > 0.f && !Ground->bGrounded && Ground->bTouchingWall && WallJumpCooldown <= 0.f)
	{
		// A wall jump rises like a jump and kicks away from the wall, keeping speed along it.
		const float Into = HorizontalVelocity.Dot(Ground->WallNormal);
		HorizontalVelocity += Ground->WallNormal * (Player.WallJumpSpeed - std::min(Into, 0.f));
		VerticalVelocity = Player.JumpSpeed;
		JumpBufferRemaining = 0.f;
		bJumpedThisUpdate = true;
		WallJumpCooldown = Player.WallJumpCooldownSeconds;
	}
	else if (!Ground->bGrounded)
	{
		VerticalVelocity -= Gravity * StepSeconds;
	}

	const auto Moved = World.MoveCharacter(Character, HorizontalVelocity + FVector3::Up() * VerticalVelocity + Carried, StepSeconds);
	if (!Moved)
	{
		return std::unexpected(Moved.error());
	}

	// Walls and ceilings absorb the velocity they block instead of letting it build up.
	const FVector3 Resolved = Moved->Velocity - Carried;
	HorizontalVelocity = {Resolved.X, 0.f, Resolved.Z};
	if (!Moved->bGrounded && VerticalVelocity > 0.f)
	{
		VerticalVelocity = std::min(VerticalVelocity, Resolved.Y);
	}

	constexpr float EyeSpeed = 6.f;
	const float TargetEye = bCrouched ? Player.CrouchEyeHeight : Player.EyeHeight;
	EyeHeight += std::clamp(TargetEye - EyeHeight, -EyeSpeed * StepSeconds, EyeSpeed * StepSeconds);
	bGrounded = Moved->bGrounded;
	PreviousFoot = CurrentFoot;
	CurrentFoot = Moved->FootPosition;
	if (CurrentFoot.Y < Player.RespawnHeight)
	{
		++Objective.Respawns;
		Events.push_back("The player fell and returned to the start");
		if (Objective.State != EGameObjectiveState::Exploring)
		{
			Objective.State = EGameObjectiveState::Ready;
			Objective.ElapsedSeconds = 0.;
		}

		HorizontalVelocity = {};
		VerticalVelocity = 0.f;
		bSliding = false;
		PreviousFoot = CurrentFoot = SpawnPosition;
		Yaw = SpawnYaw;
		return World.TeleportCharacter(Character, SpawnPosition);
	}

	return {};
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
	const FVector3 Center = CurrentFoot + FVector3::Up() * (Player.Height * 0.5f);
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
	if (Input.bJump)
	{
		Session.JumpBufferRemaining = JumpBufferSeconds;
	}

	if (Input.bToggleCamera)
	{
		Session.bThirdPerson = !Session.bThirdPerson;
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
		return Session.StepPlayer(World, StepSeconds);
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
	}

	for (const FLevelTriggerEvent& Event : Session.Simulation.TakeTriggerEvents())
	{
		Session.Events.push_back(std::format("{} {} {}", Session.Entities[Event.ObjectIndex].Name, Event.bEntered ? "entered" : "left", Session.Entities[Event.TriggerObjectIndex].Name));
	}

	Session.UpdateObjective(DeltaSeconds);
	Session.UpdateBody(DeltaSeconds);
	return {};
}

void FGameSession::FImplementation::UpdateBody(const float DeltaSeconds)
{
	if (!Body)
	{
		return;
	}

	// In first person the body follows the view; in third person it turns to face where it moves and holds that facing when it stops.
	const float Speed = std::hypot(HorizontalVelocity.X, HorizontalVelocity.Z);
	const float TargetYaw = !bThirdPerson ? Yaw : Speed > 0.5f ? std::atan2(HorizontalVelocity.X, HorizontalVelocity.Z)
	                                                           : BodyYaw;
	const float Turn = std::remainder(TargetYaw - BodyYaw, 2.f * std::numbers::pi_v<float>);
	const float MaximumTurn = (bThirdPerson ? 10.f : 30.f) * DeltaSeconds;
	BodyYaw = std::remainder(BodyYaw + std::clamp(Turn, -MaximumTurn, MaximumTurn), 2.f * std::numbers::pi_v<float>);

	Body->Update(DeltaSeconds, {.GroundSpeed = Speed, .bGrounded = bGrounded, .bCrouched = bCrouched, .bSliding = bSliding, .bJumped = bJumpedThisUpdate});
	bJumpedThisUpdate = false;
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
	const FVector3 Eye = CurrentFoot + FVector3::Up() * EyeHeight;
	const auto Ball = Simulation.Spawn({
	    .Shape = EPhysicsShape::Sphere,
	    .Radius = ProjectileRadius,
	    .Position = Eye + Direction * (Player.Radius + ProjectileRadius + 0.1f),
	    .MotionType = EPhysicsMotionType::Dynamic,
	    .Properties = {.MassKg = ProjectileMassKg, .Friction = 0.5f, .Restitution = 0.35f, .LinearDamping = 0.01f, .AngularDamping = 0.2f},
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

	if (auto Launched = Simulation.GetPhysicsWorld()->SetVelocity(*BallBody, Direction * ProjectileSpeed + HorizontalVelocity); !Launched)
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

		// A broken destructible draws its pieces instead.
		if (!Session.Simulation.IsIntact(Index))
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
	const FVector3 Foot = Session.PreviousFoot * (1.f - Alpha) + Session.CurrentFoot * Alpha;
	Session.Visibility.assign(Session.Models.size(), EMeshVisibility::Everywhere);
	if (Session.Body && Session.Body->GetMesh())
	{
		const bool bFirstPerson = !Session.bThirdPerson && Session.Body->GetCameraMesh();
		if (const auto Skinned = Session.Body->Skin(bFirstPerson); Skinned)
		{
			const FMatrix4 BodyModel = FMatrix4::Translation(Foot) * FMatrix4::Rotation(FQuaternion::FromAxisAngle(FVector3::Up(), Session.BodyYaw));
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

	const FVector3 Eye = Session.GetCameraPosition(Foot);
	const float Aspect = Extent.Height > 0 ? static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height) : 1.f;
	Session.View = {
	    .View = FMatrix4::Rotation(GetLookOrientation(Session.Yaw, Session.Pitch).Conjugated()) * FMatrix4::Translation(-Eye),
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

FVector3 FGameSession::FImplementation::GetCameraPosition(const FVector3& Foot)
{
	FVector3 Eye = Foot + FVector3::Up() * EyeHeight;
	const auto Head = Body ? Body->GetHeadPosition() : std::nullopt;
	if (!bThirdPerson && Head)
	{
		// Eyes sit in front of and above the head joint, in the body's facing, so the camera stays clear of the neck when the body leans.
		constexpr float EyeAbove = 0.07f;
		constexpr float EyeAhead = 0.24f;
		const FQuaternion Facing = FQuaternion::FromAxisAngle(FVector3::Up(), BodyYaw);
		const FVector3 Target = Foot + Facing.RotateVector(*Head) + FVector3::Up() * EyeAbove + Facing.RotateVector(FVector3::Forward()) * EyeAhead;
		const float Follow = 1.f - std::exp(-LastDeltaSeconds / 0.05f);
		SmoothedEye = SmoothedEye ? *SmoothedEye + (Target - *SmoothedEye) * Follow : Target;
		return *SmoothedEye;
	}

	SmoothedEye.reset();
	FPhysicsWorld* const World = Simulation.GetPhysicsWorld();
	if (!bThirdPerson || !World)
	{
		return Eye;
	}

	// The boom snaps in when something comes between the camera and the shoulders, and eases back out so it does not pop.
	constexpr float CameraRadius = 0.2f;
	const FQuaternion Look = GetLookOrientation(Yaw, Pitch);
	const FVector3 Pivot = Foot + FVector3::Up() * (EyeHeight * 0.92f) + Look.RotateVector(-FVector3::Left()) * Player.ThirdPersonShoulderOffset;
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
	return Implementation->CurrentFoot;
}

bool FGameSession::IsPlayerGrounded() const noexcept
{
	return Implementation->bGrounded;
}

bool FGameSession::IsPlayerCrouched() const noexcept
{
	return Implementation->bCrouched;
}

bool FGameSession::IsPlayerSliding() const noexcept
{
	return Implementation->bSliding;
}

FVector3 FGameSession::GetPlayerVelocity() const noexcept
{
	return Implementation->HorizontalVelocity + FVector3::Up() * Implementation->VerticalVelocity;
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
