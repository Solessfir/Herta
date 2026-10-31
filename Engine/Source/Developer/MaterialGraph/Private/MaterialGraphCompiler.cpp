#include "Herta/MaterialGraph/MaterialGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>

namespace Herta
{
namespace
{
constexpr std::array<std::string_view, 6> TextureSlots{"baseColor", "metallic", "roughness", "normal", "occlusion", "emissive"};
constexpr std::array<std::string_view, 6> TextureNames{"BaseColorTexture", "MetallicTexture", "RoughnessTexture", "NormalTexture", "OcclusionTexture", "EmissiveTexture"};
constexpr std::string_view Swizzles = "xyzw";

// One value of the generated function: a constant the compiler can fold, or Slang code of a known width.
struct FExpression
{
	std::string Code{};
	std::uint32_t Width = 1;
	std::optional<std::array<float, 4>> Constant{};
};

std::string FormatFloat(const float Value)
{
	std::string Text = std::format("{}", Value == 0.f ? 0.f : Value);
	if (Text.find_first_of(".e") == std::string::npos)
	{
		Text += ".0";
	}

	return Text;
}

std::string TypeName(const std::uint32_t Width)
{
	return Width == 1 ? "float" : std::format("float{}", Width);
}

FExpression MakeConstant(const std::array<float, 4>& Value, const std::uint32_t Width)
{
	FExpression Expression{.Width = Width, .Constant = Value};
	if (Width == 1)
	{
		Expression.Code = FormatFloat(Value[0]);
		return Expression;
	}

	Expression.Code = TypeName(Width) + "(";
	for (std::uint32_t Index = 0; Index < Width; ++Index)
	{
		Expression.Code += (Index == 0 ? "" : ", ") + FormatFloat(Value[Index]);
	}

	Expression.Code += ")";
	return Expression;
}

class FCompiler
{
public:
	explicit FCompiler(const FGraph& Graph);

	std::optional<FMaterialGraphCompilation> Compile();

	std::vector<FGraphDiagnostic> Diagnostics;

private:
	std::optional<FExpression> Output(const FGraphPin& Pin);
	std::optional<FExpression> Input(const FGraphNode& Node, std::string_view Key, std::optional<float> Default = {});
	std::optional<FExpression> EvaluateOutput(const FGraphNode& Node, std::string_view Key);
	std::optional<FExpression> Elementwise(const FGraphNode& Node, std::span<const FExpression> Operands, std::string_view Function, const std::function<float(std::span<const float>)>& Fold);
	std::optional<FExpression> Fit(const FGraphNode& Node, std::string_view Pin, FExpression Value, std::uint32_t Width);
	FExpression Bind(FExpression Value);
	void Error(FGraphNodeId Node, std::string_view Pin, std::string Message);

	const FGraph& Graph;
	const FGraphSchema& Schema;
	std::map<FGraphPin, FGraphPin> Sources;
	std::map<FGraphPin, std::optional<FExpression>> Evaluated;
	std::set<std::uint32_t> Reached;
	std::map<std::string, std::string> SharedValues;
	std::string Body;
	std::uint32_t NextValue = 1;
};

FCompiler::FCompiler(const FGraph& InGraph)
    : Graph(InGraph)
    , Schema(GetMaterialGraphSchema())
{
	for (const FGraphLink& Link : Graph.Links)
	{
		Sources.emplace(Link.To, Link.From);
	}
}

void FCompiler::Error(const FGraphNodeId Node, const std::string_view Pin, std::string Message)
{
	Diagnostics.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Pin = std::string(Pin), .Message = std::move(Message)});
}

// Values used more than once, or costly to repeat, live in one local; identical code reuses it, which merges duplicated subgraphs.
FExpression FCompiler::Bind(FExpression Value)
{
	if (Value.Constant || Value.Code.starts_with("Input.") || Value.Code.starts_with("Value"))
	{
		return Value;
	}

	const std::string Key = TypeName(Value.Width) + " " + Value.Code;
	auto [Found, bInserted] = SharedValues.try_emplace(Key, std::format("Value{}", NextValue));
	if (bInserted)
	{
		Body += std::format("    {} {} = {};\n", TypeName(Value.Width), Found->second, Value.Code);
		++NextValue;
	}

	return {.Code = Found->second, .Width = Value.Width};
}

