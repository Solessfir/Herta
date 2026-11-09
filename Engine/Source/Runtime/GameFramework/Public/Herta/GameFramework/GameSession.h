#pragma once

#include "Herta/GameFramework/CharacterMover.h"
#include "Herta/GameFramework/LevelSimulation.h"
#include "Herta/Level/World.h"
#include "Herta/Math/Vector.h"
#include "Herta/RHI/Presentation.h"
#include "Herta/Renderer/MeshRenderer.h"
#include "Herta/Renderer/Visuals.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
class IGameAssets;
class IGraphicsDevice;

struct FGameError
{
	std::string Message;
};

struct FGameInput
{
	// Movement relative to where the player faces: X strafes right and Y walks forward, each between -1 and 1.
	FVector2 Move{};
	// Look change since the previous update, in radians. Positive yaw turns left and positive pitch looks up.
	float YawDelta = 0.f;
	float PitchDelta = 0.f;
	// Pressed since the previous update. A jump is remembered briefly, so pressing just before landing still jumps. There is no sprint:
	// holding forward builds speed, as in Mirror's Edge.
	bool bJump = false;
	// Held. Crouches, and pressing it at a run slides.
	bool bCrouch = false;
	// Pressed since the previous update. Throws a ball where the player looks.
	bool bThrow = false;
	// Pressed since the previous update. Switches between the first-person and the third-person camera.
	bool bToggleCamera = false;
};

// A gamepad's player controls as it reports them this frame: sticks from -1 to 1 with +X right and +Y up, and whether each button is down.
struct FGameGamepad
{
	FVector2 Move{};
	FVector2 Look{};
	bool bJump = false;
	bool bCrouch = false;
	bool bThrow = false;
	bool bToggleCamera = false;
};

// Adds a gamepad to keyboard and mouse input. Sticks ignore their resting drift, look turns faster the further the stick is pushed, and a jump
// registers once per press.
class FGameGamepadControls
{
public:
	void Apply(FGameInput& Input, const FGameGamepad& Gamepad, float DeltaSeconds);
	// Forgets held buttons, so a press made while paused does not act on resuming.
	void Reset() noexcept;

private:
	bool bJumpDown = true;
	bool bThrowDown = true;
	bool bToggleCameraDown = true;
};

// The player: Mirror's Edge movement through a character mover, its camera, and where falls return it to the start. Under that tuning the
// Sandbox gauges read: walk up 0.18 m steps, step up 0.35 m, jump 0.5 m steps, the 0.6 m hurdle, and a 2 m gap; the 1.3 m hurdle takes a
// mantle.
struct FGamePlayerSettings
{
	FMovementTuning Movement{};
	FMovementModeSet Modes{};
	// Extra movement modes, asked before the built-in ones.
	std::vector<FMovementModeFactory> CustomModes{};
	// TdPawn.BaseEyeHeight puts the eyes 0.14 m under the top of the capsule, standing or crouched.
	float EyeHeight = 1.66f;
	float CrouchEyeHeight = 1.08f;
	float VerticalFieldOfView = 1.2f;
	// Falling below this height returns the player to the start.
	float RespawnHeight = -30.f;
	// The third-person camera sits this far behind the shoulders, centered on the player; a positive offset moves it right for an over-the-shoulder view.
	float ThirdPersonDistance = 3.f;
	float ThirdPersonShoulderOffset = 0.f;
};

enum class EGameObjectiveState : std::uint8_t
{
	// The level has no Start and Finish triggers to time a run between.
	Exploring,
	// Standing in the Start zone; the clock starts on leaving it.
	Ready,
	Running,
	Finished,
};

struct FGameObjective
{
	EGameObjectiveState State = EGameObjectiveState::Exploring;
	// The current run, or the finishing time once finished.
	double ElapsedSeconds = 0.;
	std::optional<double> BestSeconds{};
	// Falls that returned the player to the start during this session.
	std::uint32_t Respawns = 0;
};

// One playthrough of a level: the authored entities stay untouched while physics, the player, and the objective run on top of them.
class FGameSession final
{
public:
	// Device builds soft body meshes; without one they simulate but do not render.
	// Tasks, when given, runs physics jobs on its workers; the result is the same either way.
	// Character, a skinned model, is the body the player plays as; without one the player is an invisible capsule.
	[[nodiscard]] static std::expected<std::unique_ptr<FGameSession>, FGameError> Create(std::span<const FLevelEntity> Entities, const IGameAssets& Assets, IGraphicsDevice* Device, const FGamePlayerSettings& Player = {}, FTaskSystem* Tasks = nullptr, std::shared_ptr<const FCookedModel> Character = nullptr);

	~FGameSession();

	FGameSession(const FGameSession&) = delete;
	FGameSession& operator=(const FGameSession&) = delete;
	FGameSession(FGameSession&&) = delete;
	FGameSession& operator=(FGameSession&&) = delete;

	// Resets physics, the player, and the current run to the authored start. The best time survives.
	[[nodiscard]] std::expected<void, FGameError> Restart();
	[[nodiscard]] std::expected<void, FGameError> Update(float DeltaSeconds, const FGameInput& Input);

	// The view from the player's eyes. The spans stay valid until the next Update, Restart, or GetRenderView.
	[[nodiscard]] const FMeshRenderView& GetRenderView(FExtent2D Extent);
	[[nodiscard]] const FGameObjective& GetObjective() const noexcept;
	// For debug views such as collision wireframes.
	const FPhysicsWorld* GetPhysicsWorld() const noexcept;
	// Entities the simulation left out, and why.
	[[nodiscard]] std::span<const std::string> GetWarnings() const noexcept;
	// Trigger and objective events since the previous call, as log lines.
	[[nodiscard]] std::vector<std::string> TakeEvents();
	[[nodiscard]] FVector3 GetPlayerFootPosition() const noexcept;
	[[nodiscard]] bool IsPlayerGrounded() const noexcept;
	[[nodiscard]] bool IsPlayerCrouched() const noexcept;
	[[nodiscard]] bool IsPlayerSliding() const noexcept;
	[[nodiscard]] bool IsPlayerMantling() const noexcept;
	[[nodiscard]] bool IsPlayerWallRunning() const noexcept;
	[[nodiscard]] FVector3 GetPlayerVelocity() const noexcept;
	// The look direction around the vertical axis; 0 faces +Z.
	[[nodiscard]] float GetPlayerYaw() const noexcept;
	[[nodiscard]] bool IsThirdPerson() const noexcept;
	// The animated body's active state machine state, or empty without a body.
	[[nodiscard]] std::optional<std::size_t> GetCharacterState() const noexcept;
	[[nodiscard]] std::string_view GetCharacterStateName() const noexcept;
	// The first enabled Sky light's environment, for the host's environment filtering.
	[[nodiscard]] std::shared_ptr<const FCookedTexture> GetEnvironment() const;

private:
	struct FImplementation;
	explicit FGameSession(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
