#include "Herta/Platform/FileDialog.h"

#include <algorithm>
#include <format>

#ifdef HERTA_PLATFORM_WINDOWS
	#include <shobjidl.h>
	#include <windows.h>
	#include <wrl/client.h>
#else
	#include "Herta/Platform/Process.h"

	#include <cstdlib>
	#include <optional>
	#include <ranges>
#endif

namespace Herta
{
#ifdef HERTA_PLATFORM_WINDOWS
namespace
{
[[nodiscard]] std::wstring Widen(const std::string_view Text)
{
	if (Text.empty())
	{
		return {};
	}

	const int Length = MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), nullptr, 0);
	std::wstring Result(static_cast<std::size_t>(std::max(Length, 0)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int>(Text.size()), Result.data(), Length);
	return Result;
}

// Balances CoInitializeEx when this call initialized COM; a thread already in another apartment is used as is.
class FComScope final
{
public:
	FComScope()
	    : bInitialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
	{
	}

	~FComScope()
	{
		if (bInitialized)
		{
			CoUninitialize();
		}
	}

	FComScope(const FComScope&) = delete;
	FComScope& operator=(const FComScope&) = delete;

private:
	bool bInitialized = false;
};
}

static std::expected<std::vector<std::filesystem::path>, FFileDialogError> OpenFilesDialogImpl(const std::string_view Title, const std::span<const FFileDialogFilter> Filters, const std::filesystem::path& InitialDirectory)
{
	const FComScope Com;
	Microsoft::WRL::ComPtr<IFileOpenDialog> Dialog;
	if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
	{
		return std::unexpected(FFileDialogError{"Could not create the Windows file dialog"});
	}

	FILEOPENDIALOGOPTIONS Options = 0;
	Dialog->GetOptions(&Options);
	Dialog->SetOptions(Options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
	Dialog->SetTitle(Widen(Title).c_str());
	if (!InitialDirectory.empty())
	{
		Microsoft::WRL::ComPtr<IShellItem> Folder;
		if (FAILED(SHCreateItemFromParsingName(InitialDirectory.c_str(), nullptr, IID_PPV_ARGS(&Folder))) || FAILED(Dialog->SetFolder(Folder.Get())))
		{
			return std::unexpected(FFileDialogError{"Could not set the Windows file dialog's initial directory"});
		}
	}

	// COMDLG_FILTERSPEC points into these strings, so they must outlive SetFileTypes.
	std::vector<std::wstring> Names;
	std::vector<std::wstring> Patterns;
	for (const FFileDialogFilter& Filter : Filters)
	{
		std::wstring Pattern;
		for (const std::string& Extension : Filter.Extensions)
		{
			Pattern += (Pattern.empty() ? L"*." : L";*.") + Widen(Extension);
		}

		Names.push_back(Widen(Filter.Name));
		Patterns.push_back(std::move(Pattern));
	}

	std::vector<COMDLG_FILTERSPEC> Specs;
	for (std::size_t Index = 0; Index < Names.size(); ++Index)
	{
		Specs.push_back({Names[Index].c_str(), Patterns[Index].c_str()});
	}

	if (!Specs.empty())
	{
		Dialog->SetFileTypes(static_cast<UINT>(Specs.size()), Specs.data());
	}

	const HRESULT Shown = Dialog->Show(nullptr);
	if (Shown == HRESULT_FROM_WIN32(ERROR_CANCELLED))
	{
		return std::vector<std::filesystem::path>{};
	}

	Microsoft::WRL::ComPtr<IShellItemArray> Items;
	DWORD Count = 0;
	if (FAILED(Shown) || FAILED(Dialog->GetResults(&Items)) || FAILED(Items->GetCount(&Count)))
	{
		return std::unexpected(FFileDialogError{std::format("The Windows file dialog failed with HRESULT 0x{:08x}", static_cast<unsigned long>(Shown))});
	}

	std::vector<std::filesystem::path> Paths;
	for (DWORD Index = 0; Index < Count; ++Index)
	{
		Microsoft::WRL::ComPtr<IShellItem> Item;
		PWSTR Name = nullptr;
		if (SUCCEEDED(Items->GetItemAt(Index, &Item)) && SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Name)))
		{
			Paths.emplace_back(Name);
			CoTaskMemFree(Name);
		}
	}

	return Paths;
}
#else
namespace
{
// posix_spawn does not search PATH, so the dialog tools are resolved here.
[[nodiscard]] std::optional<std::filesystem::path> FindOnPath(const std::string_view Name)
{
	const char* const Path = std::getenv("PATH");
	if (Path == nullptr)
	{
		return std::nullopt;
	}

	std::error_code Error;
	for (const auto Part : std::views::split(std::string_view(Path), ':'))
	{
		const std::filesystem::path Candidate = std::filesystem::path(std::string_view(Part.begin(), Part.end())) / Name;
		if (!Candidate.parent_path().empty() && std::filesystem::is_regular_file(Candidate, Error))
		{
			return Candidate;
		}
	}

	return std::nullopt;
}

[[nodiscard]] std::vector<std::filesystem::path> SplitLines(const std::string& Output)
{
	std::vector<std::filesystem::path> Paths;
	for (const auto Part : std::views::split(std::string_view(Output), '\n'))
	{
		const std::string_view Line(Part.begin(), Part.end());
		if (!Line.empty())
		{
			Paths.emplace_back(Line);
		}
	}

	return Paths;
}
}

