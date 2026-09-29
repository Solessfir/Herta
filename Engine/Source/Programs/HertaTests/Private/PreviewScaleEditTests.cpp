#include "Herta/EditorCore/PreviewScaleEdit.h"

#include <array>
#include <doctest/doctest.h>
#include <limits>

namespace Herta
{
TEST_CASE("Locked preview scale preserves nonuniform proportions")
{
	std::array Scale{2.0f, 3.0f, 4.0f};
	CHECK(TrySetProportionalPreviewScale(Scale, 1, 6.0f));
	const std::array Doubled{4.0f, 6.0f, 8.0f};
	CHECK(Scale == Doubled);
	CHECK(TrySetProportionalPreviewScale(Scale, 0, 1.0f));
	const std::array Halved{1.0f, 1.5f, 2.0f};
	CHECK(Scale == Halved);
}

TEST_CASE("Locked preview scale rejects invalid edits without mutating the transform")
{
	const std::array Original{2.0f, 3.0f, 4.0f};
	for (const float Value : std::array{0.0f, -1.0f, 0.0001f, 1001.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		auto Scale = Original;
		CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, Value));
		CHECK(Scale == Original);
	}
	auto Scale = Original;
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 3, 2.0f));
	CHECK(Scale == Original);
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, 1000.0f));
	CHECK(Scale == Original);
	Scale[1] = 0.0f;
	CHECK_FALSE(TrySetProportionalPreviewScale(Scale, 0, 3.0f));
	CHECK(Scale[0] == Original[0]);
	CHECK(Scale[1] == 0.0f);
}
}
