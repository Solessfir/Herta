#include "Herta/MaterialGraph/MaterialGraph.h"
#include "Herta/ShaderCompiler/ShaderCompiler.h"
#include "TestFiles.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace Herta
{
namespace
{
struct FMaterialGraphBuilder
{
	FGraphNodeId Add(const std::string_view Type)
	{
		const FGraphNodeId Id = NextGraphNodeId(Graph);
		auto Node = MakeGraphNode(GetMaterialGraphSchema(), Type, Id, {});
		REQUIRE(Node);
		Graph.Nodes.push_back(std::move(*Node));
		return Id;
	}

	void Set(const FGraphNodeId Id, const std::string_view Key, FGraphValue Value)
	{
		auto& Properties = std::ranges::find(Graph.Nodes, Id, &FGraphNode::Id)->Properties;
		std::ranges::find(Properties, Key, &FGraphProperty::Key)->Value = std::move(Value);
	}

	void Link(const FGraphNodeId From, const std::string_view FromPin, const FGraphNodeId To, const std::string_view ToPin)
	{
		Graph.Links.push_back({.From = {.Node = From, .Key = std::string(FromPin)}, .To = {.Node = To, .Key = std::string(ToPin)}});
	}

	FGraph Graph{.Domain = std::string(MaterialGraphDomain), .DomainVersion = MaterialGraphVersion};
};

std::size_t Count(const std::string_view Text, const std::string_view Fragment)
{
	std::size_t Found = 0;
	for (std::size_t At = Text.find(Fragment); At != std::string_view::npos; At = Text.find(Fragment, At + 1))
	{
		++Found;
	}

	return Found;
}

std::filesystem::path FindEngineShaders()
{
	std::error_code Error;
	for (auto Path = std::filesystem::absolute(Tests::GetSiblingExecutable("HertaShaderWorker"), Error).parent_path(); !Path.empty() && Path != Path.parent_path(); Path = Path.parent_path())
	{
		if (std::filesystem::is_regular_file(Path / "Engine/Shaders/GraphMaterial.slangh", Error))
		{
			return Path / "Engine/Shaders";
		}
	}

	return {};
}

bool HasDiagnostic(const std::vector<FGraphDiagnostic>& Diagnostics, const FGraphNodeId Node, const std::string_view Fragment)
{
	return std::ranges::any_of(Diagnostics, [&](const FGraphDiagnostic& Diagnostic)
	{
		return Diagnostic.Node == Node && Diagnostic.Message.find(Fragment) != std::string::npos;
	});
}
}

TEST_CASE("Material graphs compile connected outputs and leave the rest to the material asset")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId Texture = Builder.Add("TextureSample");
	const FGraphNodeId Output = Builder.Add("MaterialOutput");
	Builder.Link(Texture, "RGB", Output, "BaseColor");

	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	CHECK(Compiled->Diagnostics.empty());
	CHECK(Compiled->Source.find("#include \"GraphMaterial.slangh\"") != std::string::npos);
	CHECK(Compiled->Source.find("FMaterialSample Material = DefaultMaterial(Input);") != std::string::npos);
	CHECK(Compiled->Source.find("float4 Value1 = BaseColorTexture.Sample(LinearSampler, Input.UV);") != std::string::npos);
	CHECK(Compiled->Source.find("Material.BaseColor = Value1.xyz;") != std::string::npos);
	CHECK(Compiled->Source.find("Material.Roughness") == std::string::npos);
}

TEST_CASE("Material graph parameters take slots in name order and read the visual uniforms")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId Tint = Builder.Add("VectorParameter");
	const FGraphNodeId Rough = Builder.Add("ScalarParameter");
	const FGraphNodeId Unused = Builder.Add("ScalarParameter");
	const FGraphNodeId Output = Builder.Add("MaterialOutput");
	Builder.Set(Tint, "name", std::string("Tint"));
	Builder.Set(Tint, "value", std::array<float, 4>{0.5f, 0.25f, 1.f, 1.f});
	Builder.Set(Rough, "name", std::string("Roughness"));
	Builder.Set(Rough, "value", 0.3f);
	Builder.Set(Unused, "name", std::string("Glow"));
	Builder.Link(Tint, "RGB", Output, "BaseColor");
	Builder.Link(Rough, "Value", Output, "Roughness");

	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	REQUIRE(Compiled->Parameters.size() == 3);
	CHECK(Compiled->Parameters[0] == FMaterialShaderParameter{.Name = "Glow", .Default = {0.5f, 0.f, 0.f, 0.f}});
	CHECK(Compiled->Parameters[1] == FMaterialShaderParameter{.Name = "Roughness", .Default = {0.3f, 0.f, 0.f, 0.f}});
	CHECK(Compiled->Parameters[2] == FMaterialShaderParameter{.Name = "Tint", .Default = {0.5f, 0.25f, 1.f, 1.f}, .bVector = true});
	CHECK(Compiled->Source.find("Visual.MaterialParameters[2].xyz") != std::string::npos);
	CHECK(Compiled->Source.find("Visual.MaterialParameters[1].x") != std::string::npos);
	CHECK(Compiled->Source.find("MaterialParameters[0]") == std::string::npos);

	Builder.Set(Unused, "name", std::string("Tint"));
	const auto Duplicate = CompileMaterialGraph(Builder.Graph);
	REQUIRE_FALSE(Duplicate);
	CHECK(HasDiagnostic(Duplicate.error(), Unused, "already named"));
	Builder.Set(Unused, "name", std::string("Not valid"));
	const auto Invalid = CompileMaterialGraph(Builder.Graph);
	REQUIRE_FALSE(Invalid);
	CHECK(HasDiagnostic(Invalid.error(), Unused, "letters, digits"));
}

