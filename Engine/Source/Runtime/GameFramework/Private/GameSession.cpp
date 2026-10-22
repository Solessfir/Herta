#include "Herta/GameFramework/GameSession.h"

#include "Herta/GameFramework/GameAssets.h"
#include "Herta/GameFramework/LevelSimulationSetup.h"
#include "Herta/GameFramework/SoftBodyGeometry.h"
#include "Herta/Math/AffineTransform.h"

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

	// cppcheck-suppress uninitMemberVarNoCtor ; set by aggregate initialization in Create
	const IGameAssets& Assets;
	IGraphicsDevice* Device = nullptr;
	FGamePlayerSettings Player{};

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

	// Render view storage referenced by View.
	std::vector<FMatrix4> Models{};
	std::vector<const FRenderMesh*> Meshes{};
	std::vector<std::vector<const FRenderMaterial*>> MaterialSlots{};
	std::vector<std::span<const FRenderMaterial* const>> MaterialSpans{};
	std::vector<FRenderLight> Lights{};
	std::vector<FRenderMeshVertexUpdate> VertexUpdates{};
	FMeshRenderView View{};
};

FGameSession::FGameSession(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FGameSession::~FGameSession() = default;

std::expected<std::unique_ptr<FGameSession>, FGameError> FGameSession::Create(const std::span<const FLevelEntity> Entities, const IGameAssets& Assets, IGraphicsDevice* const Device, const FGamePlayerSettings& Player)
{
	// Built in place: the session holds references and a running simulation that must not move.
	std::unique_ptr<FImplementation> Implementation(new FImplementation{.Assets = Assets, .Device = Device, .Player = Player});
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
		const auto Matrix = World.GetWorldMatrix(*World.FindEntity(Entity.Id));
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
			if (Placed[Index] || (Session.Parents[Index] && !Placed[*Session.Parents[Index]]))
			{
				continue;
			}

			Placed[Index] = true;
			Session.Order.push_back(Index);
			const auto ParentInverse = Session.Parents[Index] ? TryInverseAffine(Session.AuthoredWorld[*Session.Parents[Index]]) : std::optional{FMatrix4{}};
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
	const std::span<const FLevelSimulationBody> Bodies = Setup.Bodies.empty() && Setup.SoftBodies.empty() ? std::span<const FLevelSimulationBody>(Ground) : std::span<const FLevelSimulationBody>(Setup.Bodies);
	if (auto Started = Simulation.Start(Bodies, Setup.SoftBodies, Setup.Constraints); !Started)
	{
		return std::unexpected(FGameError{std::format("Cannot start physics: {}", Started.error().Message)});
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

	const auto Updated = Session.Simulation.Update(DeltaSeconds, [&Session](FPhysicsWorld& World, const float StepSeconds)
	{
		return Session.StepPlayer(World, StepSeconds);
	});

	if (!Updated)
	{
		return std::unexpected(FGameError{std::format("Physics failed: {}", Updated.error().Message)});
	}

	for (const FLevelTriggerEvent& Event : Session.Simulation.TakeTriggerEvents())
	{
		Session.Events.push_back(std::format("{} {} {}", Session.Entities[Event.ObjectIndex].Name, Event.bEntered ? "entered" : "left", Session.Entities[Event.TriggerObjectIndex].Name));
	}

	Session.UpdateObjective(DeltaSeconds);
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
		Models[Index] = Simulated[Index] ? *Simulated[Index] : Parents[Index] ? Models[*Parents[Index]] * ParentRelative[Index]
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
		if (!SoftBody.Mesh || Simulated.size() != SoftBody.Topology.Vertices.size() || SoftBody.Revision == Revision)
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

		SoftBody.Vertices = BuildSoftBodyModel(*Entities[SoftBody.Entity].SoftBody, SoftBody.Topology, Local).Vertices;
		SoftBody.Revision = Revision;
		VertexUpdates.push_back({.Mesh = SoftBody.Mesh.get(), .Vertices = SoftBody.Vertices});
	}
}

const FMeshRenderView& FGameSession::GetRenderView(const FExtent2D Extent)
{
	FImplementation& Session = *Implementation;
	Session.BuildModels();
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

	Session.Lights.resize(std::min(Session.Lights.size(), MaximumRenderLights));
	Visuals.bStudioPreview = std::ranges::none_of(Session.Entities, [](const FLevelEntity& Entity)
	{
		return Entity.Light.has_value();
	});

	const float Alpha = Session.Simulation.GetInterpolationAlpha();
	const FVector3 Eye = Session.PreviousFoot * (1.f - Alpha) + Session.CurrentFoot * Alpha + FVector3::Up() * Session.EyeHeight;
	const float Aspect = Extent.Height > 0 ? static_cast<float>(Extent.Width) / static_cast<float>(Extent.Height) : 1.f;
	Session.View = {
	    .View = FMatrix4::Rotation(GetLookOrientation(Session.Yaw, Session.Pitch).Conjugated()) * FMatrix4::Translation(-Eye),
	    .Projection = FMatrix4::PerspectiveReversedInfinite(Session.Player.VerticalFieldOfView, std::max(Aspect, 0.01f), CameraNear),
	    .Models = Session.Models,
	    .Meshes = Session.Meshes,
	    .Materials = Session.MaterialSpans,
	    .Lights = Session.Lights,
	    .Visuals = Visuals,
	    .VertexUpdates = Session.VertexUpdates,
	};

	return Session.View;
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
