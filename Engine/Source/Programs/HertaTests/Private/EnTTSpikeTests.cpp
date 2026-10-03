#include <doctest/doctest.h>
#include <entt/entity/organizer.hpp>
#include <entt/entity/registry.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <print>
#include <thread>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
struct FSpikePosition
{
	std::uint64_t X = 0;
	std::uint64_t Y = 0;
};

struct FSpikeVelocity
{
	std::uint64_t X = 1;
};

struct FSpikeHealth
{
	std::uint64_t Value = 100;
};

struct FSpikeHierarchy
{
	entt::entity Parent = entt::null;
	std::uint64_t Local = 1;
	std::uint64_t World = 0;
};

struct FSpikeIdentity
{
	std::uint64_t Value = 0;
};

struct FSpikeInspectorRow
{
	std::uint64_t Id = 0;
	FSpikePosition Position;
};

using FSpikeClock = std::chrono::steady_clock;

double SpikeMilliseconds(const FSpikeClock::time_point Start)
{
	return std::chrono::duration<double, std::milli>(FSpikeClock::now() - Start).count();
}

void RunSpikeIteration(const std::size_t Count, const bool bFragmented)
{
	entt::registry Registry;
	std::vector<entt::entity> Entities;
	Entities.reserve(Count);
	const auto CreationStart = FSpikeClock::now();

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const auto Entity = Registry.create();
		Entities.push_back(Entity);
		Registry.emplace<FSpikePosition>(Entity, FSpikePosition{.X = Index});

		if (!bFragmented || Index % 2 == 0)
		{
			Registry.emplace<FSpikeVelocity>(Entity);
		}

		if (bFragmented && Index % 3 == 0)
		{
			Registry.emplace<FSpikeHealth>(Entity);
		}
	}

	const double CreationMs = SpikeMilliseconds(CreationStart);
	const auto ChurnStart = FSpikeClock::now();

	if (bFragmented)
	{
		for (std::size_t Index = 0; Index < Count; Index += 5)
		{
			Registry.remove<FSpikePosition>(Entities[Index]);
			Registry.emplace<FSpikePosition>(Entities[Index], FSpikePosition{.X = Index});
		}
	}

	const double ChurnMs = SpikeMilliseconds(ChurnStart);
	constexpr std::uint64_t Passes = 8;
	const auto IterationStart = FSpikeClock::now();

	for (std::uint64_t Pass = 0; Pass < Passes; ++Pass)
	{
		auto View = Registry.view<FSpikePosition, const FSpikeVelocity>();
		View.each([](FSpikePosition& Position, const FSpikeVelocity& Velocity)
		{
			Position.X += Velocity.X;
		});
	}

	const double IterationMs = SpikeMilliseconds(IterationStart);
	std::uint64_t Sum = 0;
	std::size_t Matches = 0;

	Registry.view<const FSpikePosition, const FSpikeVelocity>().each([&](const FSpikePosition&, const FSpikeVelocity&)
	{
		++Matches;
	});

	Registry.view<const FSpikePosition>().each([&](const FSpikePosition& Position)
	{
		Sum += Position.X;
	});

	const std::size_t ExpectedMatches = bFragmented ? (Count + 1) / 2 : Count;
	CHECK(Matches == ExpectedMatches);
	CHECK(Sum == Count * (Count - 1) / 2 + Passes * ExpectedMatches);
	CHECK(Registry.storage<FSpikePosition>().size() == Count);
	std::println("EnTT spike: {} entities, {}: create {:.3f} ms, churn {:.3f} ms, {} query passes {:.3f} ms, matches {}, checksum {}", Count, bFragmented ? "fragmented" : "homogeneous", CreationMs, ChurnMs, Passes, IterationMs, Matches, Sum);
}

