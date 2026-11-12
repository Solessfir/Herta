#include "PerformancePanel.h"

#include "Herta/Renderer/Visuals.h"
#include "Herta/ToolUI/EditorText.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>

namespace Herta
{
namespace
{
constexpr ImU32 FrameColor = IM_COL32(222, 222, 228, 255);
constexpr ImU32 CpuColor = IM_COL32(122, 170, 238, 255);
constexpr ImU32 GpuColor = IM_COL32(156, 211, 174, 255);
constexpr std::size_t AverageWindow = 60;

struct FGraphSeries
{
	const FMetricHistory* History = nullptr;
	ImU32 Color = 0;
};

// Plots several histories on one shared scale with 60 and 30 fps guides, newest sample on the right.
void DrawHistoryGraph(const char* const Id, const std::span<const FGraphSeries> Series, const float Height, const float Floor)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Minimum = ImGui::GetCursorScreenPos();
	const ImVec2 Size{ImGui::GetContentRegionAvail().x, Height};
	ImGui::InvisibleButton(Id, Size);
	const ImVec2 Maximum{Minimum.x + Size.x, Minimum.y + Size.y};
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Minimum, Maximum, IM_COL32(255, 255, 255, 10), 6.f * Scale);

	float Ceiling = Floor;
	for (const FGraphSeries& Entry : Series)
	{
		Ceiling = std::max(Ceiling, Entry.History->GetMaximum());
	}

	Ceiling *= 1.1f;
	const auto Y = [&](const float Value)
	{
		return Maximum.y - 4.f * Scale - std::clamp(Value / Ceiling, 0.f, 1.f) * (Size.y - 8.f * Scale);
	};

	for (const auto& [Milliseconds, Label] : {std::pair{1000.f / 60.f, "60 fps"}, std::pair{1000.f / 30.f, "30 fps"}})
	{
		if (Milliseconds < Ceiling)
		{
			const float GuideY = Y(Milliseconds);
			Draw->AddLine({Minimum.x + 4.f * Scale, GuideY}, {Maximum.x - 4.f * Scale, GuideY}, IM_COL32(255, 255, 255, 28), Scale);
			Draw->AddText({Minimum.x + 8.f * Scale, GuideY - ImGui::GetFontSize() - 1.f * Scale}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Label);
		}
	}

	const float Step = (Size.x - 8.f * Scale) / static_cast<float>(FMetricHistory::Capacity - 1);
	for (const FGraphSeries& Entry : Series)
	{
		const std::size_t Count = Entry.History->GetCount();
		if (Count < 2)
		{
			continue;
		}

		// Right-aligned so a partially filled history grows in from the newest edge.
		const float Start = Maximum.x - 4.f * Scale - static_cast<float>(Count - 1) * Step;
		for (std::size_t Index = 0; Index < Count; ++Index)
		{
			Draw->PathLineTo({Start + static_cast<float>(Index) * Step, Y(Entry.History->At(Index))});
		}

		Draw->PathStroke(Entry.Color, 0, 1.5f * Scale);
	}

	if (ImGui::IsItemHovered())
	{
		const std::size_t Count = Series.front().History->GetCount();
		const float FromRight = (Maximum.x - 4.f * Scale - ImGui::GetIO().MousePos.x) / Step;
		const auto Back = static_cast<std::size_t>(std::max(0.f, FromRight + 0.5f));
		if (Back < Count)
		{
			const float X = Maximum.x - 4.f * Scale - static_cast<float>(Back) * Step;
			Draw->AddLine({X, Minimum.y + 4.f * Scale}, {X, Maximum.y - 4.f * Scale}, IM_COL32(255, 255, 255, 60), Scale);
			ImGui::BeginTooltip();
			ImGui::TextDisabled("%zu frames ago", Back);
			for (const FGraphSeries& Entry : Series)
			{
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Entry.Color));
				ImGui::Text("%.2f ms", Entry.History->At(Count - 1 - Back));
				ImGui::PopStyleColor();
			}

			ImGui::EndTooltip();
		}
	}
}

