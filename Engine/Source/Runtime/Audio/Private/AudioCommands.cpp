#include "Herta/Audio/AudioCommands.h"

#include <format>

namespace Herta
{
namespace
{
[[nodiscard]] std::string FormatPosition(const FVector3& Position)
{
	return std::format("({:.2f}, {:.2f}, {:.2f})", Position.X, Position.Y, Position.Z);
}

struct FFormatter
{
	std::string operator()(const FAudioPlayCommand& Play) const
	{
		std::string Text = std::format("play {} {} volume {:.3f} pitch {:.3f}", Play.Voice.Value, Play.Sound.ToString(), Play.Volume, Play.Pitch);
		if (Play.bSpatial)
		{
			Text += std::format(" at {}", FormatPosition(Play.Position));
		}

		for (const FAudioParameterValue& Parameter : Play.Parameters)
		{
			Text += std::format(" {}={:.3f}", Parameter.Name, Parameter.Value);
		}

		return Text;
	}

	std::string operator()(const FAudioStopCommand& Stop) const
	{
		return std::format("stop {} fade {:.3f}", Stop.Voice.Value, Stop.FadeSeconds);
	}

	std::string operator()(const FAudioMoveCommand& Move) const
	{
		return std::format("move {} to {}", Move.Voice.Value, FormatPosition(Move.Position));
	}

	std::string operator()(const FAudioParameterCommand& Parameter) const
	{
		return std::format("set {} {}={:.3f}", Parameter.Voice.Value, Parameter.Parameter.Name, Parameter.Parameter.Value);
	}

	std::string operator()(const FAudioListenerCommand& Listener) const
	{
		return std::format("listener at {} facing {}", FormatPosition(Listener.Listener.Position), FormatPosition(Listener.Listener.Forward));
	}

	std::string operator()(const FAudioBusVolumeCommand& Bus) const
	{
		return std::format("bus {} volume {:.3f}", Bus.Bus, Bus.Volume);
	}

	std::string operator()(const FAudioStopAllCommand& Stop) const
	{
		return std::format("stop all fade {:.3f}", Stop.FadeSeconds);
	}
};
}

std::string FormatAudioCommand(const FAudioCommand& Command)
{
	return std::visit(FFormatter{}, Command);
}
}
