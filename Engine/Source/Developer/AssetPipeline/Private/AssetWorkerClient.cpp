#include "AssetWorkerProtocol.h"
#include "FileUtilities.h"
#include "Herta/AssetPipeline/AssetCooker.h"
#include "Herta/Platform/Process.h"

#include <format>
#include <ranges>

namespace Herta
{
std::expected<FAssetCookResult, FAssetError> CookAssetInWorker(const FAssetCookRequest& Request, const FAssetWorkerOptions& Options)
{
	FProcessRequest Process;
	Process.Executable = Options.WorkerPath;
	Process.Arguments = MakeAssetWorkerArguments(Request);
	Process.Timeout = Options.Timeout;
	Process.ShouldCancel = Options.ShouldCancel;
	std::expected<FProcessResult, FProcessError> Result = RunProcess(Process);
	if (!Result)
	{
		return std::unexpected(FAssetError{std::format("Asset worker for '{}' failed: {}", Request.SourcePath, Result.error().Message)});
	}
	if (Result->ExitCode != 0)
	{
		std::string Message = Result->StandardError;
		while (!Message.empty() && (Message.back() == '\n' || Message.back() == '\r'))
		{
			Message.pop_back();
		}
		if (Message.empty())
		{
			Message = std::format("the worker exited with code {}", Result->ExitCode);
		}
		return std::unexpected(FAssetError{std::format("Cannot cook '{}': {}", Request.SourcePath, Message)});
	}
	return ParseAssetWorkerOutput(Result->StandardOutput);
}
}
