#pragma once

#include "Herta/RHI/Graphics.h"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Herta
{
class FToolUIContext;

// Fixed-length ring buffer of per-frame values, oldest first when read through At.
class FMetricHistory
{
public:
	static constexpr std::size_t Capacity = 240;

	void Push(float Value);
	[[nodiscard]] std::size_t GetCount() const noexcept;
	[[nodiscard]] float At(std::size_t Index) const noexcept;
	[[nodiscard]] float GetLatest() const noexcept;
	[[nodiscard]] float GetAverage(std::size_t Window) const noexcept;
	[[nodiscard]] float GetMaximum() const noexcept;

private:
	std::array<float, Capacity> Values{};
	std::size_t Next = 0;
	std::size_t Count = 0;
};

struct FPerformanceSample
{
	double FrameMilliseconds = 0.;
	double CpuMilliseconds = 0.;
	std::optional<double> GpuUIMilliseconds{};
	std::span<const FGpuPassTiming> Passes{};
	std::size_t EnabledLights = 0;
	std::size_t MaximumLights = 0;
	std::size_t RenderTargetBytes = 0;
	std::size_t DrawCount = 0;
};

struct FPerformancePassHistory
{
	std::string Name;
	FMetricHistory Milliseconds{};
};

struct FPerformancePanelState
{
	FMetricHistory Frame;
	FMetricHistory Cpu;
	FMetricHistory Gpu;
	// Insertion order follows the renderer's pass order; passes that stop reporting keep their history but record zero.
	std::vector<FPerformancePassHistory> Passes;
	bool bPaused = false;
};

void RecordPerformanceSample(FPerformancePanelState& State, const FPerformanceSample& Sample);

void DrawPerformancePanel(FToolUIContext& ToolUI, bool& bOpen, FPerformancePanelState& State, const FPerformanceSample& Sample);
}
