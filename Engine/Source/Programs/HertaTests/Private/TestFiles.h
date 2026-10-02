#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta::Tests
{
// A unique temporary directory removed with everything inside it.
class FScratchDirectory final
{
public:
	explicit FScratchDirectory(std::string_view Prefix);
	~FScratchDirectory();
	FScratchDirectory(const FScratchDirectory&) = delete;
	FScratchDirectory& operator=(const FScratchDirectory&) = delete;

	[[nodiscard]] const std::filesystem::path& GetPath() const noexcept
	{
		return Path;
	}

private:
	std::filesystem::path Path;
};

void WriteBytes(const std::filesystem::path& Path, std::span<const std::byte> Bytes);
void WriteText(const std::filesystem::path& Path, std::string_view Text);
// Encodes tightly packed RGBA8 rows as a PNG file.
void WritePng(const std::filesystem::path& Path, std::uint32_t Width, std::uint32_t Height, std::span<const std::uint8_t> RgbaPixels);
[[nodiscard]] std::vector<std::byte> EncodePng(std::uint32_t Width, std::uint32_t Height, std::span<const std::uint8_t> RgbaPixels);
[[nodiscard]] std::filesystem::path GetSiblingExecutable(std::string_view Name);
}
