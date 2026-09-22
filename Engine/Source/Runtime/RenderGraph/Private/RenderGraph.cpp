#include "Herta/RenderGraph/RenderGraph.h"

#include <algorithm>
#include <exception>
#include <optional>

namespace Herta
{
FRenderGraphResourceHandle FRenderGraph::ImportResource(std::string Name)
{
	Resources.push_back({std::move(Name), {}, {}, true});
	return {Resources.size() - 1};
}

FRenderGraphResourceHandle FRenderGraph::CreateResource(std::string Name, FRenderGraphCallback Acquire, std::function<void()> Release)
{
	Resources.push_back({std::move(Name), std::move(Acquire), std::move(Release), false});
	return {Resources.size() - 1};
}

FRenderGraphPassHandle FRenderGraph::AddPass(std::string Name, std::vector<FRenderGraphAccess> Accesses, FRenderGraphCallback Execute)
{
	Passes.push_back({std::move(Name), std::move(Accesses), std::move(Execute)});
	return {Passes.size() - 1};
}

void FRenderGraph::AddDependency(const FRenderGraphPassHandle Before, const FRenderGraphPassHandle After)
{
	Dependencies.emplace_back(Before, After);
}

std::expected<FRenderGraphPlan, FRenderGraphError> FRenderGraph::Compile() const
{
	std::vector<std::vector<std::size_t>> Edges(Passes.size());
	std::vector<std::size_t> Incoming(Passes.size(), 0);
	const auto AddEdge = [&](const std::size_t Before, const std::size_t After)
	{
		if (std::ranges::find(Edges[Before], After) == Edges[Before].end())
		{
			Edges[Before].push_back(After);
			++Incoming[After];
		}
	};

	for (const auto& [Before, After] : Dependencies)
	{
		if (Before.Index >= Passes.size() || After.Index >= Passes.size())
		{
			return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::InvalidPass, "Dependency refers to an invalid pass"});
		}

		AddEdge(Before.Index, After.Index);
	}

	struct FResourceHistory
	{
		std::optional<std::size_t> Writer;
		std::vector<std::size_t> Readers;
	};

	std::vector<FResourceHistory> History(Resources.size());
	for (const FResource& Resource : Resources)
	{
		if (static_cast<bool>(Resource.Acquire) != static_cast<bool>(Resource.Release))
		{
			return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::InvalidResource, "Transient resource requires both acquire and release callbacks: " + Resource.Name});
		}
	}

	for (std::size_t PassIndex = 0; PassIndex < Passes.size(); ++PassIndex)
	{
		const FPass& Pass = Passes[PassIndex];
		if (!Pass.Execute)
		{
			return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::InvalidPass, "Pass has no execution callback: " + Pass.Name});
		}

		std::vector<bool> Used(Resources.size(), false);
		for (const FRenderGraphAccess Access : Pass.Accesses)
		{
			const std::size_t ResourceIndex = Access.Resource.Index;
			if (ResourceIndex >= Resources.size())
			{
				return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::InvalidResource, "Pass refers to an invalid resource: " + Pass.Name});
			}

			if (Used[ResourceIndex] || (Access.Access != ERenderGraphAccess::Read && Access.Access != ERenderGraphAccess::Write && Access.Access != ERenderGraphAccess::ReadWrite))
			{
				return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::InvalidAccess, "Duplicate or invalid resource access in pass: " + Pass.Name});
			}

			Used[ResourceIndex] = true;
			FResourceHistory& Previous = History[ResourceIndex];
			if (Access.Access != ERenderGraphAccess::Write && !Previous.Writer && !Resources[ResourceIndex].bImported)
			{
				return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::UninitializedRead, "Pass '" + Pass.Name + "' reads uninitialized resource '" + Resources[ResourceIndex].Name + "'"});
			}

			if (Previous.Writer)
			{
				AddEdge(*Previous.Writer, PassIndex);
			}

			if (Access.Access != ERenderGraphAccess::Read)
			{
				for (const std::size_t Reader : Previous.Readers)
				{
					AddEdge(Reader, PassIndex);
				}

				Previous.Readers.clear();
				Previous.Writer = PassIndex;
			}
			else
			{
				Previous.Readers.push_back(PassIndex);
			}
		}
	}

	FRenderGraphPlan Plan;
	std::vector<bool> Scheduled(Passes.size(), false);
	while (Plan.PassOrder.size() < Passes.size())
	{
		std::size_t Next = 0;
		while (Next < Passes.size() && (Scheduled[Next] || Incoming[Next] != 0))
		{
			++Next;
		}

		if (Next == Passes.size())
		{
			return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::DependencyCycle, "Render graph contains a dependency cycle"});
		}

		Scheduled[Next] = true;
		Plan.PassOrder.push_back({Next});
		for (const std::size_t Dependent : Edges[Next])
		{
			--Incoming[Dependent];
		}
	}

	std::vector<std::optional<std::size_t>> LifetimeIndices(Resources.size());
	for (std::size_t Position = 0; Position < Plan.PassOrder.size(); ++Position)
	{
		for (const FRenderGraphAccess Access : Passes[Plan.PassOrder[Position].Index].Accesses)
		{
			std::optional<std::size_t>& Index = LifetimeIndices[Access.Resource.Index];
			if (!Index)
			{
				Index = Plan.Lifetimes.size();
				Plan.Lifetimes.push_back({Access.Resource, Position, Position});
			}
			else
			{
				Plan.Lifetimes[*Index].LastUse = Position;
			}
		}
	}

	return Plan;
}