// One line of the summary: a colored legend dot, a label, and a smoothed value.
// One headline number in a tile, with the color of its graph line when it has one.
void DrawStatTile(const char* const Label, const ImU32 Color, const std::string& Value, const std::string_view Unit, const float Width)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Minimum = ImGui::GetCursorScreenPos();
	const ImVec2 Size{Width, ImGui::GetFontSize() * 2.f + 16.f * Scale};
	ImGui::Dummy(Size);
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	Draw->AddRectFilled(Minimum, {Minimum.x + Size.x, Minimum.y + Size.y}, ImGui::GetColorU32(ImGuiCol_FrameBg), 6.f * Scale);
	float LabelX = Minimum.x + 8.f * Scale;
	if (Color != 0)
	{
		Draw->AddCircleFilled({LabelX + 3.f * Scale, Minimum.y + 6.f * Scale + ImGui::GetFontSize() * 0.5f}, 3.f * Scale, Color);
		LabelX += 11.f * Scale;
	}

	Draw->AddText({LabelX, Minimum.y + 6.f * Scale}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Label);
	const ImVec2 ValuePosition{Minimum.x + 8.f * Scale, Minimum.y + 8.f * Scale + ImGui::GetFontSize()};
	Draw->AddText(ValuePosition, ImGui::GetColorU32(ImGuiCol_Text), Value.c_str());
	Draw->AddText({ValuePosition.x + ImGui::CalcTextSize(Value.c_str()).x + 4.f * Scale, ValuePosition.y}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Unit.data(), Unit.data() + Unit.size());
}

// A pass as a row: its name, a bar against the costliest pass, and its time. The costliest pass is amber.
void DrawPassRow(const std::string_view Name, const float Milliseconds, const float Ceiling, const bool bHottest)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Minimum = ImGui::GetCursorScreenPos();
	const float Width = ImGui::GetContentRegionAvail().x;
	const float Height = ImGui::GetFontSize() + 6.f * Scale;
	ImGui::Dummy({Width, Height});
	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	if (ImGui::IsItemHovered())
	{
		Draw->AddRectFilled(Minimum, {Minimum.x + Width, Minimum.y + Height}, ImGui::GetColorU32(ImGuiCol_HeaderHovered), 4.f * Scale);
	}

	const float TextY = Minimum.y + 3.f * Scale;
	const std::string Value = std::format("{:.2f} ms", Milliseconds);
	const float ValueWidth = ImGui::CalcTextSize("00.00 ms").x;
	const float NameWidth = std::min(150.f * Scale, Width * 0.42f);
	const ImVec4 Clip{Minimum.x, Minimum.y, Minimum.x + NameWidth, Minimum.y + Height};
	Draw->AddText(nullptr, 0.f, {Minimum.x + 8.f * Scale, TextY}, ImGui::GetColorU32(ImGuiCol_TextDisabled), Name.data(), Name.data() + Name.size(), 0.f, &Clip);
	const float BarLeft = Minimum.x + NameWidth + 8.f * Scale;
	const float BarRight = Minimum.x + Width - ValueWidth - 16.f * Scale;
	const float BarY = Minimum.y + Height * 0.5f;
	if (BarRight > BarLeft)
	{
		Draw->AddRectFilled({BarLeft, BarY - 3.f * Scale}, {BarRight, BarY + 3.f * Scale}, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.f * Scale);
		const float Fill = Ceiling > 0.f ? std::clamp(Milliseconds / Ceiling, 0.f, 1.f) : 0.f;
		Draw->AddRectFilled({BarLeft, BarY - 3.f * Scale}, {BarLeft + std::max(2.f * Scale, (BarRight - BarLeft) * Fill), BarY + 3.f * Scale}, bHottest ? IM_COL32(ToolUITheme::Warning.Red, ToolUITheme::Warning.Green, ToolUITheme::Warning.Blue, 255) : IM_COL32(ToolUITheme::NeutralAccent.Red, ToolUITheme::NeutralAccent.Green, ToolUITheme::NeutralAccent.Blue, 255), 3.f * Scale);
	}

	Draw->AddText({Minimum.x + Width - 8.f * Scale - ImGui::CalcTextSize(Value.c_str()).x, TextY}, ImGui::GetColorU32(ImGuiCol_Text), Value.c_str());
}

