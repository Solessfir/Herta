#include "Herta/EditorCore/Mcp.h"

#include "Herta/Core/Json.h"

#include <simdjson.h>

#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

namespace Herta
{
namespace
{
constexpr std::string_view ServerVersion = "0.1.0";

// Tool schemas the client sees; the editor implements each one.
constexpr std::string_view ToolList = R"([
{"name": "editor_status", "description": "The open level, entity count, viewport size, whether writes are allowed, and Play state.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "editor_log", "description": "Editor log lines after a cursor. Pass the returned since and generation back to read only newer lines.", "inputSchema": {"type": "object", "properties": {"since": {"type": "integer", "minimum": 0}, "generation": {"type": "integer", "minimum": 0}, "limit": {"type": "integer", "minimum": 1, "maximum": 2000}}}},
{"name": "level_entities", "description": "Entities in the open level with their IDs, names, components, and transforms: translation in meters, rotation as a quaternion [x, y, z, w], and scale, relative to the parent when parent is true. A cube mesh is 1 m, so its scale is its size.", "inputSchema": {"type": "object", "properties": {"name_contains": {"type": "string"}, "limit": {"type": "integer", "minimum": 1, "maximum": 5000}}}},
{"name": "viewport_capture", "description": "Renders frames_ahead more frames, then returns the viewport as a PNG image read back from the GPU, scaled down to at most max_width pixels.", "inputSchema": {"type": "object", "properties": {"frames_ahead": {"type": "integer", "minimum": 1, "maximum": 600}, "max_width": {"type": "integer", "minimum": 64, "maximum": 7680}}}},
{"name": "gpu_timings", "description": "GPU time of each render pass in the latest measured frame, in milliseconds.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_start", "description": "Starts Play in the editor viewport, waiting up to 20 s for meshes and the player character to load, and returns the Play state. Play started this way only advances during play_input, and ignores the mouse, keyboard, and gamepads until play_stop.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_stop", "description": "Stops Play and returns to the authored level.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_state", "description": "The player's foot position, velocity, look yaw in radians (0 faces +Z, positive turns toward +X), grounded, crouched, sliding, and mantling flags, camera mode, animation state, and course objective.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_input", "description": "Drives the player for a number of frames of 1/60 s each, however fast the editor draws, then returns the Play state. move is [strafe right, forward] from -1 to 1; look is the total [yaw left, pitch up] in radians spread over the frames; face is a world [x, z] point to turn toward on the first frame; jump, throw, and toggle_camera press once on the first frame; sprint and crouch hold.", "inputSchema": {"type": "object", "properties": {"frames": {"type": "integer", "minimum": 1, "maximum": 3600}, "move": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "look": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "face": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "jump": {"type": "boolean"}, "sprint": {"type": "boolean"}, "crouch": {"type": "boolean"}, "throw": {"type": "boolean"}, "toggle_camera": {"type": "boolean"}}, "required": ["frames"]}}
])";

[[nodiscard]] std::string Base64(const std::string_view Bytes)
{
	constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string Output;
	Output.reserve((Bytes.size() + 2) / 3 * 4);

	for (std::size_t Index = 0; Index < Bytes.size(); Index += 3)
	{
		const auto Byte = [&](const std::size_t Offset)
		{
			return Index + Offset < Bytes.size() ? static_cast<std::uint32_t>(static_cast<unsigned char>(Bytes[Index + Offset])) : 0u;
		};

		const std::uint32_t Triple = Byte(0) << 16 | Byte(1) << 8 | Byte(2);
		Output += Alphabet[(Triple >> 18) & 63];
		Output += Alphabet[(Triple >> 12) & 63];
		Output += Index + 1 < Bytes.size() ? Alphabet[(Triple >> 6) & 63] : '=';
		Output += Index + 2 < Bytes.size() ? Alphabet[Triple & 63] : '=';
	}

	return Output;
}

[[nodiscard]] std::string MakeResult(const std::string_view Id, const std::string_view Result)
{
	return std::string(std::format(R"({{"jsonrpc": "2.0", "id": {}, "result": {}}})", Id, Result));
}

[[nodiscard]] std::string MakeError(const std::string_view Id, const int Code, const std::string_view Message)
{
	std::string Output = std::format(R"({{"jsonrpc": "2.0", "id": {}, "error": {{"code": {}, "message": )", Id, Code);
	AppendJsonString(Output, Message);
	return Output + "}}";
}

// Turns the editor's reply into MCP content: errors become tool errors the agent can read, and captures arrive as images.
[[nodiscard]] std::string ToToolResult(const std::string_view Reply)
{
	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	simdjson::dom::element Value;
	std::string Content;
	bool bError = false;
	if (Parser.parse(Reply).get(Root))
	{
		return R"({"content": [{"type": "text", "text": "The editor sent an unreadable reply"}], "isError": true})";
	}

	if (!Root["error"].get(Value))
	{
		bError = true;
		Content = R"([{"type": "text", "text": )" + ToJsonString(simdjson::minify(Value)) + "}]";
	}
	else if (!Root["result"].get(Value))
	{
		const std::string Text = simdjson::minify(Value);
		std::string_view ImagePath;
		Content = "[";
		if (!Value["image"].get_string().get(ImagePath))
		{
			std::ifstream File(std::filesystem::path(std::u8string(ImagePath.begin(), ImagePath.end())), std::ios::binary);
			const std::string Bytes{std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>()};
			if (!Bytes.empty())
			{
				Content += R"({"type": "image", "mimeType": "image/png", "data": ")" + Base64(Bytes) + "\"}, ";
			}
		}

		Content += R"({"type": "text", "text": )" + ToJsonString(Text) + "}]";
	}
	else
	{
		return R"({"content": [{"type": "text", "text": "The editor's reply has neither a result nor an error"}], "isError": true})";
	}

