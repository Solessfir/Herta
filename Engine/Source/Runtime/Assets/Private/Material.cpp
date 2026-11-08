#include "Herta/Assets/Material.h"

#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <span>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumMaterialBytes = 1024 * 1024;
constexpr std::array<std::string_view, MaterialTextureSlotCount> SlotNames{"baseColor", "metallic", "roughness", "normal", "occlusion", "emissive"};
constexpr std::array<std::string_view, 5> ChannelNames{"r", "g", "b", "a", "rgb"};

std::unexpected<FAssetError> Invalid(const std::string_view Message)
{
	return std::unexpected(FAssetError{std::string(Message)});
}

template <std::size_t N> std::expected<std::array<simdjson::dom::element, N>, FAssetError> Fields(const simdjson::dom::element Element, const std::array<std::string_view, N>& Names)
{
	simdjson::dom::object Object;
	if (Element.get_object().get(Object))
	{
		return Invalid("Expected material JSON object");
	}

	std::array<simdjson::dom::element, N> Values;
	std::uint64_t Seen = 0;

	for (const auto Field : Object)
	{
		const auto Iterator = std::ranges::find(Names, Field.key);
		if (Iterator == Names.end())
		{
			return Invalid("Unknown material JSON field");
		}

		const auto Index = static_cast<std::size_t>(Iterator - Names.begin());
		const std::uint64_t Bit = std::uint64_t{1} << Index;
		if ((Seen & Bit) != 0)
		{
			return Invalid("Duplicate material JSON field");
		}

		Seen |= Bit;
		Values[Index] = Field.value;
	}

	if (Seen != (std::uint64_t{1} << N) - 1)
	{
		return Invalid("Missing required material JSON field");
	}

	return Values;
}

bool ReadFloat(const simdjson::dom::element Element, float& Target)
{
	double Number = 0;
	if (Element.get_double().get(Number) || !std::isfinite(Number) || std::abs(Number) > std::numeric_limits<float>::max())
	{
		return false;
	}

	Target = static_cast<float>(Number);
	return true;
}

template <std::size_t N> bool ReadFloats(const simdjson::dom::element Element, std::array<float, N>& Target)
{
	simdjson::dom::array Values;
	if (Element.get_array().get(Values) || Values.size() != N)
	{
		return false;
	}

	std::size_t Index = 0;

	for (const auto Value : Values)
	{
		if (!ReadFloat(Value, Target[Index++]))
		{
			return false;
		}
	}

	return true;
}

void AppendString(std::string& Text, const std::string_view Value)
{
	Text += '"';

	for (const char Character : Value)
	{
		if (Character == '"' || Character == '\\')
		{
			Text += '\\';
		}

		Text += Character;
	}

	Text += '"';
}

void AppendFloat(std::string& Text, const float Value)
{
	std::array<char, 64> Buffer{};
	const auto Result = std::to_chars(Buffer.data(), Buffer.data() + Buffer.size(), Value == 0.f ? 0.f : Value);
	Text.append(Buffer.data(), Result.ptr);
}

template <std::size_t N> void AppendFloats(std::string& Text, const std::array<float, N>& Values)
{
	Text += '[';

	for (std::size_t Index = 0; Index < N; ++Index)
	{
		if (Index != 0)
		{
			Text += ", ";
		}

		AppendFloat(Text, Values[Index]);
	}

	Text += ']';
}

bool InRange(const float Value, const float Minimum, const float Maximum)
{
	return std::isfinite(Value) && Value >= Minimum && Value <= Maximum;
}
}

bool IsValidMaterialParameterName(const std::string_view Name)
{
	const auto IsLetter = [](const char Character)
	{
		return (Character >= 'A' && Character <= 'Z') || (Character >= 'a' && Character <= 'z');
	};

	return !Name.empty() && Name.size() <= 32 && IsLetter(Name.front()) && std::ranges::all_of(Name, [&IsLetter](const char Character)
	{
		return IsLetter(Character) || (Character >= '0' && Character <= '9') || Character == '_';
	});
}

