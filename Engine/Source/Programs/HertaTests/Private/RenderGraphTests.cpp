#include "Herta/RenderGraph/RenderGraph.h"

#include <doctest/doctest.h>

#include <stdexcept>

namespace
{
std::expected<void, Herta::FRenderGraphError> Succeed()
{
	return {};
}
}

TEST_CASE("Render graph preserves read write hazards and transient lifetimes")
{
	using namespace Herta;
	FRenderGraph Graph;
	std::vector<std::string> Events;
	const auto Texture = Graph.ImportResource("Texture");

	const auto Depth = Graph.CreateResource("Depth", [&]() -> std::expected<void, FRenderGraphError>
	{
		Events.emplace_back("Acquire");
		return {};
	}, [&]()
	{
		Events.emplace_back("Release");
	});

	const auto Draw = Graph.AddPass("Draw", {{.Resource = Texture, .Access = ERenderGraphAccess::Read}, {.Resource = Depth, .Access = ERenderGraphAccess::Write}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		Events.emplace_back("Draw");
		return {};
	});

	const auto Read = Graph.AddPass("Read", {{.Resource = Depth, .Access = ERenderGraphAccess::Read}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		Events.emplace_back("Read");
		return {};
	});

	const auto Rewrite = Graph.AddPass("Rewrite", {{.Resource = Depth, .Access = ERenderGraphAccess::ReadWrite}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		Events.emplace_back("Rewrite");
		return {};
	});

	const auto Finish = Graph.AddPass("Finish", {}, [&]() -> std::expected<void, FRenderGraphError>
	{
		Events.emplace_back("Finish");
		return {};
	});

	const auto Plan = Graph.Compile();
	REQUIRE(Plan);
	CHECK(Plan->PassOrder == std::vector<FRenderGraphPassHandle>{Draw, Read, Rewrite, Finish});
	REQUIRE(Plan->Lifetimes.size() == 2);
	CHECK(Plan->Lifetimes[1].Resource == Depth);
	CHECK(Plan->Lifetimes[1].FirstUse == 0);
	CHECK(Plan->Lifetimes[1].LastUse == 2);
	REQUIRE(Graph.Execute());
	CHECK(Events == std::vector<std::string>{"Acquire", "Draw", "Read", "Rewrite", "Release", "Finish"});
}

TEST_CASE("Render graph orders explicit dependencies deterministically")
{
	Herta::FRenderGraph Graph;
	const auto First = Graph.AddPass("First", {}, Succeed);
	const auto Second = Graph.AddPass("Second", {}, Succeed);
	const auto Third = Graph.AddPass("Third", {}, Succeed);
	Graph.AddDependency(Third, First);
	Graph.AddDependency(Third, First);
	const auto Plan = Graph.Compile();
	REQUIRE(Plan);
	CHECK(Plan->PassOrder == std::vector<Herta::FRenderGraphPassHandle>{Second, Third, First});
}

TEST_CASE("Render graph rejects cycles including resource hazards")
{
	using namespace Herta;
	FRenderGraph Graph;
	const auto Target = Graph.ImportResource("Target");
	ERenderGraphAccess FirstAccess = ERenderGraphAccess::Read;
	ERenderGraphAccess SecondAccess = ERenderGraphAccess::Write;
	SUBCASE("Write after read")
	{
	}

	SUBCASE("Read after write")
	{
		FirstAccess = ERenderGraphAccess::Write;
		SecondAccess = ERenderGraphAccess::Read;
	}

	SUBCASE("Write after write")
	{
		FirstAccess = ERenderGraphAccess::Write;
	}

	const auto First = Graph.AddPass("First", {{.Resource = Target, .Access = FirstAccess}}, Succeed);
	const auto Second = Graph.AddPass("Second", {{.Resource = Target, .Access = SecondAccess}}, Succeed);
	Graph.AddDependency(Second, First);
	const auto Plan = Graph.Compile();
	REQUIRE_FALSE(Plan);
	CHECK(Plan.error().Code == ERenderGraphErrorCode::DependencyCycle);
}

TEST_CASE("Render graph rejects reads before initialization without executing anything")
{
	using namespace Herta;
	FRenderGraph Graph;
	bool bExecuted = false;
	const auto Target = Graph.CreateResource("Uninitialized");
	const auto Pass = Graph.AddPass("Read", {{.Resource = Target, .Access = ERenderGraphAccess::ReadWrite}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		bExecuted = true;
		return {};
	});

	CHECK(Pass.Index == 0);
	const auto Result = Graph.Execute();
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Code == ERenderGraphErrorCode::UninitializedRead);
	CHECK_FALSE(bExecuted);
}

