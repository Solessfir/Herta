#include "Herta/MaterialGraph/MaterialGraph.h"

#include <array>

namespace Herta
{
namespace
{
constexpr std::uint32_t Pin(const EMaterialPinType Type)
{
	return static_cast<std::uint32_t>(Type);
}

constexpr std::uint32_t Float = Pin(EMaterialPinType::Float);
constexpr std::uint32_t Float2 = Pin(EMaterialPinType::Float2);
constexpr std::uint32_t Float3 = Pin(EMaterialPinType::Float3);
constexpr std::uint32_t Float4 = Pin(EMaterialPinType::Float4);
constexpr std::uint32_t Dynamic = Pin(EMaterialPinType::Dynamic);

constexpr std::array ValueOutput{FGraphPinDescriptor{.Key = "Value", .Name = "Value", .Type = Float}};
constexpr std::array VectorOutputs{
    FGraphPinDescriptor{.Key = "RGB", .Name = "RGB", .Type = Float3},
    FGraphPinDescriptor{.Key = "RGBA", .Name = "RGBA", .Type = Float4},
};
constexpr std::array UVOutput{FGraphPinDescriptor{.Key = "UV", .Name = "UV", .Type = Float2}};
constexpr std::array PositionOutput{FGraphPinDescriptor{.Key = "Position", .Name = "Position", .Type = Float3}};
constexpr std::array NormalOutput{FGraphPinDescriptor{.Key = "Normal", .Name = "Normal", .Type = Float3}};

constexpr std::array TextureInputs{FGraphPinDescriptor{.Key = "UV", .Name = "UV", .Type = Float2}};
constexpr std::array TextureOutputs{
    FGraphPinDescriptor{.Key = "RGB", .Name = "RGB", .Type = Float3},
    FGraphPinDescriptor{.Key = "R", .Name = "R", .Type = Float},
    FGraphPinDescriptor{.Key = "G", .Name = "G", .Type = Float},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Float},
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Float},
    FGraphPinDescriptor{.Key = "RGBA", .Name = "RGBA", .Type = Float4},
};

constexpr std::array ResultOutput{FGraphPinDescriptor{.Key = "Result", .Name = "Result", .Type = Dynamic}};
constexpr std::array ScalarResultOutput{FGraphPinDescriptor{.Key = "Result", .Name = "Result", .Type = Float}};
constexpr std::array UnaryInputs{FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = Dynamic}};
constexpr std::array BinaryInputs{
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Dynamic},
};
constexpr std::array PowerInputs{
    FGraphPinDescriptor{.Key = "Base", .Name = "Base", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "Exponent", .Name = "Exponent", .Type = Dynamic},
};
constexpr std::array LerpInputs{
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "Alpha", .Name = "Alpha", .Type = Dynamic},
};
constexpr std::array ClampInputs{
    FGraphPinDescriptor{.Key = "In", .Name = "In", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "Min", .Name = "Min", .Type = Dynamic},
    FGraphPinDescriptor{.Key = "Max", .Name = "Max", .Type = Dynamic},
};

constexpr std::array SplitOutputs{
    FGraphPinDescriptor{.Key = "R", .Name = "R", .Type = Float},
    FGraphPinDescriptor{.Key = "G", .Name = "G", .Type = Float},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Float},
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Float},
};
constexpr std::array CombineInputs{
    FGraphPinDescriptor{.Key = "R", .Name = "R", .Type = Float},
    FGraphPinDescriptor{.Key = "G", .Name = "G", .Type = Float},
    FGraphPinDescriptor{.Key = "B", .Name = "B", .Type = Float},
    FGraphPinDescriptor{.Key = "A", .Name = "A", .Type = Float},
};
constexpr std::array CombineOutputs{
    FGraphPinDescriptor{.Key = "RG", .Name = "RG", .Type = Float2},
    FGraphPinDescriptor{.Key = "RGB", .Name = "RGB", .Type = Float3},
    FGraphPinDescriptor{.Key = "RGBA", .Name = "RGBA", .Type = Float4},
};

constexpr std::array OutputInputs{
    FGraphPinDescriptor{.Key = "BaseColor", .Name = "Base Color", .Type = Float3},
    FGraphPinDescriptor{.Key = "Alpha", .Name = "Alpha", .Type = Float},
    FGraphPinDescriptor{.Key = "Metallic", .Name = "Metallic", .Type = Float},
    FGraphPinDescriptor{.Key = "Roughness", .Name = "Roughness", .Type = Float},
    FGraphPinDescriptor{.Key = "Normal", .Name = "Normal", .Type = Float3},
    FGraphPinDescriptor{.Key = "Emissive", .Name = "Emissive", .Type = Float3},
    FGraphPinDescriptor{.Key = "Occlusion", .Name = "Occlusion", .Type = Float},
};