// A small heading over a group of rows, with an optional total at the right.
void DrawGroupHeading(const char* const Label, const std::string& Total = {})
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::Dummy({0.f, 4.f * Scale});
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.f * Scale);
	ImGui::TextDisabled("%s", Label);
	if (!Total.empty())
	{
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 8.f * Scale - ImGui::CalcTextSize(Total.c_str()).x);
		ImGui::TextDisabled("%s", Total.c_str());
	}
}
}

void FMetricHistory::Push(const float Value)
{
	Values[Next] = Value;
	Next = (Next + 1) % Capacity;
	Count = std::min(Count + 1, Capacity);
}

std::size_t FMetricHistory::GetCount() const noexcept
{
	return Count;
}

float FMetricHistory::At(const std::size_t Index) const noexcept
{
	return Values[(Next + Capacity - Count + Index) % Capacity];
}

float FMetricHistory::GetLatest() const noexcept
{
	return Count == 0 ? 0.f : At(Count - 1);
}

float FMetricHistory::GetAverage(const std::size_t Window) const noexcept
{
	const std::size_t Samples = std::min(Window, Count);
	if (Samples == 0)
	{
		return 0.f;
	}

	float Sum = 0.f;
	for (std::size_t Index = Count - Samples; Index < Count; ++Index)
	{
		Sum += At(Index);
	}

	return Sum / static_cast<float>(Samples);
}

float FMetricHistory::GetMaximum() const noexcept
{
	float Maximum = 0.f;
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Maximum = std::max(Maximum, At(Index));
	}

	return Maximum;
}

void RecordPerformanceSample(FPerformancePanelState& State, const FPerformanceSample& Sample)
{
	if (State.bPaused)
	{
		return;
	}

	float GpuTotal = 0.f;
	for (const FGpuPassTiming& Pass : Sample.Passes)
	{
		GpuTotal += static_cast<float>(Pass.Milliseconds);
		if (std::ranges::find(State.Passes, Pass.Name, &FPerformancePassHistory::Name) == State.Passes.end())
		{
			State.Passes.push_back({.Name = Pass.Name});
		}
	}

	State.Frame.Push(static_cast<float>(Sample.FrameMilliseconds));
	State.Cpu.Push(static_cast<float>(Sample.CpuMilliseconds));
	State.Gpu.Push(GpuTotal + static_cast<float>(Sample.GpuUIMilliseconds.value_or(0.)));
	for (FPerformancePassHistory& History : State.Passes)
	{
		const auto Pass = std::ranges::find(Sample.Passes, History.Name, &FGpuPassTiming::Name);
		History.Milliseconds.Push(Pass == Sample.Passes.end() ? 0.f : static_cast<float>(Pass->Milliseconds));
	}
}