// A value at the width a pin needs: equal widths pass, scalars widen, and anything else is the graph's error.
std::optional<FExpression> FCompiler::Fit(const FGraphNode& Node, const std::string_view Pin, FExpression Value, const std::uint32_t Width)
{
	if (Value.Width == Width)
	{
		return Value;
	}

	if (Value.Width != 1)
	{
		Error(Node.Id, Pin, std::format("'{}' needs {} components but receives {}", Pin, Width, Value.Width));
		return std::nullopt;
	}

	if (Value.Constant)
	{
		const float Scalar = (*Value.Constant)[0];
		return MakeConstant({Scalar, Scalar, Scalar, Scalar}, Width);
	}

	return FExpression{.Code = std::format("(({})({}))", TypeName(Width), Value.Code), .Width = Width};
}

std::optional<FExpression> FCompiler::Input(const FGraphNode& Node, const std::string_view Key, const std::optional<float> Default)
{
	const auto Source = Sources.find({.Node = Node.Id, .Key = std::string(Key)});
	if (Source == Sources.end())
	{
		if (Default)
		{
			return MakeConstant({*Default, *Default, *Default, *Default}, 1);
		}

		Error(Node.Id, Key, std::format("'{}' needs a connection", Key));
		return std::nullopt;
	}

	return Output(Source->second);
}

std::optional<FExpression> FCompiler::Output(const FGraphPin& Pin)
{
	if (const auto Found = Evaluated.find(Pin); Found != Evaluated.end())
	{
		return Found->second;
	}

	const FGraphNode* const Node = FindGraphNode(Graph, Pin.Node);
	Reached.insert(Pin.Node.Value);
	auto Value = EvaluateOutput(*Node, Pin.Key);
	if (Value)
	{
		Value = Bind(std::move(*Value));
	}

	Evaluated.emplace(Pin, Value);
	return Value;
}

// Componentwise operations widen scalar operands to the widest one, fold when every operand is constant, and otherwise call Function.
std::optional<FExpression> FCompiler::Elementwise(const FGraphNode& Node, const std::span<const FExpression> Operands, const std::string_view Function, const std::function<float(std::span<const float>)>& Fold)
{
	std::uint32_t Width = 1;
	for (const FExpression& Operand : Operands)
	{
		Width = std::max(Width, Operand.Width);
	}

	// One Minus adds its own constant operand, so operands can outnumber the node's pins.
	const auto Pins = Schema.FindNode(Node.Type)->Inputs;
	std::vector<FExpression> Fitted;
	for (std::size_t Index = 0; Index < Operands.size(); ++Index)
	{
		const std::string_view Pin = Pins[std::min(Index, Pins.size() - 1)].Key;
		auto Value = Fit(Node, Pin, Operands[Index], Width);
		if (!Value)
		{
			return std::nullopt;
		}

		Fitted.push_back(std::move(*Value));
	}

	if (std::ranges::all_of(Fitted, [](const FExpression& Value)
	    {
		    return Value.Constant.has_value();
	    }))
	{
		std::array<float, 4> Result{};
		for (std::uint32_t Component = 0; Component < Width; ++Component)
		{
			std::vector<float> Arguments;
			for (const FExpression& Value : Fitted)
			{
				Arguments.push_back((*Value.Constant)[Component]);
			}

			Result[Component] = Fold(Arguments);
		}

		// A fold that overflows or divides by zero stays in the shader, where the GPU defines the result.
		if (std::ranges::all_of(Result, [](const float Value)
		    {
			    return std::isfinite(Value);
		    }))
		{
			return MakeConstant(Result, Width);
		}
	}

	std::string Code;
	if (Function.size() == 1)
	{
		Code = std::format("({} {} {})", Fitted[0].Code, Function, Fitted[1].Code);
	}
	else
	{
		Code = std::string(Function) + "(";
		for (std::size_t Index = 0; Index < Fitted.size(); ++Index)
		{
			Code += (Index == 0 ? "" : ", ") + Fitted[Index].Code;
		}

		Code += ")";
	}

	return FExpression{.Code = std::move(Code), .Width = Width};
}

