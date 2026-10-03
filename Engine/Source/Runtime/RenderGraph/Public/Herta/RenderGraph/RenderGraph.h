#pragma once

#include <cstddef>
#include <expected>
#include <functional>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Herta
{
struct FRenderGraphResourceHandle
{
	std::size_t Index = std::numeric_limits<std::size_t>::max();
	[[nodiscard]] constexpr bool operator==(const FRenderGraphResourceHandle&) const noexcept = default;
};

struct FRenderGraphPassHandle
{
	std::size_t Index = std::numeric_limits<std::size_t>::max();
	[[nodiscard]] constexpr bool operator==(const FRenderGraphPassHandle&) const noexcept = default;
};

enum class ERenderGraphAccess
{
	Read,
	Write,
	ReadWrite
};

struct FRenderGraphAccess
{
	FRenderGraphResourceHandle Resource;
	ERenderGraphAccess Access = ERenderGraphAccess::Read;
};

enum class ERenderGraphErrorCode
{
	InvalidResource,
	InvalidPass,
	InvalidAccess,
	UninitializedRead,
	DependencyCycle,
	ExecutionFailed
};

struct FRenderGraphError
{
	ERenderGraphErrorCode Code = ERenderGraphErrorCode::ExecutionFailed;
	std::string Message;
};

using FRenderGraphCallback = std::function<std::expected<void, FRenderGraphError>()>;

struct FRenderGraphResourceLifetime
{
	FRenderGraphResourceHandle Resource;
	std::size_t FirstUse = 0;
	std::size_t LastUse = 0;
};

struct FRenderGraphPlan
{
	std::vector<FRenderGraphPassHandle> PassOrder;
	// Positions in PassOrder, not pass handles. Unused resources have no lifetime.
	std::vector<FRenderGraphResourceLifetime> Lifetimes;
};

class FRenderGraph
{
public:
	[[nodiscard]] FRenderGraphResourceHandle ImportResource(std::string Name);
	// Release drops graph ownership; the RHI must retain submitted resources until GPU completion.
	[[nodiscard]] FRenderGraphResourceHandle CreateResource(std::string Name, FRenderGraphCallback Acquire = {}, std::function<void()> Release = {});
	// Declaration order defines resource versions, including overwrite and read-before-write hazards.
	FRenderGraphPassHandle AddPass(std::string Name, std::vector<FRenderGraphAccess> Accesses, FRenderGraphCallback Execute);
	void AddDependency(FRenderGraphPassHandle Before, FRenderGraphPassHandle After);
	[[nodiscard]] std::expected<FRenderGraphPlan, FRenderGraphError> Compile() const;
	[[nodiscard]] std::expected<void, FRenderGraphError> Execute() const;

private:
	struct FResource
	{
		std::string Name;
		FRenderGraphCallback Acquire;
		std::function<void()> Release;
		bool bImported = false;
	};

	struct FPass
	{
		std::string Name;
		std::vector<FRenderGraphAccess> Accesses;
		FRenderGraphCallback Execute;
	};

	std::vector<FResource> Resources;
	std::vector<FPass> Passes;
	std::vector<std::pair<FRenderGraphPassHandle, FRenderGraphPassHandle>> Dependencies;
};
}