void DrawPerformancePanel(FToolUIContext& ToolUI, bool& bOpen, FPerformancePanelState& State, const FPerformanceSample& Sample)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	ImGui::SetNextWindowSize({460.f * Scale, 560.f * Scale}, ImGuiCond_FirstUseEver);
	if (!ToolUI.BeginPanel("Performance", &bOpen))
	{
		ToolUI.EndPanel();
		return;
	}

	// Four headline numbers, each a tile; the three timings carry their graph line's color.
	using namespace Text::Editor;
	const float Frame = State.Frame.GetAverage(AverageWindow);
	const float Gap = 4.f * Scale;
	const float TileWidth = std::floor((ImGui::GetContentRegionAvail().x - Gap * 3.f) / 4.f);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {Gap, 6.f * Scale});
	DrawStatTile(ToolUI.Text(Perf_Frame, "Frame").c_str(), FrameColor, std::format("{:.1f}", Frame), "ms", TileWidth);
	ImGui::SameLine();
	DrawStatTile(ToolUI.Text(Perf_Gpu, "GPU").c_str(), GpuColor, std::format("{:.1f}", State.Gpu.GetAverage(AverageWindow)), "ms", TileWidth);
	ImGui::SameLine();
	DrawStatTile(ToolUI.Text(Perf_Cpu, "CPU").c_str(), CpuColor, std::format("{:.1f}", State.Cpu.GetAverage(AverageWindow)), "ms", TileWidth);
	ImGui::SameLine();
	DrawStatTile(ToolUI.Text(Perf_Draws, "Draws").c_str(), 0, std::to_string(Sample.DrawCount), "", TileWidth);
	ImGui::PopStyleVar();

	const std::array Series{FGraphSeries{.History = &State.Frame, .Color = FrameColor}, FGraphSeries{.History = &State.Cpu, .Color = CpuColor}, FGraphSeries{.History = &State.Gpu, .Color = GpuColor}};
	DrawHistoryGraph("##FrameGraph", Series, 96.f * Scale, 1000.f / 60.f);
	const std::string PauseText = State.bPaused ? ToolUI.Text(Perf_Resume, "Resume") : ToolUI.Text(Perf_Pause, "Pause");
	const char* const PauseLabel = PauseText.c_str();
	ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(PauseLabel).x - ImGui::GetStyle().FramePadding.x * 2.f);
	if (ImGui::SmallButton(PauseLabel))
	{
		State.bPaused = !State.bPaused;
	}

	// Bars share one scale, the costliest pass, so their lengths compare directly.
	float Ceiling = 0.f;
	float Total = static_cast<float>(Sample.GpuUIMilliseconds.value_or(0.));
	const FPerformancePassHistory* Hottest = nullptr;
	for (const FPerformancePassHistory& Pass : State.Passes)
	{
		const float Average = Pass.Milliseconds.GetAverage(AverageWindow);
		Total += Average;
		if (Average > Ceiling)
		{
			Ceiling = Average;
			Hottest = &Pass;
		}
	}

	DrawGroupHeading(ToolUI.Text(Perf_GpuPasses, "GPU passes").c_str(), std::format("{:.2f} ms", Total));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.f, 1.f * Scale});
	if (State.Passes.empty())
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.f * Scale);
		ImGui::TextDisabled("%s", ToolUI.Text(Perf_NoTimings, "No GPU timings reported").c_str());
	}

	for (const FPerformancePassHistory& Pass : State.Passes)
	{
		DrawPassRow(Pass.Name, Pass.Milliseconds.GetAverage(AverageWindow), Ceiling, &Pass == Hottest);
	}

	if (Sample.GpuUIMilliseconds)
	{
		DrawPassRow(ToolUI.Text(Perf_EditorUi, "Editor UI"), static_cast<float>(*Sample.GpuUIMilliseconds), Ceiling, false);
	}

	ImGui::PopStyleVar();

	DrawGroupHeading(ToolUI.Text(Perf_Renderer, "Renderer").c_str());
	const auto ValueRow = [Scale](const std::string& LabelText, const std::string& Value)
	{
		ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.f * Scale);
		ImGui::TextDisabled("%s", LabelText.c_str());
		ImGui::SameLine(ImGui::GetContentRegionMax().x - 8.f * Scale - ImGui::CalcTextSize(Value.c_str()).x);
		ImGui::TextUnformatted(Value.c_str());
	};

	ValueRow(ToolUI.Text(Perf_Lights, "Lights"), std::format("{} / {}", Sample.EnabledLights, Sample.MaximumLights));
	ValueRow(ToolUI.Text(Perf_RenderTargets, "Render targets"), std::format("{:.1f} MiB", static_cast<double>(Sample.RenderTargetBytes) / (1024. * 1024.)));
	ValueRow(ToolUI.Text(Perf_Shadows, "Shadows"), ToolUI.Text(Perf_ShadowSizes, std::format("{} px cascades, {} px tiles", ShadowCascadeResolution, ShadowTileResolution), std::array{FMessageArgument{"cascade", static_cast<std::int64_t>(ShadowCascadeResolution)}, FMessageArgument{"tile", static_cast<std::int64_t>(ShadowTileResolution)}}));
	ToolUI.EndPanel();
}
}
