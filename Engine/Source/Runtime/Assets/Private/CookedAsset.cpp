#include "Herta/Assets/CookedAsset.h"

#include "Herta/Core/BinaryStream.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace Herta
{
namespace
{
inline constexpr std::uint32_t CookedAssetMagic = 0x53414348; // "HCAS"
inline constexpr std::uint8_t TextureTag = 1;
inline constexpr std::uint8_t ModelTag = 2;
inline constexpr std::uint8_t MaterialTag = 3;

inline constexpr std::size_t MaximumVertices = MaximumCookedBufferBytes / sizeof(FCookedVertex);
inline constexpr std::size_t MaximumIndices = MaximumCookedBufferBytes / sizeof(std::uint32_t);
inline constexpr std::size_t MaximumMips = 13;
inline constexpr std::size_t MaximumSections = 4096;
inline constexpr std::size_t MaximumMaterials = 256;
inline constexpr std::size_t MaximumTextures = 256;
inline constexpr std::size_t MaximumNameLength = 256;

[[nodiscard]] std::unexpected<FAssetError> Invalid(const std::string_view Message)
{
	return std::unexpected(FAssetError{std::string(Message)});
}

void WriteTexture(FBinaryWriter& Writer, const FCookedTexture& Texture)
{
	Writer.Write(static_cast<std::uint8_t>(Texture.ColorSpace));
	Writer.Write(static_cast<std::uint8_t>(Texture.PixelFormat));
	Writer.Write(static_cast<std::uint32_t>(Texture.Mips.size()));

	for (const FCookedTextureMip& Mip : Texture.Mips)
	{
		Writer.Write(Mip.Width);
		Writer.Write(Mip.Height);
		Writer.WriteBytes(Mip.Pixels);
	}
}

[[nodiscard]] FCookedTexture ReadTexture(FBinaryReader& Reader)
{
	FCookedTexture Texture;
	Texture.ColorSpace = static_cast<ETextureColorSpace>(Reader.Read<std::uint8_t>());
	Texture.PixelFormat = static_cast<ETexturePixelFormat>(Reader.Read<std::uint8_t>());
	const auto MipCount = Reader.Read<std::uint32_t>();
	std::size_t TotalBytes = 0;
	if (MipCount > MaximumMips)
	{
		Reader.Invalidate();
		return Texture;
	}

	for (std::uint32_t Index = 0; Index < MipCount && Reader.IsValid(); ++Index)
	{
		FCookedTextureMip Mip;
		Mip.Width = Reader.Read<std::uint32_t>();
		Mip.Height = Reader.Read<std::uint32_t>();
		if (Mip.Width == 0 || Mip.Height == 0 || Mip.Width > MaximumCookedTextureDimension || Mip.Height > MaximumCookedTextureDimension)
		{
			Reader.Invalidate();
			break;
		}

		const std::size_t PixelSize = Texture.PixelFormat == ETexturePixelFormat::Rgba32Float ? sizeof(float) * 4 : 4;
		const std::size_t ByteCount = std::size_t{Mip.Width} * Mip.Height * PixelSize;
		if (ByteCount > MaximumCookedBufferBytes - TotalBytes)
		{
			Reader.Invalidate();
			break;
		}

		TotalBytes += ByteCount;

		const std::span<const std::byte> Pixels = Reader.ReadBytes(ByteCount);
		Mip.Pixels.assign(Pixels.begin(), Pixels.end());
		Texture.Mips.push_back(std::move(Mip));
	}

	return Texture;
}

[[nodiscard]] bool IsFinite(const std::span<const float> Values)
{
	return std::ranges::all_of(Values, [](const float Value)
	{
		return std::isfinite(Value);
	});
}
}

std::expected<void, FAssetError> ValidateCookedTexture(const FCookedTexture& Texture)
{
	if (Texture.PixelFormat != ETexturePixelFormat::Rgba8 && Texture.PixelFormat != ETexturePixelFormat::Rgba32Float)
	{
		return Invalid("Cooked texture has an unknown pixel format");
	}

	if (Texture.PixelFormat == ETexturePixelFormat::Rgba32Float && Texture.ColorSpace != ETextureColorSpace::Linear)
	{
		return Invalid("HDR textures require linear color space");
	}

	if (Texture.ColorSpace != ETextureColorSpace::Linear && Texture.ColorSpace != ETextureColorSpace::Srgb)
	{
		return Invalid("Cooked texture has an unknown color space");
	}

	if (Texture.Mips.empty() || Texture.Mips.size() > MaximumMips)
	{
		return Invalid("Cooked texture requires between 1 and 13 mips");
	}

	const FCookedTextureMip& Top = Texture.Mips.front();
	if (Top.Width == 0 || Top.Height == 0 || Top.Width > MaximumCookedTextureDimension || Top.Height > MaximumCookedTextureDimension)
	{
		return Invalid(std::format("Cooked texture dimensions must be in [1, {}]", MaximumCookedTextureDimension));
	}

	std::uint32_t Width = Top.Width;
	std::uint32_t Height = Top.Height;
	std::size_t TotalBytes = 0;

	for (std::size_t Index = 0; Index < Texture.Mips.size(); ++Index)
	{
		const FCookedTextureMip& Mip = Texture.Mips[Index];
		const std::size_t PixelSize = Texture.PixelFormat == ETexturePixelFormat::Rgba32Float ? sizeof(float) * 4 : 4;
		if (Mip.Width != Width || Mip.Height != Height || Mip.Pixels.size() != std::size_t{Width} * Height * PixelSize)
		{
			return Invalid(std::format("Cooked texture mip {} does not follow the mip chain", Index));
		}

		TotalBytes += Mip.Pixels.size();
		if (TotalBytes > MaximumCookedBufferBytes)
		{
			return Invalid("Cooked texture exceeds the 64 MiB upload limit");
		}

		if (Texture.PixelFormat == ETexturePixelFormat::Rgba32Float)
		{
			for (std::size_t Offset = 0; Offset < Mip.Pixels.size(); Offset += sizeof(float))
			{
				float Value = 0.f;
				std::memcpy(&Value, Mip.Pixels.data() + Offset, sizeof(float));
				if (!std::isfinite(Value) || Value < 0.f || Value > 1.e12f)
				{
					return Invalid("HDR texture pixels must be finite, nonnegative, and bounded");
				}
			}
		}

		Width = std::max(1u, Width / 2);
		Height = std::max(1u, Height / 2);
	}

	const FCookedTextureMip& Bottom = Texture.Mips.back();
	if (Bottom.Width != 1 || Bottom.Height != 1)
	{
		return Invalid("Cooked texture mip chain must end at 1x1");
	}

	return {};
}

std::expected<void, FAssetError> ValidateCookedModel(const FCookedModel& Model)
{
	if (Model.Vertices.empty() || Model.Vertices.size() > MaximumVertices || Model.Indices.empty() || Model.Indices.size() > MaximumIndices || Model.Indices.size() % 3 != 0)
	{
		return Invalid("Cooked model requires vertices and a triangle list within the 64 MiB buffer limits");
	}

	if (!std::ranges::all_of(Model.Vertices, [](const FCookedVertex& Vertex)
	{
		const auto LengthSquared = [](const auto& Values)
		{
			return Values[0] * Values[0] + Values[1] * Values[1] + Values[2] * Values[2];
		};
		const float Dot = Vertex.Normal[0] * Vertex.Tangent[0] + Vertex.Normal[1] * Vertex.Tangent[1] + Vertex.Normal[2] * Vertex.Tangent[2];
		return IsFinite(Vertex.Position) && IsFinite(Vertex.UV) && IsFinite(Vertex.Normal) && IsFinite(Vertex.Tangent)
		       && std::abs(LengthSquared(Vertex.Normal) - 1.f) < 0.01f && std::abs(LengthSquared(Vertex.Tangent) - 1.f) < 0.01f
		       && std::abs(Dot) < 0.01f && (Vertex.Tangent[3] == 1.f || Vertex.Tangent[3] == -1.f);
	}))
	{
		return Invalid("Cooked model vertices must be finite");
	}

	const std::size_t VertexCount = Model.Vertices.size();
	if (!std::ranges::all_of(Model.Indices, [VertexCount](const std::uint32_t Index)
	{
		return Index < VertexCount;
	}))
	{
		return Invalid("Cooked model index references a missing vertex");
	}

	if (Model.Sections.empty() || Model.Sections.size() > MaximumSections || Model.Materials.empty() || Model.Materials.size() > MaximumMaterials || Model.Textures.size() > MaximumTextures)
	{
		return Invalid("Cooked model requires sections, materials, and textures within their limits");
	}

	for (const FCookedMeshSection& Section : Model.Sections)
	{
		if (Section.IndexCount == 0 || Section.IndexCount % 3 != 0 || Section.FirstIndex > Model.Indices.size() || Section.IndexCount > Model.Indices.size() - Section.FirstIndex || Section.Material >= Model.Materials.size())
		{
			return Invalid("Cooked model section is outside its index or material range");
		}
	}

	for (const FCookedMaterial& Material : Model.Materials)
	{
		FMaterialAsset Descriptor{.Name = Material.Name.empty() ? "Default" : Material.Name, .Parameters = Material.Parameters};

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			Descriptor.Textures[Slot].Channel = Material.Channels[Slot];
		}

		if (Material.Name.empty() || Material.Name.size() > MaximumNameLength || !ValidateMaterial(Descriptor))
		{
			return Invalid("Cooked model material has an invalid name or parameters");
		}

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			if ((Material.Textures[Slot] != NoCookedTexture && Material.Textures[Slot] >= Model.Textures.size()) || Material.Channels[Slot] > EMaterialChannel::Rgb)
			{
				return Invalid("Cooked model material has an invalid texture or channel");
			}

			if (Material.Textures[Slot] != NoCookedTexture && Slot != 0 && Slot != 5 && Model.Textures[Material.Textures[Slot]].ColorSpace != ETextureColorSpace::Linear)
			{
				return Invalid("Cooked numerical material maps must use linear color space");
			}
		}
	}

	for (const FCookedTexture& Texture : Model.Textures)
	{
		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(Texture); !Valid)
		{
			return Valid;
		}
	}

	return {};
}

