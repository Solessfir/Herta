#pragma once

#include "Herta/AssetPipeline/AssetCooker.h"

#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
[[nodiscard]] std::vector<std::string> MakeAssetWorkerArguments(const FAssetCookRequest& Request);
[[nodiscard]] std::string FormatAssetWorkerOutput(const FAssetCookResult& Result);
[[nodiscard]] std::expected<FAssetCookResult, FAssetError> ParseAssetWorkerOutput(std::string_view Output);
}
