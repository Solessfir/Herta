#pragma once

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>

namespace Herta
{
// A shared library loaded into the process: a DLL on Windows and a shared object on Linux. Closing it unloads its code, so nothing from it,
// such as a function pointer or an object with a vtable in it, may outlive the handle.
class FDynamicLibrary final
{
public:
	[[nodiscard]] static std::expected<FDynamicLibrary, std::string> Open(const std::filesystem::path& Path);

	FDynamicLibrary() = default;
	~FDynamicLibrary();
	FDynamicLibrary(const FDynamicLibrary&) = delete;
	FDynamicLibrary& operator=(const FDynamicLibrary&) = delete;
	FDynamicLibrary(FDynamicLibrary&& Other) noexcept;
	FDynamicLibrary& operator=(FDynamicLibrary&& Other) noexcept;

	// The exported symbol, or null when the library has none by that name.
	[[nodiscard]] void* FindSymbol(const char* Name) const noexcept;
	[[nodiscard]] bool IsOpen() const noexcept;
	void Close() noexcept;

private:
	void* Handle = nullptr;
};

// The file name a module named Name has on this platform: Name.dll or libName.so.
[[nodiscard]] std::string GetDynamicLibraryFileName(std::string_view Name);
}
