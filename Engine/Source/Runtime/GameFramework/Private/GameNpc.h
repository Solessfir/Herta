#pragma once

#include "Herta/AI/AiWorld.h"
#include "Herta/Level/World.h"
#include "Herta/Math/Matrix.h"
#include "Herta/Navigation/NavigationWorld.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace Herta
{
class FPlayerCharacter;

// A Character with an AI Agent, as its StateTree sees it: it walks where its navigation crowd agent goes, turns toward where it walks or
// what it faces, and swings its sword through its animation. Without navigation it stands where it was placed and every move fails.
class FGameNpc final : public IAiBody
{
public:
	FGameNpc(FPlayerCharacter& InBody, const FAiAgentComponent& InSettings);

	// Places it back where it was authored, facing as authored, whole, with a crowd agent in Navigation when there is one.
	void Start(FNavigationWorld* InNavigation, const FMatrix4& Authored);
	// Takes its crowd agent out of the navigation it joined, which must still exist.
	void Leave();
	// Forgets a navigation that has been destroyed, along with its agent there.
	void Abandon() noexcept;
	// Takes where the crowd moved it, turns, and reads the swing's events from the last animation update.
	void Step(float Seconds, std::span<const std::string> BodyEvents);
	// Loses health to a cut, or all of it when it dies, which also takes it off the navigation.
	void Wound(float Amount);
	// On a client, where the server's agent moves it: places it as the server last had it, and starts swings it has not started yet.
	void SetPuppet(const FVector3& InFoot, float InYaw, float InGroundSpeed, float InHealth, std::uint32_t InAttacks);
	[[nodiscard]] std::uint32_t GetAttackCount() const noexcept;

	[[nodiscard]] FMatrix4 GetModel(float Alpha) const;
	[[nodiscard]] float GetGroundSpeed() const noexcept;
	[[nodiscard]] bool IsDead() const noexcept;
	[[nodiscard]] bool IsOnNavigation() const;
	// True on the step a swing's blade starts moving, when the game checks what it hits.
	[[nodiscard]] bool IsSwingStarting() const noexcept;
	[[nodiscard]] const FAiAgentComponent& GetSettings() const noexcept;

	[[nodiscard]] FVector3 GetFootPosition() const override;
	[[nodiscard]] FVector3 GetEyePosition() const override;
	[[nodiscard]] float GetYaw() const override;
	[[nodiscard]] float GetHealth() const override;
	void MoveTo(const FVector3& Destination, float Speed) override;
	void Stop() override;
	[[nodiscard]] EAiMoveStatus GetMoveStatus() const override;
	void Face(const FVector3& Point) override;
	void Animate(std::string_view Trigger) override;
	bool StartAttack(const FVector3& Target) override;
	[[nodiscard]] bool IsAttacking() const override;

private:
	FPlayerCharacter& Body;
	FAiAgentComponent Settings{};
	FNavigationWorld* Navigation = nullptr;
	FNavigationAgentId Agent{};
	FMatrix4 Authored{};
	// The authored model's scale and the offset of its origin from its feet, kept as it moves.
	FVector3 Scale = FVector3::One();

	FVector3 Foot{};
	FVector3 PreviousFoot{};
	float Yaw = 0.f;
	float PreviousYaw = 0.f;
	std::optional<float> FacingYaw{};
	float GroundSpeed = 0.f;
	bool bMoving = false;
	float Health = 1.f;

	bool bAttacking = false;
	std::uint32_t Attacks = 0;
	bool bSwingStarting = false;
	float AttackSeconds = 0.f;
};
}