void RunSpikeHierarchyAndClone()
{
	entt::registry Registry;
	std::vector<entt::entity> Entities;
	constexpr std::size_t Count = 1024;
	const auto Start = FSpikeClock::now();

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const auto Entity = Registry.create();
		Entities.push_back(Entity);
		const auto Parent = Index == 0 ? entt::null : Entities[(Index - 1) / 2];
		Registry.emplace<FSpikeHierarchy>(Entity, FSpikeHierarchy{.Parent = Parent});
		Registry.emplace<FSpikeIdentity>(Entity, FSpikeIdentity{.Value = Index + 1});
		Registry.emplace<FSpikePosition>(Entity, FSpikePosition{.X = Index});
	}

	// Hierarchy order is explicit, not an incidental sparse-set iteration order.
	for (const auto Entity : Entities)
	{
		auto& Hierarchy = Registry.get<FSpikeHierarchy>(Entity);
		Hierarchy.World = Hierarchy.Local + (Hierarchy.Parent == entt::null ? 0 : Registry.get<FSpikeHierarchy>(Hierarchy.Parent).World);
	}

	auto& Reparented = Registry.get<FSpikeHierarchy>(Entities.back());
	Reparented.Parent = Entities.front();
	Reparented.World = Reparented.Local + Registry.get<FSpikeHierarchy>(Reparented.Parent).World;
	CHECK(Reparented.World == 2);

	std::vector<entt::entity> Clones;
	Clones.reserve(Count);

	for (const auto Source : Entities)
	{
		const auto Clone = Registry.create();
		Clones.push_back(Clone);
		Registry.emplace<FSpikePosition>(Clone, Registry.get<FSpikePosition>(Source));
		Registry.emplace<FSpikeIdentity>(Clone, FSpikeIdentity{.Value = Count + Registry.get<FSpikeIdentity>(Source).Value});
	}

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const auto Source = Registry.get<FSpikeHierarchy>(Entities[Index]);
		const std::size_t ParentIndex = Index == Count - 1 ? 0 : (Index == 0 ? 0 : (Index - 1) / 2);
		const auto CloneParent = Source.Parent == entt::null ? entt::null : Clones[ParentIndex];
		Registry.emplace<FSpikeHierarchy>(Clones[Index], FSpikeHierarchy{.Parent = CloneParent, .Local = Source.Local, .World = Source.World});
		CHECK(Registry.get<FSpikeIdentity>(Clones[Index]).Value != Registry.get<FSpikeIdentity>(Entities[Index]).Value);
		CHECK(Registry.get<FSpikePosition>(Clones[Index]).X == Registry.get<FSpikePosition>(Entities[Index]).X);

		if (CloneParent != entt::null)
		{
			CHECK(std::ranges::find(Clones, CloneParent) != Clones.end());
		}
	}

	Registry.get<FSpikePosition>(Clones.back()).X = 9999;
	CHECK(Registry.get<FSpikePosition>(Entities.back()).X == Count - 1);
	CHECK(Registry.get<FSpikeHierarchy>(Clones.back()).Parent == Clones.front());
	std::println("EnTT spike: hierarchy/reparent/clone {} entities {:.3f} ms", Count, SpikeMilliseconds(Start));
}

