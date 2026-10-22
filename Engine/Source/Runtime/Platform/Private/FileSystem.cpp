#include "Herta/Platform/FileSystem.h"

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <algorithm>
	#include <chrono>
	#include <thread>
#else
	#include <fcntl.h>
	#include <sys/syscall.h>
	#include <unistd.h>

	#include <cerrno>
#endif

namespace Herta
{
std::expected<void, std::error_code> MoveAtomically(const std::filesystem::path& Source, const std::filesystem::path& Destination, const EMoveMode Mode)
{
#ifdef HERTA_PLATFORM_WINDOWS
	const DWORD Flags = MOVEFILE_WRITE_THROUGH | (Mode == EMoveMode::ReplaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
	for (int Attempt = 0;; ++Attempt)
	{
		if (MoveFileExW(Source.c_str(), Destination.c_str(), Flags) != FALSE)
		{
			return {};
		}

		// A handle opened without FILE_SHARE_DELETE on either path, or on any file inside a moved directory, blocks the rename until it closes.
		const DWORD Error = GetLastError();
		const bool bTransient = Error == ERROR_ACCESS_DENIED || Error == ERROR_SHARING_VIOLATION || Error == ERROR_LOCK_VIOLATION;
		if (!bTransient || Attempt == 20)
		{
			return std::unexpected(std::error_code(static_cast<int>(Error), std::system_category()));
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(std::min(1 << Attempt, 64)));
	}
#else
	const unsigned Flags = Mode == EMoveMode::FailIfExists ? RENAME_NOREPLACE : 0;
	if (syscall(SYS_renameat2, AT_FDCWD, Source.c_str(), AT_FDCWD, Destination.c_str(), Flags) != 0)
	{
		return std::unexpected(std::error_code(errno, std::generic_category()));
	}

	return {};
#endif
}
}
