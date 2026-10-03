#include "Herta/ShaderCompiler/ShaderCompiler.h"

#include <exception>
#include <print>
#include <string_view>

int main(const int ArgumentCount, char** const Arguments)
{
	if (ArgumentCount < 5 || ArgumentCount > 6)
	{
		std::println(stderr, "Usage: HertaShaderWorker <source.slang> <vertex|fragment> <entry-point> <output.hshader> [--debug]");
		return 2;
	}

	const std::string_view Stage(Arguments[2]);
	if ((Stage != "vertex" && Stage != "fragment") || (ArgumentCount == 6 && std::string_view(Arguments[5]) != "--debug"))
	{
		std::println(stderr, "Invalid shader stage or option");
		return 2;
	}

	try
	{
		const Herta::FShaderCompileRequest Request{.Source = Arguments[1], .EntryPoint = Arguments[3], .Stage = Stage == "vertex" ? Herta::EShaderStage::Vertex : Herta::EShaderStage::Fragment, .bDebugInformation = ArgumentCount == 6};
		const auto Shader = Herta::CompileShader(Request);
		if (!Shader)
		{
			std::println(stderr, "{}", Shader.error().Message);
			return 1;
		}

		const auto Saved = Herta::SaveCookedShader(Arguments[4], *Shader);
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
