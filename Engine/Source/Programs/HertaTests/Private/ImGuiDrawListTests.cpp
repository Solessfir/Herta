#include <doctest/doctest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <span>

namespace Herta
{
namespace
{
struct FDrawListTestContext
{
	ImGuiContext* PreviousContext = ImGui::GetCurrentContext();
	ImGuiContext* Context = ImGui::CreateContext();

	FDrawListTestContext()
	{
		ImGui::SetCurrentContext(Context);
	}

	~FDrawListTestContext()
	{
		ImGui::DestroyContext(Context);
		ImGui::SetCurrentContext(PreviousContext);
	}

	FDrawListTestContext(const FDrawListTestContext&) = delete;
	FDrawListTestContext& operator=(const FDrawListTestContext&) = delete;
	FDrawListTestContext(FDrawListTestContext&&) = delete;
	FDrawListTestContext& operator=(FDrawListTestContext&&) = delete;
};

constexpr ImU32 Red = IM_COL32(255, 0, 0, 255);
constexpr ImU32 Green = IM_COL32(0, 255, 0, 255);
constexpr ImU32 Blue = IM_COL32(0, 0, 255, 255);
constexpr ImU32 Yellow = IM_COL32(255, 255, 0, 255);

void InitializeDrawList(ImDrawList& DrawList)
{
	DrawList._ResetForNewFrame();
	DrawList.PushClipRect({0.f, 0.f}, {100.f, 100.f});
	DrawList.PushTexture(ImTextureRef(static_cast<ImTextureID>(1)));
}

void AddRectangle(ImDrawList& DrawList, const ImU32 Color)
{
	DrawList.AddRectFilled({10.f, 10.f}, {20.f, 20.f}, Color);
}

void CheckRectangleOrder(const ImDrawList& DrawList, const std::span<const ImU32> Colors)
{
	REQUIRE(DrawList.IdxBuffer.Size == static_cast<int>(Colors.size() * 6));
	unsigned int IndexCount = 0;
	for (const ImDrawCmd& Command : DrawList.CmdBuffer)
	{
		CHECK(Command.IdxOffset == IndexCount);
		for (unsigned int Index = 0; Index < Command.ElemCount; ++Index)
		{
			const unsigned int VertexIndex = Command.VtxOffset + DrawList.IdxBuffer[static_cast<int>(Command.IdxOffset + Index)];
			REQUIRE(VertexIndex < static_cast<unsigned int>(DrawList.VtxBuffer.Size));
			CHECK(DrawList.VtxBuffer[static_cast<int>(VertexIndex)].col == Colors[(IndexCount + Index) / 6]);
		}

		IndexCount += Command.ElemCount;
	}

	CHECK(IndexCount == Colors.size() * 6);
}
}

TEST_CASE("ImGui channel swaps preserve the current channel and subsequent drawing")
{
	for (const int CurrentChannel : {0, 1, 2})
	{
		CAPTURE(CurrentChannel);
		FDrawListTestContext Context;
		ImDrawList DrawList(ImGui::GetDrawListSharedData());
		InitializeDrawList(DrawList);
		DrawList.ChannelsSplit(3);
		for (const int Channel : {0, 1, 2})
		{
			DrawList.ChannelsSetCurrent(Channel);
			AddRectangle(DrawList, std::array{Red, Green, Blue}[Channel]);
		}

		DrawList.ChannelsSetCurrent(CurrentChannel);
		DrawList._Splitter.SwapChannels(&DrawList, 0, 1);
		CHECK(DrawList._Splitter._Current == CurrentChannel);
		CHECK(DrawList._IdxWritePtr == DrawList.IdxBuffer.Data + DrawList.IdxBuffer.Size);
		AddRectangle(DrawList, Yellow);
		DrawList.ChannelsMerge();
		const std::array Expected = CurrentChannel == 0 ? std::array{Green, Yellow, Red, Blue} : CurrentChannel == 1 ? std::array{Green, Red, Yellow, Blue}
		                                                                                                             : std::array{Green, Red, Blue, Yellow};
		CheckRectangleOrder(DrawList, Expected);
	}
}

TEST_CASE("ImGui swapping a channel with itself is a no-op")
{
	FDrawListTestContext Context;
	ImDrawList DrawList(ImGui::GetDrawListSharedData());
	InitializeDrawList(DrawList);
	DrawList.ChannelsSplit(3);
	AddRectangle(DrawList, Red);
	for (const int Channel : {0, 1, 2})
	{
		const ImDrawCmd* const Commands = DrawList.CmdBuffer.Data;
		const ImDrawIdx* const Indices = DrawList.IdxBuffer.Data;
		DrawList._Splitter.SwapChannels(&DrawList, Channel, Channel);
		CHECK(DrawList._Splitter._Current == 0);
		CHECK(DrawList.CmdBuffer.Data == Commands);
		CHECK(DrawList.IdxBuffer.Data == Indices);
	}

	AddRectangle(DrawList, Yellow);
	DrawList.ChannelsMerge();
	CheckRectangleOrder(DrawList, std::array{Red, Yellow});
}

TEST_CASE("ImGui channel swaps retain empty channels and accept subsequent drawing")
{
	for (const int CurrentChannel : {0, 1, 2})
	{
		CAPTURE(CurrentChannel);
		FDrawListTestContext Context;
		ImDrawList DrawList(ImGui::GetDrawListSharedData());
		InitializeDrawList(DrawList);
		DrawList.ChannelsSplit(3);
		AddRectangle(DrawList, Red);
		DrawList.ChannelsSetCurrent(2);
		AddRectangle(DrawList, Blue);
		DrawList.ChannelsSetCurrent(CurrentChannel);
		DrawList._Splitter.SwapChannels(&DrawList, 0, 1);
		CHECK(DrawList._Splitter._Current == CurrentChannel);
		AddRectangle(DrawList, Yellow);
		DrawList.ChannelsMerge();
		const std::array Expected = CurrentChannel == 0 ? std::array{Yellow, Red, Blue} : CurrentChannel == 1 ? std::array{Red, Yellow, Blue}
		                                                                                                      : std::array{Red, Blue, Yellow};
		CheckRectangleOrder(DrawList, Expected);
	}
}

TEST_CASE("ImGui channel swaps preserve callbacks and draw command metadata")
{
	FDrawListTestContext Context;
	ImDrawList DrawList(ImGui::GetDrawListSharedData());
	InitializeDrawList(DrawList);
	DrawList.Flags |= ImDrawListFlags_AllowVtxOffset;
	DrawList.ChannelsSplit(2);
	DrawList.PushClipRect({1.f, 2.f}, {30.f, 40.f});
	DrawList.PushTexture(ImTextureRef(static_cast<ImTextureID>(11)));
	AddRectangle(DrawList, Red);
	int CallbackData = 42;
	const ImDrawCallback Callback = [](const ImDrawList*, const ImDrawCmd*) {};
	DrawList.AddCallback(Callback, &CallbackData);
	DrawList.PopTexture();
	DrawList.PopClipRect();
	DrawList.ChannelsSetCurrent(1);
	DrawList.PushClipRect({5.f, 6.f}, {70.f, 80.f});
	DrawList.PushTexture(ImTextureRef(static_cast<ImTextureID>(22)));
	// Start a new vertex segment without allocating 64K vertices to trigger rollover.
	DrawList._CmdHeader.VtxOffset = static_cast<unsigned int>(DrawList.VtxBuffer.Size);
	DrawList._VtxCurrentIdx = 0;
	DrawList._OnChangedVtxOffset();
	AddRectangle(DrawList, Green);
	DrawList._Splitter.SwapChannels(&DrawList, 0, 1);
	DrawList.ChannelsMerge();
	CheckRectangleOrder(DrawList, std::array{Green, Red});
	int CallbackCount = 0;
	int RectangleCount = 0;
	for (const ImDrawCmd& Command : DrawList.CmdBuffer)
	{
		if (Command.UserCallback != nullptr)
		{
			++CallbackCount;
			CHECK(Command.UserCallback == Callback);
			CHECK(Command.UserCallbackData == &CallbackData);
			CHECK(Command.IdxOffset == 12);
		}

		if (Command.ElemCount != 0)
		{
			const bool bGreen = RectangleCount++ == 0;
			CHECK(Command.GetTexID() == static_cast<ImTextureID>(bGreen ? 22 : 11));
			CHECK(Command.VtxOffset == (bGreen ? 4u : 0u));
			CHECK(Command.ClipRect.x == (bGreen ? 5.f : 1.f));
			CHECK(Command.ClipRect.y == (bGreen ? 6.f : 2.f));
			CHECK(Command.ClipRect.z == (bGreen ? 70.f : 30.f));
			CHECK(Command.ClipRect.w == (bGreen ? 80.f : 40.f));
		}
	}

	CHECK(CallbackCount == 1);
	CHECK(RectangleCount == 2);
}
}