TEST_CASE("Material graph reroutes pass their input through unchanged")
{
	FMaterialGraphBuilder Direct;
	const FGraphNodeId DirectTexture = Direct.Add("TextureSample");
	const FGraphNodeId DirectOutput = Direct.Add("MaterialOutput");
	Direct.Link(DirectTexture, "RGB", DirectOutput, "BaseColor");

	FMaterialGraphBuilder Routed;
	const FGraphNodeId Texture = Routed.Add("TextureSample");
	const FGraphNodeId Output = Routed.Add("MaterialOutput");
	const FGraphNodeId First = Routed.Add("Reroute");
	const FGraphNodeId Second = Routed.Add("Reroute");
	Routed.Link(Texture, "RGB", First, "In");
	Routed.Link(First, "Out", Second, "In");
	Routed.Link(Second, "Out", Output, "BaseColor");

	const auto Expected = CompileMaterialGraph(Direct.Graph);
	const auto Compiled = CompileMaterialGraph(Routed.Graph);
	REQUIRE(Expected);
	REQUIRE(Compiled);
	CHECK(Compiled->Diagnostics.empty());
	CHECK(Compiled->Source == Expected->Source);
	CHECK(GetMaterialGraphSchema().FindNode("Reroute")->bReroute);

	// An open reroute is an error on the reroute itself.
	FMaterialGraphBuilder Open;
	const FGraphNodeId Loose = Open.Add("Reroute");
	const FGraphNodeId OpenOutput = Open.Add("MaterialOutput");
	Open.Link(Loose, "Out", OpenOutput, "Roughness");
	const auto Failed = CompileMaterialGraph(Open.Graph);
	REQUIRE_FALSE(Failed);
	CHECK(HasDiagnostic(Failed.error(), Loose, "'In' needs a connection"));
}

TEST_CASE("Material graphs fold constants and widen scalars")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId Two = Builder.Add("Constant");
	const FGraphNodeId Three = Builder.Add("Constant");
	const FGraphNodeId Product = Builder.Add("Multiply");
	const FGraphNodeId Half = Builder.Add("Constant");
	const FGraphNodeId Inverse = Builder.Add("OneMinus");
	const FGraphNodeId Output = Builder.Add("MaterialOutput");
	Builder.Set(Two, "value", 2.f);
	Builder.Set(Three, "value", 0.125f);
	Builder.Set(Half, "value", 0.5f);
	Builder.Link(Two, "Value", Product, "A");
	Builder.Link(Three, "Value", Product, "B");
	Builder.Link(Product, "Result", Output, "Roughness");
	Builder.Link(Half, "Value", Inverse, "In");
	Builder.Link(Inverse, "Result", Output, "BaseColor");

	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	CHECK(Compiled->Source.find("Material.Roughness = 0.25;") != std::string::npos);
	CHECK(Compiled->Source.find("Material.BaseColor = float3(0.5, 0.5, 0.5);") != std::string::npos);
	CHECK(Count(Compiled->Source, " Value") == 0);
}

TEST_CASE("Material graphs share identical subexpressions")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId First = Builder.Add("TextureSample");
	const FGraphNodeId Second = Builder.Add("TextureSample");
	const FGraphNodeId Sum = Builder.Add("Add");
	const FGraphNodeId Output = Builder.Add("MaterialOutput");
	Builder.Link(First, "R", Sum, "A");
	Builder.Link(Second, "G", Sum, "B");
	Builder.Link(Sum, "Result", Output, "Metallic");

	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	CHECK(Count(Compiled->Source, ".Sample(") == 1);
	CHECK(Compiled->Source.find("float Value2 = (Value1.x + Value1.y);") != std::string::npos);
}