void RunSpikeBarrierAndInspection()
{
	entt::registry Registry;
	std::vector<FSpikeInspectorRow> Snapshot;
	std::vector<entt::entity> PendingDestroy;
	constexpr std::size_t Count = 1000;

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const auto Entity = Registry.create();
		Registry.emplace<FSpikePosition>(Entity, FSpikePosition{.X = Index});
		Registry.emplace<FSpikeIdentity>(Entity, FSpikeIdentity{.Value = Index + 1});
	}

	{
		auto View = Registry.view<const FSpikePosition, const FSpikeIdentity>();
		View.each([&](const entt::entity Entity, const FSpikePosition& Position, const FSpikeIdentity& Identity)
		{
			Snapshot.push_back({.Id = Identity.Value, .Position = Position});

			if (Identity.Value % 2 == 0)
			{
				PendingDestroy.push_back(Entity);
			}
		});
	}

	CHECK(Registry.storage<FSpikePosition>().size() == Count);
	const auto BarrierStart = FSpikeClock::now();

	for (const auto Entity : PendingDestroy)
	{
		Registry.destroy(Entity);
	}

	for (std::size_t Index = 0; Index < PendingDestroy.size(); ++Index)
	{
		const auto Entity = Registry.create();
		Registry.emplace<FSpikePosition>(Entity, FSpikePosition{.X = 9999});
		Registry.emplace<FSpikeIdentity>(Entity, FSpikeIdentity{.Value = Count + Index + 1});
	}

	for (const auto Entity : PendingDestroy)
	{
		CHECK_FALSE(Registry.valid(Entity));
	}

	CHECK(Snapshot.size() == Count);
	std::uint64_t Sum = 0;

	for (const auto& Row : Snapshot)
	{
		Sum += Row.Position.X;
		CHECK(Row.Id == Row.Position.X + 1);
	}

	CHECK(Sum == Count * (Count - 1) / 2);
	CHECK(Registry.view<const FSpikePosition>().size() == Count);
	std::println("EnTT spike: deferred destroy/create barrier {} commands {:.3f} ms; inspector snapshot survives", PendingDestroy.size() * 2, SpikeMilliseconds(BarrierStart));

	entt::registry RelocationRegistry;
	const auto First = RelocationRegistry.create();
	const auto Last = RelocationRegistry.create();
	RelocationRegistry.emplace<FSpikePosition>(First, FSpikePosition{.X = 1});
	RelocationRegistry.emplace<FSpikePosition>(Last, FSpikePosition{.X = 2});
	const auto OldAddress = reinterpret_cast<std::uintptr_t>(&RelocationRegistry.get<FSpikePosition>(Last));
	RelocationRegistry.remove<FSpikePosition>(First);
	const auto NewAddress = reinterpret_cast<std::uintptr_t>(&RelocationRegistry.get<FSpikePosition>(Last));
	CHECK(OldAddress != NewAddress);
	CHECK(RelocationRegistry.get<FSpikePosition>(Last).X == 2);
}

void RunSpikeParallelQueries()
{
	entt::registry Registry;
	constexpr std::size_t Count = 100000;

	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		const auto Entity = Registry.create();
		Registry.emplace<FSpikePosition>(Entity);
		Registry.emplace<FSpikeVelocity>(Entity);
	}

	// Construct pools and views before worker launch; no structural changes until join.
	auto PositionView = Registry.view<FSpikePosition>();
	auto VelocityView = Registry.view<FSpikeVelocity>();
	const auto ReadView = std::as_const(Registry).view<const FSpikeVelocity>();
	std::uint64_t ReadSum = 0;
	const auto Start = FSpikeClock::now();
	{
		std::jthread PositionWriter([&]
		{
			PositionView.each([](FSpikePosition& Position)
			{
				Position.X = 7;
			});
		});

		std::jthread Reader([&]
		{
			ReadView.each([&](const FSpikeVelocity& Velocity)
			{
				ReadSum += Velocity.X;
			});
		});
	}

	CHECK(ReadSum == Count);
	{
		std::jthread PositionWriter([&]
		{
			PositionView.each([](FSpikePosition& Position)
			{
				Position.X += 1;
			});
		});

		std::jthread VelocityWriter([&]
		{
			VelocityView.each([](FSpikeVelocity& Velocity)
			{
				Velocity.X = 2;
			});
		});
	}

	{
		std::jthread XWriter([&]
		{
			PositionView.each([](FSpikePosition& Position)
			{
				Position.X += 1;
			});
		});

		std::jthread YWriter([&]
		{
			PositionView.each([](FSpikePosition& Position)
			{
				Position.Y = 3;
			});
		});
	}

	bool bValid = true;
	Registry.view<const FSpikePosition, const FSpikeVelocity>().each([&](const FSpikePosition& Position, const FSpikeVelocity& Velocity)
	{
		bValid = bValid && Position.X == 9 && Position.Y == 3 && Velocity.X == 2;
	});

	CHECK(bValid);
	std::println("EnTT spike: prepared parallel queries {} entities {:.3f} ms", Count, SpikeMilliseconds(Start));
}

