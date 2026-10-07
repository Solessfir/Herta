#include "PerformancePanel.h"

#include "Herta/Renderer/Visuals.h"
#include "Herta/ToolUI/ToolUI.h"

#include <imgui.h>

#include <algorithm>
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
void DrawLegendValue(const char* const Label, const ImU32 Color, const std::string& Value)
{
	const float Scale = ImGui::GetFontSize() / 15.f;
	const ImVec2 Position = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddCircleFilled({Position.x + 4.f * Scale, Position.y + ImGui::GetTextLineHeight() * 0.5f}, 3.5f * Scale, Color);
	ImGui::SetCursorScreenPos({Position.x + 14.f * Scale, Position.y});
	ImGui::TextDisabled("%s", Label);
	ImGui::SameLine();
	ImGui::TextUnformatted(Value.c_str());
}

void DrawSparkline(const FMetricHistory& History, const float Width, const float Height, const float Ceiling)
{
	const ImVec2 Minimum = ImGui::GetCursorScreenPos();
	ImGui::Dummy({Width, Height});
	const std::size_t Count = History.GetCount();
	if (Count < 2 || Ceiling <= 0.f)
	{
		return;
	}

	ImDrawList* const Draw = ImGui::GetWindowDrawList();
	const float Step = Width / static_cast<float>(FMetricHistory::Capacity - 1);
	const float Start = Minimum.x + Width - static_cast<float>(Count - 1) * Step;
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Draw->PathLineTo({Start + static_cast<float>(Index) * Step, Minimum.y + Height - std::clamp(History.At(Index) / Ceiling, 0.f, 1.f) * Height});
	}

	Draw->PathStroke(GpuColor, 0, ImGui::GetFontSize() / 15.f);
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

	const float Frame = State.Frame.GetAverage(AverageWindow);
	ImGui::Text("%.2f ms", Frame);
	ImGui::SameLine();
	ImGui::TextDisabled("%.0f fps", Frame > 0.f ? 1000.f / Frame : 0.f);
	ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Resume").x - ImGui::GetStyle().FramePadding.x * 2.f);
	if (ImGui::SmallButton(State.bPaused ? "Resume" : "Pause"))
	{
		State.bPaused = !State.bPaused;
	}

	DrawLegendValue("Frame", FrameColor, std::format("{:.2f} ms", Frame));
	ImGui::SameLine(0.f, 18.f * Scale);
	DrawLegendValue("CPU", CpuColor, std::format("{:.2f} ms", State.Cpu.GetAverage(AverageWindow)));
	ImGui::SameLine(0.f, 18.f * Scale);
	DrawLegendValue("GPU", GpuColor, std::format("{:.2f} ms", State.Gpu.GetAverage(AverageWindow)));
	const std::array Series{FGraphSeries{.History = &State.Frame, .Color = FrameColor}, FGraphSeries{.History = &State.Cpu, .Color = CpuColor}, FGraphSeries{.History = &State.Gpu, .Color = GpuColor}};
	DrawHistoryGraph("##FrameGraph", Series, 110.f * Scale, 1000.f / 60.f);
	ImGui::Spacing();

	if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen))
	{
		float Ceiling = 0.f;
		for (const FPerformancePassHistory& Pass : State.Passes)
		{
			Ceiling = std::max(Ceiling, Pass.Milliseconds.GetMaximum());
		}

		if (State.Passes.empty())
		{
			ImGui::TextDisabled("No GPU timings reported");
		}
		else if (ImGui::BeginTable("##Passes", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
		{
			ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 1.f);
			ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 1.f);
			ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed, 64.f * Scale);
			// Every row shares one scale, so sparkline heights compare pass costs directly.
			for (const FPerformancePassHistory& Pass : State.Passes)
			{
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::TextUnformatted(Pass.Name.c_str());
				ImGui::TableSetColumnIndex(1);
				DrawSparkline(Pass.Milliseconds, ImGui::GetContentRegionAvail().x, ImGui::GetTextLineHeight(), Ceiling);
				ImGui::TableSetColumnIndex(2);
				const std::string Value = std::format("{:.3f}", Pass.Milliseconds.GetAverage(AverageWindow));
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(Value.c_str()).x);
				ImGui::TextUnformatted(Value.c_str());
			}

			ImGui::EndTable();
		}

		if (Sample.GpuUIMilliseconds)
		{
			ImGui::TextDisabled("Editor UI %.3f ms", *Sample.GpuUIMilliseconds);
		}
	}

	if (ImGui::CollapsingHeader("Renderer", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::TextDisabled("Lights");
		ImGui::SameLine(110.f * Scale);
		ImGui::Text("%zu / %zu", Sample.EnabledLights, Sample.MaximumLights);
		ImGui::TextDisabled("Draws");
		ImGui::SameLine(110.f * Scale);
		ImGui::Text("%zu", Sample.DrawCount);
		ImGui::TextDisabled("Render targets");
		ImGui::SameLine(110.f * Scale);
		ImGui::Text("%.1f MiB", static_cast<double>(Sample.RenderTargetBytes) / (1024. * 1024.));
		ImGui::TextDisabled("Shadows");
		ImGui::SameLine(110.f * Scale);
		ImGui::Text("%u px cascades, %u px local tiles", ShadowCascadeResolution, ShadowTileResolution);
	}

	ToolUI.EndPanel();
}
}
