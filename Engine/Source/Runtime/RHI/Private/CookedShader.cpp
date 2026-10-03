#include "Herta/RHI/CookedShader.h"

#include <fstream>
#include <limits>

namespace Herta
{
namespace
{
constexpr std::uint32_t Magic = 0x48534848;
constexpr std::uint32_t Version = 1;
constexpr std::size_t MaximumAssetSize = 16 * 1024 * 1024;
constexpr std::uint32_t MaximumRecords = 4096;

void AppendInteger(std::vector<std::byte>& Bytes, const std::uint64_t Value, const std::size_t Size)
{
	for (std::size_t Index = 0; Index < Size; ++Index)
	{
		Bytes.push_back(static_cast<std::byte>((Value >> (Index * 8)) & 0xff));
	}
}

void AppendString(std::vector<std::byte>& Bytes, const std::string& Value)
{
	AppendInteger(Bytes, Value.size(), 4);
	const auto Data = std::as_bytes(std::span(Value));
	Bytes.insert(Bytes.end(), Data.begin(), Data.end());
}

class FReader
{
public:
	explicit FReader(const std::span<const std::byte> InBytes)
	    : Bytes(InBytes)
	{
	}

	std::uint64_t Integer(const std::size_t Size)
	{
		if (Size > Bytes.size() - Offset)
		{
			bValid = false;
			return 0;
		}

		std::uint64_t Value = 0;
		for (std::size_t Index = 0; Index < Size; ++Index)
		{
			Value |= static_cast<std::uint64_t>(Bytes[Offset++]) << (Index * 8);
		}

		return Value;
	}

	std::string String()
	{
		const auto Size = static_cast<std::size_t>(Integer(4));
		if (!bValid || Size > MaximumRecords || Size > Bytes.size() - Offset)
		{
			bValid = false;
			return {};
		}

		std::string Value;
		Value.reserve(Size);
		for (std::size_t Index = 0; Index < Size; ++Index)
		{
			const char Character = static_cast<char>(Bytes[Offset++]);
			bValid &= Character != '\0';
			Value.push_back(Character);
		}

		return Value;
	}

