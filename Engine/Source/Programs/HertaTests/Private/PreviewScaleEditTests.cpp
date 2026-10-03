#include "Herta/EditorCore/PreviewScaleEdit.h"

#include <doctest/doctest.h>

#include <array>
#include <limits>

namespace Herta
{
TEST_CASE("Locked preview scale preserves nonuniform proportions")
{
	std::array Scale{2.f, 3.f, 4.f};
	CHECK(TrySetProportionalPreviewScale(Scale, 1, 6.f));
	const std::array Doubled{4.f, 6.f, 8.f};
	CHECK(Scale == Doubled);
	CHECK(TrySetProportionalPreviewScale(Scale, 0, 1.f));
	const std::array Halved{1.f, 1.5f, 2.f};
	CHECK(Scale == Halved);
}

TEST_CASE("Locked preview scale rejects invalid edits without mutating the transform")
{
	const std::array Original{2.f, 3.f, 4.f};
	for (const float Value : std::array{0.f, -1.f, 0.0001f, 1001.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		auto Scale = Original;
		CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, Value));
		CHECK(Scale == Original);
	}

	auto Scale = Original;
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 3, 2.f));
	CHECK(Scale == Original);
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, 1000.f));
	CHECK(Scale == Original);
	Scale[1] = 0.f;
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, 3.f));
	CHECK(Scale[0] == Original[0]);
	CHECK(Scale[1] == 0.f);
}
}
