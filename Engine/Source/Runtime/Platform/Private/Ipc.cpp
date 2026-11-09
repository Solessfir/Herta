#include "Herta/Platform/Ipc.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <vector>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <windows.h>

	#include <sddl.h>
#else
	#include <fcntl.h>
	#include <poll.h>
	#include <sys/socket.h>
	#include <sys/stat.h>
	#include <sys/un.h>
	#include <unistd.h>

	#include <cerrno>
	#include <cstdlib>
#endif

namespace Herta
{
namespace
{
using FMilliseconds = std::chrono::milliseconds;

[[nodiscard]] std::unexpected<FIpcError> Fail(std::string Message, const bool bDisconnected = false)
{
	return std::unexpected(FIpcError{.Message = std::move(Message), .bDisconnected = bDisconnected});
}

#ifdef HERTA_PLATFORM_WINDOWS
[[nodiscard]] std::wstring Widen(const std::string_view Text)
{
	const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
	std::wstring Wide(static_cast<std::size_t>(std::max(Length, 0)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Wide.data(), Length);
	return Wide;
}

[[nodiscard]] std::string SystemMessage(const DWORD Error)
{
	return std::format("system error {}", Error);
}

struct FHandle
{
	HANDLE Value = INVALID_HANDLE_VALUE;

	FHandle() = default;

	explicit FHandle(const HANDLE InValue) noexcept
	    : Value(InValue)
	{
	}

	FHandle(const FHandle&) = delete;

	FHandle(FHandle&& Other) noexcept
	    : Value(std::exchange(Other.Value, INVALID_HANDLE_VALUE))
	{
	}

	FHandle& operator=(const FHandle&) = delete;

	FHandle& operator=(FHandle&& Other) noexcept
	{
		std::swap(Value, Other.Value);
		return *this;
	}

	~FHandle()
	{
		if (Value != INVALID_HANDLE_VALUE && Value != nullptr)
		{
			CloseHandle(Value);
		}
	}
};

// Waits for an overlapped operation, cancelling it on timeout so the buffer is free again when this returns.
[[nodiscard]] std::expected<std::optional<DWORD>, FIpcError> Finish(const HANDLE Pipe, OVERLAPPED& Overlapped, const BOOL bStarted, const FMilliseconds Timeout)
{
	DWORD Bytes = 0;
	if (bStarted)
	{
		if (!GetOverlappedResult(Pipe, &Overlapped, &Bytes, FALSE))
		{
			return Fail(SystemMessage(GetLastError()), true);
		}

		return Bytes;
	}

	const DWORD Error = GetLastError();
	if (Error != ERROR_IO_PENDING)
	{
		return Fail(SystemMessage(Error), Error == ERROR_BROKEN_PIPE || Error == ERROR_NO_DATA || Error == ERROR_PIPE_NOT_CONNECTED);
	}

	if (WaitForSingleObject(Overlapped.hEvent, static_cast<DWORD>(std::clamp<long long>(Timeout.count(), 0, INFINITE - 1))) == WAIT_TIMEOUT)
	{
		CancelIoEx(Pipe, &Overlapped);
		if (GetOverlappedResult(Pipe, &Overlapped, &Bytes, TRUE) || GetLastError() != ERROR_OPERATION_ABORTED)
		{
			return Bytes > 0 ? std::optional(Bytes) : std::nullopt;
		}

		return std::nullopt;
	}

	if (!GetOverlappedResult(Pipe, &Overlapped, &Bytes, FALSE))
	{
		const DWORD Failed = GetLastError();
		return Fail(SystemMessage(Failed), Failed == ERROR_BROKEN_PIPE || Failed == ERROR_PIPE_NOT_CONNECTED);
	}

	return Bytes;
}
#else
[[nodiscard]] std::string SystemMessage(const int Error)
{
	return std::format("system error {}", Error);
}

struct FHandle
{
	int Value = -1;

	FHandle() = default;

	explicit FHandle(const int InValue) noexcept
	    : Value(InValue)
	{
	}

	FHandle(const FHandle&) = delete;

	FHandle(FHandle&& Other) noexcept
	    : Value(std::exchange(Other.Value, -1))
	{
	}

	FHandle& operator=(const FHandle&) = delete;

	FHandle& operator=(FHandle&& Other) noexcept
	{
		std::swap(Value, Other.Value);
		return *this;
	}

	~FHandle()
	{
		if (Value >= 0)
		{
			close(Value);
		}
	}
};

[[nodiscard]] int PollTimeout(const FMilliseconds Timeout)
{
	return static_cast<int>(std::clamp<long long>(Timeout.count(), 0, 1'000'000'000));
}

[[nodiscard]] std::expected<sockaddr_un, FIpcError> MakeAddress(const std::string_view Endpoint)
{
	sockaddr_un Address{};
	Address.sun_family = AF_UNIX;
	if (Endpoint.empty() || Endpoint.size() >= sizeof(Address.sun_path))
	{
		return Fail("IPC endpoint path is empty or too long for a Unix socket");
	}

	std::memcpy(Address.sun_path, Endpoint.data(), Endpoint.size());
	return Address;
}
#endif
}

struct FIpcConnection::FImplementation
{
	FHandle Pipe;
	std::vector<char> Pending;

#ifdef HERTA_PLATFORM_WINDOWS
	FHandle ReadEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
	FHandle WriteEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};

	// Reads whatever arrives within Timeout; empty when nothing does.
	[[nodiscard]] std::expected<std::size_t, FIpcError> ReadSome(const FMilliseconds Timeout)
	{
		std::array<char, 64 * 1024> Buffer{};
		OVERLAPPED Overlapped{};
		Overlapped.hEvent = ReadEvent.Value;
		ResetEvent(ReadEvent.Value);
		const BOOL bStarted = ReadFile(Pipe.Value, Buffer.data(), static_cast<DWORD>(Buffer.size()), nullptr, &Overlapped);
		const auto Read = Finish(Pipe.Value, Overlapped, bStarted, Timeout);
		if (!Read)
		{
			return std::unexpected(Read.error());
		}

		if (*Read && **Read == 0 && bStarted)
		{
			return Fail("The other side closed the connection", true);
		}

		const std::size_t Bytes = Read->value_or(0);
		Pending.insert(Pending.end(), Buffer.data(), Buffer.data() + Bytes);
		return Bytes;
	}

	[[nodiscard]] std::expected<void, FIpcError> WriteAll(const std::string_view Bytes, const FMilliseconds Timeout)
	{
		std::size_t Written = 0;
		while (Written < Bytes.size())
		{
			OVERLAPPED Overlapped{};
			Overlapped.hEvent = WriteEvent.Value;
			ResetEvent(WriteEvent.Value);
			const DWORD Chunk = static_cast<DWORD>(std::min<std::size_t>(Bytes.size() - Written, 1 << 20));
			const BOOL bStarted = WriteFile(Pipe.Value, Bytes.data() + Written, Chunk, nullptr, &Overlapped);
			const auto Result = Finish(Pipe.Value, Overlapped, bStarted, Timeout);
			if (!Result)
			{
				return std::unexpected(Result.error());
			}

			if (!*Result || **Result == 0)
			{
				return Fail("Timed out sending an IPC message");
			}

			Written += **Result;
		}

		return {};
	}
#else
	[[nodiscard]] std::expected<std::size_t, FIpcError> ReadSome(const FMilliseconds Timeout)
	{
		pollfd Poll{.fd = Pipe.Value, .events = POLLIN, .revents = 0};
		const int Ready = poll(&Poll, 1, PollTimeout(Timeout));
		if (Ready < 0)
		{
			return errno == EINTR ? std::expected<std::size_t, FIpcError>(0) : Fail(SystemMessage(errno));
		}

		if (Ready == 0)
		{
			return 0;
		}

		std::array<char, 64 * 1024> Buffer{};
		const ssize_t Read = recv(Pipe.Value, Buffer.data(), Buffer.size(), 0);
		if (Read == 0)
		{
			return Fail("The other side closed the connection", true);
		}

		if (Read < 0)
		{
			return errno == EINTR || errno == EAGAIN ? std::expected<std::size_t, FIpcError>(0) : Fail(SystemMessage(errno), errno == ECONNRESET);
		}

		Pending.insert(Pending.end(), Buffer.data(), Buffer.data() + Read);
		return static_cast<std::size_t>(Read);
	}

	[[nodiscard]] std::expected<void, FIpcError> WriteAll(const std::string_view Bytes, const FMilliseconds Timeout)
	{
		std::size_t Written = 0;
		while (Written < Bytes.size())
		{
			pollfd Poll{.fd = Pipe.Value, .events = POLLOUT, .revents = 0};
			const int Ready = poll(&Poll, 1, PollTimeout(Timeout));
			if (Ready <= 0)
			{
				return Ready == 0 ? Fail("Timed out sending an IPC message") : Fail(SystemMessage(errno));
			}

			// MSG_NOSIGNAL turns a closed peer into an error instead of a process-killing SIGPIPE.
			const ssize_t Sent = send(Pipe.Value, Bytes.data() + Written, Bytes.size() - Written, MSG_NOSIGNAL);
			if (Sent < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
				{
					continue;
				}

				return Fail(SystemMessage(errno), errno == EPIPE || errno == ECONNRESET);
			}

			Written += static_cast<std::size_t>(Sent);
		}

		return {};
	}
#endif
};

FIpcConnection::FIpcConnection(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FIpcConnection::FIpcConnection(FIpcConnection&&) noexcept = default;

FIpcConnection& FIpcConnection::operator=(FIpcConnection&&) noexcept = default;

FIpcConnection::~FIpcConnection() = default;

std::expected<FIpcConnection, FIpcError> FIpcConnection::Connect(const std::string_view Endpoint, const FMilliseconds Timeout)
{
	auto Implementation = std::make_unique<FImplementation>();
#ifdef HERTA_PLATFORM_WINDOWS
	const std::wstring Name = Widen(Endpoint);
	const auto Deadline = std::chrono::steady_clock::now() + Timeout;
	while (true)
	{
		Implementation->Pipe = FHandle(CreateFileW(Name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
		if (Implementation->Pipe.Value != INVALID_HANDLE_VALUE)
		{
			break;
		}

		const DWORD Error = GetLastError();
		const auto Remaining = std::chrono::duration_cast<FMilliseconds>(Deadline - std::chrono::steady_clock::now());
		if (Error != ERROR_PIPE_BUSY || Remaining.count() <= 0)
		{
			return Fail(Error == ERROR_FILE_NOT_FOUND ? std::string("Nothing is listening on the IPC endpoint") : SystemMessage(Error));
		}

		WaitNamedPipeW(Name.c_str(), static_cast<DWORD>(Remaining.count()));
	}
#else
	(void)Timeout;
	const auto Address = MakeAddress(Endpoint);
	if (!Address)
	{
		return std::unexpected(Address.error());
	}

	Implementation->Pipe = FHandle(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	if (Implementation->Pipe.Value < 0 || connect(Implementation->Pipe.Value, reinterpret_cast<const sockaddr*>(&*Address), sizeof(*Address)) != 0)
	{
		return Fail(errno == ENOENT || errno == ECONNREFUSED ? std::string("Nothing is listening on the IPC endpoint") : SystemMessage(errno));
	}
#endif

	return FIpcConnection(std::move(Implementation));
}

std::expected<void, FIpcError> FIpcConnection::Send(const std::string_view Message, const FMilliseconds Timeout)
{
	if (Message.size() > MaximumIpcMessageBytes)
	{
		return Fail("IPC message exceeds the 16 MiB limit");
	}

	std::string Frame(4 + Message.size(), '\0');
	const auto Length = static_cast<std::uint32_t>(Message.size());

	for (std::size_t Byte = 0; Byte < 4; ++Byte)
	{
		Frame[Byte] = static_cast<char>((Length >> (Byte * 8)) & 0xffu);
	}

	std::ranges::copy(Message, Frame.begin() + 4);
	return Implementation->WriteAll(Frame, Timeout);
}

std::expected<std::optional<std::string>, FIpcError> FIpcConnection::Receive(const FMilliseconds Timeout)
{
	const auto Deadline = std::chrono::steady_clock::now() + Timeout;
	while (true)
	{
		std::vector<char>& Pending = Implementation->Pending;
		if (Pending.size() >= 4)
		{
			std::uint32_t Length = 0;

			for (std::size_t Byte = 0; Byte < 4; ++Byte)
			{
				Length |= static_cast<std::uint32_t>(static_cast<unsigned char>(Pending[Byte])) << (Byte * 8);
			}

			if (Length > MaximumIpcMessageBytes)
			{
				return Fail("Received an IPC message over the 16 MiB limit");
			}

			if (Pending.size() >= 4 + std::size_t{Length})
			{
				std::string Message(Pending.begin() + 4, Pending.begin() + 4 + Length);
				Pending.erase(Pending.begin(), Pending.begin() + 4 + Length);
				return Message;
			}
		}

		const auto Remaining = std::chrono::duration_cast<FMilliseconds>(Deadline - std::chrono::steady_clock::now());
		const auto Read = Implementation->ReadSome(std::max(Remaining, FMilliseconds{0}));
		if (!Read)
		{
			return std::unexpected(Read.error());
		}

		if (*Read == 0 && std::chrono::steady_clock::now() >= Deadline)
		{
			return std::nullopt;
		}
	}
}

struct FIpcListener::FImplementation
{
	std::string Endpoint;
#ifdef HERTA_PLATFORM_WINDOWS
	FHandle Waiting;
	FHandle ConnectEvent{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
	OVERLAPPED Overlapped{};
	bool bConnectPending = false;
	bool bFirst = true;

	[[nodiscard]] std::expected<void, FIpcError> CreateInstance()
	{
		// Only the user who started the editor may open the pipe, and never from another machine.
		PSECURITY_DESCRIPTOR Descriptor = nullptr;
		if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &Descriptor, nullptr))
		{
			return Fail(SystemMessage(GetLastError()));
		}

		SECURITY_ATTRIBUTES Attributes{.nLength = sizeof(SECURITY_ATTRIBUTES), .lpSecurityDescriptor = Descriptor, .bInheritHandle = FALSE};
		const DWORD Mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (bFirst ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
		Waiting = FHandle(CreateNamedPipeW(Widen(Endpoint).c_str(), Mode, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &Attributes));
		const DWORD Error = GetLastError();
		LocalFree(Descriptor);
		if (Waiting.Value == INVALID_HANDLE_VALUE)
		{
			return Fail(Error == ERROR_ACCESS_DENIED ? std::string("Another process already listens on this IPC endpoint") : SystemMessage(Error));
		}

		bFirst = false;
		bConnectPending = false;
		return {};
	}
#else
	FHandle Socket;
#endif

	FImplementation() = default;
	FImplementation(const FImplementation&) = delete;
	FImplementation(FImplementation&&) = delete;
	FImplementation& operator=(const FImplementation&) = delete;
	FImplementation& operator=(FImplementation&&) = delete;

	~FImplementation()
	{
#ifdef HERTA_PLATFORM_WINDOWS
		if (bConnectPending)
		{
			CancelIoEx(Waiting.Value, &Overlapped);
			DWORD Bytes = 0;
			GetOverlappedResult(Waiting.Value, &Overlapped, &Bytes, TRUE);
		}
#else
		if (Socket.Value >= 0)
		{
			unlink(Endpoint.c_str());
		}
#endif
	}
};

FIpcListener::FIpcListener(std::unique_ptr<FImplementation> InImplementation) noexcept
    : Implementation(std::move(InImplementation))
{
}

FIpcListener::FIpcListener(FIpcListener&&) noexcept = default;

FIpcListener& FIpcListener::operator=(FIpcListener&&) noexcept = default;

FIpcListener::~FIpcListener() = default;

std::expected<FIpcListener, FIpcError> FIpcListener::Listen(const std::string_view Endpoint)
{
	auto Implementation = std::make_unique<FImplementation>();
	Implementation->Endpoint = std::string(Endpoint);
#ifdef HERTA_PLATFORM_WINDOWS
	if (auto Created = Implementation->CreateInstance(); !Created)
	{
		return std::unexpected(Created.error());
	}
#else
	const auto Address = MakeAddress(Endpoint);
	if (!Address)
	{
		return std::unexpected(Address.error());
	}

	// A socket file left by a crashed editor would block the bind; nothing can be listening on it once its owner is gone.
	if (FIpcConnection::Connect(Endpoint, FMilliseconds{0}))
	{
		return Fail("Another process already listens on this IPC endpoint");
	}

	unlink(Implementation->Endpoint.c_str());
	Implementation->Socket = FHandle(socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	const mode_t Previous = umask(0077);
	const bool bBound = Implementation->Socket.Value >= 0 && bind(Implementation->Socket.Value, reinterpret_cast<const sockaddr*>(&*Address), sizeof(*Address)) == 0;
	umask(Previous);
	if (!bBound || chmod(Implementation->Endpoint.c_str(), 0600) != 0 || listen(Implementation->Socket.Value, 4) != 0)
	{
		return Fail(SystemMessage(errno));
	}
#endif

	return FIpcListener(std::move(Implementation));
}

std::expected<std::optional<FIpcConnection>, FIpcError> FIpcListener::Accept(const FMilliseconds Timeout)
{
	FImplementation& Listener = *Implementation;
#ifdef HERTA_PLATFORM_WINDOWS
	if (!Listener.bConnectPending)
	{
		Listener.Overlapped = {};
		Listener.Overlapped.hEvent = Listener.ConnectEvent.Value;
		ResetEvent(Listener.ConnectEvent.Value);
		if (ConnectNamedPipe(Listener.Waiting.Value, &Listener.Overlapped))
		{
			SetEvent(Listener.ConnectEvent.Value);
		}
		else if (const DWORD Error = GetLastError(); Error == ERROR_PIPE_CONNECTED)
		{
			SetEvent(Listener.ConnectEvent.Value);
		}
		else if (Error != ERROR_IO_PENDING)
		{
			return Fail(SystemMessage(Error));
		}

		Listener.bConnectPending = true;
	}

	if (WaitForSingleObject(Listener.ConnectEvent.Value, static_cast<DWORD>(std::clamp<long long>(Timeout.count(), 0, INFINITE - 1))) == WAIT_TIMEOUT)
	{
		return std::nullopt;
	}

	auto Connection = std::make_unique<FIpcConnection::FImplementation>();
	Connection->Pipe = std::move(Listener.Waiting);
	if (auto Created = Listener.CreateInstance(); !Created)
	{
		return std::unexpected(Created.error());
	}

	return std::optional(FIpcConnection(std::move(Connection)));
#else
	pollfd Poll{.fd = Listener.Socket.Value, .events = POLLIN, .revents = 0};
	const int Ready = poll(&Poll, 1, PollTimeout(Timeout));
	if (Ready <= 0)
	{
		return Ready == 0 || errno == EINTR ? std::expected<std::optional<FIpcConnection>, FIpcError>(std::nullopt) : Fail(SystemMessage(errno));
	}

	const int Accepted = accept4(Listener.Socket.Value, nullptr, nullptr, SOCK_CLOEXEC);
	if (Accepted < 0)
	{
		return errno == EINTR || errno == EAGAIN ? std::expected<std::optional<FIpcConnection>, FIpcError>(std::nullopt) : Fail(SystemMessage(errno));
	}

	auto Connection = std::make_unique<FIpcConnection::FImplementation>();
	Connection->Pipe = FHandle(Accepted);
	return std::optional(FIpcConnection(std::move(Connection)));
#endif
}

std::string MakeIpcEndpoint(const std::string_view Name)
{
#ifdef HERTA_PLATFORM_WINDOWS
	return std::format(R"(\\.\pipe\Herta-{})", Name);
#else
	// The runtime directory is private to the user; /tmp is shared, so the user ID keeps names apart there.
	if (const char* const Runtime = std::getenv("XDG_RUNTIME_DIR"); Runtime && *Runtime)
	{
		return std::format("{}/herta-{}.sock", Runtime, Name);
	}

	return std::format("/tmp/herta-{}-{}.sock", getuid(), Name);
#endif
}
}