	std::span<const std::byte> Bytes;
	std::size_t Offset = 0;
	bool bValid = true;
};

bool IsValidString(const std::string& Value)
{
	return Value.size() <= MaximumRecords && Value.find('\0') == std::string::npos;
}

bool IsValidAsset(const FShaderAsset& Shader)
{
	if (Shader.Stage > EShaderStage::Fragment || Shader.EntryPoint.empty() || Shader.CompilerVersion.empty() || !IsValidString(Shader.EntryPoint) || !IsValidString(Shader.CompilerVersion) || !IsValidString(Shader.PermutationKey) || Shader.Bindings.size() > MaximumRecords || Shader.Dependencies.size() > MaximumRecords || Shader.Bytecode.size() < 5 || Shader.Bytecode.size() > MaximumAssetSize / 4 || Shader.Bytecode[0] != 0x07230203 || Shader.Bytecode[3] == 0 || Shader.Bytecode[4] != 0 || Shader.PushConstantSize > 128 || Shader.PushConstantSize % 4 != 0)
	{
		return false;
	}

	for (const FShaderBinding& Binding : Shader.Bindings)
	{
		if (!IsValidString(Binding.Name) || Binding.Name.empty() || Binding.Type > EShaderBindingType::ConstantBuffer)
		{
			return false;
		}
	}

	for (const FShaderSourceDependency& Dependency : Shader.Dependencies)
	{
		if (!IsValidString(Dependency.Path) || Dependency.Path.empty())
		{
			return false;
		}
	}

	return true;
}
}

std::uint64_t HashShaderContent(const std::span<const std::byte> Bytes) noexcept
{
	std::uint64_t Hash = 14695981039346656037ull;
	for (const std::byte Byte : Bytes)
	{
		Hash = (Hash ^ static_cast<std::uint64_t>(Byte)) * 1099511628211ull;
	}

	return Hash;
}

std::expected<std::vector<std::byte>, FShaderError> SerializeCookedShader(const FShaderAsset& Shader)
{
	if (!IsValidAsset(Shader))
	{
		return std::unexpected(FShaderError{"Invalid cooked shader metadata or SPIR-V header"});
	}

	std::vector<std::byte> Bytes;
	AppendInteger(Bytes, Magic, 4);
	AppendInteger(Bytes, Version, 4);
	AppendInteger(Bytes, static_cast<std::uint32_t>(Shader.Stage), 4);
	AppendInteger(Bytes, Shader.bDebugInformation ? 1 : 0, 4);
	AppendInteger(Bytes, Shader.PushConstantSize, 4);
	AppendString(Bytes, Shader.EntryPoint);
	AppendString(Bytes, Shader.CompilerVersion);
	AppendString(Bytes, Shader.PermutationKey);
	AppendInteger(Bytes, Shader.Bindings.size(), 4);
	for (const FShaderBinding& Binding : Shader.Bindings)
	{
		AppendString(Bytes, Binding.Name);
		AppendInteger(Bytes, static_cast<std::uint32_t>(Binding.Type), 4);
		AppendInteger(Bytes, Binding.Binding, 4);
		AppendInteger(Bytes, Binding.Space, 4);
	}

	AppendInteger(Bytes, Shader.Dependencies.size(), 4);
	for (const FShaderSourceDependency& Dependency : Shader.Dependencies)
	{
		AppendString(Bytes, Dependency.Path);
		AppendInteger(Bytes, Dependency.ContentHash, 8);
	}

	AppendInteger(Bytes, Shader.Bytecode.size(), 4);
	for (const std::uint32_t Word : Shader.Bytecode)
	{
		AppendInteger(Bytes, Word, 4);
	}

	AppendInteger(Bytes, HashShaderContent(Bytes), 8);
	if (Bytes.size() > MaximumAssetSize)
	{
		return std::unexpected(FShaderError{"Cooked shader exceeds the 16 MiB size limit"});
	}

	return Bytes;
}

std::expected<FShaderAsset, FShaderError> DeserializeCookedShader(const std::span<const std::byte> Bytes)
{
	if (Bytes.size() < 8 || Bytes.size() > MaximumAssetSize)
	{
		return std::unexpected(FShaderError{"Invalid cooked shader size"});
	}

	FReader Checksum(Bytes.last(8));
	if (Checksum.Integer(8) != HashShaderContent(Bytes.first(Bytes.size() - 8)))
	{
		return std::unexpected(FShaderError{"Cooked shader checksum mismatch"});
	}

	FReader Reader(Bytes.first(Bytes.size() - 8));
	if (Reader.Integer(4) != Magic || Reader.Integer(4) != Version)
	{
		return std::unexpected(FShaderError{"Unsupported cooked shader format or version"});
	}

	FShaderAsset Shader;
	Shader.Stage = static_cast<EShaderStage>(Reader.Integer(4));
	const auto Flags = Reader.Integer(4);
	Shader.bDebugInformation = Flags == 1;
	Shader.PushConstantSize = static_cast<std::uint32_t>(Reader.Integer(4));
	Shader.EntryPoint = Reader.String();
	Shader.CompilerVersion = Reader.String();
	Shader.PermutationKey = Reader.String();
	const auto BindingCount = Reader.Integer(4);
	if (BindingCount > MaximumRecords)
	{
		return std::unexpected(FShaderError{"Too many cooked shader bindings"});
	}

	for (std::uint64_t Index = 0; Index < BindingCount && Reader.bValid; ++Index)
	{
		FShaderBinding Binding;
		Binding.Name = Reader.String();
		Binding.Type = static_cast<EShaderBindingType>(Reader.Integer(4));
		Binding.Binding = static_cast<std::uint32_t>(Reader.Integer(4));
		Binding.Space = static_cast<std::uint32_t>(Reader.Integer(4));
		Shader.Bindings.push_back(std::move(Binding));
	}

	const auto DependencyCount = Reader.Integer(4);
	if (DependencyCount > MaximumRecords)
	{
		return std::unexpected(FShaderError{"Too many cooked shader dependencies"});
	}

	for (std::uint64_t Index = 0; Index < DependencyCount && Reader.bValid; ++Index)
	{
		FShaderSourceDependency Dependency;
		Dependency.Path = Reader.String();
		Dependency.ContentHash = Reader.Integer(8);
		Shader.Dependencies.push_back(std::move(Dependency));
	}

	const auto WordCount = Reader.Integer(4);
	if (WordCount > (Reader.Bytes.size() - Reader.Offset) / 4)
	{
		return std::unexpected(FShaderError{"Truncated cooked shader bytecode"});
	}

	Shader.Bytecode.reserve(static_cast<std::size_t>(WordCount));
	for (std::uint64_t Index = 0; Index < WordCount; ++Index)
	{
		Shader.Bytecode.push_back(static_cast<std::uint32_t>(Reader.Integer(4)));
	}

	if (!Reader.bValid || Reader.Offset != Reader.Bytes.size() || Flags > 1 || !IsValidAsset(Shader))
	{
		return std::unexpected(FShaderError{"Invalid cooked shader metadata or bytecode"});
	}

	return Shader;
}

std::expected<FShaderAsset, FShaderError> LoadCookedShader(const std::filesystem::path& Path)
{
	std::ifstream Stream(Path, std::ios::binary | std::ios::ate);
	if (!Stream)
	{
		return std::unexpected(FShaderError{"Cannot open cooked shader: " + Path.string()});
	}

	const auto Size = Stream.tellg();
	if (Size < 0 || Size > static_cast<std::streamoff>(MaximumAssetSize))
	{
		return std::unexpected(FShaderError{"Invalid cooked shader file size: " + Path.string()});
	}

	std::vector<std::byte> Bytes(static_cast<std::size_t>(Size));
	Stream.seekg(0);
	if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), Size))
	{
		return std::unexpected(FShaderError{"Cannot read cooked shader: " + Path.string()});
	}

	return DeserializeCookedShader(Bytes);
}
}
