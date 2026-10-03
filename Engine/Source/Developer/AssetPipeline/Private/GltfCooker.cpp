#include "GltfCooker.h"

#include "FileUtilities.h"
#include "Herta/AssetPipeline/TextureCooker.h"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>

namespace Herta
{
namespace
{
static_assert(std::is_same_v<unsigned int, std::uint32_t>, "meshoptimizer indices are unsigned int");
static_assert(sizeof(FCookedVertex) == sizeof(float) * 5, "Vertex deduplication compares raw bytes, so vertices must not contain padding");

// Quantized attributes decode through fastgltf's accessor tools. Other required extensions fail the parse instead of cooking incorrect data.
inline constexpr fastgltf::Extensions SupportedExtensions = fastgltf::Extensions::KHR_mesh_quantization;
inline constexpr std::size_t MaximumWarnings = 32;

[[nodiscard]] std::expected<fastgltf::Asset, FAssetError> ParseGltf(const std::span<const std::byte> SourceBytes, const std::filesystem::path& Directory, const fastgltf::Options Options)
{
	fastgltf::Expected<fastgltf::GltfDataBuffer> Data = fastgltf::GltfDataBuffer::FromBytes(SourceBytes.data(), SourceBytes.size());
	if (Data.error() != fastgltf::Error::None)
	{
		return std::unexpected(FAssetError{std::format("Cannot read glTF: {}", fastgltf::getErrorMessage(Data.error()))});
	}

	fastgltf::Parser Parser(SupportedExtensions);
	fastgltf::Expected<fastgltf::Asset> Asset = Parser.loadGltf(Data.get(), Directory, Options);
	if (Asset.error() != fastgltf::Error::None)
	{
		return std::unexpected(FAssetError{std::format("Cannot parse glTF: {}", fastgltf::getErrorMessage(Asset.error()))});
	}

	if (const fastgltf::Error Error = fastgltf::validate(Asset.get()); Error != fastgltf::Error::None)
	{
		return std::unexpected(FAssetError{std::format("Invalid glTF: {}", fastgltf::getErrorMessage(Error))});
	}

	return std::move(Asset.get());
}

[[nodiscard]] std::expected<std::string, FAssetError> ResolveUri(const std::string& SourcePath, const fastgltf::URI& Uri)
{
	if (!Uri.isLocalPath())
	{
		return std::unexpected(FAssetError{std::format("glTF URI '{}' is not a relative file path", Uri.path())});
	}

	const std::filesystem::path Relative = Uri.fspath();
	if (Relative.has_root_name() || Relative.has_root_directory())
	{
		return std::unexpected(FAssetError{std::format("glTF URI '{}' must be relative", Uri.path())});
	}

	std::string Resolved = GenericPathToUtf8((Utf8ToPath(SourcePath).parent_path() / Relative).lexically_normal());
	if (!IsValidAssetPath(Resolved))
	{
		return std::unexpected(FAssetError{std::format("glTF URI '{}' escapes the content root or is not portable", Uri.path())});
	}

	return Resolved;
}

[[nodiscard]] std::span<const std::byte> GetBufferBytes(const fastgltf::Buffer& Buffer)
{
	return std::visit(fastgltf::visitor{[](const auto&)
	{
		return std::span<const std::byte>{};
	},
	                      [](const fastgltf::sources::Array& Array)
	{
		return std::span<const std::byte>(Array.bytes.data(), Array.bytes.size_bytes());
	},
	                      [](const fastgltf::sources::Vector& Vector)
	{
		return std::span<const std::byte>(Vector.bytes.data(), Vector.bytes.size());
	},
	                      [](const fastgltf::sources::ByteView& View)
	{
		return std::span<const std::byte>(View.bytes.data(), View.bytes.size());
	}},
	    Buffer.data);
}

[[nodiscard]] std::optional<std::span<const std::byte>> GetImageBytes(const fastgltf::Asset& Asset, const fastgltf::Image& Image)
{
	if (const auto* View = std::get_if<fastgltf::sources::BufferView>(&Image.data))
	{
		const fastgltf::BufferView& BufferView = Asset.bufferViews[View->bufferViewIndex];
		const std::span<const std::byte> Buffer = GetBufferBytes(Asset.buffers[BufferView.bufferIndex]);
		if (BufferView.byteOffset > Buffer.size() || BufferView.byteLength > Buffer.size() - BufferView.byteOffset)
		{
			return std::nullopt;
		}

		return Buffer.subspan(BufferView.byteOffset, BufferView.byteLength);
	}

	if (const auto* Array = std::get_if<fastgltf::sources::Array>(&Image.data))
	{
		return std::span<const std::byte>(Array->bytes.data(), Array->bytes.size_bytes());
	}

	if (const auto* View = std::get_if<fastgltf::sources::ByteView>(&Image.data))
	{
		return std::span<const std::byte>(View->bytes.data(), View->bytes.size());
	}

	return std::nullopt;
}

struct FMaterialBucket
{
	std::vector<FCookedVertex> Vertices;
	std::vector<std::uint32_t> Indices;
};

class FGltfModelBuilder
{
public:
	FGltfModelBuilder(const fastgltf::Asset& InAsset, std::vector<std::string>& InWarnings)
	    : Asset(InAsset)
	    , Warnings(InWarnings)
	{
	}

