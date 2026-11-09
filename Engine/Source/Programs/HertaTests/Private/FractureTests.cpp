#include "Herta/GameFramework/Fracture.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>

namespace Herta
{
TEST_CASE("Box fractures fill the box with convex chunks joined by face bonds, the same way every time")
{
	const FVector3 Half{2.f, 1.5f, 0.2f};
	const FFracture Fracture = FractureBox(Half, 16, 7);
	REQUIRE(Fracture.Chunks.size() == 16);

	// The chunks tile the box exactly.
	const float Volume = std::accumulate(Fracture.Chunks.begin(), Fracture.Chunks.end(), 0.f, [](const float Sum, const FFractureChunk& Chunk)
	{
		return Sum + Chunk.Volume;
	});

	CHECK(Volume == doctest::Approx(8.f * Half.X * Half.Y * Half.Z).epsilon(1e-4));
	for (const FFractureChunk& Chunk : Fracture.Chunks)
	{
		CHECK(Chunk.Points.size() >= 4);
		CHECK(Chunk.Faces.size() >= 4);
		CHECK(Chunk.Volume > 0.f);
		for (const FVector3& Point : Chunk.Points)
		{
			CHECK(std::abs(Point.X) <= Half.X + 1e-4f);
			CHECK(std::abs(Point.Y) <= Half.Y + 1e-4f);
			CHECK(std::abs(Point.Z) <= Half.Z + 1e-4f);
		}
	}

	CHECK(std::ranges::any_of(Fracture.Chunks, &FFractureChunk::bTouchesBottom));
	CHECK(std::ranges::any_of(Fracture.Chunks, [](const FFractureChunk& Chunk)
	{
		return !Chunk.bTouchesBottom;
	}));

	// Every chunk is bonded to at least one other, and bonds name each pair once.
	REQUIRE_FALSE(Fracture.Bonds.empty());
	for (std::uint32_t Chunk = 0; Chunk < Fracture.Chunks.size(); ++Chunk)
	{
		CHECK(std::ranges::any_of(Fracture.Bonds, [Chunk](const FFractureBond& Bond)
		{
			return Bond.First == Chunk || Bond.Second == Chunk;
		}));
	}

	for (const FFractureBond& Bond : Fracture.Bonds)
	{
		CHECK(Bond.First < Bond.Second);
		CHECK(Bond.Area > 0.f);
	}

	const FFracture Again = FractureBox(Half, 16, 7);
	REQUIRE(Again.Bonds.size() == Fracture.Bonds.size());
	CHECK(Again.Chunks[5].Points == Fracture.Chunks[5].Points);
	CHECK(FractureBox(Half, 16, 8).Chunks[5].Points != Fracture.Chunks[5].Points);
	CHECK(FractureBox(Half, 1000, 1).Chunks.size() == MaximumFractureCells);

	const FCookedModel Model = BuildFractureChunkModel(Fracture, 3);
	const auto Valid = ValidateCookedModel(Model);
	CHECK_MESSAGE(Valid, (Valid ? std::string() : Valid.error().Message));
	CHECK(Model.Indices.size() % 3 == 0);
	CHECK(Model.Indices.size() >= 12);
}
}
