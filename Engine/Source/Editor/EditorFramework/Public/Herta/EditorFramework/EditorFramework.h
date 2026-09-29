#pragma once

#include "Herta/EditorFramework/OutputLog.h"
#include "Herta/RHI/Presentation.h"
#include "Herta/Renderer/MeshRenderer.h"

#include <expected>
#include <functional>
#include <memory>
#include <span>
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

	// RenderViewport runs after viewport layout/input and must set the image before it is queued for display.
	[[nodiscard]] std::expected<void, FEditorFrameworkError> Draw(const std::function<void()>& RenderViewport);
	void SetViewportImage(std::uint64_t TextureId) noexcept;
	[[nodiscard]] FExtent2D GetViewportExtent() const noexcept;
	[[nodiscard]] FMeshRenderView GetViewportRenderView() const noexcept;
	[[nodiscard]] std::span<const FDebugDrawList> GetViewportDebugDrawLists() const noexcept;
	[[nodiscard]] FOutputLogModel& GetOutputLog() noexcept;
	[[nodiscard]] const FOutputLogModel& GetOutputLog() const noexcept;

private:
	explicit FEditorFramework(std::unique_ptr<FImplementation> Implementation) noexcept;

	std::unique_ptr<FImplementation> Implementation;
};
}
