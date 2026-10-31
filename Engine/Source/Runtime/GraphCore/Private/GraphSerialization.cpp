#include "Herta/GraphCore/Graph.h"

#include <simdjson.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <tuple>

namespace Herta
{
namespace
{
constexpr std::size_t MaximumGraphBytes = 64 * 1024 * 1024;

std::unexpected<FGraphError> Failed(std::string Message)
{
	return std::unexpected(FGraphError{std::move(Message)});
}

void AppendString(std::string& Text, const std::string_view Value)
{
	Text += '"';
	for (const char Character : Value)
	{
		if (Character == '"' || Character == '\\')
		{
			Text += '\\';
			Text += Character;
		}
		else if (static_cast<unsigned char>(Character) < 0x20)
		{
			Text += std::format("\\u{:04x}", static_cast<unsigned>(Character));
		}
		else
		{
			Text += Character;
		}
	}

	Text += '"';
}

void AppendFloat(std::string& Text, const float Value)
{
	std::array<char, 64> Buffer{};
	const auto Result = std::to_chars(Buffer.data(), Buffer.data() + Buffer.size(), Value == 0.f ? 0.f : Value);
	Text.append(Buffer.data(), Result.ptr);
}

void AppendValue(std::string& Text, const FGraphValue& Value)
{
	if (const bool* const Flag = std::get_if<bool>(&Value))
	{
		Text += *Flag ? "true" : "false";
	}
	else if (const std::int32_t* const Integer = std::get_if<std::int32_t>(&Value))
	{
		Text += std::to_string(*Integer);
	}
	else if (const float* const Scalar = std::get_if<float>(&Value))
	{
		AppendFloat(Text, *Scalar);
	}
	else if (const auto* const Vector = std::get_if<std::array<float, 4>>(&Value))
	{
		Text += '[';
		for (std::size_t Index = 0; Index < Vector->size(); ++Index)
		{
			Text += Index == 0 ? "" : ", ";
			AppendFloat(Text, (*Vector)[Index]);
		}

		Text += ']';
	}
	else
	{
		AppendString(Text, std::get<std::string>(Value));
	}
}

bool ReadFloat(const simdjson::dom::element Element, float& Value)
{
	double Number = 0;
	if (Element.get_double().get(Number) || !std::isfinite(Number) || std::abs(Number) > 3.4e38)
	{
		return false;
	}

	Value = static_cast<float>(Number);
	return true;
}

// The schema's default decides how to read each value, so an integer property never accepts 1.5 and a float accepts 1.
bool ReadValue(const simdjson::dom::element Element, const FGraphValue& Default, FGraphValue& Value)
{
	if (std::holds_alternative<bool>(Default))
	{
		bool Flag = false;
		if (Element.get_bool().get(Flag))
		{
			return false;
		}

		Value = Flag;
		return true;
	}

	if (std::holds_alternative<std::int32_t>(Default))
	{
		std::int64_t Integer = 0;
		if (Element.get_int64().get(Integer) || Integer < INT32_MIN || Integer > INT32_MAX)
		{
			return false;
		}

		Value = static_cast<std::int32_t>(Integer);
		return true;
	}

	if (std::holds_alternative<float>(Default))
	{
		float Scalar = 0;
		if (!ReadFloat(Element, Scalar))
		{
			return false;
		}

		Value = Scalar;
		return true;
	}

	if (std::holds_alternative<std::array<float, 4>>(Default))
	{
		simdjson::dom::array Array;
		if (Element.get_array().get(Array) || Array.size() != 4)
		{
			return false;
		}

		std::array<float, 4> Vector{};
		std::size_t Index = 0;
		for (const simdjson::dom::element Item : Array)
		{
			if (!ReadFloat(Item, Vector[Index++]))
			{
				return false;
			}
		}

		Value = Vector;
		return true;
	}

	std::string_view String;
	if (Element.get_string().get(String))
	{
		return false;
	}

	Value = std::string(String);
	return true;
}

bool ReadPin(const simdjson::dom::element Element, FGraphPin& Pin)
{
	simdjson::dom::array Array;
	std::uint64_t Node = 0;
	std::string_view Key;
	if (Element.get_array().get(Array) || Array.size() != 2 || Array.at(0).get_uint64().get(Node) || Node == 0 || Node > UINT32_MAX || Array.at(1).get_string().get(Key))
	{
		return false;
	}

	Pin = {.Node = {static_cast<std::uint32_t>(Node)}, .Key = std::string(Key)};
	return true;
}
}

std::expected<std::string, FGraphError> SerializeGraph(const FGraph& Graph, const FGraphSchema& Schema)
{
	const auto Diagnostics = ValidateGraph(Graph, Schema);
	const auto Structural = std::ranges::find(Diagnostics, EGraphSeverity::Error, &FGraphDiagnostic::Severity);
	if (Structural != Diagnostics.end())
	{
		return Failed("Cannot save a structurally invalid graph: " + Structural->Message);
	}

	std::vector<const FGraphNode*> Nodes;
	for (const FGraphNode& Node : Graph.Nodes)
	{
		Nodes.push_back(&Node);
	}

	std::ranges::sort(Nodes, {}, [](const FGraphNode* const Node)
	{
		return Node->Id;
	});

	std::vector<FGraphLink> Links = Graph.Links;
	std::ranges::sort(Links, {}, [](const FGraphLink& Link)
	{
		return std::tie(Link.To, Link.From);
	});

	std::string Text = std::format("{{\n  \"format\": \"HertaGraph\",\n  \"version\": {},\n  \"domain\": ", GraphFormatVersion);
	AppendString(Text, Graph.Domain);
	Text += std::format(",\n  \"domainVersion\": {},\n  \"nodes\": [", Graph.DomainVersion);
	for (std::size_t Index = 0; Index < Nodes.size(); ++Index)
	{
		const FGraphNode& Node = *Nodes[Index];
		Text += Index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ";
		Text += std::to_string(Node.Id.Value);
		Text += ", \"type\": ";
		AppendString(Text, Node.Type);
		Text += ", \"position\": [";
		AppendFloat(Text, Node.Position[0]);
		Text += ", ";
		AppendFloat(Text, Node.Position[1]);
		Text += "], \"properties\": {";
		for (std::size_t Property = 0; Property < Node.Properties.size(); ++Property)
		{
			Text += Property == 0 ? "" : ", ";
			AppendString(Text, Node.Properties[Property].Key);
			Text += ": ";
			AppendValue(Text, Node.Properties[Property].Value);
		}

		Text += "}}";
	}

	Text += Nodes.empty() ? "],\n  \"links\": [" : "\n  ],\n  \"links\": [";
	for (std::size_t Index = 0; Index < Links.size(); ++Index)
	{
		const FGraphLink& Link = Links[Index];
		Text += Index == 0 ? "\n    {\"from\": [" : ",\n    {\"from\": [";
		Text += std::to_string(Link.From.Node.Value);
		Text += ", ";
		AppendString(Text, Link.From.Key);
		Text += "], \"to\": [";
		Text += std::to_string(Link.To.Node.Value);
		Text += ", ";
		AppendString(Text, Link.To.Key);
		Text += "]}";
	}

	std::vector<const FGraphComment*> Comments;
	for (const FGraphComment& Comment : Graph.Comments)
	{
		Comments.push_back(&Comment);
	}

	std::ranges::sort(Comments, {}, [](const FGraphComment* const Comment)
	{
		return Comment->Id;
	});

	Text += Links.empty() ? "],\n  \"comments\": [" : "\n  ],\n  \"comments\": [";
	for (std::size_t Index = 0; Index < Comments.size(); ++Index)
	{
		const FGraphComment& Comment = *Comments[Index];
		Text += Index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ";
		Text += std::to_string(Comment.Id.Value);
		Text += ", \"text\": ";
		AppendString(Text, Comment.Text);
		Text += ", \"rect\": [";
		for (std::size_t Corner = 0; Corner < Comment.Rect.size(); ++Corner)
		{
			Text += Corner == 0 ? "" : ", ";
			AppendFloat(Text, Comment.Rect[Corner]);
		}

		Text += "]}";
	}

	Text += Comments.empty() ? "]\n}\n" : "\n  ]\n}\n";
	return Text;
}

std::expected<FGraph, FGraphError> DeserializeGraph(const std::string_view Text, const FGraphSchema& Schema)
{
	if (Text.empty() || Text.size() > MaximumGraphBytes)
	{
		return Failed("Graph JSON is empty or exceeds 64 MiB");
	}

	simdjson::dom::parser Parser;
	simdjson::dom::element Root;
	simdjson::dom::object Object;
	if (Parser.parse(Text.data(), Text.size()).get(Root) || Root.get_object().get(Object))
	{
		return Failed("Invalid graph JSON");
	}

	std::string_view Format, Domain;
	std::uint64_t Version = 0, DomainVersion = 0;
	simdjson::dom::array NodeArray, LinkArray, CommentArray;
	if (Object["format"].get_string().get(Format) || Format != "HertaGraph" || Object["version"].get_uint64().get(Version) || Version < 1 || Version > GraphFormatVersion || Object.size() != (Version == 1 ? 6u : 7u)
	    || Object["domain"].get_string().get(Domain) || Object["domainVersion"].get_uint64().get(DomainVersion) || DomainVersion > UINT32_MAX || Object["nodes"].get_array().get(NodeArray) || Object["links"].get_array().get(LinkArray)
	    || (Version >= 2 && Object["comments"].get_array().get(CommentArray)))
	{
		return Failed("Invalid graph header or unsupported version");
	}

	if (NodeArray.size() > MaximumGraphNodes || LinkArray.size() > MaximumGraphLinks)
	{
		return Failed("The graph exceeds the node or link limit");
	}

	FGraph Graph{.Domain = std::string(Domain), .DomainVersion = static_cast<std::uint32_t>(DomainVersion)};
	for (const simdjson::dom::element Element : NodeArray)
	{
		simdjson::dom::object Node;
		std::uint64_t Id = 0;
		std::string_view Type;
		simdjson::dom::array Position;
		simdjson::dom::object Properties;
		if (Element.get_object().get(Node) || Node.size() != 4 || Node["id"].get_uint64().get(Id) || Id == 0 || Id > UINT32_MAX || Node["type"].get_string().get(Type) || Node["position"].get_array().get(Position) || Position.size() != 2
		    || Node["properties"].get_object().get(Properties))
		{
			return Failed("Invalid graph node");
		}

		auto Made = MakeGraphNode(Schema, Type, {static_cast<std::uint32_t>(Id)}, {});
		if (!Made)
		{
			return std::unexpected(Made.error());
		}

		if (!ReadFloat(Position.at(0), Made->Position[0]) || !ReadFloat(Position.at(1), Made->Position[1]))
		{
			return Failed("Invalid graph node position");
		}

		// Properties a file leaves out keep their defaults, so a schema can gain a property without a migration.
		for (const auto Field : Properties)
		{
			const auto Found = std::ranges::find(Made->Properties, Field.key, &FGraphProperty::Key);
			if (Found == Made->Properties.end())
			{
				return Failed(std::format("Unknown property '{}' on a '{}' node", Field.key, Type));
			}

			if (!ReadValue(Field.value, FGraphValue(Found->Value), Found->Value))
			{
				return Failed(std::format("Invalid value for property '{}' on a '{}' node", Field.key, Type));
			}
		}

		Graph.Nodes.push_back(std::move(*Made));
	}

	for (const simdjson::dom::element Element : LinkArray)
	{
		simdjson::dom::object Link;
		FGraphLink Parsed;
		if (Element.get_object().get(Link) || Link.size() != 2 || !ReadPin(Link["from"], Parsed.From) || !ReadPin(Link["to"], Parsed.To))
		{
			return Failed("Invalid graph link");
		}

		Graph.Links.push_back(std::move(Parsed));
	}

	if (Version >= 2)
	{
		if (CommentArray.size() > MaximumGraphComments)
		{
			return Failed("The graph exceeds the comment limit");
		}

		for (const simdjson::dom::element Element : CommentArray)
		{
			simdjson::dom::object Comment;
			std::uint64_t Id = 0;
			std::string_view CommentText;
			simdjson::dom::array Rect;
			if (Element.get_object().get(Comment) || Comment.size() != 3 || Comment["id"].get_uint64().get(Id) || Id == 0 || Id > UINT32_MAX || Comment["text"].get_string().get(CommentText) || Comment["rect"].get_array().get(Rect) || Rect.size() != 4)
			{
				return Failed("Invalid graph comment");
			}

			FGraphComment Parsed{.Id = {static_cast<std::uint32_t>(Id)}, .Text = std::string(CommentText)};
			std::size_t Corner = 0;
			for (const simdjson::dom::element Value : Rect)
			{
				if (!ReadFloat(Value, Parsed.Rect[Corner++]))
				{
					return Failed("Invalid graph comment box");
				}
			}

			Graph.Comments.push_back(std::move(Parsed));
		}
	}

	const auto Diagnostics = ValidateGraph(Graph, Schema);
	const auto Structural = std::ranges::find(Diagnostics, EGraphSeverity::Error, &FGraphDiagnostic::Severity);
	if (Structural != Diagnostics.end())
	{
		return Failed(Structural->Message);
	}

	return Graph;
}
}
