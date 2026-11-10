#include "Herta/Audio/StagedSounds.h"

#include <format>
#include <set>
#include <system_error>
#include <vector>

namespace Herta
{
std::expected<void, FAudioError> AddStagedSounds(FAudioEngine& Engine, const FCookedContent& Content, const std::span<const FAssetId> Sounds)
{
	std::vector<FAssetId> Pending(Sounds.begin(), Sounds.end());
	std::set<FAssetId> Added;
	while (!Pending.empty())
	{
		const FAssetId Sound = Pending.back();
		Pending.pop_back();
		if (!Sound.IsValid() || !Added.insert(Sound).second)
		{
			continue;
		}

		// A streamed sound only needs its header; reading the whole file would load what it streams.
		bool bStreamed = false;
		for (const std::filesystem::path& Mount : Content.GetMountRoots())
		{
			std::error_code Error;
			const std::filesystem::path File = GetStagedAssetPath(Mount, Sound);
			if (!std::filesystem::is_regular_file(File, Error))
			{
				continue;
			}

			if (const auto Stream = ReadCookedSoundStream(File); Stream && Stream->bStream)
			{
				if (auto Result = Engine.AddSoundStream(Sound, *Stream); !Result)
				{
					return Result;
				}

				bStreamed = true;
			}

			break;
		}

		if (bStreamed)
		{
			continue;
		}

		auto Loaded = Content.Load(Sound);
		if (!Loaded)
		{
			return std::unexpected(FAudioError{std::format("Cannot load sound {}: {}", Sound.ToString(), Loaded.error().Message)});
		}

		if (auto* const Wave = std::get_if<FCookedSound>(&*Loaded))
		{
			if (auto Result = Engine.AddSound(Sound, std::make_shared<const FCookedSound>(std::move(*Wave))); !Result)
			{
				return Result;
			}
		}
		else if (auto* const Graph = std::get_if<FCookedSoundGraph>(&*Loaded))
		{
			for (const FAssetId& Child : GetSoundGraphWaves(Graph->Graph))
			{
				Pending.push_back(Child);
			}

			if (auto Result = Engine.AddSoundGraph(Sound, std::make_shared<const FCookedSoundGraph>(std::move(*Graph))); !Result)
			{
				return Result;
			}
		}
		else
		{
			return std::unexpected(FAudioError{std::format("Asset {} is not a sound", Sound.ToString())});
		}
	}

	return {};
}
}
