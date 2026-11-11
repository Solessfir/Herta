// An editor plugin for HertaTests, built like a third-party one: from this file and EditorPlugin.h alone.
#include "Herta/EditorCore/EditorPlugin.h"

#include <memory>
#include <string>

namespace Herta
{
namespace
{
class FGreetingPanel final : public IEditorPanel
{
public:
	void Draw(IEditorPluginUI& UI) override
	{
		UI.Text("Hello from a plugin");
		if (UI.Button("Count"))
		{
			++Count;
		}
	}

private:
	int Count = 0;
};

void Register(IEditorPluginRegistry& Registry)
{
	Registry.AddPanel("Greeting", std::make_unique<FGreetingPanel>());
	Registry.AddMenuItem("Say hello", [&Host = Registry.GetHost()]
	{
		Host.Log(EEditorPluginLog::Info, "Hello");
	});

	Registry.AddCommand("test.greet", "Greet the arguments", false, [](const std::span<const std::string_view> Arguments) -> FEditorPluginCommandResult
	{
		std::string Text = "Hello";
		for (const std::string_view Argument : Arguments)
		{
			Text += ' ';
			Text += Argument;
		}

		return {.bSucceeded = !Arguments.empty(), .Message = std::move(Text)};
	});
}
}
}

HERTA_EDITOR_PLUGIN(Herta::Register)