	void AddMesh(const std::size_t MeshIndex, const fastgltf::math::fmat4x4& World)
	{
		const fastgltf::Mesh& Mesh = Asset.meshes[MeshIndex];
		for (std::size_t PrimitiveIndex = 0; PrimitiveIndex < Mesh.primitives.size() && !Error; ++PrimitiveIndex)
		{
			AddPrimitive(Mesh, PrimitiveIndex, World);
		}
	}

	[[nodiscard]] std::expected<FCookedModel, FAssetError> Build()
	{
		if (Error)
		{
			return std::unexpected(std::move(*Error));
		}

		if (Buckets.empty())
		{
			return std::unexpected(FAssetError{"glTF contains no triangle geometry"});
		}

		FCookedModel Model;
		std::map<std::pair<std::size_t, std::array<float, 4>>, std::uint32_t> TextureLookup;
		for (auto& [MaterialKey, Bucket] : Buckets)
		{
			FCookedMaterial Material{.Name = "Default", .BaseColorTexture = 0};
			std::array<float, 4> Factor{1.f, 1.f, 1.f, 1.f};
			std::optional<std::size_t> ImageIndex;
			if (MaterialKey > 0)
			{
				const fastgltf::Material& Source = Asset.materials[MaterialKey - 1];
				Material.Name = std::string(Source.name.begin(), Source.name.end()).substr(0, 256);
				for (std::size_t Channel = 0; Channel < 4; ++Channel)
				{
					Factor[Channel] = static_cast<float>(Source.pbrData.baseColorFactor[Channel]);
				}

				if (Source.pbrData.baseColorTexture && Asset.textures[Source.pbrData.baseColorTexture->textureIndex].imageIndex)
				{
					ImageIndex = *Asset.textures[Source.pbrData.baseColorTexture->textureIndex].imageIndex;
				}
			}

			// Image indices are offset by one so zero means "factor only".
			const auto Key = std::make_pair(ImageIndex ? *ImageIndex + 1 : 0, Factor);
			auto Texture = TextureLookup.find(Key);
			if (Texture == TextureLookup.end())
			{
				Texture = TextureLookup.emplace(Key, static_cast<std::uint32_t>(Model.Textures.size())).first;
				Model.Textures.push_back(CookBaseColor(ImageIndex, Factor));
			}

			Material.BaseColorTexture = Texture->second;
			Model.Materials.push_back(std::move(Material));

			const auto VertexBase = static_cast<std::uint32_t>(Model.Vertices.size());
			Model.Sections.push_back({.FirstIndex = static_cast<std::uint32_t>(Model.Indices.size()), .IndexCount = static_cast<std::uint32_t>(Bucket.Indices.size()), .Material = static_cast<std::uint32_t>(Model.Materials.size() - 1)});
			Model.Vertices.insert(Model.Vertices.end(), Bucket.Vertices.begin(), Bucket.Vertices.end());
			for (const std::uint32_t Index : Bucket.Indices)
			{
				Model.Indices.push_back(VertexBase + Index);
			}
		}

		Optimize(Model);
		if (std::expected<void, FAssetError> Valid = ValidateCookedModel(Model); !Valid)
		{
			return std::unexpected(std::move(Valid.error()));
		}

		return Model;
	}

private:
	// Instanced meshes would otherwise repeat the same warning once per node.
	void Warn(std::string Message)
	{
		if (Warnings.size() < MaximumWarnings && std::ranges::find(Warnings, Message) == Warnings.end())
		{
			Warnings.push_back(std::move(Message));
		}
	}

