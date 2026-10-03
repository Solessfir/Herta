#include "Herta/AssetPipeline/AssetCooker.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace
{
[[nodiscard]] int Run(const std::vector<std::string>& Arguments)
{
	const std::vector<std::string_view> Views(Arguments.begin(), Arguments.end());
	return Herta::RunAssetWorker(Views);
}
}

#ifdef HERTA_PLATFORM_WINDOWS
// Narrow main receives arguments in the ANSI code page, which cannot represent every content path.
int wmain(const int ArgumentCount, wchar_t** const Arguments)
{
	try
	{
		std::vector<std::string> Utf8Arguments;
		for (int Index = 1; Index < ArgumentCount; ++Index)
		{
			const std::u8string Text = std::filesystem::path(Arguments[Index]).u8string();
			Utf8Arguments.emplace_back(Text.begin(), Text.end());
		}

		return Run(Utf8Arguments);
	}
	catch (...)
	{
		return 1;
	}
}
#else
int main(const int ArgumentCount, char** const Arguments)
{
	try
	{
		return Run(std::vector<std::string>(Arguments + 1, Arguments + ArgumentCount));
	}
	catch (...)
	{
		return 1;
	}
}
#endif