static std::expected<std::vector<std::filesystem::path>, FFileDialogError> OpenFilesDialogImpl(const std::string_view Title, const std::span<const FFileDialogFilter> Filters, const std::filesystem::path& InitialDirectory)
{
	const auto Patterns = [](const FFileDialogFilter& Filter)
	{
		std::string Pattern;
		for (const std::string& Extension : Filter.Extensions)
		{
			Pattern += (Pattern.empty() ? "*." : " *.") + Extension;
		}

		return Pattern;
	};

	// Both tools exit with 1 on cancel and print one chosen path per line.
	if (const std::optional<std::filesystem::path> Zenity = FindOnPath("zenity"))
	{
		FProcessRequest Request{.Executable = *Zenity, .Arguments = {"--file-selection", "--multiple", "--separator=\n", std::format("--title={}", Title)}};
		if (!InitialDirectory.empty())
		{
			Request.Arguments.push_back(std::format("--filename={}/", InitialDirectory.string()));
		}
		for (const FFileDialogFilter& Filter : Filters)
		{
			Request.Arguments.push_back(std::format("--file-filter={} | {}", Filter.Name, Patterns(Filter)));
		}

		const std::expected<FProcessResult, FProcessError> Result = RunProcess(Request);
		if (!Result)
		{
			return std::unexpected(FFileDialogError{std::format("zenity failed: {}", Result.error().Message)});
		}

		return Result->ExitCode == 0 ? SplitLines(Result->StandardOutput) : std::vector<std::filesystem::path>{};
	}

	if (const std::optional<std::filesystem::path> KDialog = FindOnPath("kdialog"))
	{
		std::string FilterSpec;
		for (const FFileDialogFilter& Filter : Filters)
		{
			FilterSpec += std::format("{}{}|{}", FilterSpec.empty() ? "" : "\n", Patterns(Filter), Filter.Name);
		}

		const char* const Home = std::getenv("HOME");
		const std::string Directory = InitialDirectory.empty() ? (Home != nullptr ? Home : ".") : InitialDirectory.string();
		const std::expected<FProcessResult, FProcessError> Result = RunProcess({.Executable = *KDialog, .Arguments = {"--title", std::string(Title), "--getopenfilename", Directory, FilterSpec, "--multiple", "--separate-output"}});
		if (!Result)
		{
			return std::unexpected(FFileDialogError{std::format("kdialog failed: {}", Result.error().Message)});
		}

		return Result->ExitCode == 0 ? SplitLines(Result->StandardOutput) : std::vector<std::filesystem::path>{};
	}

	return std::unexpected(FFileDialogError{"No file dialog is available. Install zenity or kdialog, or drop files onto the editor instead"});
}
#endif

std::expected<std::vector<std::filesystem::path>, FFileDialogError> OpenFilesDialog(const std::string_view Title, const std::span<const FFileDialogFilter> Filters, const std::filesystem::path& InitialDirectory)
{
	if (InitialDirectory.empty())
	{
		return OpenFilesDialogImpl(Title, Filters, {});
	}

	std::error_code Error;
	const std::filesystem::path Directory = std::filesystem::absolute(InitialDirectory, Error);
	if (Error || !std::filesystem::is_directory(Directory, Error))
	{
		return std::unexpected(FFileDialogError{std::format("File dialog initial directory is unavailable: {}", InitialDirectory.string())});
	}

	return OpenFilesDialogImpl(Title, Filters, Directory);
}
}