std::expected<void, FRenderGraphError> FRenderGraph::Execute() const
{
	const auto Plan = Compile();
	if (!Plan)
	{
		return std::unexpected(Plan.error());
	}

	std::vector<bool> Acquired(Resources.size(), false);
	const auto Release = [&](const std::size_t Index) -> std::expected<void, FRenderGraphError>
	{
		if (Acquired[Index])
		{
			Acquired[Index] = false;
			try
			{
				Resources[Index].Release();
			}
			catch (...)
			{
				return std::unexpected(FRenderGraphError{ERenderGraphErrorCode::ExecutionFailed, "Resource release failed: " + Resources[Index].Name});
			}
		}

		return {};
	};
	const auto Run = [&]() -> std::expected<void, FRenderGraphError>
	{
		for (std::size_t Position = 0; Position < Plan->PassOrder.size(); ++Position)
		{
			for (const FRenderGraphResourceLifetime Lifetime : Plan->Lifetimes)
			{
				const FResource& Resource = Resources[Lifetime.Resource.Index];
				if (Lifetime.FirstUse == Position && Resource.Acquire)
				{
					if (const auto Result = Resource.Acquire(); !Result)
					{
						return Result;
					}

					Acquired[Lifetime.Resource.Index] = true;
				}
			}

			if (const auto Result = Passes[Plan->PassOrder[Position].Index].Execute(); !Result)
			{
				return Result;
			}

			for (const FRenderGraphResourceLifetime Lifetime : Plan->Lifetimes)
			{
				if (Lifetime.LastUse == Position)
				{
					if (const auto Result = Release(Lifetime.Resource.Index); !Result)
					{
						return Result;
					}
				}
			}
		}

		return {};
	};

	std::expected<void, FRenderGraphError> Result;
	try
	{
		Result = Run();
	}
	catch (const std::exception& Error)
	{
		Result = std::unexpected(FRenderGraphError{ERenderGraphErrorCode::ExecutionFailed, Error.what()});
	}
	catch (...)
	{
		Result = std::unexpected(FRenderGraphError{ERenderGraphErrorCode::ExecutionFailed, "Render graph callback threw an unknown exception"});
	}

	for (std::size_t Index = Resources.size(); Index > 0; --Index)
	{
		const auto Released = Release(Index - 1);
		if (Result && !Released)
		{
			Result = Released;
		}
	}

	return Result;
}
}
