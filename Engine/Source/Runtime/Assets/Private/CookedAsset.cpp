#include "Herta/Assets/CookedAsset.h"

#include "Herta/Core/BinaryStream.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Herta
{
namespace
{
inline constexpr std::uint32_t CookedAssetMagic = 0x53414348; // "HCAS"
inline constexpr std::uint8_t TextureTag = 1;
inline constexpr std::uint8_t ModelTag = 2;

// Matches the per-recording GPU upload budget so every valid cooked buffer can be uploaded in one piece.
inline constexpr std::size_t MaximumBufferBytes = std::size_t{64} * 1024 * 1024;
inline constexpr std::size_t MaximumVertices = MaximumBufferBytes / sizeof(FCookedVertex);
inline constexpr std::size_t MaximumIndices = MaximumBufferBytes / sizeof(std::uint32_t);
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
	const auto MipCount = Reader.Read<std::uint32_t>();
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

		const std::span<const std::byte> Pixels = Reader.ReadBytes(std::size_t{Mip.Width} * Mip.Height * 4);
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
	for (std::size_t Index = 0; Index < Texture.Mips.size(); ++Index)
	{
		const FCookedTextureMip& Mip = Texture.Mips[Index];
		if (Mip.Width != Width || Mip.Height != Height || Mip.Pixels.size() != std::size_t{Width} * Height * 4)
		{
			return Invalid(std::format("Cooked texture mip {} does not follow the mip chain", Index));
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
		return IsFinite(Vertex.Position) && IsFinite(Vertex.UV);
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
	if (Model.Sections.empty() || Model.Sections.size() > MaximumSections || Model.Materials.empty() || Model.Materials.size() > MaximumMaterials || Model.Textures.empty() || Model.Textures.size() > MaximumTextures)
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
		if (Material.Name.size() > MaximumNameLength || Material.BaseColorTexture >= Model.Textures.size())
		{
			return Invalid("Cooked model material has an invalid name or texture");
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
		Writer.WriteString(Material.Name);
		Writer.Write(Material.BaseColorTexture);
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
	if (VertexCount <= MaximumVertices && Reader.CanRead(VertexCount, sizeof(float) * 5))
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
			Material.Name = Reader.ReadString(MaximumNameLength);
			Material.BaseColorTexture = Reader.Read<std::uint32_t>();
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