TEST_CASE("Material graph errors point at the node that caused them")
{
	FMaterialGraphBuilder Mismatch;
	const FGraphNodeId UV = Mismatch.Add("TexCoord");
	const FGraphNodeId Position = Mismatch.Add("WorldPosition");
	const FGraphNodeId Sum = Mismatch.Add("Add");
	const FGraphNodeId Output = Mismatch.Add("MaterialOutput");
	Mismatch.Link(UV, "UV", Sum, "A");
	Mismatch.Link(Position, "Position", Sum, "B");
	Mismatch.Link(Sum, "Result", Output, "Emissive");
	const auto Widths = CompileMaterialGraph(Mismatch.Graph);
	REQUIRE_FALSE(Widths);
	CHECK(HasDiagnostic(Widths.error(), Sum, "needs 3 components but receives 2"));

	FMaterialGraphBuilder Open;
	const FGraphNodeId Lonely = Open.Add("Multiply");
	const FGraphNodeId OpenOutput = Open.Add("MaterialOutput");
	Open.Link(Lonely, "Result", OpenOutput, "Roughness");
	const auto Missing = CompileMaterialGraph(Open.Graph);
	REQUIRE_FALSE(Missing);
	CHECK(HasDiagnostic(Missing.error(), Lonely, "'A' needs a connection"));

	FMaterialGraphBuilder Slot;
	const FGraphNodeId Texture = Slot.Add("TextureSample");
	const FGraphNodeId SlotOutput = Slot.Add("MaterialOutput");
	Slot.Set(Texture, "slot", std::string("detail"));
	Slot.Link(Texture, "RGB", SlotOutput, "BaseColor");
	const auto BadSlot = CompileMaterialGraph(Slot.Graph);
	REQUIRE_FALSE(BadSlot);
	CHECK(HasDiagnostic(BadSlot.error(), Texture, "not a material texture slot"));

	FMaterialGraphBuilder Empty;
	Empty.Add("Constant");
	const auto NoOutput = CompileMaterialGraph(Empty.Graph);
	REQUIRE_FALSE(NoOutput);
	CHECK(HasDiagnostic(NoOutput.error(), {}, "Material Output"));
}

TEST_CASE("Material graphs warn about nodes that do not reach the output")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId Unused = Builder.Add("Constant");
	Builder.Add("MaterialOutput");
	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	REQUIRE(Compiled->Diagnostics.size() == 1);
	CHECK(Compiled->Diagnostics[0].Severity == EGraphSeverity::Warning);
	CHECK(Compiled->Diagnostics[0].Node == Unused);
}

TEST_CASE("Generated material graph shaders compile against the engine mesh shading")
{
	FMaterialGraphBuilder Builder;
	const FGraphNodeId UV = Builder.Add("TexCoord");
	const FGraphNodeId Tiling = Builder.Add("Constant");
	const FGraphNodeId Scaled = Builder.Add("Multiply");
	const FGraphNodeId Base = Builder.Add("TextureSample");
	const FGraphNodeId Tint = Builder.Add("Vector");
	const FGraphNodeId Tinted = Builder.Add("Multiply");
	const FGraphNodeId Normal = Builder.Add("TextureSample");
	const FGraphNodeId Unpacked = Builder.Add("Multiply");
	const FGraphNodeId Offset = Builder.Add("Subtract");
	const FGraphNodeId Rough = Builder.Add("Split");
	const FGraphNodeId Position = Builder.Add("WorldPosition");
	const FGraphNodeId Wave = Builder.Add("Sin");
	const FGraphNodeId Glow = Builder.Add("Saturate");
	const FGraphNodeId Output = Builder.Add("MaterialOutput");
	Builder.Set(Tiling, "value", 4.f);
	Builder.Set(Normal, "slot", std::string("normal"));
	Builder.Set(Tint, "value", std::array<float, 4>{1.f, 0.8f, 0.6f, 1.f});
	Builder.Link(UV, "UV", Scaled, "A");
	Builder.Link(Tiling, "Value", Scaled, "B");
	Builder.Link(Scaled, "Result", Base, "UV");
	Builder.Link(Base, "RGB", Tinted, "A");
	Builder.Link(Tint, "RGB", Tinted, "B");
	Builder.Link(Tinted, "Result", Output, "BaseColor");
	Builder.Link(Base, "A", Output, "Alpha");
	Builder.Link(Normal, "RGB", Unpacked, "A");
	Builder.Link(Tiling, "Value", Unpacked, "B");
	Builder.Link(Unpacked, "Result", Offset, "A");
	Builder.Link(Tiling, "Value", Offset, "B");
	Builder.Link(Offset, "Result", Output, "Normal");
	Builder.Link(Base, "RGBA", Rough, "In");
	Builder.Link(Rough, "G", Output, "Roughness");
	Builder.Link(Position, "Position", Wave, "In");
	Builder.Link(Wave, "Result", Glow, "In");
	Builder.Link(Glow, "Result", Output, "Emissive");

	const auto Compiled = CompileMaterialGraph(Builder.Graph);
	REQUIRE(Compiled);
	const std::filesystem::path Shaders = FindEngineShaders();
	REQUIRE_FALSE(Shaders.empty());
	const Tests::FScratchDirectory Scratch("HertaMaterialGraph");
	const auto Source = Scratch.GetPath() / "Graph.slang";
	{
		std::ofstream Stream(Source);
		Stream << Compiled->Source;
	}

	for (const auto& [Entry, Stage] : std::array{std::pair{"vertexMain", EShaderStage::Vertex}, std::pair{"instancedVertexMain", EShaderStage::Vertex}, std::pair{"fragmentMain", EShaderStage::Fragment}})
	{
		const auto Shader = CompileShader({.Source = Source, .EntryPoint = Entry, .Stage = Stage, .IncludeRoots = {Shaders}});
		INFO(Compiled->Source);
		REQUIRE_MESSAGE(Shader.has_value(), (Shader ? "" : Shader.error().Message));
	}
}
}
