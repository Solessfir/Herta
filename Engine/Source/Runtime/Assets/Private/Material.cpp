#include "Herta/Assets/Material.h"

#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
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

std::expected<void, FAssetError> ValidateMaterialParameters(const FMaterialParameters& Parameters)
{
	if (!std::ranges::all_of(Parameters.BaseColor, [](const float Value)
	{
		return InRange(Value, 0.f, 1.f);
	}) || !InRange(Parameters.Metallic, 0.f, 1.f)
	    || !InRange(Parameters.Roughness, 0.f, 1.f) || !InRange(Parameters.NormalStrength, 0.f, 8.f) || !InRange(Parameters.OcclusionStrength, 0.f, 1.f) || !std::ranges::all_of(Parameters.Emissive, [](const float Value)
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

	if (!Material.ShaderPath.empty() && (!IsValidAssetPath(Material.ShaderPath) || !Material.ShaderPath.ends_with(".slang")))
	{
		return Invalid("Material shader must be a portable content-relative .slang path");
	}

	if (auto Valid = ValidateMaterialParameters(Material.Parameters); !Valid)
	{
		return Valid;
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

	std::string Text = "{\n  \"format\": \"HertaMaterial\",\n  \"version\": 1,\n  \"name\": ";
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
	Text += "\n}\n";
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

	const auto Header = Fields(Root, std::array<std::string_view, 6>{"format", "version", "name", "parameters", "textures", "shader"});
	if (!Header)
	{
		return std::unexpected(Header.error());
	}

	std::string_view Format, Name, Shader;
	std::uint64_t Version = 0;
	if ((*Header)[0].get_string().get(Format) || Format != "HertaMaterial" || (*Header)[1].get_uint64().get(Version) || Version != MaterialAssetVersion
	    || (*Header)[2].get_string().get(Name) || (*Header)[5].get_string().get(Shader))
	{
		return Invalid("Invalid material header or unsupported version");
	}

	FMaterialAsset Material;
	Material.Name = Name;
	Material.ShaderPath = Shader;
	const auto Values = Fields((*Header)[3], std::array<std::string_view, 11>{"baseColor", "metallic", "roughness", "normalStrength", "occlusionStrength", "emissive", "emissiveIntensity", "uvScale", "uvOffset", "blendMode", "alphaCutoff"});
	if (!Values)
	{
		return std::unexpected(Values.error());
	}

	auto& Parameters = Material.Parameters;
	std::string_view Blend;
	if (!ReadFloats((*Values)[0], Parameters.BaseColor) || !ReadFloat((*Values)[1], Parameters.Metallic) || !ReadFloat((*Values)[2], Parameters.Roughness)
	    || !ReadFloat((*Values)[3], Parameters.NormalStrength) || !ReadFloat((*Values)[4], Parameters.OcclusionStrength)
	    || !ReadFloats((*Values)[5], Parameters.Emissive) || !ReadFloat((*Values)[6], Parameters.EmissiveIntensity)
	    || !ReadFloats((*Values)[7], Parameters.UVScale) || !ReadFloats((*Values)[8], Parameters.UVOffset)
	    || (*Values)[9].get_string().get(Blend) || (Blend != "opaque" && Blend != "masked") || !ReadFloat((*Values)[10], Parameters.AlphaCutoff))
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

	if (auto Valid = ValidateMaterial(Material); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Material;
}
}