const std::array ConstantProperties{FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = 1.f}};
const std::array VectorProperties{FGraphPropertyDescriptor{.Key = "value", .Name = "Value", .Default = std::array<float, 4>{1.f, 1.f, 1.f, 1.f}}};
const std::array TextureProperties{FGraphPropertyDescriptor{.Key = "slot", .Name = "Texture Slot", .Default = std::string("baseColor")}};

constexpr FGraphNodeDescriptor Unary(const std::string_view Type, const std::string_view Name)
{
	return {.Type = Type, .Name = Name, .Category = "Math", .Inputs = UnaryInputs, .Outputs = ResultOutput};
}

constexpr FGraphNodeDescriptor Binary(const std::string_view Type, const std::string_view Name)
{
	return {.Type = Type, .Name = Name, .Category = "Math", .Inputs = BinaryInputs, .Outputs = ResultOutput};
}

const std::array MaterialNodes{
    FGraphNodeDescriptor{.Type = "Constant", .Name = "Constant", .Category = "Values", .Outputs = ValueOutput, .Properties = ConstantProperties},
    FGraphNodeDescriptor{.Type = "Vector", .Name = "Vector", .Category = "Values", .Outputs = VectorOutputs, .Properties = VectorProperties},
    FGraphNodeDescriptor{.Type = "TexCoord", .Name = "Texture Coordinate", .Category = "Inputs", .Outputs = UVOutput},
    FGraphNodeDescriptor{.Type = "WorldPosition", .Name = "World Position", .Category = "Inputs", .Outputs = PositionOutput},
    FGraphNodeDescriptor{.Type = "VertexNormal", .Name = "Vertex Normal", .Category = "Inputs", .Outputs = NormalOutput},
    FGraphNodeDescriptor{.Type = "TextureSample", .Name = "Texture Sample", .Category = "Textures", .Inputs = TextureInputs, .Outputs = TextureOutputs, .Properties = TextureProperties},
    Binary("Add", "Add"),
    Binary("Subtract", "Subtract"),
    Binary("Multiply", "Multiply"),
    Binary("Divide", "Divide"),
    Binary("Min", "Min"),
    Binary("Max", "Max"),
    FGraphNodeDescriptor{.Type = "Dot", .Name = "Dot", .Category = "Math", .Inputs = BinaryInputs, .Outputs = ScalarResultOutput},
    FGraphNodeDescriptor{.Type = "Power", .Name = "Power", .Category = "Math", .Inputs = PowerInputs, .Outputs = ResultOutput},
    FGraphNodeDescriptor{.Type = "Lerp", .Name = "Lerp", .Category = "Math", .Inputs = LerpInputs, .Outputs = ResultOutput},
    FGraphNodeDescriptor{.Type = "Clamp", .Name = "Clamp", .Category = "Math", .Inputs = ClampInputs, .Outputs = ResultOutput},
    Unary("Saturate", "Saturate"),
    Unary("OneMinus", "One Minus"),
    Unary("Abs", "Abs"),
    Unary("Sqrt", "Square Root"),
    Unary("Frac", "Frac"),
    Unary("Floor", "Floor"),
    Unary("Sin", "Sine"),
    Unary("Cos", "Cosine"),
    Unary("Normalize", "Normalize"),
    FGraphNodeDescriptor{.Type = "Length", .Name = "Length", .Category = "Math", .Inputs = UnaryInputs, .Outputs = ScalarResultOutput},
    FGraphNodeDescriptor{.Type = "Split", .Name = "Split", .Category = "Channels", .Inputs = UnaryInputs, .Outputs = SplitOutputs},
    FGraphNodeDescriptor{.Type = "Combine", .Name = "Combine", .Category = "Channels", .Inputs = CombineInputs, .Outputs = CombineOutputs},
    FGraphNodeDescriptor{.Type = MaterialGraphOutputType, .Name = "Material Output", .Category = "Output", .Inputs = OutputInputs, .MaximumCount = 1},
};

// Equal widths connect, a scalar widens to any vector, and dynamic pins take anything; the compiler checks dynamic widths per node.
bool CanConnectMaterialPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To || To == Dynamic || From == Dynamic || From == Float;
}

const FGraphSchema MaterialSchema{.Domain = MaterialGraphDomain, .Version = MaterialGraphVersion, .Nodes = MaterialNodes, .CanConnect = CanConnectMaterialPins};
}

const FGraphSchema& GetMaterialGraphSchema()
{
	return MaterialSchema;
}
}