	void Fail(std::string Message)
	{
		if (!Error)
		{
			Error = FAssetError{std::move(Message)};
		}
	}

	void AddPrimitive(const fastgltf::Mesh& Mesh, const std::size_t PrimitiveIndex, const fastgltf::math::fmat4x4& World)
	{
		const fastgltf::Primitive& Primitive = Mesh.primitives[PrimitiveIndex];
		const std::string MeshName(Mesh.name.begin(), Mesh.name.end());
		if (Primitive.type != fastgltf::PrimitiveType::Triangles)
		{
			Warn(std::format("Mesh '{}' primitive {} is not a triangle list and was skipped", MeshName, PrimitiveIndex));
			return;
		}

		const auto* Position = Primitive.findAttribute("POSITION");
		if (Position == Primitive.attributes.end())
		{
			Warn(std::format("Mesh '{}' primitive {} has no positions and was skipped", MeshName, PrimitiveIndex));
			return;
		}

		std::size_t TexCoordSet = 0;
		const std::size_t MaterialKey = Primitive.materialIndex ? *Primitive.materialIndex + 1 : 0;
		if (MaterialKey > 0)
		{
			const fastgltf::Material& Material = Asset.materials[MaterialKey - 1];
			if (Material.pbrData.baseColorTexture)
			{
				TexCoordSet = Material.pbrData.baseColorTexture->texCoordIndex;
			}
		}

		const fastgltf::Accessor& PositionAccessor = Asset.accessors[Position->accessorIndex];
		const std::size_t VertexCount = PositionAccessor.count;
		FMaterialBucket& Bucket = Buckets[MaterialKey];
		const std::size_t VertexBase = Bucket.Vertices.size();
		if (VertexCount == 0 || VertexCount > std::numeric_limits<std::uint32_t>::max() - VertexBase)
		{
			Fail(std::format("Mesh '{}' primitive {} has an unsupported vertex count", MeshName, PrimitiveIndex));
			return;
		}

		Bucket.Vertices.resize(VertexBase + VertexCount);

		fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(Asset, PositionAccessor, [&](const fastgltf::math::fvec3& Local, const std::size_t Index)
		{
			std::array<float, 3>& Target = Bucket.Vertices[VertexBase + Index].Position;
			for (std::size_t Row = 0; Row < 3; ++Row)
			{
				Target[Row] = World[0][Row] * Local[0] + World[1][Row] * Local[1] + World[2][Row] * Local[2] + World[3][Row];
			}
		});

		const std::string TexCoordName = std::format("TEXCOORD_{}", TexCoordSet);
		if (const auto* TexCoord = Primitive.findAttribute(TexCoordName); TexCoord != Primitive.attributes.end())
		{
			const fastgltf::Accessor& TexCoordAccessor = Asset.accessors[TexCoord->accessorIndex];
			if (TexCoordAccessor.count != VertexCount)
			{
				Fail(std::format("Mesh '{}' primitive {} has mismatched {} and POSITION counts", MeshName, PrimitiveIndex, TexCoordName));
				return;
			}

			fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(Asset, TexCoordAccessor, [&](const fastgltf::math::fvec2& UV, const std::size_t Index)
			{
				Bucket.Vertices[VertexBase + Index].UV = {UV[0], UV[1]};
			});
		}

		const std::size_t IndexBase = Bucket.Indices.size();
		if (Primitive.indicesAccessor)
		{
			bool bOutOfRange = false;
			fastgltf::iterateAccessor<std::uint32_t>(Asset, Asset.accessors[*Primitive.indicesAccessor], [&](const std::uint32_t Index)
			{
				bOutOfRange |= Index >= VertexCount;
				Bucket.Indices.push_back(static_cast<std::uint32_t>(VertexBase + std::min<std::size_t>(Index, VertexCount - 1)));
			});

			if (bOutOfRange)
			{
				Fail(std::format("Mesh '{}' primitive {} references a missing vertex", MeshName, PrimitiveIndex));
				return;
			}
		}
		else
		{
			for (std::size_t Index = 0; Index < VertexCount; ++Index)
			{
				Bucket.Indices.push_back(static_cast<std::uint32_t>(VertexBase + Index));
			}
		}

		if ((Bucket.Indices.size() - IndexBase) % 3 != 0)
		{
			Fail(std::format("Mesh '{}' primitive {} does not contain whole triangles", MeshName, PrimitiveIndex));
			return;
		}

		// A mirroring transform turns counter-clockwise triangles clockwise, so restore the canonical winding once here.
		const auto Column = [&World](const std::size_t Index)
		{
			return std::array<float, 3>{World[Index][0], World[Index][1], World[Index][2]};
		};

		const std::array<float, 3> X = Column(0);
		const std::array<float, 3> Y = Column(1);
		const std::array<float, 3> Z = Column(2);
		const float Determinant = X[0] * (Y[1] * Z[2] - Y[2] * Z[1]) - X[1] * (Y[0] * Z[2] - Y[2] * Z[0]) + X[2] * (Y[0] * Z[1] - Y[1] * Z[0]);
		if (Determinant < 0.f)
		{
			for (std::size_t Index = IndexBase; Index < Bucket.Indices.size(); Index += 3)
			{
				std::swap(Bucket.Indices[Index + 1], Bucket.Indices[Index + 2]);
			}
		}
	}