TEST_CASE("Render graph rejects invalid resources passes and duplicate access")
{
	using namespace Herta;
	FRenderGraph Graph;
	const auto Target = Graph.ImportResource("Target");
	ERenderGraphErrorCode Expected = ERenderGraphErrorCode::InvalidResource;
	SUBCASE("Invalid resource")
	{
		Graph.AddPass("Invalid", {{.Resource = {}, .Access = ERenderGraphAccess::Read}}, Succeed);
	}

	SUBCASE("Invalid dependency")
	{
		Graph.AddDependency({}, {});
		Expected = ERenderGraphErrorCode::InvalidPass;
	}

	SUBCASE("Missing callback")
	{
		Graph.AddPass("Missing", {}, {});
		Expected = ERenderGraphErrorCode::InvalidPass;
	}

	SUBCASE("Duplicate access")
	{
		Graph.AddPass("Duplicate", {{.Resource = Target, .Access = ERenderGraphAccess::Read}, {.Resource = Target, .Access = ERenderGraphAccess::Write}}, Succeed);
		Expected = ERenderGraphErrorCode::InvalidAccess;
	}

	SUBCASE("Invalid access enum")
	{
		Graph.AddPass("Invalid", {{.Resource = Target, .Access = static_cast<ERenderGraphAccess>(99)}}, Succeed);
		Expected = ERenderGraphErrorCode::InvalidAccess;
	}

	SUBCASE("Unpaired lifetime callbacks")
	{
		static_cast<void>(Graph.CreateResource("Missing release", Succeed));
	}

	const auto Plan = Graph.Compile();
	REQUIRE_FALSE(Plan);
	CHECK(Plan.error().Code == Expected);
}

TEST_CASE("Render graph releases acquired resources after a pass fails or throws")
{
	using namespace Herta;
	FRenderGraph Graph;
	int Acquires = 0;
	int Releases = 0;
	bool bThrow = false;
	SUBCASE("Returned error")
	{
	}

	SUBCASE("Exception")
	{
		bThrow = true;
	}

	const auto Target = Graph.CreateResource("Target", [&]() -> std::expected<void, FRenderGraphError>
	{
		++Acquires;
		return {};
	}, [&]()
	{
		++Releases;
	});

	Graph.AddPass("Fail", {{.Resource = Target, .Access = ERenderGraphAccess::Write}}, [&]() -> std::expected<void, FRenderGraphError>
	{
		if (bThrow)
		{
			throw std::runtime_error("Draw failed");
		}

		return std::unexpected(FRenderGraphError{.Code = ERenderGraphErrorCode::ExecutionFailed, .Message = "Draw failed"});
	});

	const auto Result = Graph.Execute();
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Draw failed");
	CHECK(Acquires == 1);
	CHECK(Releases == 1);
}

TEST_CASE("Render graph acquisition failure cleans up only resources acquired successfully")
{
	using namespace Herta;
	FRenderGraph Graph;
	int Releases = 0;
	const auto First = Graph.CreateResource("First", Succeed, [&]()
	{
		++Releases;
	});

	const auto Second = Graph.CreateResource("Second", []() -> std::expected<void, FRenderGraphError>
	{
		return std::unexpected(FRenderGraphError{.Code = ERenderGraphErrorCode::ExecutionFailed, .Message = "Allocation failed"});
	}, [&]()
	{
		Releases += 100;
	});

	Graph.AddPass("Draw", {{.Resource = First, .Access = ERenderGraphAccess::Write}, {.Resource = Second, .Access = ERenderGraphAccess::Write}}, Succeed);
	const auto Result = Graph.Execute();
	REQUIRE_FALSE(Result);
	CHECK(Result.error().Message == "Allocation failed");
	CHECK(Releases == 1);
}

TEST_CASE("Render graph never acquires unused transient resources")
{
	Herta::FRenderGraph Graph;
	bool bAcquired = false;
	static_cast<void>(Graph.CreateResource("Unused", [&]() -> std::expected<void, Herta::FRenderGraphError>
	{
		bAcquired = true;
		return {};
	}, []() {}));

	const auto Plan = Graph.Compile();
	REQUIRE(Plan);
	CHECK(Plan->PassOrder.empty());
	CHECK(Plan->Lifetimes.empty());
	REQUIRE(Graph.Execute());
	CHECK_FALSE(bAcquired);
}