std::expected<std::vector<std::byte>, FAssetError> SerializeCookedAsset(const FCookedAsset& Asset)
{
	FBinaryWriter Writer;
	Writer.Write(CookedAssetMagic);
	Writer.Write(CookedAssetFormatVersion);

	if (const auto* Texture = std::get_if<FCookedTexture>(&Asset))
	{
		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(*Texture); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		Writer.Write(TextureTag);
		WriteTexture(Writer, *Texture);
		return Writer.TakeBytes();
	}

	if (const auto* Material = std::get_if<FMaterialAsset>(&Asset))
	{
		const auto Text = SerializeMaterial(*Material);
		if (!Text)
		{
			return std::unexpected(Text.error());
		}

		Writer.Write(MaterialTag);
		Writer.WriteString(*Text);
		return Writer.TakeBytes();
	}

	const FCookedModel& Model = std::get<FCookedModel>(Asset);

	if (std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	Writer.Write(ModelTag);
	Writer.Write(static_cast<std::uint32_t>(Model.Vertices.size()));

	for (const FCookedVertex& Vertex : Model.Vertices)
	{
		for (const float Value : Vertex.Position)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.UV)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.Normal)
		{
			Writer.WriteFloat(Value);
		}

		for (const float Value : Vertex.Tangent)
		{
			Writer.WriteFloat(Value);
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Indices.size()));

	for (const std::uint32_t Index : Model.Indices)
	{
		Writer.Write(Index);
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Sections.size()));

	for (const FCookedMeshSection& Section : Model.Sections)
	{
		Writer.Write(Section.FirstIndex);
		Writer.Write(Section.IndexCount);
		Writer.Write(Section.Material);
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Materials.size()));

	for (const FCookedMaterial& Material : Model.Materials)
	{
		const auto Text = SerializeMaterial(FMaterialAsset{.Name = Material.Name.empty() ? "Default" : Material.Name, .Parameters = Material.Parameters});
		if (!Text)
		{
			return std::unexpected(Text.error());
		}

		Writer.WriteString(*Text);

		for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
		{
			Writer.Write(Material.Textures[Slot]);
			Writer.Write(static_cast<std::uint8_t>(Material.Channels[Slot]));
		}
	}

	Writer.Write(static_cast<std::uint32_t>(Model.Textures.size()));

	for (const FCookedTexture& Texture : Model.Textures)
	{
		WriteTexture(Writer, Texture);
	}

	return Writer.TakeBytes();
}

