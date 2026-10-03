#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
// Little-endian regardless of host order so serialized bytes are identical on every platform.
class FBinaryWriter final
{
public:
	template <std::unsigned_integral T>
	void Write(const T Value)
	{
		for (std::size_t Index = 0; Index < sizeof(T); ++Index)
		{
			Bytes.push_back(static_cast<std::byte>((static_cast<std::uint64_t>(Value) >> (Index * 8)) & 0xff));
		}
	}

	void WriteFloat(const float Value)
	{
		Write(std::bit_cast<std::uint32_t>(Value));
	}

	void WriteString(const std::string_view Value)
	{
		Write(static_cast<std::uint32_t>(Value.size()));
		WriteBytes(std::as_bytes(std::span(Value.data(), Value.size())));
	}

	void WriteBytes(const std::span<const std::byte> Data)
	{
		Bytes.insert(Bytes.end(), Data.begin(), Data.end());
	}

	[[nodiscard]] const std::vector<std::byte>& GetBytes() const noexcept
	{
		return Bytes;
	}

	[[nodiscard]] std::vector<std::byte> TakeBytes() noexcept
	{
		return std::move(Bytes);
	}

private:
	std::vector<std::byte> Bytes;
};

// Reads past the end or over a limit invalidate the reader and return zero values, so callers check IsValid once at the end.
class FBinaryReader final
{
public:
	explicit FBinaryReader(const std::span<const std::byte> InBytes) noexcept
	    : Bytes(InBytes)
	{
	}

	template <std::unsigned_integral T>
	[[nodiscard]] T Read() noexcept
	{
		const std::span<const std::byte> Data = ReadBytes(sizeof(T));
		std::uint64_t Value = 0;
		for (std::size_t Index = 0; Index < Data.size(); ++Index)
		{
			Value |= static_cast<std::uint64_t>(Data[Index]) << (Index * 8);
		}

		return static_cast<T>(Value);
	}

	[[nodiscard]] float ReadFloat() noexcept
	{
		return std::bit_cast<float>(Read<std::uint32_t>());
	}

	[[nodiscard]] std::string ReadString(const std::size_t MaximumLength)
	{
		const auto Length = Read<std::uint32_t>();
		if (Length > MaximumLength)
		{
			bValid = false;
			return {};
		}

		const std::span<const std::byte> Data = ReadBytes(Length);
		return {reinterpret_cast<const char*>(Data.data()), Data.size()};
	}

	[[nodiscard]] std::span<const std::byte> ReadBytes(const std::size_t Count) noexcept
	{
		if (!bValid || Count > Bytes.size() - Offset)
		{
			bValid = false;
			return {};
		}

		const std::span<const std::byte> Data = Bytes.subspan(Offset, Count);
		Offset += Count;
		return Data;
	}

	// Rejects counts that could not fit in the remaining bytes before callers allocate for them.
	[[nodiscard]] bool CanRead(const std::uint64_t Count, const std::size_t ElementSize) noexcept
	{
		if (!bValid || ElementSize == 0 || Count > (Bytes.size() - Offset) / ElementSize)
		{
			bValid = false;
		}

		return bValid;
	}

	void Invalidate() noexcept
	{
		bValid = false;
	}

	[[nodiscard]] bool IsValid() const noexcept
	{
		return bValid;
	}

	[[nodiscard]] bool IsAtEnd() const noexcept
	{
		return Offset == Bytes.size();
	}

private:
	std::span<const std::byte> Bytes;
	std::size_t Offset = 0;
	bool bValid = true;
};
}
