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
{"name": "automation_targets", "description": "The processes this server can drive: the editor, and every game, dedicated server, and networked client started with --automation, by the name each tool's target argument takes, with whether it still answers and whether it allows writes.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "editor_status", "description": "The open level, entity count, viewport size, whether writes are allowed, and Play state.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "editor_log", "description": "Editor log lines after a cursor. Pass the returned since and generation back to read only newer lines.", "inputSchema": {"type": "object", "properties": {"since": {"type": "integer", "minimum": 0}, "generation": {"type": "integer", "minimum": 0}, "limit": {"type": "integer", "minimum": 1, "maximum": 2000}}}},
{"name": "level_entities", "description": "Entities in the open level with their IDs, names, components, and transforms: translation in meters, rotation as a quaternion [x, y, z, w], and scale, relative to the parent when parent is true. A cube mesh is 1 m, so its scale is its size.", "inputSchema": {"type": "object", "properties": {"name_contains": {"type": "string"}, "limit": {"type": "integer", "minimum": 1, "maximum": 5000}}}},
{"name": "viewport_capture", "description": "Renders frames_ahead more frames, then returns what the viewport panel shows as a PNG image read back from the GPU, scaled down to at most max_width pixels.", "inputSchema": {"type": "object", "properties": {"frames_ahead": {"type": "integer", "minimum": 1, "maximum": 600}, "max_width": {"type": "integer", "minimum": 64, "maximum": 7680}}}},
{"name": "editor_capture", "description": "Returns the whole editor window as the next presented frame shows it, panels, menus, popups, and dialogs included, as a PNG image scaled down to at most max_width pixels. HDR frames come back with paper white as white.", "inputSchema": {"type": "object", "properties": {"max_width": {"type": "integer", "minimum": 64, "maximum": 7680}}}},
{"name": "ui_items", "description": "Items the editor laid out in its last frame: buttons, menu items, tabs, tree nodes, fields, and other widgets, each with its label, window, and screen rect [x, y, width, height], and whether it is disabled, checked, opened, or takes text. contains and window filter by label and window name; unlabeled includes items without a label.", "inputSchema": {"type": "object", "properties": {"contains": {"type": "string"}, "window": {"type": "string"}, "limit": {"type": "integer", "minimum": 1, "maximum": 5000}, "unlabeled": {"type": "boolean"}}}},
{"name": "ui_click", "description": "Clicks the item with id, or the one whose label matches label, as ui_items shows them: an exact match first, otherwise one containing it, optionally only in a window whose name contains window, and the index-th of several. x and y instead click that screen position, in the coordinates of ui_items rects, for parts of a row that are not items of their own. Answers a few frames later, once the click has landed. button is left, right, middle, or the side buttons back and forward; double double-clicks. Fails with not_found or disabled.", "inputSchema": {"type": "object", "properties": {"x": {"type": "number"}, "y": {"type": "number"}, "label": {"type": "string"}, "id": {"type": "integer", "minimum": 0}, "window": {"type": "string"}, "index": {"type": "integer", "minimum": 0}, "button": {"type": "string", "enum": ["left", "right", "middle", "back", "forward"]}, "double": {"type": "boolean"}}}},
{"name": "ui_scroll", "description": "Turns the mouse wheel over the item with id, as ui_items lists it: positive lines scroll down, negative up. Answers a few frames later.", "inputSchema": {"type": "object", "properties": {"id": {"type": "integer", "minimum": 0}, "lines": {"type": "number"}}, "required": ["id"]}},
{"name": "panel_float", "description": "Undocks a panel by name, such as Details or Content Browser, to screen bounds in the coordinates of ui_items rects. Bounds that cross the main window's edge give it a window of its own. View > Reset layout docks it back.", "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "x": {"type": "number"}, "y": {"type": "number"}, "width": {"type": "number"}, "height": {"type": "number"}}, "required": ["name", "x", "y", "width", "height"]}},
{"name": "ui_drag", "description": "Drags with the left button from the item with id from to the item with id to, as ui_items lists them, over several frames, such as a Content Browser tile onto a folder. Answers once the drop has landed.", "inputSchema": {"type": "object", "properties": {"from": {"type": "integer", "minimum": 0}, "to": {"type": "integer", "minimum": 0}}, "required": ["from", "to"]}},
{"name": "ui_type", "description": "Types text into the focused field, then presses keys in order, each a name such as Enter, Escape, Tab, Backspace, Delete, Up, F2, a letter or digit, or a chord such as Ctrl+S or Ctrl+Shift+Z. Click a field with ui_click first to focus it.", "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "keys": {"type": "array", "items": {"type": "string"}}}}},
{"name": "graph_open", "description": "Opens a graph in its document panel by content key, such as Game/Logic/Shrine.hgameplaygraph or Game/AI/Guard.hstatetree: Material Graphs, animation graphs, Audio Graphs, VFX Graphs, StateTrees, and gameplay graphs.", "inputSchema": {"type": "object", "properties": {"path": {"type": "string"}}, "required": ["path"]}},
{"name": "graph_state", "description": "Every open graph document: its kind, content key, whether it has unsaved edits, node and link counts, compile diagnostics with their nodes, and the nodes lit as running, such as Play's active StateTree states or gameplay graph nodes that ran in the last second. graph includes the draft as canonical graph text.", "inputSchema": {"type": "object", "properties": {"graph": {"type": "boolean"}}}},
{"name": "audio_voices", "description": "The editor's last 64 sound plays, newest first: the sound or graph asset, its bus, voice, volume, pitch, seconds since it started, whether it still plays, and where a spatial one is. playing lists only those still playing. Also returns the mixer's statistics.", "inputSchema": {"type": "object", "properties": {"playing": {"type": "boolean"}}}},
{"name": "ui_stats", "description": "What the last frame drew for the editor's UI over every native window: visible windows, draw lists, draw commands, vertices, and indices, the UI's GPU time in milliseconds, the CPU time of the frame's phases, and the five draw lists with the most indices, named by window. Answers a few frames later, since GPU timing starts with the request.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "module_reload", "description": "Reloads the open project's game modules, or the one named, from their current builds, keeping Play running with each system's state. Reports whether every module reloaded; a build that cannot take the running state leaves the previous one running and says a restart is needed. Needs --automation=write.", "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}}}},
{"name": "viewport_view", "description": "Brings the panel or document named focus to the front, such as Viewport or a document kind like StateTree, shows or hides viewport overlays, optionally points the editor camera from position at target, both world [x, y, z] points, and returns the overlays, the camera, and the latest navigation build: whether it is building, its triangles, how long it took, and any error. Navigation builds while its overlay is on or Play runs.", "inputSchema": {"type": "object", "properties": {"focus": {"type": "string"}, "navigation": {"type": "boolean"}, "collision": {"type": "boolean"}, "position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}, "target": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}}}},
{"name": "editor_commands", "description": "The editor's console commands with their descriptions, whether each mutates (changes files, the open level, or other lasting state), and whether this session may run those.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "editor_command", "description": "Runs one editor console command line, as the Output Log would, and returns its exit code and message. Commands that mutate need an editor started with --automation=write and otherwise fail with read_only. Level edits such as entity.move are undoable transactions: level.undo takes the last one back.", "inputSchema": {"type": "object", "properties": {"command": {"type": "string"}}, "required": ["command"]}},
{"name": "gpu_timings", "description": "GPU time of each render pass in the latest measured frame, in milliseconds.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_start", "description": "Starts Play in the editor viewport, waiting up to 20 s for meshes and the player character to load, and returns the Play state. at places the player's feet at a world [x, y, z] point facing yaw radians, also in a running Play. Play started this way only advances during play_input, and ignores the mouse, keyboard, and gamepads until play_stop.", "inputSchema": {"type": "object", "properties": {"at": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}, "yaw": {"type": "number"}}}},
{"name": "play_teleport", "description": "Places the player's feet at a world [x, y, z] point, facing yaw radians (0 faces +Z, positive turns toward +X) or keeping the current facing, standing and still, as a respawn does, and returns the Play state.", "inputSchema": {"type": "object", "properties": {"position": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}, "yaw": {"type": "number"}}, "required": ["position"]}},
{"name": "play_stop", "description": "Stops Play and returns to the authored level.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_state", "description": "The player's foot position, velocity, look yaw in radians (0 faces +Z, positive turns toward +X), grounded, crouched, sliding, mantling, wall-running, and ragdoll flags, the active movement mode, speed, wall contact and normal, ground normal, camera position and distance from the body, camera mode, animation state, course objective, how many placed characters there are, how many cuts they have taken, and how many cut pieces remain, each AI agent's StateTree state, feet, health, and whether it sees or remembers the player, and the loaded navigation's tiles, obstacles, and crowd agents.", "inputSchema": {"type": "object", "properties": {}}},
{"name": "play_input", "description": "Drives the player for a number of frames of 1/60 s each, however fast the editor draws, then returns the Play state. move is [strafe right, forward] from -1 to 1; look is the total [yaw left, pitch up] in radians spread over the frames; face is a world [x, z] point to turn toward on the first frame; steer_to turns toward a world [x, z] point every frame and stops within 0.3 m of it; until stops early once grounded, airborne, landed, mode_changed, or mode:<Name> holds, with Name a movement mode such as Walking, Falling, Slide, WallRun, WallKick, or Mantle; trace returns the mode, feet, velocity, ground, wall contact, animation state, and animation events every that many frames and on every mode change; the result counts the animation events, such as Footstep and Land, fired over the run; jump, throw, and toggle_camera press once on the first frame, as do ragdoll, which collapses the player into a ragdoll or gets it back up, and attack, which swings the sword; block holds the sword up; sprint and crouch hold; sprint with move forward runs at 7.2 m/s, and without it at 4 m/s.", "inputSchema": {"type": "object", "properties": {"frames": {"type": "integer", "minimum": 1, "maximum": 3600}, "move": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "look": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "face": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "steer_to": {"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 2}, "until": {"type": "string"}, "trace": {"type": "integer", "minimum": 0, "maximum": 3600}, "jump": {"type": "boolean"}, "sprint": {"type": "boolean"}, "crouch": {"type": "boolean"}, "throw": {"type": "boolean"}, "toggle_camera": {"type": "boolean"}, "ragdoll": {"type": "boolean"}, "attack": {"type": "boolean"}, "block": {"type": "boolean"}}, "required": ["frames"]}}
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

std::string ReadMcpTarget(const std::string_view Arguments)
{
	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	std::string_view Target;
	if (Parser.parse(Arguments).get(Root) || Root["target"].get_string().get(Target) || Target.empty())
	{
		return "editor";
	}

	return std::string(Target);
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
		// A client that sends no version gets the newest one the server speaks.
		std::string_view Requested;
		if (Message["params"]["protocolVersion"].get_string().get(Requested) != simdjson::SUCCESS)
		{
			Requested = "2025-06-18";
		}

		std::string Result = R"({"protocolVersion": )" + ToJsonString(Requested);
		Result += std::format(R"(, "capabilities": {{"tools": {{"listChanged": false}}}}, "serverInfo": {{"name": "herta-editor", "version": "{}"}}, )", ServerVersion);
		Result += R"("instructions": "Drives a running Herta editor, and games, dedicated servers, and networked clients, started with --automation: read state and logs, capture windows, click and type in the editor's UI, inspect graphs and sounds, and play with scripted input. Pass target to address a process other than the editor; automation_targets lists them."})";
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

		// Every tool takes a target, so one server drives the editor, games, a dedicated server, and its clients by name.
		std::string Listed = simdjson::minify(Tools);
		constexpr std::string_view Target = R"("target":{"type":"string","description":"The process to call: editor (the default), or a name automation_targets lists, such as game, server, or client-2"})";
		// Schemas with properties gain target first; empty ones after, so none is matched twice.
		const std::string Open = std::string(R"("properties":{")");
		for (std::size_t At = Listed.find(Open); At != std::string::npos; At = Listed.find(Open, At + Open.size() + Target.size()))
		{
			Listed.insert(At + Open.size() - 1, std::string(Target) + ",");
		}

		const std::string Empty = std::string(R"("properties":{})");
		for (std::size_t At = Listed.find(Empty); At != std::string::npos; At = Listed.find(Empty, At))
		{
			Listed.replace(At, Empty.size(), std::format(R"("properties":{{{}}})", Target));
		}

		return MakeResult(Id, std::format(R"({{"tools": {}}})", Listed));
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