std::expected<FCookedAsset, FAssetError> DeserializeCookedAsset(const std::span<const std::byte> Bytes)
{
	FBinaryReader Reader(Bytes);

	if (Reader.Read<std::uint32_t>() != CookedAssetMagic || Reader.Read<std::uint32_t>() != CookedAssetFormatVersion)
	{
		return Invalid("Unsupported cooked asset format or version");
	}

	const auto Tag = Reader.Read<std::uint8_t>();
	if (Tag == MaterialTag)
	{
		const std::string Text = Reader.ReadString(1024 * 1024);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked material");
		}

		const auto Material = DeserializeMaterial(Text);
		return Material ? std::expected<FCookedAsset, FAssetError>(*Material) : std::unexpected(Material.error());
	}

	if (Tag == TextureTag)
	{
		FCookedTexture Texture = ReadTexture(Reader);
		if (!Reader.IsValid() || !Reader.IsAtEnd())
		{
			return Invalid("Truncated or oversized cooked texture");
		}

		if (std::expected<void, FAssetError> Valid = ValidateCookedTexture(Texture); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Texture;
	}

	if (Tag != ModelTag)
	{
		return Invalid("Unknown cooked asset type");
	}

	FCookedModel Model;
	const auto VertexCount = Reader.Read<std::uint32_t>();
	if (VertexCount <= MaximumVertices && Reader.CanRead(VertexCount, sizeof(float) * 12))
	{
		Model.Vertices.resize(VertexCount);

		for (FCookedVertex& Vertex : Model.Vertices)
		{
			for (float& Value : Vertex.Position)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.UV)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.Normal)
			{
				Value = Reader.ReadFloat();
			}

			for (float& Value : Vertex.Tangent)
			{
				Value = Reader.ReadFloat();
			}
		}
	}

	const auto IndexCount = Reader.Read<std::uint32_t>();
	if (IndexCount <= MaximumIndices && Reader.CanRead(IndexCount, sizeof(std::uint32_t)))
	{
		Model.Indices.resize(IndexCount);

		for (std::uint32_t& Index : Model.Indices)
		{
			Index = Reader.Read<std::uint32_t>();
		}
	}

	const auto SectionCount = Reader.Read<std::uint32_t>();
	if (SectionCount <= MaximumSections && Reader.CanRead(SectionCount, sizeof(std::uint32_t) * 3))
	{
		Model.Sections.resize(SectionCount);

		for (FCookedMeshSection& Section : Model.Sections)
		{
			Section.FirstIndex = Reader.Read<std::uint32_t>();
			Section.IndexCount = Reader.Read<std::uint32_t>();
			Section.Material = Reader.Read<std::uint32_t>();
		}
	}

	const auto MaterialCount = Reader.Read<std::uint32_t>();
	if (MaterialCount <= MaximumMaterials && Reader.CanRead(MaterialCount, sizeof(std::uint32_t) * 2))
	{
		Model.Materials.resize(MaterialCount);

		for (FCookedMaterial& Material : Model.Materials)
		{
			const auto Parsed = DeserializeMaterial(Reader.ReadString(1024 * 1024));
			if (!Parsed)
			{
				return std::unexpected(Parsed.error());
			}

			Material.Name = Parsed->Name;
			Material.Parameters = Parsed->Parameters;

			for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
			{
				Material.Textures[Slot] = Reader.Read<std::uint32_t>();
				Material.Channels[Slot] = static_cast<EMaterialChannel>(Reader.Read<std::uint8_t>());
			}
		}
	}

	const auto TextureCount = Reader.Read<std::uint32_t>();
	if (TextureCount <= MaximumTextures && Reader.CanRead(TextureCount, sizeof(std::uint32_t) + 1))
	{
		for (std::uint32_t Index = 0; Index < TextureCount && Reader.IsValid(); ++Index)
		{
			Model.Textures.push_back(ReadTexture(Reader));
		}
	}

	if (!Reader.IsValid() || !Reader.IsAtEnd() || VertexCount > MaximumVertices || IndexCount > MaximumIndices || SectionCount > MaximumSections || MaterialCount > MaximumMaterials || TextureCount > MaximumTextures)
	{
		return Invalid("Truncated or oversized cooked model");
	}

	if (std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
	{
		return std::unexpected(std::move(Valid.error()));
	}

	return Model;
}
}
