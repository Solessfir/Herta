#pragma once

#include "Herta/EditorFramework/OutputLog.h"

#include <expected>
#include <memory>
#include <string>

namespace Herta
{
class FEditorCommandRegistry;
class FLogService;
class FToolUIContext;

struct FEditorFrameworkError
{
	std::string Message;
};

struct FEditorFrameworkDescriptor
{
	FLogService* Log = nullptr;
	FEditorCommandRegistry* Commands = nullptr;
	FToolUIContext* ToolUI = nullptr;
};

class FEditorFramework final
{
public:
	struct FImplementation;

	[[nodiscard]] static std::expected<std::unique_ptr<FEditorFramework>, FEditorFrameworkError> Create(FEditorFrameworkDescriptor Descriptor);

	~FEditorFramework();

	FEditorFramework(const FEditorFramework&) = delete;
	FEditorFramework& operator=(const FEditorFramework&) = delete;
	FEditorFramework(FEditorFramework&&) = delete;
	FEditorFramework& operator=(FEditorFramework&&) = delete;

	[[nodiscard]] std::expected<void, FEditorFrameworkError> Draw();
	[[nodiscard]] FOutputLogModel& GetOutputLog() noexcept;
	[[nodiscard]] const FOutputLogModel& GetOutputLog() const noexcept;

private:
	explicit FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