void SpikeWritePosition(entt::view<entt::get_t<FSpikePosition>> View)
{
	View.each([](FSpikePosition& Position)
	{
		++Position.X;
	});
}

void SpikeReadPosition(entt::view<entt::get_t<const FSpikePosition>> View)
{
	CHECK(View.size() == 1);
	CHECK(View.get<FSpikePosition>(*View.begin()).X == 1);
}

void SpikeWriteVelocity(entt::view<entt::get_t<FSpikeVelocity>> View)
{
	View.each([](FSpikeVelocity& Velocity)
	{
		++Velocity.X;
	});
}

void RunSpikeOrganizer()
{
	entt::organizer Organizer;
	Organizer.emplace<&SpikeWritePosition>("position-write");
	Organizer.emplace<&SpikeWriteVelocity>("velocity-write");
	Organizer.emplace<&SpikeReadPosition>("position-read");
	Organizer.emplace<&SpikeWritePosition>("position-write-again");
	const auto Graph = Organizer.graph();
	const auto RepeatedGraph = Organizer.graph();
	REQUIRE(Graph.size() == 4);
	CHECK(Graph[0].top_level());
	CHECK(Graph[1].top_level());
	CHECK_FALSE(Graph[2].top_level());
	CHECK_FALSE(Graph[3].top_level());
	CHECK(std::ranges::find(Graph[0].out_edges(), 2) != Graph[0].out_edges().end());
	CHECK(std::ranges::find(Graph[2].out_edges(), 3) != Graph[2].out_edges().end());
	CHECK(Graph[0].rw_count() == 1);
	CHECK(Graph[2].ro_count() >= 1);

	entt::registry Registry;

	for (std::size_t Index = 0; Index < Graph.size(); ++Index)
	{
		CHECK(Graph[Index].out_edges() == RepeatedGraph[Index].out_edges());
		Graph[Index].prepare(Registry);
	}

	const auto Entity = Registry.create();
	Registry.emplace<FSpikePosition>(Entity);
	Registry.emplace<FSpikeVelocity>(Entity);
	std::vector<std::size_t> Order;
	std::vector<bool> Completed(Graph.size(), false);

	// A stable ready-node policy belongs to Herta; organizer supplies dependencies only.
	while (Order.size() != Graph.size())
	{
		bool bProgress = false;

		for (std::size_t Index = 0; Index < Graph.size(); ++Index)
		{
			if (!Completed[Index] && std::ranges::all_of(Graph[Index].in_edges(), [&](const std::size_t Parent)
			{
				return Completed[Parent];
			}))
			{
				Graph[Index].callback()(Graph[Index].data(), Registry);
				Completed[Index] = true;
				Order.push_back(Index);
				bProgress = true;
			}
		}

		REQUIRE(bProgress);
	}

	CHECK((Order == std::vector<std::size_t>{0, 1, 2, 3}));
	CHECK(Registry.get<FSpikePosition>(Entity).X == 2);
	CHECK(Registry.get<FSpikeVelocity>(Entity).X == 2);
}
} // namespace

TEST_CASE("EnTT adoption spike" * doctest::skip())
{
#ifdef __clang__
	std::println("EnTT spike compiler: Clang {}", __clang_version__);
#elif defined(_MSC_VER)
	std::println("EnTT spike compiler: MSVC {}", _MSC_FULL_VER);
#elif defined(__GNUC__)
	std::println("EnTT spike compiler: GCC {}", __VERSION__);
#endif
	std::println("EnTT spike logical CPUs: {}; wall-clock measurements, no timing thresholds", std::thread::hardware_concurrency());

	for (const std::size_t Count : {100000u, 1000000u})
	{
		RunSpikeIteration(Count, false);
		RunSpikeIteration(Count, true);
	}

	RunSpikeHierarchyAndClone();
	RunSpikeBarrierAndInspection();
	RunSpikeParallelQueries();
	RunSpikeOrganizer();
}
} // namespace Herta
