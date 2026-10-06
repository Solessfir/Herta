#include "Herta/Renderer/MeshRenderer.h"

#include <format>

namespace Herta
{
std::expected<std::shared_ptr<const FRenderMaterial>, FPresentationError> FRenderMaterial::Create(IGraphicsDevice& Device, const FMaterialAsset& Material, const std::array<std::optional<FCookedTexture>, MaterialTextureSlotCount>& Sources, const std::string_view Name)
{
	if (const auto Valid = ValidateMaterial(Material); !Valid)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
	}

	auto Result = std::make_shared<FRenderMaterial>();
	Result->Parameters = Material.Parameters;
	if (!Material.ShaderPath.empty())
	{
		Result->ShaderKey = Material.ShaderPath.starts_with("Engine/") || Material.ShaderPath.starts_with("Game/") ? Material.ShaderPath : std::format("{}/{}", Name.starts_with("Engine/") ? "Engine" : "Game", Material.ShaderPath);
	}

	for (std::size_t Slot = 0; Slot < MaterialTextureSlotCount; ++Slot)
	{
		Result->Channels[Slot] = Material.Textures[Slot].Channel;
		if (!Sources[Slot])
		{
			if (Material.Textures[Slot].Texture.IsValid())
			{
				return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "A referenced material texture has not been loaded"});
			}

			continue;
		}

		const FCookedTexture& Source = *Sources[Slot];
		if (Source.PixelFormat == ETexturePixelFormat::Rgba32Float && Material.Textures[Slot].ColorSpace != ETextureColorSpace::Linear)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = "HDR textures require Linear color space"});
		}

		if (const auto Valid = ValidateCookedTexture(Source); !Valid)
		{
			return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
		}

		for (std::size_t Previous = 0; Previous < Slot; ++Previous)
		{
			if (Material.Textures[Slot].Texture.IsValid() && Material.Textures[Previous].Texture == Material.Textures[Slot].Texture && Material.Textures[Previous].ColorSpace == Material.Textures[Slot].ColorSpace)
			{
				Result->Textures[Slot] = Result->Textures[Previous];
				break;
			}
		}

		if (Result->Textures[Slot])
		{
			continue;
		}

		const ETextureFormat Format = Source.PixelFormat == ETexturePixelFormat::Rgba32Float ? ETextureFormat::Rgba32Float : Material.Textures[Slot].ColorSpace == ETextureColorSpace::Srgb ? ETextureFormat::Rgba8Srgb
		                                                                                                                                                                                    : ETextureFormat::Rgba8;
		auto Texture = Device.CreateTexture({.Name = std::format("{} map {}", Name, Slot), .Extent = {.Width = Source.Mips[0].Width, .Height = Source.Mips[0].Height}, .Format = Format, .MipLevels = static_cast<std::uint32_t>(Source.Mips.size())});
		if (!Texture)
		{
			return std::unexpected(Texture.error());
		}

		if (auto Begun = Device.BeginCommands(); !Begun)
		{
			return std::unexpected(Begun.error());
		}

		std::size_t Uploaded = 0;
		for (std::uint32_t Mip = 0; Mip < Source.Mips.size(); ++Mip)
		{
			const auto& Bytes = Source.Mips[Mip].Pixels;
			if (Bytes.size() > MaximumUploadBytesPerRecording - Uploaded)
			{
				if (auto Submitted = Device.SubmitCommands(); !Submitted)
				{
					return std::unexpected(Submitted.error());
				}

				if (auto Begun = Device.BeginCommands(); !Begun)
				{
					return std::unexpected(Begun.error());
				}

				Uploaded = 0;
			}

			if (auto Written = Device.WriteTexture(*Texture, Mip, Bytes); !Written)
			{
				Device.CancelCommands();
				return std::unexpected(Written.error());
			}

			Uploaded += Bytes.size();
		}

		if (auto Submitted = Device.SubmitCommands(); !Submitted)
		{
			return std::unexpected(Submitted.error());
		}

		Result->Textures[Slot] = std::move(*Texture);
	}

	return Result;
}

const FMaterialParameters& FRenderMaterial::GetParameters() const noexcept
{
	return Parameters;
}

std::string_view FRenderMaterial::GetShaderKey() const noexcept
{
	return ShaderKey;
}

std::expected<std::shared_ptr<const FRenderMaterial>, FPresentationError> FRenderMaterial::WithParameters(const FMaterialParameters& NewParameters) const
{
	if (const auto Valid = ValidateMaterialParameters(NewParameters); !Valid)
	{
		return std::unexpected(FPresentationError{.Code = EPresentationErrorCode::InvalidDescriptor, .Message = Valid.error().Message});
	}

	auto Copy = std::make_shared<FRenderMaterial>(*this);
	Copy->Parameters = NewParameters;
	return Copy;
}
}
