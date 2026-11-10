#include "TestFiles.h"

#include "Herta/Platform/Process.h"

#include <atomic>
#include <chrono>
#include <format>
#include <fstream>
#include <random>

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace Herta::Tests
{
std::filesystem::path MakeScratchPath(const std::string_view Prefix)
{
	// The counter separates paths within one process; the random part separates processes that start at the same tick.
	static std::atomic<std::uint64_t> Counter{0};
	static const std::uint64_t ProcessSalt = (static_cast<std::uint64_t>(std::random_device{}()) << 32) | std::random_device{}();
	return std::filesystem::temp_directory_path() / std::format("{}-{:016x}-{}-{}", Prefix, ProcessSalt, std::chrono::steady_clock::now().time_since_epoch().count(), Counter.fetch_add(1));
}

FScratchDirectory::FScratchDirectory(const std::string_view Prefix)
    : Path(MakeScratchPath(Prefix))
{
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

std::vector<std::byte> EncodeWav(const std::uint32_t SampleRate, const std::uint32_t Channels, const std::span<const std::int16_t> Samples)
{
	std::vector<std::byte> Wav;
	const auto Put = [&Wav](const std::uint64_t Value, const std::size_t Size)
	{
		for (std::size_t Byte = 0; Byte < Size; ++Byte)
		{
			Wav.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		}
	};

	const auto Tag = [&Wav](const std::string_view Text)
	{
		for (const char Character : Text)
		{
			Wav.push_back(static_cast<std::byte>(Character));
		}
	};

	const auto Bytes = static_cast<std::uint32_t>(Samples.size() * 2);
	Tag("RIFF");
	Put(36 + Bytes, 4);
	Tag("WAVEfmt ");
	Put(16, 4);
	Put(1, 2);
	Put(Channels, 2);
	Put(SampleRate, 4);
	Put(std::uint64_t{SampleRate} * Channels * 2, 4);
	Put(Channels * 2, 2);
	Put(16, 2);
	Tag("data");
	Put(Bytes, 4);
	for (const std::int16_t Sample : Samples)
	{
		Put(static_cast<std::uint16_t>(Sample), 2);
	}

	return Wav;
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