std::string SerializeMaterialShaderParameters(const std::span<const FMaterialShaderParameter> Parameters)
{
	std::string Text;
	for (const FMaterialShaderParameter& Parameter : Parameters)
	{
		Text += std::format("{} {}", Parameter.Name, Parameter.bVector ? "vector" : "scalar");
		for (const float Component : Parameter.Default)
		{
			Text += ' ';
			AppendFloat(Text, Component);
		}

		Text += '\n';
	}

	return Text;
}

std::expected<std::vector<FMaterialShaderParameter>, FAssetError> DeserializeMaterialShaderParameters(std::string_view Text)
{
	std::vector<FMaterialShaderParameter> Parameters;
	while (!Text.empty())
	{
		const std::size_t End = Text.find('\n');
		const std::string_view Line = Text.substr(0, End);
		Text = End == std::string_view::npos ? std::string_view{} : Text.substr(End + 1);
		std::array<std::string_view, 6> Fields{};
		std::size_t Count = 0;
		for (std::size_t Start = 0; Start <= Line.size() && Count < Fields.size() + 1;)
		{
			const std::size_t Space = std::min(Line.find(' ', Start), Line.size());
			if (Count == Fields.size())
			{
				++Count;
				break;
			}

			Fields[Count++] = Line.substr(Start, Space - Start);
			Start = Space + 1;
		}

		FMaterialShaderParameter Parameter{.Name = std::string(Fields[0]), .bVector = Fields[1] == "vector"};
		bool bValid = Count == Fields.size() && (Fields[1] == "vector" || Fields[1] == "scalar") && IsValidMaterialParameterName(Parameter.Name)
		              && (Parameters.empty() || Parameters.back().Name < Parameter.Name) && Parameters.size() < MaterialParameterSlotCount;
		for (std::size_t Component = 0; bValid && Component < 4; ++Component)
		{
			const std::string_view Number = Fields[2 + Component];
			const auto [Next, Error] = std::from_chars(Number.data(), Number.data() + Number.size(), Parameter.Default[Component]);
			bValid = Error == std::errc{} && Next == Number.data() + Number.size() && std::isfinite(Parameter.Default[Component]);
		}

		if (!bValid)
		{
			return Invalid("Invalid material shader parameter table");
		}

		Parameters.push_back(std::move(Parameter));
	}

	return Parameters;
}

void ResolveMaterialParameters(const std::span<const FMaterialShaderParameter> Parameters, const std::span<const FMaterialParameterValue> Values, FMaterialParameterSlots& Slots)
{
	auto Value = Values.begin();
	for (std::size_t Slot = 0; Slot < Parameters.size() && Slot < Slots.size(); ++Slot)
	{
		const std::string& Name = Parameters[Slot].Name;
		while (Value != Values.end() && Value->Name < Name)
		{
			++Value;
		}

		Slots[Slot] = Value != Values.end() && Value->Name == Name ? Value->Value : Parameters[Slot].Default;
	}
}

std::string GetMountedMaterialShaderPath(const FMaterialAsset& Material, const std::string_view Name)
{
	if (Material.ShaderPath.empty() || Material.ShaderPath.starts_with("Engine/") || Material.ShaderPath.starts_with("Game/"))
	{
		return Material.ShaderPath;
	}

	return std::string(Name.starts_with("Engine/") ? "Engine/" : "Game/") + Material.ShaderPath;
}

std::expected<void, FAssetError> ValidateMaterialParameters(const FMaterialParameters& Parameters)
{
	if (!std::ranges::all_of(Parameters.BaseColor, [](const float Value)
	{
		return InRange(Value, 0.f, 1.f);
	}) || !InRange(Parameters.Metallic, 0.f, 1.f)
	    || !InRange(Parameters.Roughness, 0.f, 1.f) || !InRange(Parameters.DiffuseRoughness, 0.f, 1.f) || !std::ranges::all_of(Parameters.SpecularColor, [](const float Value)
	{
		return InRange(Value, 0.f, 1.f);
	}) || !InRange(Parameters.NormalStrength, 0.f, 8.f)
	    || !InRange(Parameters.OcclusionStrength, 0.f, 1.f) || !std::ranges::all_of(Parameters.Emissive, [](const float Value)
	{
		return InRange(Value, 0.f, 1'000'000.f);
	}) || !InRange(Parameters.EmissiveIntensity, 0.f, 1'000'000.f)
	    || !std::ranges::all_of(Parameters.UVScale, [](const float Value)
	{
		return InRange(Value, -10'000.f, 10'000.f);
	}) || !std::ranges::all_of(Parameters.UVOffset, [](const float Value)
	{
		return InRange(Value, -10'000.f, 10'000.f);
	}) || !InRange(Parameters.AlphaCutoff, 0.f, 1.f)
	    || (Parameters.BlendMode != EMaterialBlendMode::Opaque && Parameters.BlendMode != EMaterialBlendMode::Masked))
	{
		return Invalid("Material parameters are nonfinite or outside their supported ranges");
	}

	return {};
}