std::optional<FExpression> FCompiler::EvaluateOutput(const FGraphNode& Node, const std::string_view Key)
{
	const std::string_view Type = Node.Type;
	const auto Property = [&Node](const std::string_view Name) -> const FGraphValue&
	{
		return std::ranges::find(Node.Properties, Name, &FGraphProperty::Key)->Value;
	};

	const auto Swizzle = [&Node, this](const FExpression& Value, const std::string_view Components) -> std::optional<FExpression>
	{
		for (const char Component : Components)
		{
			if (Swizzles.find(Component) >= Value.Width)
			{
				Error(Node.Id, {}, std::format("Channel '{}' is beyond a {}-component input", Component, Value.Width));
				return std::nullopt;
			}
		}

		const auto Width = static_cast<std::uint32_t>(Components.size());
		if (Value.Constant)
		{
			std::array<float, 4> Picked{};
			for (std::size_t Index = 0; Index < Components.size(); ++Index)
			{
				Picked[Index] = (*Value.Constant)[Swizzles.find(Components[Index])];
			}

			return MakeConstant(Picked, Width);
		}

		return FExpression{.Code = Value.Code + "." + std::string(Components), .Width = Width};
	};

	const auto Channels = [](const std::string_view Pin) -> std::string_view
	{
		return Pin == "R" ? "x" : Pin == "G" ? "y" : Pin == "B" ? "z" : Pin == "A" ? "w" : Pin == "RG" ? "xy" : Pin == "RGB" ? "xyz" : "xyzw";
	};

	if (Type == "Constant")
	{
		const float Value = std::get<float>(Property("value"));
		return MakeConstant({Value, Value, Value, Value}, 1);
	}

	if (Type == "Vector")
	{
		const FExpression Value = MakeConstant(std::get<std::array<float, 4>>(Property("value")), 4);
		return Swizzle(Value, Channels(Key));
	}

	if (Type == "TexCoord")
	{
		return FExpression{.Code = "Input.UV", .Width = 2};
	}

	if (Type == "WorldPosition")
	{
		return FExpression{.Code = "Input.WorldPosition", .Width = 3};
	}

	if (Type == "VertexNormal")
	{
		return FExpression{.Code = "normalize(Input.Normal)", .Width = 3};
	}

	if (Type == "TextureSample")
	{
		const std::string& Slot = std::get<std::string>(Property("slot"));
		const auto Found = std::ranges::find(TextureSlots, Slot);
		if (Found == TextureSlots.end())
		{
			Error(Node.Id, {}, std::format("'{}' is not a material texture slot", Slot));
			return std::nullopt;
		}

		const bool bConnected = Sources.contains({.Node = Node.Id, .Key = "UV"});
		std::optional<FExpression> UV = bConnected ? Input(Node, "UV") : FExpression{.Code = "Input.UV", .Width = 2};
		UV = UV ? Fit(Node, "UV", *UV, 2) : std::nullopt;
		if (!UV)
		{
			return std::nullopt;
		}

		const FExpression Sample = Bind({.Code = std::format("{}.Sample(LinearSampler, {})", TextureNames[static_cast<std::size_t>(Found - TextureSlots.begin())], UV->Code), .Width = 4});
		return Swizzle(Sample, Channels(Key));
	}

	if (Type == "Split")
	{
		const auto In = Input(Node, "In");
		return In ? Swizzle(*In, Channels(Key)) : std::nullopt;
	}

	if (Type == "Combine")
	{
		// Channels left open are zero.
		constexpr std::array<std::string_view, 4> Pins{"R", "G", "B", "A"};
		const std::size_t Count = Channels(Key).size();
		std::vector<FExpression> Parts;
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			const std::string_view Pin = Pins[Index];
			auto Part = Input(Node, Pin, 0.f);
			if (!Part || !(Part = Fit(Node, Pin, *Part, 1)))
			{
				return std::nullopt;
			}

			Parts.push_back(std::move(*Part));
		}

		const auto Width = static_cast<std::uint32_t>(Parts.size());
		if (std::ranges::all_of(Parts, [](const FExpression& Part)
		    {
			    return Part.Constant.has_value();
		    }))
		{
			std::array<float, 4> Value{};
			for (std::size_t Index = 0; Index < Parts.size(); ++Index)
			{
				Value[Index] = (*Parts[Index].Constant)[0];
			}

			return MakeConstant(Value, Width);
		}

		std::string Code = TypeName(Width) + "(";
		for (std::size_t Index = 0; Index < Parts.size(); ++Index)
		{
			Code += (Index == 0 ? "" : ", ") + Parts[Index].Code;
		}

		return FExpression{.Code = Code + ")", .Width = Width};
	}

	// Math nodes read their inputs in schema order; Clamp's bounds default to the unit range when left open.
	const FGraphNodeDescriptor* const Descriptor = Schema.FindNode(Type);
	std::vector<FExpression> Operands;
	for (const FGraphPinDescriptor& Pin : Descriptor->Inputs)
	{
		const std::optional<float> Default = Type == "Clamp" && Pin.Key == "Min" ? std::optional(0.f) : Type == "Clamp" && Pin.Key == "Max" ? std::optional(1.f) : std::nullopt;
		auto Operand = Input(Node, Pin.Key, Default);
		if (!Operand)
		{
			return std::nullopt;
		}

		Operands.push_back(std::move(*Operand));
	}

	using FFold = std::function<float(std::span<const float>)>;
	static const std::map<std::string_view, std::pair<std::string_view, FFold>> Operations{
	    {"Add", {"+", [](const std::span<const float> A) { return A[0] + A[1]; }}},
	    {"Subtract", {"-", [](const std::span<const float> A) { return A[0] - A[1]; }}},
	    {"Multiply", {"*", [](const std::span<const float> A) { return A[0] * A[1]; }}},
	    {"Divide", {"/", [](const std::span<const float> A) { return A[0] / A[1]; }}},
	    {"Min", {"min", [](const std::span<const float> A) { return std::min(A[0], A[1]); }}},
	    {"Max", {"max", [](const std::span<const float> A) { return std::max(A[0], A[1]); }}},
	    {"Power", {"pow", [](const std::span<const float> A) { return std::pow(A[0], A[1]); }}},
	    {"Lerp", {"lerp", [](const std::span<const float> A) { return A[0] + (A[1] - A[0]) * A[2]; }}},
	    {"Clamp", {"clamp", [](const std::span<const float> A) { return std::clamp(A[0], A[1], std::max(A[1], A[2])); }}},
	    {"Saturate", {"saturate", [](const std::span<const float> A) { return std::clamp(A[0], 0.f, 1.f); }}},
	    {"Abs", {"abs", [](const std::span<const float> A) { return std::abs(A[0]); }}},
	    {"Sqrt", {"sqrt", [](const std::span<const float> A) { return std::sqrt(A[0]); }}},
	    {"Frac", {"frac", [](const std::span<const float> A) { return A[0] - std::floor(A[0]); }}},
	    {"Floor", {"floor", [](const std::span<const float> A) { return std::floor(A[0]); }}},
	    {"Sin", {"sin", [](const std::span<const float> A) { return std::sin(A[0]); }}},
	    {"Cos", {"cos", [](const std::span<const float> A) { return std::cos(A[0]); }}},
	};

	if (const auto Found = Operations.find(Type); Found != Operations.end())
	{
		return Elementwise(Node, Operands, Found->second.first, Found->second.second);
	}

	if (Type == "OneMinus")
	{
		const FExpression One = MakeConstant({1.f, 1.f, 1.f, 1.f}, 1);
		const std::array Pair{One, Operands[0]};
		return Elementwise(Node, Pair, "-", [](const std::span<const float> A)
		{
			return A[0] - A[1];
		});
	}

	if (Type == "Dot")
	{
		if (Operands[0].Width != Operands[1].Width)
		{
			Error(Node.Id, "B", std::format("Dot needs equal widths, not {} and {}", Operands[0].Width, Operands[1].Width));
			return std::nullopt;
		}

		return FExpression{.Code = Operands[0].Width == 1 ? std::format("({} * {})", Operands[0].Code, Operands[1].Code) : std::format("dot({}, {})", Operands[0].Code, Operands[1].Code), .Width = 1};
	}

	if (Type == "Length")
	{
		return FExpression{.Code = Operands[0].Width == 1 ? std::format("abs({})", Operands[0].Code) : std::format("length({})", Operands[0].Code), .Width = 1};
	}

	if (Type == "Normalize")
	{
		if (Operands[0].Width == 1)
		{
			Error(Node.Id, "In", "Normalize needs a vector");
			return std::nullopt;
		}

		return FExpression{.Code = std::format("normalize({})", Operands[0].Code), .Width = Operands[0].Width};
	}

	Error(Node.Id, {}, std::format("The compiler does not know '{}' nodes", Type));
	return std::nullopt;
}

