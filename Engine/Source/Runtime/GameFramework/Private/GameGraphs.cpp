#include "GameGraphs.h"

#include "Herta/Math/AffineTransform.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Herta
{
namespace
{
// Shipping runs the C++ generated from each graph; other builds run the call list, which traces for the editor and needs no rebuild.
#ifdef HERTA_SHIPPING
constexpr EGameplayBackend PreferredBackend = EGameplayBackend::Native;
#else
constexpr EGameplayBackend PreferredBackend = EGameplayBackend::CallList;
#endif

// Long enough for the editor to see a node that ran once light up and fade.
constexpr float ActivitySeconds = 2.f;

[[nodiscard]] bool IsInsideBox(const FMatrix4& WorldToLocal, const FVector3& HalfExtents, const FVector3& Point)
{
	const FVector4 Local = WorldToLocal * FVector4{Point.X, Point.Y, Point.Z, 1.f};
	return std::abs(Local.X) <= HalfExtents.X && std::abs(Local.Y) <= HalfExtents.Y && std::abs(Local.Z) <= HalfExtents.Z;
}
}

std::vector<FAssetId> FGameGraphs::GetGraphs(const std::span<const FLevelEntity> Entities)
{
	std::vector<FAssetId> Graphs;
	for (const FLevelEntity& Entity : Entities)
	{
		if (Entity.GameplayGraph && Entity.GameplayGraph->Graph.IsValid())
		{
			Graphs.push_back(Entity.GameplayGraph->Graph);
		}
	}

	std::ranges::sort(Graphs);
	const auto Duplicates = std::ranges::unique(Graphs);
	Graphs.erase(Duplicates.begin(), Duplicates.end());
	return Graphs;
}

void FGameGraphs::Add(const FAssetId& Id, std::shared_ptr<const FCookedGameplayGraph> Graph)
{
	Programs.insert_or_assign(Id, FGameplayProgram::Create(std::move(Graph), PreferredBackend));
	for (FInstance& Instance : Instances)
	{
		if (Instance.Graph == Id)
		{
			Instance.Frame = Programs.at(Id).MakeFrame();
			Instance.bStarted = false;
		}
	}
}

void FGameGraphs::Start(const std::span<const FLevelEntity> Entities, const std::span<const FMatrix4> AuthoredWorld)
{
	Instances.clear();
	Activity.clear();
	for (std::size_t Index = 0; Index < Entities.size() && Index < AuthoredWorld.size(); ++Index)
	{
		const FLevelEntity& Entity = Entities[Index];
		if (!Entity.GameplayGraph || !Entity.GameplayGraph->Graph.IsValid())
		{
			continue;
		}

		FInstance Instance{.Graph = Entity.GameplayGraph->Graph, .Position = {AuthoredWorld[Index](0, 3), AuthoredWorld[Index](1, 3), AuthoredWorld[Index](2, 3)}};
		if (Entity.Trigger)
		{
			Instance.WorldToLocal = TryInverseAffine(AuthoredWorld[Index]);
			Instance.HalfExtents = Entity.Trigger->Size * 0.5f;
		}

		if (const auto Program = Programs.find(Instance.Graph); Program != Programs.end())
		{
			Instance.Frame = Program->second.MakeFrame();
		}

		Instances.push_back(std::move(Instance));
	}
}

void FGameGraphs::Update(const float DeltaSeconds, const std::span<const FVector3> Players, IGameplayHost& Host)
{
	for (auto& [Graph, Nodes] : Activity)
	{
		for (auto& [Node, Seconds] : Nodes)
		{
			Seconds += DeltaSeconds;
		}

		std::erase_if(Nodes, [](const auto& Entry)
		{
			return Entry.second > ActivitySeconds;
		});
	}

	for (FInstance& Instance : Instances)
	{
		const auto Found = Programs.find(Instance.Graph);
		if (Found == Programs.end())
		{
			continue;
		}

		const FGameplayProgram& Program = Found->second;
		float Distance = std::numeric_limits<float>::max();
		bool bInside = false;
		for (const FVector3& Player : Players)
		{
			Distance = std::min(Distance, (Player - Instance.Position).Length());
			bInside = bInside || (Instance.WorldToLocal && IsInsideBox(*Instance.WorldToLocal, Instance.HalfExtents, Player));
		}

		Trace.clear();
		FGameplayContext Context{.Slots = Instance.Frame, .Host = &Host, .SelfPosition = Instance.Position, .DeltaSeconds = DeltaSeconds, .PlayerDistance = Distance, .Trace = &Trace};
		if (!Instance.bStarted)
		{
			Instance.bStarted = true;
			Program.Run(EGameplayEvent::Start, Context);
		}

		if (bInside != Instance.bInside)
		{
			Instance.bInside = bInside;
			Program.Run(bInside ? EGameplayEvent::PlayerEnter : EGameplayEvent::PlayerExit, Context);
		}

		Program.Run(EGameplayEvent::Tick, Context);
		if (!Trace.empty())
		{
			std::map<std::uint32_t, float>& Nodes = Activity[Instance.Graph];
			for (const std::uint32_t Node : Trace)
			{
				Nodes[Node] = 0.f;
			}
		}
	}
}

std::vector<FGameGraphNodeActivity> FGameGraphs::GetActivity(const FAssetId& Graph) const
{
	std::vector<FGameGraphNodeActivity> Result;
	if (const auto Found = Activity.find(Graph); Found != Activity.end())
	{
		for (const auto& [Node, Seconds] : Found->second)
		{
			Result.push_back({.Node = Node, .SecondsAgo = Seconds});
		}
	}

	return Result;
}
}
