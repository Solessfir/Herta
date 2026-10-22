#pragma once

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
	// Pressed since the previous update. A jump is remembered briefly, so pressing just before landing still jumps.
	bool bJump = false;
	bool bSprint = false;
};

// The Sandbox controller limits: walk up 0.18 m steps, step up 0.35 m, jump 0.5 m steps, the 0.6 m hurdle, and a 2 m gap, but not the 1.3 m hurdle.
struct FGamePlayerSettings
{
	float Height = 1.8f;
	float Radius = 0.3f;
	float EyeHeight = 1.65f;
	float StepHeight = 0.35f;
	float WalkSpeed = 4.5f;
	float SprintSpeed = 7.f;
	// Lifts the feet about 0.75 m.
	float JumpSpeed = 3.85f;
	float GroundAcceleration = 40.f;
	float AirAcceleration = 12.f;
	float VerticalFieldOfView = 1.2f;
	// Falling below this height returns the player to the start.
	float RespawnHeight = -30.f;
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
	[[nodiscard]] static std::expected<std::unique_ptr<FGameSession>, FGameError> Create(std::span<const FLevelEntity> Entities, const IGameAssets& Assets, IGraphicsDevice* Device, const FGamePlayerSettings& Player = {});

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
	// Entities the simulation left out, and why.
	[[nodiscard]] std::span<const std::string> GetWarnings() const noexcept;
	// Trigger and objective events since the previous call, as log lines.
	[[nodiscard]] std::vector<std::string> TakeEvents();
	[[nodiscard]] FVector3 GetPlayerFootPosition() const noexcept;
	[[nodiscard]] bool IsPlayerGrounded() const noexcept;
	// The first enabled Sky light's environment, for the host's environment filtering.
	[[nodiscard]] std::shared_ptr<const FCookedTexture> GetEnvironment() const;

private:
	struct FImplementation;
	explicit FGameSession(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
