#include "Herta/EditorCore/EditorPlugin.h"
#include "TerminalText.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef HERTA_PLATFORM_WINDOWS
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#include <windows.h>
#else
	#include <csignal>
	#include <cstdlib>
	#include <fcntl.h>
	#include <poll.h>
	#include <pty.h>
	#include <sys/ioctl.h>
	#include <sys/wait.h>
	#include <unistd.h>

extern char** environ;
#endif

namespace Herta
{
namespace
{
constexpr short Columns = 160;
constexpr short Rows = 40;

// A shell attached to a pseudo terminal. The editor's thread only swaps buffers under a lock: a reader thread drains the output and a writer
// thread feeds input, so a shell that stops reading or floods output never holds a frame.
class FTerminalSession final
{
public:
	FTerminalSession() = default;
	FTerminalSession(const FTerminalSession&) = delete;
	FTerminalSession& operator=(const FTerminalSession&) = delete;
	FTerminalSession(FTerminalSession&&) = delete;
	FTerminalSession& operator=(FTerminalSession&&) = delete;
	~FTerminalSession();

	[[nodiscard]] std::string Start(const std::string& WorkingDirectory);
	void Write(std::string_view Bytes);
	// Output since the last call, and whether the shell has exited.
	[[nodiscard]] std::string TakeOutput(bool& bExited);

private:
	void ReadOutput();
	void WriteInput();
	void Stop();

	std::mutex Mutex;
	std::condition_variable InputReady;
	std::string Output;
	std::string Input;
	bool bStopping = false;
	std::atomic<bool> bExited = false;

#ifdef HERTA_PLATFORM_WINDOWS
	HPCON Console = nullptr;
	HANDLE InputWrite = nullptr;
	HANDLE OutputRead = nullptr;
	HANDLE Process = nullptr;
#else
	int Master = -1;
	pid_t Child = -1;
#endif

	// Declared last, so they are joined before the handles they use close.
	std::thread Reader;
	std::thread Writer;
};

FTerminalSession::~FTerminalSession()
{
	Stop();
}

#ifdef HERTA_PLATFORM_WINDOWS
[[nodiscard]] std::wstring Widen(const std::string_view Text)
{
	std::wstring Result(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), static_cast<int>(Result.size()));
	return Result;
}

// PowerShell 7, then Windows PowerShell, then the command processor.
[[nodiscard]] std::wstring FindShell()
{
	for (const wchar_t* const Name : {L"pwsh.exe", L"powershell.exe"})
	{
		std::wstring Path(MAX_PATH, L'\0');
		const DWORD Length = SearchPathW(nullptr, Name, nullptr, static_cast<DWORD>(Path.size()), Path.data(), nullptr);
		if (Length > 0 && Length < Path.size())
		{
			Path.resize(Length);
			return Path;
		}
	}

	std::wstring ComSpec(MAX_PATH, L'\0');
	const DWORD Length = GetEnvironmentVariableW(L"ComSpec", ComSpec.data(), static_cast<DWORD>(ComSpec.size()));
	ComSpec.resize(Length > 0 && Length < ComSpec.size() ? Length : 0);
	return ComSpec.empty() ? std::wstring(L"cmd.exe") : ComSpec;
}

std::string FTerminalSession::Start(const std::string& WorkingDirectory)
{
	HANDLE InputRead = nullptr;
	HANDLE OutputWrite = nullptr;
	if (!CreatePipe(&InputRead, &InputWrite, nullptr, 0) || !CreatePipe(&OutputRead, &OutputWrite, nullptr, 0))
	{
		return std::format("Cannot create the terminal's pipes (Windows error {})", GetLastError());
	}

	const HRESULT Created = CreatePseudoConsole({Columns, Rows}, InputRead, OutputWrite, 0, &Console);
	// The console holds its own references; ours would keep the pipes open after it closes.
	CloseHandle(InputRead);
	CloseHandle(OutputWrite);
	if (FAILED(Created))
	{
		Console = nullptr;
		return std::format("Cannot create a pseudo console (HRESULT {:#x})", static_cast<unsigned long>(Created));
	}

	SIZE_T AttributeSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributeSize);
	std::vector<std::byte> AttributeStorage(AttributeSize);
	const auto Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributeStorage.data());
	STARTUPINFOEXW Startup{};
	Startup.StartupInfo.cb = sizeof(Startup);
	// Null standard handles, or a shell started from a console program such as the editor writes to that console instead of the pseudo one.
	Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	Startup.lpAttributeList = Attributes;
	if (!InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributeSize) || !UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, Console, sizeof(Console), nullptr, nullptr))
	{
		return std::format("Cannot attach the pseudo console (Windows error {})", GetLastError());
	}

	std::wstring CommandLine = L"\"" + FindShell() + L"\"";
	const std::wstring Directory = Widen(WorkingDirectory);
	PROCESS_INFORMATION Information{};
	const BOOL bStarted = CreateProcessW(nullptr, CommandLine.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, Directory.empty() ? nullptr : Directory.c_str(), &Startup.StartupInfo, &Information);
	DeleteProcThreadAttributeList(Attributes);
	if (!bStarted)
	{
		return std::format("Cannot start the shell (Windows error {})", GetLastError());
	}

	CloseHandle(Information.hThread);
	Process = Information.hProcess;
	Reader = std::thread([this]
	{
		ReadOutput();
	});

	Writer = std::thread([this]
	{
		WriteInput();
	});

	return {};
}

