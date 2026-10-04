#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace Herta
{
struct FScalingStatistics
{
	double Median = 0.;
	double P95 = 0.;
	double P99 = 0.;
};

inline FScalingStatistics SummarizeScalingSamples(const std::span<const double> Samples)
{
	if (Samples.empty())
	{
		return {};
	}

	std::vector<double> Sorted(Samples.begin(), Samples.end());
	std::ranges::sort(Sorted);
	const auto Percentile = [&Sorted](const double Fraction)
	{
		return Sorted[static_cast<std::size_t>(std::ceil(Fraction * static_cast<double>(Sorted.size()))) - 1];
	};

	const std::size_t Middle = Sorted.size() / 2;
	const double Median = Sorted.size() % 2 == 0 ? (Sorted[Middle - 1] + Sorted[Middle]) * 0.5 : Sorted[Middle];
	return {.Median = Median, .P95 = Percentile(0.95), .P99 = Percentile(0.99)};
}
}
