#include "Herta/RHI/CookedShader.h"

#include <doctest/doctest.h>

namespace Herta
{
namespace
{
FShaderAsset MakeShader()
{
	FShaderAsset Shader;
	Shader.Stage = EShaderStage::Fragment;
	Shader.EntryPoint = "fragmentMain";
	Shader.CompilerVersion = "test-compiler";
	Shader.PermutationKey = "test";
	Shader.PushConstantSize = 64;
	Shader.bDebugInformation = true;
	Shader.Bindings = {{"Texture", EShaderBindingType::Texture, 0, 0}, {"Sampler", EShaderBindingType::Sampler, 128, 0}};
	Shader.Dependencies = {{"TexturedMesh.slang", 42}};
	Shader.Bytecode = {0x07230203, 0x00010500, 0, 1, 0};
	return Shader;
}
}

TEST_CASE("Cooked shaders preserve bytecode and reflected metadata")
{
	const FShaderAsset Shader = MakeShader();
	const auto Bytes = SerializeCookedShader(Shader);
	REQUIRE(Bytes);
	const auto Repeated = SerializeCookedShader(Shader);
	REQUIRE(Repeated);
	CHECK(*Repeated == *Bytes);
	const auto Loaded = DeserializeCookedShader(*Bytes);
	REQUIRE(Loaded);
	CHECK(Loaded->Stage == EShaderStage::Fragment);
	CHECK(Loaded->EntryPoint == "fragmentMain");
	CHECK(Loaded->CompilerVersion == "test-compiler");
	CHECK(Loaded->PermutationKey == "test");
	CHECK(Loaded->PushConstantSize == 64);
	CHECK(Loaded->bDebugInformation);
	REQUIRE(Loaded->Bindings.size() == 2);
	CHECK(Loaded->Bindings[1].Binding == 128);
	CHECK(Loaded->Bindings[1].Type == EShaderBindingType::Sampler);
	REQUIRE(Loaded->Dependencies.size() == 1);
	CHECK(Loaded->Dependencies[0].ContentHash == 42);
	CHECK(Loaded->Bytecode == Shader.Bytecode);
}

TEST_CASE("Cooked shaders reject corruption and every truncated prefix")
{
	const auto Bytes = SerializeCookedShader(MakeShader());
	REQUIRE(Bytes);
	for (std::size_t Length = 0; Length < Bytes->size(); ++Length)
	{
		CHECK_FALSE(DeserializeCookedShader(std::span(*Bytes).first(Length)));
	}

	auto Corrupt = *Bytes;
	Corrupt[12] ^= std::byte{0xff};
	CHECK_FALSE(DeserializeCookedShader(Corrupt));
	Corrupt = *Bytes;
	Corrupt.push_back(std::byte{0});
	CHECK_FALSE(DeserializeCookedShader(Corrupt));
}

TEST_CASE("Cooked shaders reject unsupported metadata")
{
	FShaderAsset Shader = MakeShader();
	Shader.Bytecode[0] = 0;
	CHECK_FALSE(SerializeCookedShader(Shader));
	Shader = MakeShader();
	Shader.Stage = static_cast<EShaderStage>(99);
	CHECK_FALSE(SerializeCookedShader(Shader));
	Shader = MakeShader();
	Shader.PushConstantSize = 129;
	CHECK_FALSE(SerializeCookedShader(Shader));
	Shader = MakeShader();
	Shader.EntryPoint.clear();
	CHECK_FALSE(SerializeCookedShader(Shader));
	Shader = MakeShader();
	Shader.CompilerVersion.assign(4097, 'x');
	CHECK_FALSE(SerializeCookedShader(Shader));
}
}