std::expected<void, FAssetError> ValidateMaterial(const FMaterialAsset& Material)
{
	if (Material.Name.empty() || Material.Name.size() > 256 || !simdjson::validate_utf8(Material.Name.data(), Material.Name.size())
	    || std::ranges::any_of(Material.Name, [](const unsigned char Byte)
	{
		return Byte < 0x20 || Byte == 0x7f;
	}))
	{
		return Invalid("Material name must be valid UTF-8 without control characters and at most 256 bytes");
	}

	// A Material Graph compiles to the same custom shader contract as hand-written Slang.
	if (!Material.ShaderPath.empty() && (!IsValidAssetPath(Material.ShaderPath) || !(Material.ShaderPath.ends_with(".slang") || Material.ShaderPath.ends_with(".hmatgraph"))))
	{
		return Invalid("Material shader must be a portable content-relative .slang or .hmatgraph path");
	}

	if (auto Valid = ValidateMaterialParameters(Material.Parameters); !Valid)
	{
		return Valid;
	}

	if (Material.ParameterValues.size() > MaximumMaterialParameterValues)
	{
		return Invalid("Material has more than 32 graph parameter values");
	}

	for (std::size_t Index = 0; Index < Material.ParameterValues.size(); ++Index)
	{
		const FMaterialParameterValue& Value = Material.ParameterValues[Index];
		if (!IsValidMaterialParameterName(Value.Name) || (Index > 0 && Material.ParameterValues[Index - 1].Name >= Value.Name) || !std::ranges::all_of(Value.Value, [](const float Component)
		    {
			    return std::isfinite(Component);
		    }))
		{
			return Invalid("Material graph parameter values need valid unique names in sorted order and finite values");
		}
	}

	for (std::size_t Slot = 0; Slot < Material.Textures.size(); ++Slot)
	{
		const auto& Binding = Material.Textures[Slot];
		if (Binding.Channel > EMaterialChannel::Rgb || Binding.ColorSpace > ETextureColorSpace::Srgb)
		{
			return Invalid("Material texture has an unknown channel or color space");
		}

		const bool bColor = Slot == static_cast<std::size_t>(EMaterialTextureSlot::BaseColor) || Slot == static_cast<std::size_t>(EMaterialTextureSlot::Emissive);
		const bool bNormal = Slot == static_cast<std::size_t>(EMaterialTextureSlot::Normal);
		if ((!bColor && Binding.ColorSpace != ETextureColorSpace::Linear) || ((bColor || bNormal) && Binding.Channel != EMaterialChannel::Rgb) || (!bColor && !bNormal && Binding.Channel == EMaterialChannel::Rgb))
		{
			return Invalid("Color and normal maps require RGB; scalar maps require an explicit linear channel");
		}
	}

	return {};
}