	[[nodiscard]] FCookedTexture CookBaseColor(const std::optional<std::size_t> ImageIndex, const std::array<float, 4>& Factor)
	{
		if (ImageIndex)
		{
			const std::optional<std::span<const std::byte>> Bytes = GetImageBytes(Asset, Asset.images[*ImageIndex]);
			if (!Bytes)
			{
				Warn(std::format("Image {} has no embedded or loadable data; using the base color factor", *ImageIndex));
			}
			else if (std::expected<FCookedTexture, FAssetError> Texture = CookEncodedTexture(*Bytes, ETextureColorSpace::Srgb, Factor))
			{
				return std::move(*Texture);
			}
			else
			{
				Warn(std::format("Image {}: {}; using the base color factor", *ImageIndex, Texture.error().Message));
			}
		}

		constexpr std::array White{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
		std::expected<FCookedTexture, FAssetError> Texture = CookTexture(1, 1, White, ETextureColorSpace::Srgb, Factor);
		return Texture ? std::move(*Texture) : FCookedTexture{};
	}

	static void Optimize(FCookedModel& Model)
	{
		std::vector<unsigned int> Remap(Model.Vertices.size());
		const std::size_t UniqueVertices = meshopt_generateVertexRemap(Remap.data(), Model.Indices.data(), Model.Indices.size(), Model.Vertices.data(), Model.Vertices.size(), sizeof(FCookedVertex));
		std::vector<FCookedVertex> Vertices(UniqueVertices);
		meshopt_remapVertexBuffer(Vertices.data(), Model.Vertices.data(), Model.Vertices.size(), sizeof(FCookedVertex), Remap.data());
		meshopt_remapIndexBuffer(Model.Indices.data(), Model.Indices.data(), Model.Indices.size(), Remap.data());

		std::vector<std::uint32_t> Optimized(Model.Indices.size());
		for (const FCookedMeshSection& Section : Model.Sections)
		{
			meshopt_optimizeVertexCache(Optimized.data() + Section.FirstIndex, Model.Indices.data() + Section.FirstIndex, Section.IndexCount, Vertices.size());
		}

		Model.Indices = std::move(Optimized);

		Model.Vertices.resize(Vertices.size());
		Model.Vertices.resize(meshopt_optimizeVertexFetch(Model.Vertices.data(), Model.Indices.data(), Model.Indices.size(), Vertices.data(), Vertices.size(), sizeof(FCookedVertex)));
	}

	const fastgltf::Asset& Asset;
	std::vector<std::string>& Warnings;
	// Material index plus one, so the default material sorts first and the output order is deterministic.
	std::map<std::size_t, FMaterialBucket> Buckets;
	std::optional<FAssetError> Error;
};
}

std::expected<std::vector<std::string>, FAssetError> FindGltfDependencies(const std::filesystem::path& ContentRoot, const std::string& SourcePath, const std::span<const std::byte> SourceBytes)
{
	std::expected<fastgltf::Asset, FAssetError> Parsed = ParseGltf(SourceBytes, (ContentRoot / Utf8ToPath(SourcePath)).parent_path(), fastgltf::Options::None);
	if (!Parsed)
	{
		return std::unexpected(std::move(Parsed.error()));
	}

	std::set<std::string> Dependencies;
	const auto AddUri = [&](const fastgltf::DataSource& Source) -> std::expected<void, FAssetError>
	{
		if (const auto* Uri = std::get_if<fastgltf::sources::URI>(&Source))
		{
			std::expected<std::string, FAssetError> Resolved = ResolveUri(SourcePath, Uri->uri);
			if (!Resolved)
			{
				return std::unexpected(std::move(Resolved.error()));
			}

			Dependencies.insert(std::move(*Resolved));
		}

		return {};
	};

	for (const fastgltf::Buffer& Buffer : Parsed->buffers)
	{
		if (std::expected<void, FAssetError> Added = AddUri(Buffer.data); !Added)
		{
			return std::unexpected(std::move(Added.error()));
		}
	}

	for (const fastgltf::Image& Image : Parsed->images)
	{
		if (std::expected<void, FAssetError> Added = AddUri(Image.data); !Added)
		{
			return std::unexpected(std::move(Added.error()));
		}
	}

	return std::vector<std::string>(Dependencies.begin(), Dependencies.end());
}

std::expected<FCookedModel, FAssetError> CookGltf(const std::filesystem::path& ContentRoot, const std::string& SourcePath, const std::span<const std::byte> SourceBytes, std::vector<std::string>& Warnings)
{
	// Resolve URIs before letting the loader read anything, so a crafted file cannot read outside the content root.
	if (std::expected<std::vector<std::string>, FAssetError> Dependencies = FindGltfDependencies(ContentRoot, SourcePath, SourceBytes); !Dependencies)
	{
		return std::unexpected(std::move(Dependencies.error()));
	}

	std::expected<fastgltf::Asset, FAssetError> Parsed = ParseGltf(SourceBytes, (ContentRoot / Utf8ToPath(SourcePath)).parent_path(), fastgltf::Options::LoadExternalBuffers | fastgltf::Options::LoadExternalImages);
	if (!Parsed)
	{
		return std::unexpected(std::move(Parsed.error()));
	}

	const fastgltf::Asset& Asset = *Parsed;
	for (const fastgltf::Buffer& Buffer : Asset.buffers)
	{
		if (GetBufferBytes(Buffer).size() < Buffer.byteLength)
		{
			return std::unexpected(FAssetError{"glTF buffer data is missing or shorter than declared"});
		}
	}

	FGltfModelBuilder Builder(Asset, Warnings);
	if (Asset.scenes.empty())
	{
		for (std::size_t MeshIndex = 0; MeshIndex < Asset.meshes.size(); ++MeshIndex)
		{
			Builder.AddMesh(MeshIndex, fastgltf::math::fmat4x4());
		}
	}
	else
	{
		const std::size_t SceneIndex = Asset.defaultScene ? *Asset.defaultScene : 0;
		fastgltf::iterateSceneNodes(Asset, SceneIndex, fastgltf::math::fmat4x4(), [&](const fastgltf::Node& Node, const fastgltf::math::fmat4x4& World)
		{
			if (Node.meshIndex)
			{
				Builder.AddMesh(*Node.meshIndex, World);
			}
		});
	}

	return Builder.Build();
}
}
