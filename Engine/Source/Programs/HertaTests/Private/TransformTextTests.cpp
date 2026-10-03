#include "Herta/EditorCore/TransformText.h"

#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

namespace Herta
{
TEST_CASE("Transform numeric expressions honor arithmetic precedence and parentheses")
{
	CHECK(EvaluateNumericExpression("10/2") == doctest::Approx(5.f));
	CHECK(EvaluateNumericExpression("2 + 3 * 4") == doctest::Approx(14.f));
	CHECK(EvaluateNumericExpression("-(2 + 3) * 4") == doctest::Approx(-20.f));
	CHECK(EvaluateNumericExpression("1e2 / 4") == doctest::Approx(25.f));
	CHECK(EvaluateNumericExpression(" + - - 8 ") == doctest::Approx(8.f));
}

TEST_CASE("Transform numeric expressions reject invalid, nonfinite, and unbounded input")
{
	CHECK_FALSE(EvaluateNumericExpression(""));
	CHECK_FALSE(EvaluateNumericExpression("1+"));
	CHECK_FALSE(EvaluateNumericExpression("1/0"));
	CHECK_FALSE(EvaluateNumericExpression("(1+2"));
	CHECK_FALSE(EvaluateNumericExpression("1)"));
	CHECK_FALSE(EvaluateNumericExpression("1e100"));
	CHECK_FALSE(EvaluateNumericExpression("nan"));
	CHECK_FALSE(EvaluateNumericExpression(std::string(257, '1')));
	CHECK_FALSE(EvaluateNumericExpression(std::string(34, '(') + "1" + std::string(34, ')')));
}

TEST_CASE("Transform vector clipboard text round-trips with UE component labels")
{
	const FVector3 Original{std::nextafter(1.f, 2.f), -1234.5678f, std::numeric_limits<float>::max()};
	const std::string Text = FormatTransformVectorClipboard(Original);
	CHECK(Text.starts_with("(X="));
	const auto Parsed = ParseTransformVectorClipboard(Text);
	REQUIRE(Parsed);
	CHECK(*Parsed == Original);
	CHECK((ParseTransformVectorClipboard(" (X=10/2, Y=-3, Z=(2+1)*2) ") == FVector3{5, -3, 6}));
	CHECK_FALSE(ParseTransformVectorClipboard("(X=1,Y=2,Z=3) trailing"));
	CHECK_FALSE(ParseTransformVectorClipboard("(X=1,Y=2,Z=1/0)"));
	CHECK_FALSE(ParseTransformVectorClipboard("(X=1,Y=2,W=3)"));
}

TEST_CASE("Transform location and scale clipboard preserve XYZ samples")
{
	CHECK((ParseTransformVectorClipboard("(X=0.000000,Y=0.000000,Z=0.000000)") == FVector3{0, 0, 0}));
	CHECK((ParseTransformVectorClipboard("(X=1.000000,Y=1.000000,Z=1.000000)") == FVector3{1, 1, 1}));
	CHECK((ParseTransformVectorClipboard("(X=3310.000000,Y=570.000000,Z=410.000000)") == FVector3{3310.f, 570.f, 410.f}));
	CHECK((ParseTransformVectorClipboard("(X=1.250000,Y=0.500000,Z=2.750000)") == FVector3{1.25f, 0.5f, 2.75f}));
}

TEST_CASE("Transform rotation clipboard uses UE Pitch Yaw Roll labels without conversion")
{
	const FVector3 Original{12.5f, -34.25f, 78.75f};
	const std::string Text = FormatTransformRotationClipboard(Original);
	CHECK(Text == "(Pitch=12.5,Yaw=-34.25,Roll=78.75)");
	CHECK(ParseTransformRotationClipboard(Text) == Original);
	CHECK((ParseTransformRotationClipboard("(Pitch=0.000000,Yaw=0.000000,Roll=0.000000)") == FVector3{0, 0, 0}));
	CHECK((ParseTransformRotationClipboard("(Pitch=10.250000,Yaw=-20.500000,Roll=90.125000)") == FVector3{10.25f, -20.5f, 90.125f}));
	CHECK_FALSE(ParseTransformRotationClipboard("(X=1,Y=2,Z=3)"));
	CHECK_FALSE(ParseTransformRotationClipboard("(Pitch=1,Yaw=2,Roll=3) trailing"));
	CHECK_FALSE(ParseTransformRotationClipboard("(Pitch=1,Yaw=2,Roll=1/0)"));
	CHECK_FALSE(ParseTransformRotationClipboard("(Pitch=1,Yaw=2,Z=3)"));
}
}
