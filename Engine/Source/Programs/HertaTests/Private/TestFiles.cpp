#include "TestFiles.h"

#include "Herta/Platform/Process.h"

#include <atomic>
#include <chrono>
#include <fstream>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace Herta::Tests
{
FScratchDirectory::FScratchDirectory(const std::string_view Prefix)
{
	static std::atomic<std::uint64_t> Counter{0};
	Path = std::filesystem::temp_directory_path() / (std::string(Prefix) + "-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(Counter.fetch_add(1)));
	std::filesystem::create_directories(Path);
}

FScratchDirectory::~FScratchDirectory()
{
	std::error_code Error;
	std::filesystem::remove_all(Path, Error);
}

void WriteBytes(const std::filesystem::path& Path, const std::span<const std::byte> Bytes)
{
	std::filesystem::create_directories(Path.parent_path());
	std::ofstream Stream(Path, std::ios::binary | std::ios::trunc);
	Stream.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
}

void WriteText(const std::filesystem::path& Path, const std::string_view Text)
{
	WriteBytes(Path, std::as_bytes(std::span(Text.data(), Text.size())));
}

std::vector<std::byte> EncodePng(const std::uint32_t Width, const std::uint32_t Height, const std::span<const std::uint8_t> RgbaPixels)
{
	std::vector<std::byte> Bytes;

	const auto Append = [](void* const Context, void* const Data, const int Size)
	{
		auto& Output = *static_cast<std::vector<std::byte>*>(Context);
		const auto* const First = static_cast<const std::byte*>(Data);
		Output.insert(Output.end(), First, First + Size);
	};

	stbi_write_png_to_func(Append, &Bytes, static_cast<int>(Width), static_cast<int>(Height), 4, RgbaPixels.data(), static_cast<int>(Width * 4));
	return Bytes;
}

void WritePng(const std::filesystem::path& Path, const std::uint32_t Width, const std::uint32_t Height, const std::span<const std::uint8_t> RgbaPixels)
{
	WriteBytes(Path, EncodePng(Width, Height, RgbaPixels));
}

std::filesystem::path GetSiblingExecutable(const std::string_view Name)
{
	const std::filesystem::path Executable = GetExecutablePath();
	std::filesystem::path Sibling = Executable.parent_path() / Name;
	Sibling += Executable.extension();
	return Sibling;
}
}
