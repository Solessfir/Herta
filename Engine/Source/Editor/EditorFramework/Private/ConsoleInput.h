#pragma once

#include <imgui.h>

#include <algorithm>
#include <string_view>

namespace Herta
{
constexpr bool HasConsoleCommandText(const std::string_view Text)
{
	return Text.find_first_not_of(" \t\r\n\v\f") != std::string_view::npos;
}

constexpr bool ShouldDismissConsoleSuggestions(const bool bCommandActive, const bool bSuggestionsHovered, const bool bEscapePressed)
{
	return bEscapePressed || (!bCommandActive && !bSuggestionsHovered);
}

inline int HandleConsoleInputShortcuts(ImGuiInputTextCallbackData& Data)
{
	const ImGuiIO& IO = ImGui::GetIO();
	if (Data.EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
	{
		return IO.KeyAlt && !IO.KeyCtrl && !IO.KeyShift && !IO.KeySuper && (ImGui::IsKeyDown(ImGuiKey_B) || ImGui::IsKeyDown(ImGuiKey_F)) ? 1 : 0;
	}

	if ((Data.EventFlag != ImGuiInputTextFlags_CallbackAlways && Data.EventFlag != ImGuiInputTextFlags_CallbackEdit) || IO.KeySuper || IO.KeyCtrl == IO.KeyAlt)
	{
		return 0;
	}

	const auto MoveCursor = [&](const int Position)
	{
		Data.CursorPos = Data.SelectionStart = Data.SelectionEnd = Position;
	};
	const auto Erase = [&](int Begin, int End)
	{
		if (Data.HasSelection())
		{
			Begin = std::min(Data.SelectionStart, Data.SelectionEnd);
			End = std::max(Data.SelectionStart, Data.SelectionEnd);
		}

		if (End > Begin)
		{
			Data.DeleteChars(Begin, End - Begin);
		}

		MoveCursor(Begin);
	};
	const auto IsSpace = [](const char Byte)
	{
		return Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n' || Byte == '\v' || Byte == '\f';
	};
	const auto IsWord = [](const unsigned char Byte)
	{
		// Keep UTF-8 sequences together so word movement cannot split a character.
		return (Byte >= 'a' && Byte <= 'z') || (Byte >= 'A' && Byte <= 'Z') || (Byte >= '0' && Byte <= '9') || Byte == '_' || Byte >= 0x80;
	};

	if (IO.KeyCtrl)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_A, true))
		{
			MoveCursor(IO.KeyShift ? Data.BufTextLen : 0);
			if (IO.KeyShift)
			{
				Data.SelectionStart = 0;
			}
		}
		else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_E, true))
		{
			MoveCursor(Data.BufTextLen);
		}
		else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_U, true))
		{
			Erase(0, Data.CursorPos);
		}
		else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_K, true))
		{
			Erase(Data.CursorPos, Data.BufTextLen);
		}
		else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_W, true))
		{
			int Begin = Data.CursorPos;
			while (Begin > 0 && IsSpace(Data.Buf[Begin - 1]))
			{
				--Begin;
			}

			while (Begin > 0 && !IsSpace(Data.Buf[Begin - 1]))
			{
				--Begin;
			}

			Erase(Begin, Data.CursorPos);
		}
	}
	else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_B, true))
	{
		int Position = Data.CursorPos;
		while (Position > 0 && !IsWord(static_cast<unsigned char>(Data.Buf[Position - 1])))
		{
			--Position;
		}

		while (Position > 0 && IsWord(static_cast<unsigned char>(Data.Buf[Position - 1])))
		{
			--Position;
		}

		MoveCursor(Position);
	}
	else if (!IO.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F, true))
	{
		int Position = Data.CursorPos;
		while (Position < Data.BufTextLen && !IsWord(static_cast<unsigned char>(Data.Buf[Position])))
		{
			++Position;
		}

		while (Position < Data.BufTextLen && IsWord(static_cast<unsigned char>(Data.Buf[Position])))
		{
			++Position;
		}

		MoveCursor(Position);
	}

	return 0;
}
}
