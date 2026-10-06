#include "Herta/ShaderCompiler/ShaderCompiler.h"

#include <exception>
#include <print>
#include <string_view>
#include <vector>

namespace
{
int Run(const int ArgumentCount, const char* const* Arguments)
{
	if (ArgumentCount < 5)
	{
		std::println(stderr, "Usage: HertaShaderWorker <source.slang> <vertex|fragment> <entry-point> <output.hshader> [--debug] [--include-root <path>]");
		return 2;
	}

	const std::string_view Stage(Arguments[2]);
	if (Stage != "vertex" && Stage != "fragment")
	{
		std::println(stderr, "Invalid shader stage or option");
		return 2;
	}

	try
	{
		const std::string_view SourcePath(Arguments[1]);
		const std::string_view OutputPath(Arguments[4]);
		Herta::FShaderCompileRequest Request{.Source = std::filesystem::path(std::u8string(SourcePath.begin(), SourcePath.end())), .EntryPoint = Arguments[3], .Stage = Stage == "vertex" ? Herta::EShaderStage::Vertex : Herta::EShaderStage::Fragment};

		for (int Index = 5; Index < ArgumentCount; ++Index)
		{
			const std::string_view Option(Arguments[Index]);
			if (Option == "--debug")
			{
				Request.bDebugInformation = true;
			}
			else if (Option == "--include-root" && Index + 1 < ArgumentCount)
			{
				const std::string_view Path(Arguments[++Index]);
				Request.IncludeRoots.emplace_back(std::u8string(Path.begin(), Path.end()));
			}
			else
			{
				std::println(stderr, "Unknown or incomplete shader worker option: {}", Option);
				return 2;
			}
		}

		const auto Shader = Herta::CompileShader(Request);
		if (!Shader)
		{
			std::println(stderr, "{}", Shader.error().Message);
			return 1;
		}

		const auto Saved = Herta::SaveCookedShader(std::filesystem::path(std::u8string(OutputPath.begin(), OutputPath.end())), *Shader);
		if (!Saved)
		{
			std::println(stderr, "{}", Saved.error().Message);
			return 1;
		}

		std::println("Cooked {}: {} words, Slang {}", Arguments[4], Shader->Bytecode.size(), Shader->CompilerVersion);
		return 0;
	}
	catch (const std::exception& Error)
	{
		std::println(stderr, "Shader worker failed: {}", Error.what());
		return 1;
	}
}
}

#ifdef HERTA_PLATFORM_WINDOWS
int wmain(const int ArgumentCount, wchar_t** const Arguments)
{
	try
	{
		std::vector<std::string> Utf8Arguments;
		Utf8Arguments.reserve(static_cast<std::size_t>(ArgumentCount));

		for (int Index = 0; Index < ArgumentCount; ++Index)
		{
			const auto Text = std::filesystem::path(Arguments[Index]).u8string();
			Utf8Arguments.emplace_back(Text.begin(), Text.end());
		}

		std::vector<const char*> Pointers;
		Pointers.reserve(Utf8Arguments.size());

		for (const auto& Argument : Utf8Arguments)
		{
			Pointers.push_back(Argument.c_str());
		}

		return Run(ArgumentCount, Pointers.data());
	}
	catch (const std::exception& Error)
	{
		std::println(stderr, "Shader worker failed: {}", Error.what());
		return 1;
	}
}
#else
int main(const int ArgumentCount, char** const Arguments)
{
	return Run(ArgumentCount, Arguments);
}
#endif
