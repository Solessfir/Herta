#include "EditorFrameworkPrivate.h"

namespace Herta
{
namespace
{
// An identity of its own, so a preview never collides with a level's effects.
constexpr FAssetId PreviewEffect{0x5652455649455700, 1};
}

void FEditorFramework::FImplementation::UpdateEffects(const float DeltaSeconds)
{
	PreviewParticles.Update(DeltaSeconds);
}

void FEditorFramework::FImplementation::DrawVfxGraphPanel()
{
	if (!VfxGraphPanel.IsOpen())
	{
		if (PreviewDesc)
		{
			PreviewDesc.reset();
			PreviewParticles.Clear();
		}

		return;
	}

	if (VfxGraphRevision != VfxGraphPanel.GetDraftRevision())
	{
		VfxGraphRevision = VfxGraphPanel.GetDraftRevision();
		const auto Compiled = CompileVfxGraph(VfxGraphPanel.GetDraft());
		PreviewDesc = Compiled ? std::optional(Compiled->System) : std::nullopt;
		if (PreviewDesc)
		{
			(void)PreviewParticles.AddSystem(PreviewEffect, std::make_shared<const FCookedParticleSystem>(*PreviewDesc));
		}
	}

	const std::string_view Status = VfxGraphPanel.IsDirty() ? "Previews play the draft; saving recooks it for Play" : "Previews play the graph as Play will";
	VfxGraphPanel.Draw(*ToolUI, {.Status = Status, .DrawSidebar = [this]
	{
		ImGui::TextDisabled("Preview");
		ImGui::BeginDisabled(!PreviewDesc || PlaySession != nullptr);
		if (ToolUIButton("Play at pivot", EToolUIMenuIcon::Light))
		{
			PreviewParticles.Clear();
			PreviewEffectId = PreviewParticles.Spawn(PreviewEffect, ViewportCamera.GetPivot());
		}

		ImGui::EndDisabled();
		ImGui::SameLine();
		if (ToolUIButton("Stop", EToolUIMenuIcon::Delete))
		{
			PreviewParticles.Clear();
		}

		const FParticleStats Stats = PreviewParticles.GetStats();
		ImGui::TextDisabled("%s", !PreviewDesc ? "Fix the graph to preview it" : Stats.Effects > 0 ? std::format("{} particles in the viewport", Stats.Particles).c_str() : "Plays at the viewport's pivot");
	}});
}

void FEditorFramework::FImplementation::OpenVfxGraphKey(const std::string& Key)
{
	const bool bEngine = Key.starts_with("Engine/");
	const std::string_view Relative = std::string_view(Key).substr(Key.find('/') + 1);
	const std::filesystem::path File = (bEngine ? AssetPaths.EngineContentRoot : AssetPaths.ContentRoot) / std::filesystem::path(std::u8string(Relative.begin(), Relative.end()));
	std::ifstream Stream(File, std::ios::binary);
	const std::string Text{std::istreambuf_iterator<char>(Stream), std::istreambuf_iterator<char>()};
	auto Graph = DeserializeGraph(Text, GetVfxGraphSchema());
	if (!Graph)
	{
		HERTA_LOG_ERROR(*Log, EditorLog, "Cannot open the VFX Graph {}: {}", Key, Graph.error().Message);
		return;
	}

	if (VfxGraphPanel.IsOpen() && VfxGraphPanel.GetPath() != Key && VfxGraphPanel.IsDirty())
	{
		HERTA_LOG_WARNING(*Log, EditorLog, "Save or close {} before opening another VFX Graph", VfxGraphPanel.GetPath());
		return;
	}

	VfxGraphPanel.Open(Key, File, std::move(*Graph), bEngine);
	VfxGraphRevision = UINT64_MAX;
}
}