void FTerminalSession::ReadOutput()
{
	std::vector<char> Buffer(16384);
	DWORD Read = 0;
	while (ReadFile(OutputRead, Buffer.data(), static_cast<DWORD>(Buffer.size()), &Read, nullptr) && Read > 0)
	{
		const std::scoped_lock Lock(Mutex);
		Output.append(Buffer.data(), Read);
	}

	bExited = true;
}

void FTerminalSession::WriteInput()
{
	std::unique_lock Lock(Mutex);
	while (true)
	{
		InputReady.wait(Lock, [this]
		{
			return bStopping || !Input.empty();
		});

		if (bStopping)
		{
			return;
		}

		const std::string Bytes = std::exchange(Input, {});
		Lock.unlock();
		DWORD Written = 0;
		const bool bWritten = WriteFile(InputWrite, Bytes.data(), static_cast<DWORD>(Bytes.size()), &Written, nullptr);
		Lock.lock();
		if (!bWritten)
		{
			return;
		}
	}
}

void FTerminalSession::Stop()
{
	{
		const std::scoped_lock Lock(Mutex);
		bStopping = true;
	}

	InputReady.notify_all();
	// Closing the console ends the shell and everything it started, then breaks the output pipe, which ends the reader. The reader must keep
	// draining until then, since the console waits for its last output to be read.
	if (Console)
	{
		ClosePseudoConsole(Console);
	}

	if (Writer.joinable())
	{
		CancelSynchronousIo(Writer.native_handle());
		Writer.join();
	}

	if (Reader.joinable())
	{
		CancelSynchronousIo(Reader.native_handle());
		Reader.join();
	}

	for (HANDLE Handle : {InputWrite, OutputRead, Process})
	{
		if (Handle)
		{
			CloseHandle(Handle);
		}
	}

	Console = nullptr;
	InputWrite = OutputRead = Process = nullptr;
}
#else
std::string FTerminalSession::Start(const std::string& WorkingDirectory)
{
	const winsize Size{.ws_row = Rows, .ws_col = Columns, .ws_xpixel = 0, .ws_ypixel = 0};
	int Slave = -1;
	if (openpty(&Master, &Slave, nullptr, nullptr, &Size) != 0)
	{
		return std::format("Cannot open a pseudo terminal (errno {})", errno);
	}

	const char* const ShellVariable = std::getenv("SHELL");
	const std::string Shell = ShellVariable && *ShellVariable && access(ShellVariable, X_OK) == 0 ? ShellVariable : "/bin/sh";
	// The panel shows text without cursor addressing, so programs should not draw full-screen interfaces. The environment is built before the
	// fork, since the child of a threaded process may only make native calls until exec.
	std::vector<std::string> Environment{"TERM=dumb"};
	for (char* const* Entry = environ; *Entry != nullptr; ++Entry)
	{
		if (!std::string_view(*Entry).starts_with("TERM="))
		{
			Environment.emplace_back(*Entry);
		}
	}

	std::vector<char*> EnvironmentPointers;
	EnvironmentPointers.reserve(Environment.size() + 1);
	for (std::string& Variable : Environment)
	{
		EnvironmentPointers.push_back(Variable.data());
	}

	EnvironmentPointers.push_back(nullptr);
	const std::array<const char*, 3> Arguments{Shell.c_str(), "-i", nullptr};
	Child = fork();
	if (Child < 0)
	{
		close(Slave);
		return std::format("Cannot start the shell (errno {})", errno);
	}

	if (Child == 0)
	{
		// A session of its own with the terminal as its controlling one, so job control and Ctrl+C reach the shell, not the editor.
		setsid();
		ioctl(Slave, TIOCSCTTY, 0);
		dup2(Slave, STDIN_FILENO);
		dup2(Slave, STDOUT_FILENO);
		dup2(Slave, STDERR_FILENO);
		close(Slave);
		close(Master);
		if (!WorkingDirectory.empty() && chdir(WorkingDirectory.c_str()) != 0)
		{
			_exit(126);
		}

		execve(Shell.c_str(), const_cast<char* const*>(Arguments.data()), EnvironmentPointers.data());
		_exit(127);
	}

	close(Slave);
	fcntl(Master, F_SETFD, FD_CLOEXEC);
	Reader = std::thread([this]
	{
		ReadOutput();
	});

	Writer = std::thread([this]
	{
		WriteInput();
	});

	return {};
}

