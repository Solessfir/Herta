#include "Herta/Platform/DynamicLibrary.h"

#include <format>
#include <utility>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>
#else
	#include <dlfcn.h>
#endif

namespace Herta
{
std::expected<FDynamicLibrary, std::string> FDynamicLibrary::Open(const std::filesystem::path& Path)
{
	FDynamicLibrary Library;
#ifdef HERTA_PLATFORM_WINDOWS
	// The library's own folder comes first, so a module finds libraries shipped beside it before any on the PATH.
	Library.Handle = LoadLibraryExW(Path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (!Library.Handle)
	{
		return std::unexpected(std::format("Cannot load {}: Windows error {}", Path.string(), GetLastError()));
	}
#else
	// Local symbols keep two modules, or two versions of one during a reload, from binding to each other's functions.
	Library.Handle = dlopen(Path.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!Library.Handle)
	{
		const char* const Error = dlerror();
		return std::unexpected(std::format("Cannot load {}: {}", Path.string(), Error ? Error : "unknown error"));
	}
#endif

	return Library;
}

FDynamicLibrary::~FDynamicLibrary()
{
	Close();
}

FDynamicLibrary::FDynamicLibrary(FDynamicLibrary&& Other) noexcept
    : Handle(std::exchange(Other.Handle, nullptr))
{
}

FDynamicLibrary& FDynamicLibrary::operator=(FDynamicLibrary&& Other) noexcept
{
	if (this != &Other)
	{
		Close();
		Handle = std::exchange(Other.Handle, nullptr);
	}

	return *this;
}

void* FDynamicLibrary::FindSymbol(const char* const Name) const noexcept
{
	if (!Handle)
	{
		return nullptr;
	}

#ifdef HERTA_PLATFORM_WINDOWS
	return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(Handle), Name));
#else
	return dlsym(Handle, Name);
#endif
}

bool FDynamicLibrary::IsOpen() const noexcept
{
	return Handle != nullptr;
}

void FDynamicLibrary::Close() noexcept
{
	if (!Handle)
	{
		return;
	}

#ifdef HERTA_PLATFORM_WINDOWS
	FreeLibrary(static_cast<HMODULE>(std::exchange(Handle, nullptr)));
#else
	dlclose(std::exchange(Handle, nullptr));
#endif
}

std::string GetDynamicLibraryFileName(const std::string_view Name)
{
#ifdef HERTA_PLATFORM_WINDOWS
	return std::format("{}.dll", Name);
#else
	return std::format("lib{}.so", Name);
#endif
}
}
