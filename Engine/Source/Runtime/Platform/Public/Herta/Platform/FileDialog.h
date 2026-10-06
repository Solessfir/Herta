#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Herta
{
struct FFileDialogFilter
{
	std::string Name;
	// Without dots, such as "png".
	std::vector<std::string> Extensions;
};

struct FFileDialogError
{
	std::string Message;
};

// Shows the platform's open-file dialog with multiple selection and blocks until the user chooses or cancels; an empty result means cancelled.
// Windows uses IFileOpenDialog. Linux runs zenity, then kdialog, so desktop portals and themes apply; neither being installed is an error.
[[nodiscard]] std::expected<std::vector<std::filesystem::path>, FFileDialogError> OpenFilesDialog(std::string_view Title, std::span<const FFileDialogFilter> Filters, const std::filesystem::path& InitialDirectory = {});

// Starts the desktop file manager without waiting for its window to close.
[[nodiscard]] std::expected<void, FFileDialogError> OpenDirectoryInFileManager(const std::filesystem::path& Directory);
}
