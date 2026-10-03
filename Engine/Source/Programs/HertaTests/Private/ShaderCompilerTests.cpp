#include "Herta/ShaderCompiler/ShaderCompiler.h"

#include <doctest/doctest.h>

#include <chrono>
#include <fstream>

namespace Herta
{
namespace
{
class FShaderFixture
{
public:
	FShaderFixture()
	    : Directory(std::filesystem::temp_directory_path() / ("HertaShaderTests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		std::filesystem::create_directories(Directory);
	}

	~FShaderFixture()
	{
		std::error_code Error;
		std::filesystem::remove_all(Directory, Error);
	}

	FShaderFixture(const FShaderFixture&) = delete;
	FShaderFixture& operator=(const FShaderFixture&) = delete;
	FShaderFixture(FShaderFixture&&) = delete;
	FShaderFixture& operator=(FShaderFixture&&) = delete;

	void Write(const std::string& Name, const std::string& Text) const
	{
		std::ofstream Stream(Directory / Name);
		Stream << Text;
		Stream.close();
		REQUIRE(Stream.good());
	}

	std::filesystem::path Directory;
};
}

TEST_CASE("Slang worker cooks deterministic shaders and tracks included sources")
{
	const FShaderFixture Fixture;
	Fixture.Write("Shared.slang", "float4 Project(float3 Position) { return float4(Position, 1); }\n");
	Fixture.Write("Vertex.slang", "#include \"Shared.slang\"\n[shader(\"vertex\")] float4 vertexMain(float3 Position : POSITION) : SV_Position { return Project(Position); }\n");
	const FShaderCompileRequest Request{.Source = Fixture.Directory / "Vertex.slang", .EntryPoint = "vertexMain", .Stage = EShaderStage::Vertex, .bDebugInformation = false};
	const auto Shader = CompileShader(Request);
	REQUIRE_MESSAGE(Shader.has_value(), (Shader ? "" : Shader.error().Message));
	CHECK(Shader->Stage == EShaderStage::Vertex);
	CHECK(Shader->EntryPoint == "vertexMain");
	CHECK_FALSE(Shader->CompilerVersion.empty());
	CHECK(Shader->Bytecode[0] == 0x07230203);
	CHECK(Shader->Dependencies.size() == 2);
	const auto Repeated = CompileShader(Request);
	REQUIRE(Repeated);
	const auto FirstBytes = SerializeCookedShader(*Shader);
	const auto SecondBytes = SerializeCookedShader(*Repeated);
	REQUIRE(FirstBytes);
	REQUIRE(SecondBytes);
	CHECK(*FirstBytes == *SecondBytes);
	const auto Output = Fixture.Directory / "Vertex.hshader";
	REQUIRE(SaveCookedShader(Output, *Shader));
	REQUIRE(SaveCookedShader(Output, *Repeated));
	const auto Loaded = LoadCookedShader(Output);
	REQUIRE(Loaded);
	CHECK(Loaded->Bytecode == Shader->Bytecode);

	Fixture.Write("Shared.slang", "broken source\n");
	CHECK_FALSE(CompileShader(Request));
	const auto Preserved = LoadCookedShader(Output);
	REQUIRE(Preserved);
	CHECK(Preserved->Bytecode == Shader->Bytecode);
}

TEST_CASE("Slang worker rejects missing entry points and stage mismatches")
{
	const FShaderFixture Fixture;
	Fixture.Write("Fragment.slang", "struct FConstants { float4 Color; };\n[[vk::push_constant]] ConstantBuffer<FConstants> Constants;\n[[vk::binding(0,0)]] Texture2D<float4> Texture;\n[[vk::binding(128,0)]] SamplerState Sampler;\n[shader(\"fragment\")] float4 fragmentMain() : SV_Target { return Texture.Sample(Sampler, float2(0,0)) * Constants.Color; }\n");
	CHECK_FALSE(CompileShader({Fixture.Directory / "Missing.slang", "fragmentMain", EShaderStage::Fragment, false}));
	CHECK_FALSE(CompileShader({Fixture.Directory / "Fragment.slang", "missing", EShaderStage::Fragment, false}));
	CHECK_FALSE(CompileShader({Fixture.Directory / "Fragment.slang", "fragmentMain", EShaderStage::Vertex, false}));
	const auto Shader = CompileShader({.Source = Fixture.Directory / "Fragment.slang", .EntryPoint = "fragmentMain", .Stage = EShaderStage::Fragment, .bDebugInformation = true});
	REQUIRE(Shader);
	CHECK(Shader->bDebugInformation);
	CHECK(Shader->PushConstantSize == 16);
	REQUIRE(Shader->Bindings.size() == 2);
	CHECK(Shader->Bindings[0].Binding == 0);
	CHECK(Shader->Bindings[0].Type == EShaderBindingType::Texture);
	CHECK(Shader->Bindings[1].Binding == 128);
	CHECK(Shader->Bindings[1].Type == EShaderBindingType::Sampler);
}

TEST_CASE("Slang worker rejects unsupported resource kinds instead of treating them as textures")
{
	const FShaderFixture Fixture;
	for (const std::string_view Type : {"RWTexture2D<float4>", "StructuredBuffer<float4>", "Texture3D<float4>", "Texture2DArray<float4>"})
	{
		Fixture.Write("Unsupported.slang", "[[vk::binding(0,0)]] " + std::string(Type) + " Resource;\n[shader(\"fragment\")] float4 fragmentMain() : SV_Target { return float4(1,0,0,1); }\n");
		const auto Shader = CompileShader({.Source = Fixture.Directory / "Unsupported.slang", .EntryPoint = "fragmentMain", .Stage = EShaderStage::Fragment, .bDebugInformation = false});
		REQUIRE_FALSE(Shader);
		CHECK(Shader.error().Message.find("Only read-only Texture2D") != std::string::npos);
	}
}
}
