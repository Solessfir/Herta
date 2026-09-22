#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
enum class EShaderStage : std::uint32_t
{
	Vertex,
	Fragment
};

enum class EShaderBindingType : std::uint32_t
{
	Texture,
	Sampler,
	ConstantBuffer
};

struct FShaderBinding
{
	std::string Name;
	EShaderBindingType Type = EShaderBindingType::Texture;
	std::uint32_t Binding = 0;
	std::uint32_t Space = 0;
};

struct FShaderSourceDependency
{
	std::string Path;
	std::uint64_t ContentHash = 0;
};

struct FShaderAsset
{
	EShaderStage Stage = EShaderStage::Vertex;
	std::string EntryPoint;
	std::string CompilerVersion;
	std::string PermutationKey;
	std::uint32_t PushConstantSize = 0;
	bool bDebugInformation = false;
	std::vector<FShaderBinding> Bindings;
	std::vector<FShaderSourceDependency> Dependencies;
	std::vector<std::uint32_t> Bytecode;
};

struct FShaderError
{
	std::string Message;
};

// Stable content identity and corruption detection, not an authenticity guarantee.
[[nodiscard]] std::uint64_t HashShaderContent(std::span<const std::byte> Bytes) noexcept;
[[nodiscard]] std::expected<std::vector<std::byte>, FShaderError> SerializeCookedShader(const FShaderAsset& Shader);
[[nodiscard]] std::expected<FShaderAsset, FShaderError> DeserializeCookedShader(std::span<const std::byte> Bytes);
[[nodiscard]] std::expected<FShaderAsset, FShaderError> LoadCookedShader(const std::filesystem::path& Path);
}