std::optional<FMaterialGraphCompilation> FCompiler::Compile()
{
	Diagnostics = ValidateGraph(Graph, Schema);
	if (std::ranges::any_of(Diagnostics, [](const FGraphDiagnostic& Diagnostic)
	    {
		    return Diagnostic.Severity == EGraphSeverity::Error;
	    }))
	{
		return std::nullopt;
	}

	const auto OutputNode = std::ranges::find(Graph.Nodes, MaterialGraphOutputType, &FGraphNode::Type);
	if (OutputNode == Graph.Nodes.end())
	{
		Error({}, {}, "The graph needs a Material Output node");
		return std::nullopt;
	}

	Reached.insert(OutputNode->Id.Value);
	struct FTarget
	{
		std::string_view Pin{};
		std::string_view Field{};
		std::uint32_t Width = 0;
	};

	constexpr std::array Targets{
	    FTarget{.Pin = "BaseColor", .Field = "BaseColor", .Width = 3},
	    FTarget{.Pin = "Alpha", .Field = "Alpha", .Width = 1},
	    FTarget{.Pin = "Metallic", .Field = "Metallic", .Width = 1},
	    FTarget{.Pin = "Roughness", .Field = "Roughness", .Width = 1},
	    FTarget{.Pin = "Normal", .Field = "Normal", .Width = 3},
	    FTarget{.Pin = "Emissive", .Field = "Emissive", .Width = 3},
	    FTarget{.Pin = "Occlusion", .Field = "Occlusion", .Width = 1},
	};

	std::string Assignments;
	for (const FTarget& Target : Targets)
	{
		if (!Sources.contains({.Node = OutputNode->Id, .Key = std::string(Target.Pin)}))
		{
			continue;
		}

		auto Value = Input(*OutputNode, Target.Pin);
		Value = Value ? Fit(*OutputNode, Target.Pin, *Value, Target.Width) : std::nullopt;
		if (Value)
		{
			// The graph writes tangent-space normals, as normal maps store them; shading needs world space.
			const std::string Code = Target.Pin == "Normal" ? std::format("GraphNormal(Input, {})", Value->Code) : Value->Code;
			Assignments += std::format("    Material.{} = {};\n", Target.Field, Code);
		}
	}

	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (!Reached.contains(Node.Id.Value))
		{
			Diagnostics.push_back({.Severity = EGraphSeverity::Warning, .Node = Node.Id, .Message = "Not connected to the Material Output"});
		}
	}

	if (std::ranges::any_of(Diagnostics, [](const FGraphDiagnostic& Diagnostic)
	    {
		    return Diagnostic.Severity == EGraphSeverity::Error;
	    }))
	{
		return std::nullopt;
	}

	FMaterialGraphCompilation Result;
	Result.Source = "// Generated by the Herta Material Graph compiler. Edit the graph, not this text.\n#include \"GraphMaterial.slangh\"\n\nFMaterialSample EvaluateMaterial(FVertexOutput Input)\n{\n"
	                "    FMaterialSample Material = DefaultMaterial(Input);\n";
	Result.Source += Body + Assignments + "    return Material;\n}\n";
	Result.Diagnostics = std::move(Diagnostics);
	return Result;
}
}

std::expected<FMaterialGraphCompilation, std::vector<FGraphDiagnostic>> CompileMaterialGraph(const FGraph& Graph)
{
	FCompiler Compiler(Graph);
	auto Result = Compiler.Compile();
	if (!Result)
	{
		return std::unexpected(std::move(Compiler.Diagnostics));
	}

	return std::move(*Result);
}
}