void FTerminalSession::ReadOutput()
{
	std::vector<char> Buffer(16384);
	while (true)
	{
		{
			const std::scoped_lock Lock(Mutex);
			if (bStopping)
			{
				break;
			}
		}

		// Polling with a timeout lets Stop end the reader even while a background job keeps the terminal open.
		pollfd Descriptor{.fd = Master, .events = POLLIN, .revents = 0};
		const int Ready = poll(&Descriptor, 1, 100);
		if (Ready < 0 && errno != EINTR)
		{
			break;
		}

		if (Ready <= 0)
		{
			continue;
		}

		const ssize_t Read = read(Master, Buffer.data(), Buffer.size());
		if (Read <= 0)
		{
			break;
		}

		const std::scoped_lock Lock(Mutex);
		Output.append(Buffer.data(), static_cast<std::size_t>(Read));
	}

	bExited = true;
}

void FTerminalSession::WriteInput()
{
	std::unique_lock Lock(Mutex);
	while (true)
	{
		InputReady.wait(Lock, [this]
		{
			return bStopping || !Input.empty();
		});

		if (bStopping)
		{
			return;
		}

		std::string Bytes = std::exchange(Input, {});
		Lock.unlock();
		std::string_view Pending = Bytes;
		while (!Pending.empty())
		{
			pollfd Descriptor{.fd = Master, .events = POLLOUT, .revents = 0};
			if (poll(&Descriptor, 1, 100) <= 0)
			{
				const std::scoped_lock Check(Mutex);
				if (bStopping)
				{
					return;
				}

				continue;
			}

			const ssize_t Written = write(Master, Pending.data(), Pending.size());
			if (Written <= 0)
			{
				return;
			}

			Pending.remove_prefix(static_cast<std::size_t>(Written));
		}

		Lock.lock();
	}
}

void FTerminalSession::Stop()
{
	{
		const std::scoped_lock Lock(Mutex);
		bStopping = true;
	}

	InputReady.notify_all();
	if (Child > 0)
	{
		kill(-Child, SIGHUP);
		kill(Child, SIGKILL);
	}

	if (Writer.joinable())
	{
		Writer.join();
	}

	if (Reader.joinable())
	{
		Reader.join();
	}

	if (Child > 0)
	{
		waitpid(Child, nullptr, 0);
		Child = -1;
	}

	if (Master >= 0)
	{
		close(Master);
		Master = -1;
	}
}
#endif

void FTerminalSession::Write(std::string_view Bytes)
{
	{
		const std::scoped_lock Lock(Mutex);
		Input += Bytes;
	}

	InputReady.notify_one();
}

std::string FTerminalSession::TakeOutput(bool& bHasExited)
{
	bHasExited = bExited;
	const std::scoped_lock Lock(Mutex);
	return std::exchange(Output, {});
}

class FTerminalPanel final : public IEditorPanel
{
public:
	explicit FTerminalPanel(IEditorPluginHost& InHost)
	    : Host(InHost)
	{
	}

	void Draw(IEditorPluginUI& UI) override
	{
		if (!Session && !bFailed)
		{
			Restart();
		}

		bool bExited = false;
		if (Session)
		{
			Text.Append(Session->TakeOutput(bExited));
		}

		if (UI.Button("Restart"))
		{
			Restart();
			bFocusInput = true;
		}

		UI.SameLine();
		UI.BeginDisabled(!Session || bExited);
		if (UI.Button("Interrupt"))
		{
			// Ctrl+C, which the terminal turns into an interrupt for the running command.
			Session->Write("\x03");
			bFocusInput = true;
		}

		UI.EndDisabled();
		UI.SameLine();
		if (UI.Button("Clear"))
		{
			Text.Clear();
			bFocusInput = true;
		}

		if (bExited)
		{
			UI.SameLine();
			UI.TextDisabled("The shell has exited.");
		}

		UI.BeginScrollRegion("##Output", -1.5f, true);
		const std::deque<std::string>& Lines = Text.GetLines();
		UI.MonospaceLines(Lines.size(), [&Lines](const std::size_t Index)
		{
			return std::string_view(Lines[Index]);
		});

		UI.EndScrollRegion();
		if (bFocusInput)
		{
			UI.FocusNext();
			bFocusInput = false;
		}

		if (UI.InputText("##Input", Line, "Type a command and press Enter") && Session && !bExited)
		{
			// The terminal echoes what it receives, so the line is not added to the output here.
			Session->Write(Line + "\r");
			Line.clear();
			bFocusInput = true;
		}
	}

private:
	void Restart()
	{
		Session.reset();
		Session = std::make_unique<FTerminalSession>();
		const std::string Error = Session->Start(Host.GetProjectDirectory().empty() ? Host.GetEngineDirectory() : Host.GetProjectDirectory());
		bFailed = !Error.empty();
		if (bFailed)
		{
			Host.Log(EEditorPluginLog::Warning, Error);
			Text.Append(Error + "\n");
			Session.reset();
		}
	}

	IEditorPluginHost& Host;
	FTerminalText Text;
	std::string Line;
	std::unique_ptr<FTerminalSession> Session;
	bool bFailed = false;
	bool bFocusInput = false;
};

void Register(IEditorPluginRegistry& Registry)
{
	Registry.AddPanel("Terminal", std::make_unique<FTerminalPanel>(Registry.GetHost()));
}
}
}

HERTA_EDITOR_PLUGIN(Herta::Register)
