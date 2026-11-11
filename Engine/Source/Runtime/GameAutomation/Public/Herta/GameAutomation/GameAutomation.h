#pragma once

#include "Herta/Audio/AudioEngine.h"
#include "Herta/GameFramework/GameSession.h"
#include "Herta/Math/Matrix.h"
#include "Herta/RHI/Presentation.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace Herta
{
class FLogService;
class FGameClient;
class FGameServer;

// What one frame of a game or server process offers automation; any part may be missing, such as the window on a dedicated server.
struct FGameAutomationFrame
{
	FGameSession* Session = nullptr;
	FAudioEngine* Audio = nullptr;
	const FGameClient* Client = nullptr;
	const FGameServer* Server = nullptr;
	// Asks the window for its next presented frame, and takes it once presented; both empty without a window.
	std::function<void()> RequestCapture{};
	std::function<std::optional<std::expected<FPresentationCapture, FPresentationError>>()> TakeCapture{};
	// The camera's view matrix, for the camera position play_state reports.
	FMatrix4 View{};
	bool bPaused = false;
	bool bPresented = false;
};

// Game and server automation over the same protocol and tool names as the editor's: play_state, play_input, play_teleport, editor_capture,
// audio_voices, and editor_log, plus network_state. Scripted input replaces the devices' while it runs.
class FGameAutomation final
{
public:
	[[nodiscard]] static std::expected<std::unique_ptr<FGameAutomation>, std::string> Create(std::string_view Name, bool bAllowWrites, FLogService& Log);

	FGameAutomation(const FGameAutomation&) = delete;
	FGameAutomation& operator=(const FGameAutomation&) = delete;
	FGameAutomation(FGameAutomation&&) = delete;
	FGameAutomation& operator=(FGameAutomation&&) = delete;
	~FGameAutomation();

	// Takes requests and answers those that are ready. Call once a frame, after the frame's update and present.
	void Pump(const FGameAutomationFrame& Frame);
	// The scripted input for this frame while a play_input run lasts, to use instead of the devices'.
	[[nodiscard]] std::optional<FGameInput> TakeInput();
	[[nodiscard]] const std::filesystem::path& GetSessionFile() const noexcept;

	struct FImplementation;

private:
	explicit FGameAutomation(std::unique_ptr<FImplementation> InImplementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};

// The session's player, objective, characters, agents, effects, navigation, and regions as JSON members after a leading comma, the same
// fields the editor's play_state reports.
void AppendGameSessionState(std::string& Output, const FGameSession& Session, const FMatrix4& View);
[[nodiscard]] std::string DescribeAudioStats(const FAudioStats& Stats);
[[nodiscard]] std::string DescribeAudioPlays(const FAudioEngine& Audio, bool bPlayingOnly, const std::function<std::string(const FAssetId&)>& Label);
}