std::expected<std::string, FAssetError> SerializeMaterial(const FMaterialAsset& Material)
{
	if (auto Valid = ValidateMaterial(Material); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	std::string Text = std::format("{{\n  \"format\": \"HertaMaterial\",\n  \"version\": {},\n  \"name\": ", MaterialAssetVersion);
	AppendString(Text, Material.Name);
	Text += ",\n  \"parameters\": {\n    \"baseColor\": ";
	AppendFloats(Text, Material.Parameters.BaseColor);
	const auto Scalar = [&Text](const std::string_view Key, const float Value)
	{
		Text += ",\n    \"";
		Text += Key;
		Text += "\": ";
		AppendFloat(Text, Value);
	};

	Scalar("metallic", Material.Parameters.Metallic);
	Scalar("roughness", Material.Parameters.Roughness);
	Scalar("diffuseRoughness", Material.Parameters.DiffuseRoughness);
	Text += ",\n    \"specularColor\": ";
	AppendFloats(Text, Material.Parameters.SpecularColor);
	Scalar("normalStrength", Material.Parameters.NormalStrength);
	Scalar("occlusionStrength", Material.Parameters.OcclusionStrength);
	Text += ",\n    \"emissive\": ";
	AppendFloats(Text, Material.Parameters.Emissive);
	Scalar("emissiveIntensity", Material.Parameters.EmissiveIntensity);
	Text += ",\n    \"uvScale\": ";
	AppendFloats(Text, Material.Parameters.UVScale);
	Text += ",\n    \"uvOffset\": ";
	AppendFloats(Text, Material.Parameters.UVOffset);
	Text += ",\n    \"blendMode\": ";
	AppendString(Text, Material.Parameters.BlendMode == EMaterialBlendMode::Masked ? "masked" : "opaque");
	Scalar("alphaCutoff", Material.Parameters.AlphaCutoff);
	Text += "\n  },\n  \"textures\": {\n";

	for (std::size_t Slot = 0; Slot < SlotNames.size(); ++Slot)
	{
		const auto& Binding = Material.Textures[Slot];
		Text += "    \"";
		Text += SlotNames[Slot];
		Text += "\": {\"asset\": ";
		AppendString(Text, Binding.Texture.IsValid() ? Binding.Texture.ToString() : "");
		Text += ", \"channel\": ";
		AppendString(Text, ChannelNames[static_cast<std::size_t>(Binding.Channel)]);
		Text += ", \"colorSpace\": ";
		AppendString(Text, Binding.ColorSpace == ETextureColorSpace::Srgb ? "srgb" : "linear");
		Text += Slot + 1 == SlotNames.size() ? "}\n" : "},\n";
	}

	Text += "  },\n  \"shader\": ";
	AppendString(Text, Material.ShaderPath);
	Text += ",\n  \"graphParameters\": {";
	for (std::size_t Index = 0; Index < Material.ParameterValues.size(); ++Index)
	{
		Text += Index == 0 ? "\n    " : ",\n    ";
		AppendString(Text, Material.ParameterValues[Index].Name);
		Text += ": ";
		AppendFloats(Text, Material.ParameterValues[Index].Value);
	}

	Text += Material.ParameterValues.empty() ? "}\n}\n" : "\n  }\n}\n";
	return Text;
}

std::expected<FMaterialAsset, FAssetError> DeserializeMaterial(const std::string_view Text)
{
	if (Text.empty() || Text.size() > MaximumMaterialBytes)
	{
		return Invalid("Material JSON is empty or exceeds 1 MiB");
	}

	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	if (Parser.parse(Text.data(), Text.size()).get(Root))
	{
		return Invalid("Invalid material JSON");
	}

	// Version 3 adds the graph parameter values; earlier versions have exactly the first six fields.
	std::array<simdjson::dom::element, 7> HeaderFields;
	constexpr std::array<std::string_view, 7> HeaderNames{"format", "version", "name", "parameters", "textures", "shader", "graphParameters"};
	bool bGraphParameters = false;
	if (const auto Current = Fields(Root, HeaderNames))
	{
		HeaderFields = *Current;
		bGraphParameters = true;
	}
	else if (const auto Earlier = Fields(Root, std::array<std::string_view, 6>{"format", "version", "name", "parameters", "textures", "shader"}))
	{
		std::ranges::copy(*Earlier, HeaderFields.begin());
	}
	else
	{
		return std::unexpected(Earlier.error());
	}

	const auto* const Header = &HeaderFields;

	std::string_view Format, Name, Shader;
	std::uint64_t Version = 0;
	if ((*Header)[0].get_string().get(Format) || Format != "HertaMaterial" || (*Header)[1].get_uint64().get(Version) || Version < 1 || Version > MaterialAssetVersion
	    || (*Header)[2].get_string().get(Name) || (*Header)[5].get_string().get(Shader) || (Version >= 3) != bGraphParameters)
	{
		return Invalid("Invalid material header or unsupported version");
	}

	FMaterialAsset Material;
	Material.Name = Name;
	Material.ShaderPath = Shader;
	constexpr std::array<std::string_view, 11> VersionOneFields{"baseColor", "metallic", "roughness", "normalStrength", "occlusionStrength", "emissive", "emissiveIntensity", "uvScale", "uvOffset", "blendMode", "alphaCutoff"};
	constexpr std::array<std::string_view, 13> FieldNames{"baseColor", "metallic", "roughness", "normalStrength", "occlusionStrength", "emissive", "emissiveIntensity", "uvScale", "uvOffset", "blendMode", "alphaCutoff", "diffuseRoughness", "specularColor"};
	std::array<simdjson::dom::element, 13> Values;
	if (Version == 1)
	{
		const auto First = Fields((*Header)[3], VersionOneFields);
		if (!First)
		{
			return std::unexpected(First.error());
		}

		std::ranges::copy(*First, Values.begin());
	}
	else if (const auto All = Fields((*Header)[3], FieldNames); All)
	{
		Values = *All;
	}
	else
	{
		return std::unexpected(All.error());
	}

	auto& Parameters = Material.Parameters;
	if (Version != 1 && (!ReadFloat(Values[11], Parameters.DiffuseRoughness) || !ReadFloats(Values[12], Parameters.SpecularColor)))
	{
		return Invalid("Invalid material parameter value");
	}

	std::string_view Blend;
	if (!ReadFloats(Values[0], Parameters.BaseColor) || !ReadFloat(Values[1], Parameters.Metallic) || !ReadFloat(Values[2], Parameters.Roughness)
	    || !ReadFloat(Values[3], Parameters.NormalStrength) || !ReadFloat(Values[4], Parameters.OcclusionStrength)
	    || !ReadFloats(Values[5], Parameters.Emissive) || !ReadFloat(Values[6], Parameters.EmissiveIntensity)
	    || !ReadFloats(Values[7], Parameters.UVScale) || !ReadFloats(Values[8], Parameters.UVOffset)
	    || Values[9].get_string().get(Blend) || (Blend != "opaque" && Blend != "masked") || !ReadFloat(Values[10], Parameters.AlphaCutoff))
	{
		return Invalid("Invalid material parameter value");
	}

	Parameters.BlendMode = Blend == "masked" ? EMaterialBlendMode::Masked : EMaterialBlendMode::Opaque;
	const auto Bindings = Fields((*Header)[4], SlotNames);
	if (!Bindings)
	{
		return std::unexpected(Bindings.error());
	}

	for (std::size_t Slot = 0; Slot < SlotNames.size(); ++Slot)
	{
		const auto Binding = Fields((*Bindings)[Slot], std::array<std::string_view, 3>{"asset", "channel", "colorSpace"});
		std::string_view Asset, Channel, ColorSpace;
		if (!Binding || (*Binding)[0].get_string().get(Asset) || (*Binding)[1].get_string().get(Channel) || (*Binding)[2].get_string().get(ColorSpace))
		{
			return Invalid("Invalid material texture binding");
		}

		auto& Target = Material.Textures[Slot];
		if (!Asset.empty())
		{
			const auto Id = FAssetId::Parse(Asset);
			if (!Id || !Id->IsValid())
			{
				return Invalid("Invalid material texture asset ID");
			}

			Target.Texture = *Id;
		}

		const auto ChannelIndex = std::ranges::find(ChannelNames, Channel);
		if (ChannelIndex == ChannelNames.end() || (ColorSpace != "srgb" && ColorSpace != "linear"))
		{
			return Invalid("Unknown material texture channel or color space");
		}

		Target.Channel = static_cast<EMaterialChannel>(ChannelIndex - ChannelNames.begin());
		Target.ColorSpace = ColorSpace == "srgb" ? ETextureColorSpace::Srgb : ETextureColorSpace::Linear;
	}

	if (Version >= 3)
	{
		simdjson::dom::object GraphValues;
		if ((*Header)[6].get_object().get(GraphValues) || GraphValues.size() > MaximumMaterialParameterValues)
		{
			return Invalid("Invalid material graph parameter values");
		}

		for (const auto Field : GraphValues)
		{
			FMaterialParameterValue Value{.Name = std::string(Field.key)};
			if (!ReadFloats(Field.value, Value.Value))
			{
				return Invalid("Invalid material graph parameter value");
			}

			Material.ParameterValues.push_back(std::move(Value));
		}
	}

	if (auto Valid = ValidateMaterial(Material); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Material;
}
}
