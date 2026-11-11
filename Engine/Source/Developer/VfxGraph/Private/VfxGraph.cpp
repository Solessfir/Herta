#include "Herta/VfxGraph/VfxGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace Herta
{
namespace
{
enum EVfxPin : std::uint32_t
{
	EmitterPin = 1,
	SpawnPin,
	VelocityPin,
	LifetimePin,
	SizePin,
	ColorPin,
	ForcesPin,
	CollisionPin,
	RibbonPin,
};

constexpr std::array SystemInputs{
    FGraphPinDescriptor{.Key = "Emitter1", .Name = "Emitter 1", .Type = EmitterPin},
    FGraphPinDescriptor{.Key = "Emitter2", .Name = "Emitter 2", .Type = EmitterPin},
    FGraphPinDescriptor{.Key = "Emitter3", .Name = "Emitter 3", .Type = EmitterPin},
    FGraphPinDescriptor{.Key = "Emitter4", .Name = "Emitter 4", .Type = EmitterPin},
};
constexpr std::array EmitterInputs{
    FGraphPinDescriptor{.Key = "Spawn", .Name = "Spawn", .Type = SpawnPin},
    FGraphPinDescriptor{.Key = "Velocity", .Name = "Velocity", .Type = VelocityPin},
    FGraphPinDescriptor{.Key = "Lifetime", .Name = "Lifetime", .Type = LifetimePin},
    FGraphPinDescriptor{.Key = "Size", .Name = "Size", .Type = SizePin},
    FGraphPinDescriptor{.Key = "Color", .Name = "Color", .Type = ColorPin},
    FGraphPinDescriptor{.Key = "Forces", .Name = "Forces", .Type = ForcesPin},
    FGraphPinDescriptor{.Key = "Collision", .Name = "Collision", .Type = CollisionPin},
    FGraphPinDescriptor{.Key = "Ribbon", .Name = "Ribbon", .Type = RibbonPin},
};
constexpr std::array EmitterOutputs{FGraphPinDescriptor{.Key = "Emitter", .Name = "Emitter", .Type = EmitterPin}};
constexpr std::array SpawnOutputs{FGraphPinDescriptor{.Key = "Spawn", .Name = "Spawn", .Type = SpawnPin}};
constexpr std::array VelocityOutputs{FGraphPinDescriptor{.Key = "Velocity", .Name = "Velocity", .Type = VelocityPin}};
constexpr std::array LifetimeOutputs{FGraphPinDescriptor{.Key = "Lifetime", .Name = "Lifetime", .Type = LifetimePin}};
constexpr std::array SizeOutputs{FGraphPinDescriptor{.Key = "Size", .Name = "Size", .Type = SizePin}};
constexpr std::array ColorOutputs{FGraphPinDescriptor{.Key = "Color", .Name = "Color", .Type = ColorPin}};
constexpr std::array ForcesOutputs{FGraphPinDescriptor{.Key = "Forces", .Name = "Forces", .Type = ForcesPin}};
constexpr std::array CollisionOutputs{FGraphPinDescriptor{.Key = "Collision", .Name = "Collision", .Type = CollisionPin}};
constexpr std::array RibbonOutputs{FGraphPinDescriptor{.Key = "Ribbon", .Name = "Ribbon", .Type = RibbonPin}};

const std::array EmitterProperties{
    FGraphPropertyDescriptor{.Key = "name", .Name = "Name", .Default = std::string("Emitter")},
    FGraphPropertyDescriptor{.Key = "maxParticles", .Name = "Max Particles", .Default = std::int32_t{256}},
    FGraphPropertyDescriptor{.Key = "duration", .Name = "Duration (s)", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "spawnRadius", .Name = "Spawn Radius (m)", .Default = 0.f},
};
const std::array SpawnProperties{
    FGraphPropertyDescriptor{.Key = "rate", .Name = "Rate (/s)", .Default = 0.f},
    FGraphPropertyDescriptor{.Key = "burst", .Name = "Burst", .Default = std::int32_t{20}},
};
const std::array VelocityProperties{
    FGraphPropertyDescriptor{.Key = "direction", .Name = "Direction", .Default = std::array{0.f, 1.f, 0.f, 0.f}},
    FGraphPropertyDescriptor{.Key = "angle", .Name = "Cone Angle (deg)", .Default = 30.f},
    FGraphPropertyDescriptor{.Key = "speedMin", .Name = "Speed Min (m/s)", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "speedMax", .Name = "Speed Max (m/s)", .Default = 2.f},
};
const std::array LifetimeProperties{
    FGraphPropertyDescriptor{.Key = "min", .Name = "Min (s)", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "max", .Name = "Max (s)", .Default = 1.f},
};
const std::array SizeProperties{
    FGraphPropertyDescriptor{.Key = "start", .Name = "Start (m)", .Default = 0.1f},
    FGraphPropertyDescriptor{.Key = "end", .Name = "End (m)", .Default = 0.1f},
};
const std::array ColorProperties{
    FGraphPropertyDescriptor{.Key = "start", .Name = "Start", .Default = std::array{1.f, 1.f, 1.f, 1.f}},
    FGraphPropertyDescriptor{.Key = "end", .Name = "End", .Default = std::array{1.f, 1.f, 1.f, 0.f}},
    FGraphPropertyDescriptor{.Key = "emissive", .Name = "Emissive (nits)", .Default = 0.f},
};
const std::array ForcesProperties{
    FGraphPropertyDescriptor{.Key = "gravity", .Name = "Gravity", .Default = 1.f},
    FGraphPropertyDescriptor{.Key = "drag", .Name = "Drag (/s)", .Default = 0.f},
};
const std::array CollisionProperties{
    FGraphPropertyDescriptor{.Key = "restitution", .Name = "Restitution", .Default = 0.3f},
    FGraphPropertyDescriptor{.Key = "friction", .Name = "Friction", .Default = 0.5f},
    FGraphPropertyDescriptor{.Key = "stick", .Name = "Stick", .Default = false},
};

const std::array VfxNodes{
    FGraphNodeDescriptor{.Type = "System", .Name = "System", .Category = "Output", .Inputs = SystemInputs, .MaximumCount = 1},
    FGraphNodeDescriptor{.Type = "Emitter", .Name = "Emitter", .Category = "Emitters", .Inputs = EmitterInputs, .Outputs = EmitterOutputs, .Properties = EmitterProperties},
    FGraphNodeDescriptor{.Type = "Spawn", .Name = "Spawn", .Category = "Modules", .Outputs = SpawnOutputs, .Properties = SpawnProperties},
    FGraphNodeDescriptor{.Type = "ConeVelocity", .Name = "Cone Velocity", .Category = "Modules", .Outputs = VelocityOutputs, .Properties = VelocityProperties},
    FGraphNodeDescriptor{.Type = "Lifetime", .Name = "Lifetime", .Category = "Modules", .Outputs = LifetimeOutputs, .Properties = LifetimeProperties},
    FGraphNodeDescriptor{.Type = "SizeOverLife", .Name = "Size Over Life", .Category = "Modules", .Outputs = SizeOutputs, .Properties = SizeProperties},
    FGraphNodeDescriptor{.Type = "ColorOverLife", .Name = "Color Over Life", .Category = "Modules", .Outputs = ColorOutputs, .Properties = ColorProperties},
    FGraphNodeDescriptor{.Type = "Forces", .Name = "Gravity and Drag", .Category = "Modules", .Outputs = ForcesOutputs, .Properties = ForcesProperties},
    FGraphNodeDescriptor{.Type = "Collision", .Name = "Collision", .Category = "Modules", .Outputs = CollisionOutputs, .Properties = CollisionProperties},
    FGraphNodeDescriptor{.Type = "Ribbon", .Name = "Ribbon", .Category = "Modules", .Outputs = RibbonOutputs},
};

bool CanConnectVfxPins(const std::uint32_t From, const std::uint32_t To)
{
	return From == To;
}

const FGraphSchema VfxSchema{.Domain = VfxGraphDomain, .Version = VfxGraphVersion, .Nodes = VfxNodes, .CanConnect = CanConnectVfxPins};

[[nodiscard]] const FGraphValue& Property(const FGraphNode& Node, const std::string_view Key)
{
	return std::ranges::find(Node.Properties, Key, &FGraphProperty::Key)->Value;
}

[[nodiscard]] float Number(const FGraphNode& Node, const std::string_view Key)
{
	return std::get<float>(Property(Node, Key));
}

[[nodiscard]] std::array<float, 4> Vector(const FGraphNode& Node, const std::string_view Key)
{
	return std::get<std::array<float, 4>>(Property(Node, Key));
}
}

const FGraphSchema& GetVfxGraphSchema()
{
	return VfxSchema;
}

std::expected<FVfxGraphCompilation, std::vector<FGraphDiagnostic>> CompileVfxGraph(const FGraph& Graph)
{
	std::vector<FGraphDiagnostic> Errors;
	for (FGraphDiagnostic& Diagnostic : ValidateGraph(Graph, VfxSchema))
	{
		if (Diagnostic.Severity == EGraphSeverity::Error)
		{
			Errors.push_back(std::move(Diagnostic));
		}
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	const auto Fail = [&Errors](const FGraphNodeId Node, std::string Message)
	{
		Errors.push_back({.Severity = EGraphSeverity::Error, .Node = Node, .Message = std::move(Message)});
	};

	const auto Source = [&Graph](const FGraphNodeId Node, const std::string_view Pin) -> const FGraphNode*
	{
		const auto Link = std::ranges::find_if(Graph.Links, [&](const FGraphLink& Candidate)
		{
			return Candidate.To.Node == Node && Candidate.To.Key == Pin;
		});

		return Link == Graph.Links.end() ? nullptr : FindGraphNode(Graph, Link->From.Node);
	};

	const auto System = std::ranges::find(Graph.Nodes, std::string("System"), &FGraphNode::Type);
	if (System == Graph.Nodes.end())
	{
		Fail({}, "A VFX Graph needs a System node");
		return std::unexpected(std::move(Errors));
	}

	FVfxGraphCompilation Result;
	std::vector<FGraphNodeId> Used;
	for (const FGraphPinDescriptor& Pin : SystemInputs)
	{
		const FGraphNode* const Node = Source(System->Id, Pin.Key);
		if (!Node)
		{
			continue;
		}

		Used.push_back(Node->Id);
		FParticleEmitterDesc Emitter{
		    .Name = std::get<std::string>(Property(*Node, "name")),
		    .MaximumParticles = static_cast<std::uint32_t>(std::max(std::get<std::int32_t>(Property(*Node, "maxParticles")), 0)),
		    .DurationSeconds = Number(*Node, "duration"),
		    .SpawnRadius = Number(*Node, "spawnRadius"),
		};

		const FGraphNode* const Spawn = Source(Node->Id, "Spawn");
		if (!Spawn)
		{
			Fail(Node->Id, "Link a Spawn module to the emitter");
			continue;
		}

		Emitter.SpawnRate = Number(*Spawn, "rate");
		Emitter.Burst = static_cast<std::uint32_t>(std::max(std::get<std::int32_t>(Property(*Spawn, "burst")), 0));
		if (const FGraphNode* const Velocity = Source(Node->Id, "Velocity"))
		{
			const auto Direction = Vector(*Velocity, "direction");
			const float Length = std::hypot(Direction[0], Direction[1], Direction[2]);
			if (!(Length > 1e-4f))
			{
				Fail(Velocity->Id, "The velocity direction must not be zero");
			}
			else
			{
				Emitter.Direction = {Direction[0] / Length, Direction[1] / Length, Direction[2] / Length};
			}

			Emitter.ConeAngle = Number(*Velocity, "angle") * std::numbers::pi_v<float> / 180.f;
			Emitter.SpeedMinimum = Number(*Velocity, "speedMin");
			Emitter.SpeedMaximum = Number(*Velocity, "speedMax");
		}

		if (const FGraphNode* const Lifetime = Source(Node->Id, "Lifetime"))
		{
			Emitter.LifetimeMinimum = Number(*Lifetime, "min");
			Emitter.LifetimeMaximum = Number(*Lifetime, "max");
		}

		if (const FGraphNode* const Size = Source(Node->Id, "Size"))
		{
			Emitter.SizeStart = Number(*Size, "start");
			Emitter.SizeEnd = Number(*Size, "end");
		}

		if (const FGraphNode* const Color = Source(Node->Id, "Color"))
		{
			Emitter.ColorStart = Vector(*Color, "start");
			Emitter.ColorEnd = Vector(*Color, "end");
			Emitter.Emissive = Number(*Color, "emissive");
		}

		if (const FGraphNode* const Forces = Source(Node->Id, "Forces"))
		{
			Emitter.Gravity = Number(*Forces, "gravity");
			Emitter.Drag = Number(*Forces, "drag");
		}

		Emitter.bRibbon = Source(Node->Id, "Ribbon") != nullptr;
		if (const FGraphNode* const Collision = Source(Node->Id, "Collision"))
		{
			Emitter.bCollision = true;
			Emitter.Restitution = Number(*Collision, "restitution");
			Emitter.Friction = Number(*Collision, "friction");
			Emitter.bStick = std::get<bool>(Property(*Collision, "stick"));
		}

		// Each emitter is checked on its own, so its diagnostic lands on its node.
		if (auto Valid = ValidateParticleSystem({.Emitters = {Emitter}}); !Valid)
		{
			Fail(Node->Id, Valid.error().Message);
			continue;
		}

		Result.System.Emitters.push_back(std::move(Emitter));
	}

	if (Errors.empty() && Result.System.Emitters.empty())
	{
		Fail(System->Id, "Link at least one emitter to the System node");
	}

	for (const FGraphNode& Node : Graph.Nodes)
	{
		if (Node.Type == "Emitter" && !std::ranges::contains(Used, Node.Id))
		{
			Result.Diagnostics.push_back({.Severity = EGraphSeverity::Warning, .Node = Node.Id, .Message = "This emitter is not linked to the System node, so it never plays"});
		}
	}

	if (!Errors.empty())
	{
		return std::unexpected(std::move(Errors));
	}

	return Result;
}

FGraph MakeDefaultVfxGraph()
{
	FGraph Graph{.Domain = std::string(VfxGraphDomain), .DomainVersion = VfxGraphVersion};
	const std::array<std::pair<std::string_view, std::array<float, 2>>, 6> Layout{{
	    {"System", {300.f, 0.f}},
	    {"Emitter", {0.f, 0.f}},
	    {"Spawn", {-320.f, -120.f}},
	    {"ConeVelocity", {-320.f, 0.f}},
	    {"Lifetime", {-320.f, 160.f}},
	    {"ColorOverLife", {-320.f, 280.f}},
	}};

	for (std::uint32_t Index = 0; Index < Layout.size(); ++Index)
	{
		if (auto Node = MakeGraphNode(VfxSchema, Layout[Index].first, {Index + 1}, Layout[Index].second))
		{
			Graph.Nodes.push_back(std::move(*Node));
		}
	}

	Graph.Links = {
	    {.From = {.Node = {2}, .Key = "Emitter"}, .To = {.Node = {1}, .Key = "Emitter1"}},
	    {.From = {.Node = {3}, .Key = "Spawn"}, .To = {.Node = {2}, .Key = "Spawn"}},
	    {.From = {.Node = {4}, .Key = "Velocity"}, .To = {.Node = {2}, .Key = "Velocity"}},
	    {.From = {.Node = {5}, .Key = "Lifetime"}, .To = {.Node = {2}, .Key = "Lifetime"}},
	    {.From = {.Node = {6}, .Key = "Color"}, .To = {.Node = {2}, .Key = "Color"}},
	};

	return Graph;
}
}
