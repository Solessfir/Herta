#pragma once

#include "Herta/EditorFramework/OutputLog.h"

#include <imgui.h>

#include <algorithm>
#include <limits>
#include <string_view>

namespace Herta
{
[[nodiscard]] inline std::size_t GetNextOutputLogUtf8Boundary(const std::string_view Text, const std::size_t ByteOffset) noexcept
{
	if (ByteOffset >= Text.size())
	{
		return Text.size();
	}

	const unsigned char FirstByte = static_cast<unsigned char>(Text[ByteOffset]);
	std::size_t CodePointSize = 1;
	if ((FirstByte & 0xE0u) == 0xC0u)
	{
		CodePointSize = 2;
	}
	else if ((FirstByte & 0xF0u) == 0xE0u)
	{
		CodePointSize = 3;
	}
	else if ((FirstByte & 0xF8u) == 0xF0u)
	{
		CodePointSize = 4;
	}

	return std::min(Text.size(), ByteOffset + CodePointSize);
}

struct FOutputLogColumns
{
	float CategoryX = 0.f;
	float MessageX = 0.f;
};

[[nodiscard]] inline float MeasureOutputLogTextPrefix(const FOutputLogLine& Line, const std::size_t ByteCount, const FOutputLogColumns Columns)
{
	const std::size_t End = std::min(ByteCount, Line.Text.size());
	const char* const Text = Line.Text.data();
	if (End <= Line.TimeEnd)
	{
		return ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text, Text + End).x;
	}

	if (End < Line.CategoryBegin)
	{
		const float TimeWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text, Text + Line.TimeEnd).x;
		return TimeWidth + (Columns.CategoryX - TimeWidth) * static_cast<float>(End - Line.TimeEnd) / static_cast<float>(Line.CategoryBegin - Line.TimeEnd);
	}

	if (End <= Line.CategoryEnd)
	{
		return Columns.CategoryX + ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text + Line.CategoryBegin, Text + End).x;
	}

	if (End < Line.MessageBegin)
	{
		const float CategoryEndX = Columns.CategoryX + ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text + Line.CategoryBegin, Text + Line.CategoryEnd).x;
		return CategoryEndX + (Columns.MessageX - CategoryEndX) * static_cast<float>(End - Line.CategoryEnd) / static_cast<float>(Line.MessageBegin - Line.CategoryEnd);
	}

	return Columns.MessageX + ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text + Line.MessageBegin, Text + End).x;
}

[[nodiscard]] inline std::size_t FindOutputLogByteAtX(const FOutputLogLine& Line, const float LocalX, const FOutputLogColumns Columns)
{
	if (LocalX <= 0.f)
	{
		return 0;
	}

	const std::string_view Text = Line.Text;
	const float TimeWidth = MeasureOutputLogTextPrefix(Line, Line.TimeEnd, Columns);
	const float CategoryEndX = MeasureOutputLogTextPrefix(Line, Line.CategoryEnd, Columns);
	float TextX = 0.f;
	for (std::size_t ByteOffset = 0; ByteOffset < Text.size();)
	{
		const std::size_t NextByteOffset = GetNextOutputLogUtf8Boundary(Text, ByteOffset);
		float CharacterWidth = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), std::numeric_limits<float>::max(), 0.f, Text.data() + ByteOffset, Text.data() + NextByteOffset).x;
		if (ByteOffset >= Line.TimeEnd && ByteOffset < Line.CategoryBegin)
		{
			CharacterWidth = (Columns.CategoryX - TimeWidth) / static_cast<float>(Line.CategoryBegin - Line.TimeEnd);
		}
		else if (ByteOffset >= Line.CategoryEnd && ByteOffset < Line.MessageBegin)
		{
			CharacterWidth = (Columns.MessageX - CategoryEndX) / static_cast<float>(Line.MessageBegin - Line.CategoryEnd);
		}

		if (LocalX < TextX + CharacterWidth * 0.5f)
		{
			return ByteOffset;
		}

		TextX += CharacterWidth;
		ByteOffset = NextByteOffset;
	}

	return Text.size();
}
}
