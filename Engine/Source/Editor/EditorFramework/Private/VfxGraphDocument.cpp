#include "GraphDocumentPanel.h"

namespace Herta
{
namespace
{
// Each module type gets its own pin color, matching the emitter input it feeds.
FToolUIColor VfxPinColor(const std::uint32_t Type)
{
	constexpr std::array<FToolUIColor, 9> Colors{{
	    {200, 200, 200, 255},
	    {230, 160, 70, 255},
	    {120, 200, 120, 255},
	    {110, 170, 230, 255},
	    {200, 200, 110, 255},
	    {170, 140, 220, 255},
	    {230, 120, 160, 255},
	    {140, 200, 200, 255},
	    {210, 130, 100, 255},
	}};

	return Colors[std::min<std::size_t>(Type, Colors.size() - 1)];
}

FToolUIColor VfxCategoryColor(const std::string_view Category)
{
	if (Category == "Emitters")
	{
		return {200, 120, 60, 255};
	}

	if (Category == "Output")
	{
		return {150, 110, 190, 255};
	}

	return {70, 120, 160, 255};
}

std::vector<FGraphDiagnostic> CompileVfxDocument(const FGraph& Graph)
{
	const auto Compiled = CompileVfxGraph(Graph);
	return Compiled ? Compiled->Diagnostics : Compiled.error();
}

std::string_view VfxPropertyTooltip(const std::string_view NodeType, const std::string_view Key)
{
	if (NodeType == "Emitter")
	{
		if (Key == "duration")
		{
			return "Seconds the emitter keeps spawning at its rate; zero spawns until the effect is stopped. A burst spawns once either way";
		}

		if (Key == "maxParticles")
		{
			return "Particles of this emitter alive at once, up to 4096; spawns past it are dropped";
		}
	}

	if (NodeType == "ConeVelocity" && Key == "direction")
	{
		return "The cone's axis in the effect's space, where +Y is the direction gameplay spawns the effect along, such as a surface normal";
	}

	if (NodeType == "ColorOverLife" && Key == "emissive")
	{
		return "Luminance in nits added on top of the lit color, for sparks and embers that glow";
	}

	if (NodeType == "Collision" && Key == "stick")
	{
		return "Particles stay where they land, such as blood drops";
	}

	return {};
}

const FGraphCanvasDomain VfxCanvas{.Schema = &GetVfxGraphSchema(), .PinColor = VfxPinColor, .CategoryColor = VfxCategoryColor};
const FGraphDocumentDomain VfxDocument{.Canvas = &VfxCanvas, .Title = "VFX Graph", .Compile = CompileVfxDocument, .PropertyTooltip = VfxPropertyTooltip};
}

const FGraphDocumentDomain& GetVfxGraphDocumentDomain()
{
	return VfxDocument;
}
}