	return std::format(R"({{"content": {}, "isError": {}}})", Content, bError ? "true" : "false");
}

}

std::optional<std::string> HandleMcpMessage(const std::string_view Line, const FMcpToolCall& Call)
{
	simdjson::dom::parser Parser;
	simdjson::dom::element Message;
	std::string_view Method;
	if (Parser.parse(Line).get(Message) || Message["method"].get_string().get(Method))
	{
		return std::nullopt;
	}

	// A notification, such as notifications/initialized, needs no reply.
	simdjson::dom::element IdElement;
	if (Message["id"].get(IdElement))
	{
		return std::nullopt;
	}

	const std::string Id = simdjson::minify(IdElement);
	if (Method == "initialize")
	{
		std::string_view Requested = "2025-06-18";
		(void)Message["params"]["protocolVersion"].get_string().get(Requested);
		std::string Result = R"({"protocolVersion": )" + ToJsonString(Requested);
		Result += std::format(R"(, "capabilities": {{"tools": {{"listChanged": false}}}}, "serverInfo": {{"name": "herta-editor", "version": "{}"}}, )", ServerVersion);
		Result += R"("instructions": "Drives a running Herta editor started with --automation: read its state and log, capture the viewport, and run Play with scripted input."})";
		return MakeResult(Id, Result);
	}

	if (Method == "ping")
	{
		return MakeResult(Id, "{}");
	}

	if (Method == "tools/list")
	{
		// MCP over stdio is one message per line, so the readable multi-line list is minified before sending.
		simdjson::dom::parser ListParser;
		simdjson::dom::element Tools;
		if (ListParser.parse(ToolList).get(Tools))
		{
			return MakeError(Id, -32603, "The built-in tool list is not valid JSON");
		}

		return MakeResult(Id, std::format(R"({{"tools": {}}})", simdjson::minify(Tools)));
	}

	if (Method == "tools/call")
	{
		std::string_view Tool;
		simdjson::dom::element Arguments;
		if (Message["params"]["name"].get_string().get(Tool))
		{
			return MakeError(Id, -32602, "tools/call needs a tool name");
		}

		const std::string ArgumentText = Message["params"]["arguments"].get(Arguments) ? std::string("{}") : simdjson::minify(Arguments);
		const auto Reply = Call(Tool, ArgumentText);
		return MakeResult(Id, Reply ? ToToolResult(*Reply) : std::format(R"({{"content": [{{"type": "text", "text": {}}}], "isError": true}})", ToJsonString(Reply.error())));
	}

	return MakeError(Id, -32601, std::format("Unknown method {}", Method));
}
}
