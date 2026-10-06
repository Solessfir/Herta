#include "Herta/AssetPipeline/ShaderSources.h"
#include "Herta/Platform/Process.h"
#include "Herta/ShaderCompiler/ShaderCompiler.h"
#include "MaterialShaders.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
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

TEST_CASE("Slang visual authoring ABI reflects multiple textures and draw uniforms")
{
	const FShaderFixture Fixture;
	Fixture.Write("Material.slang", "struct FUniforms { float4 Color; };\n[[vk::binding(64,0)]] ConstantBuffer<FUniforms> Uniforms;\n[[vk::binding(0,0)]] Texture2D<float4> Base;\n[[vk::binding(15,0)]] Texture2D<float4> Normal;\n[[vk::binding(128,0)]] SamplerState Sampler;\n[shader(\"fragment\")] float4 fragmentMain() : SV_Target { return (Base.Sample(Sampler,float2(0,0))+Normal.Sample(Sampler,float2(0,0))) * Uniforms.Color; }\n");
	const auto Shader = CompileShader({.Source = Fixture.Directory / "Material.slang", .EntryPoint = "fragmentMain", .Stage = EShaderStage::Fragment});
	REQUIRE_MESSAGE(Shader.has_value(), (Shader ? "" : Shader.error().Message));
	REQUIRE(Shader->Bindings.size() == 4);
	CHECK(Shader->Bindings[0].Type == EShaderBindingType::ConstantBuffer);
	CHECK(Shader->Bindings[0].Binding == 64);
	CHECK(Shader->Bindings[0].ByteSize == 16);
	CHECK(Shader->Bindings[2].Binding == 15);
	CHECK(SerializeCookedShader(*Shader));
}

TEST_CASE("Slang visual authoring ABI rejects out of range resource registers")
{
	const FShaderFixture Fixture;
	for (const std::string_view Declaration : {"[[vk::binding(16,0)]] Texture2D<float4> Resource;", "[[vk::binding(0,1)]] Texture2D<float4> Resource;", "[[vk::binding(129,0)]] SamplerState Resource;", "struct FUniforms { float4 Color; }; [[vk::binding(65,0)]] ConstantBuffer<FUniforms> Resource;", "struct FUniforms { float4 Color[1025]; }; [[vk::binding(64,0)]] ConstantBuffer<FUniforms> Resource;"})
	{
		Fixture.Write("Invalid.slang", std::string(Declaration) + "\n[shader(\"fragment\")] float4 fragmentMain() : SV_Target { return float4(1,0,0,1); }\n");
		CHECK_FALSE(CompileShader({.Source = Fixture.Directory / "Invalid.slang", .EntryPoint = "fragmentMain", .Stage = EShaderStage::Fragment}));
	}
}

TEST_CASE("Shader include roots resolve shared engine code and track source hashes")
{
	const FShaderFixture Fixture;
	std::filesystem::create_directories(Fixture.Directory / "Engine");
	std::filesystem::create_directories(Fixture.Directory / "Game");
	Fixture.Write("Engine/Common.slangh", "float4 Project(float3 Position) { return float4(Position,1); }\n");
	Fixture.Write("Game/Material.slang", "#include \"Common.slangh\"\n[shader(\"vertex\")] float4 vertexMain(float3 Position:POSITION):SV_Position { return Project(Position); }\n");
	const FShaderCompileRequest Request{.Source = Fixture.Directory / "Game/Material.slang", .EntryPoint = "vertexMain", .Stage = EShaderStage::Vertex, .IncludeRoots = {Fixture.Directory / "Engine"}};
	const auto Shader = CompileShader(Request);
	REQUIRE_MESSAGE(Shader.has_value(), (Shader ? "" : Shader.error().Message));
	CHECK(Shader->Dependencies.size() == 2);
	const std::array<std::filesystem::path, 2> Roots{Fixture.Directory / "Engine", Fixture.Directory / "Game"};
	const auto Sources = CollectShaderSources(Roots, 1, "Material.slang");
	REQUIRE(Sources);
	REQUIRE(Sources->size() == 2);
	CHECK((*Sources)[0].Path == "Material.slang");
	CHECK((*Sources)[1].Root == 0);
	CHECK((*Sources)[1].Path == "Common.slangh");
	Fixture.Write("Game/Material.slang", "// #include \"Missing.slangh\"\n/*\n#include \"AlsoMissing.slangh\"\n*/\n# include \"Common.slangh\"\nfloat includeFactor = 1;\n");
	const auto Commented = CollectShaderSources(Roots, 1, "Material.slang");
	REQUIRE(Commented);
	CHECK(Commented->size() == 2);

	CHECK_FALSE(CollectShaderSources(Roots, 1, "../Engine/Common.slangh"));
	Fixture.Write("Game/Material.slang", "#include \"../../Outside.slangh\"\n");
	CHECK_FALSE(CollectShaderSources(Roots, 1, "Material.slang"));
}

TEST_CASE("Material shader source keys are deterministic and include shared files")
{
	std::array<FShaderSourceDependency, 2> Sources{
	    FShaderSourceDependency{.Path = "Game/Material.slang", .ContentHash = 1},
	    FShaderSourceDependency{.Path = "Engine/Common.slangh", .ContentHash = 2},
	};
	const auto Key = ComputeMaterialShaderSourceKey(Sources);
	std::ranges::reverse(Sources);
	CHECK(ComputeMaterialShaderSourceKey(Sources) == Key);
	Sources[0].ContentHash = 3;
	CHECK(ComputeMaterialShaderSourceKey(Sources) != Key);
}

TEST_CASE("Shader worker include-root arguments compile and failures preserve cooked output")
{
	const FShaderFixture Fixture;
	std::filesystem::create_directories(Fixture.Directory / "Include");
	Fixture.Write("Include/Shared.slangh", "float4 Project(float3 Position) { return float4(Position,1); }\n");
	Fixture.Write("Material.slang", "#include \"Shared.slangh\"\n[shader(\"vertex\")] float4 vertexMain(float3 Position:POSITION):SV_Position { return Project(Position); }\n");
	const auto PathText = [](const std::filesystem::path& Path)
	{
		const auto Utf8 = Path.generic_u8string();
		return std::string(Utf8.begin(), Utf8.end());
	};

	const auto Output = Fixture.Directory / "Material.hshader";
	const FProcessRequest Request{
	    .Executable = Tests::GetSiblingExecutable("HertaShaderWorker"),
	    .Arguments = {PathText(Fixture.Directory / "Material.slang"), "vertex", "vertexMain", PathText(Output), "--include-root", PathText(Fixture.Directory / "Include")},
	    .Timeout = std::chrono::seconds(30),
	};

	const auto Compiled = RunProcess(Request);
	REQUIRE(Compiled);
	REQUIRE_MESSAGE(Compiled->ExitCode == 0, Compiled->StandardError);
	const auto Original = LoadCookedShader(Output);
	REQUIRE(Original);
	Fixture.Write("Material.slang", "not valid Slang\n");
	const auto Failure = RunProcess(Request);
	REQUIRE(Failure);
	CHECK(Failure->ExitCode != 0);
	CHECK_FALSE(Failure->StandardError.empty());
	const auto Preserved = LoadCookedShader(Output);
	REQUIRE(Preserved);
	CHECK(Preserved->Bytecode == Original->Bytecode);
}
}
