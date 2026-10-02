#include "AssetWorkerProtocol.h"

#include "FileUtilities.h"

#include <algorithm>
#include <exception>
#include <format>
#include <print>
#include <ranges>

namespace Herta
{
namespace
{
inline constexpr std::string_view OutputHeader = "HertaAssetCook/1";
inline constexpr std::string_view Usage = "Usage: HertaAssetWorker cook --content-root <path> --derived-data <path> --platform <name> [--force] <content-path>";

[[nodiscard]] std::string SingleLine(std::string Text)
{
	std::ranges::replace_if(Text, [](const char Character)
	                        {
		                        return Character == '\n' || Character == '\r';
	                        },
	                        ' ');
	return Text;
}
}

std::vector<std::string> MakeAssetWorkerArguments(const FAssetCookRequest& Request)
{
	std::vector<std::string> Arguments{"cook", "--content-root", PathToUtf8(Request.ContentRoot), "--derived-data", PathToUtf8(Request.DerivedDataRoot), "--platform", Request.TargetPlatform};
	if (Request.bForce)
	{
		Arguments.emplace_back("--force");
	}
	Arguments.push_back(Request.SourcePath);
	return Arguments;
}

std::string FormatAssetWorkerOutput(const FAssetCookResult& Result)
{
	std::string Output = std::format("{}\nKey {}\nCache {}\n", OutputHeader, ToString(Result.Key), Result.bCacheHit ? "hit" : "miss");
	for (const std::string& Warning : Result.Warnings)
	{
		Output.append(std::format("Warning {}\n", SingleLine(Warning)));
	}
	return Output;
}

std::expected<FAssetCookResult, FAssetError> ParseAssetWorkerOutput(const std::string_view Output)
{
	FAssetCookResult Result;
	bool bHeader = false;
	bool bKey = false;
	bool bCache = false;
	for (const auto Range : std::views::split(Output, '\n'))
	{
		std::string_view Line(Range.begin(), Range.end());
		if (Line.ends_with('\r'))
		{
			Line.remove_suffix(1);
		}
		if (Line.empty())
		{
			continue;
		}
		if (!bHeader)
		{
			bHeader = Line == OutputHeader;
			if (!bHeader)
			{
				break;
			}
		}
		else if (Line.starts_with("Key "))
		{
			const std::optional<FHash128> Key = ParseHash128(Line.substr(4));
			bKey = Key.has_value();
			Result.Key = Key.value_or(FHash128{});
		}
		else if (Line == "Cache hit" || Line == "Cache miss")
		{
			bCache = true;
			Result.bCacheHit = Line == "Cache hit";
		}
		else if (Line.starts_with("Warning "))
		{
			Result.Warnings.emplace_back(Line.substr(8));
		}
	}
	if (!bHeader || !bKey || !bCache)
	{
		return std::unexpected(FAssetError{"The asset worker returned malformed output"});
	}
	return Result;
}

int RunAssetWorker(const std::span<const std::string_view> Arguments)
{
	try
	{
		if (Arguments.size() == 1 && Arguments.front() == "--version")
		{
			std::println("HertaAssetWorker cooked format {}", CookedAssetFormatVersion);
			return 0;
		}

		FAssetCookRequest Request;
		bool bValid = !Arguments.empty() && Arguments.front() == "cook";
		for (std::size_t Index = 1; bValid && Index < Arguments.size(); ++Index)
		{
			const std::string_view Argument = Arguments[Index];
			const bool bHasValue = Index + 1 < Arguments.size();
			if (Argument == "--force")
			{
				Request.bForce = true;
			}
			else if (Argument == "--content-root" && bHasValue)
			{
				Request.ContentRoot = Utf8ToPath(Arguments[++Index]);
			}
			else if (Argument == "--derived-data" && bHasValue)
			{
				Request.DerivedDataRoot = Utf8ToPath(Arguments[++Index]);
			}
			else if (Argument == "--platform" && bHasValue)
			{
				Request.TargetPlatform = Arguments[++Index];
			}
			else if (!Argument.starts_with("--") && Request.SourcePath.empty())
			{
				Request.SourcePath = Argument;
			}
			else
			{
				bValid = false;
			}
		}
		if (!bValid || Request.ContentRoot.empty() || Request.DerivedDataRoot.empty() || Request.TargetPlatform.empty() || Request.SourcePath.empty())
		{
			std::println(stderr, "{}", Usage);
			return 2;
		}

		std::expected<FAssetCookResult, FAssetError> Result = CookAsset(Request);
		if (!Result)
		{
			std::println(stderr, "{}", Result.error().Message);
			return 1;
		}
		std::print("{}", FormatAssetWorkerOutput(*Result));
		return 0;
	}
	catch (const std::exception& Exception)
	{
		std::println(stderr, "Asset worker failed: {}", Exception.what());
	}
	catch (...)
	{
		std::println(stderr, "Asset worker failed with an unknown exception");
	}
	return 1;
}
}
