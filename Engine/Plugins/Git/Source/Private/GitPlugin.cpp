#include "GitStatus.h"
#include "Herta/EditorCore/EditorPlugin.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Herta
{
namespace
{
constexpr std::uint32_t LocalTimeoutMilliseconds = 30'000;
constexpr std::uint32_t NetworkTimeoutMilliseconds = 120'000;

// Source control through the Git command line, so it uses the user's own Git configuration and credential helpers. Every command runs on
// the editor's background tasks one at a time; the panel only reads results that have arrived.
class FGitClient final
{
public:
	explicit FGitClient(IEditorPluginHost& InHost)
	    : Host(InHost)
	{
	}

	void Refresh()
	{
		if (Root.empty())
		{
			const std::string Start = Host.GetProjectDirectory().empty() ? Host.GetEngineDirectory() : Host.GetProjectDirectory();
			Run({"-C", Start, "rev-parse", "--show-toplevel"}, LocalTimeoutMilliseconds, "Finding the repository", [this](const FEditorPluginProcessResult& Result)
			{
				std::string Top = Result.Output;
				while (!Top.empty() && (Top.back() == '\n' || Top.back() == '\r'))
				{
					Top.pop_back();
				}

				if (Top.empty())
				{
					Message = "The project is not in a Git repository.";
					return false;
				}

				Root = std::move(Top);
				Refresh();
				return false;
			});

			return;
		}

		Run({"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"}, LocalTimeoutMilliseconds, "Reading status", [this](const FEditorPluginProcessResult& Result)
		{
			Status = ParseGitStatus(Result.Output);
			bHasStatus = true;
			std::erase_if(Selected, [this](const std::string& Path)
			{
				return std::ranges::none_of(Status.Changes, [&Path](const FGitChange& Change)
				{
					return Change.Path == Path;
				});
			});

			return false;
		});
	}

	// Runs a command that changes the repository, then reads the status again.
	void Change(std::vector<std::string> Arguments, const std::uint32_t Timeout, std::string Description)
	{
		Run(std::move(Arguments), Timeout, std::move(Description), [](const FEditorPluginProcessResult&)
		{
			return true;
		});
	}

	[[nodiscard]] bool IsBusy() const noexcept
	{
		return !Running.empty();
	}

	[[nodiscard]] std::string Describe() const
	{
		if (!bHasStatus)
		{
			return Message.empty() ? "No status yet; run git.refresh" : Message;
		}

		return std::format("{} -> {} (ahead {}, behind {}), {} changed files", Status.Branch, Status.Upstream.empty() ? "no upstream" : Status.Upstream, Status.Ahead, Status.Behind, Status.Changes.size());
	}

	IEditorPluginHost& Host;
	std::string Root;
	FGitStatus Status;
	bool bHasStatus = false;
	std::set<std::string> Selected;
	std::string Running;
	std::string Message;

private:
	// OnSucceeded returns true when the repository changed and the status should be read again.
	void Run(std::vector<std::string> Arguments, const std::uint32_t Timeout, std::string Description, std::function<bool(const FEditorPluginProcessResult&)> OnSucceeded)
	{
		if (IsBusy())
		{
			return;
		}

		if (!Root.empty())
		{
			Arguments.insert(Arguments.begin(), {"-C", Root});
		}

		Running = std::move(Description);
		// Git never prompts on a terminal it cannot show; credential helpers, such as Git Credential Manager, still ask in their own windows.
		Host.RunProcess({.Executable = "git", .Arguments = std::move(Arguments), .Environment = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_OPTIONAL_LOCKS", "0"}}, .TimeoutMilliseconds = Timeout}, [this, OnSucceeded = std::move(OnSucceeded)](const FEditorPluginProcessResult& Result)
		{
			const std::string What = std::exchange(Running, {});
			if (!Result.bRan || Result.ExitCode != 0)
			{
				Message = std::format("{} failed: {}", What, Result.bRan ? (Result.Error.empty() ? Result.Output : Result.Error) : Result.Error);
				Host.Log(EEditorPluginLog::Warning, std::format("Git: {}", Message));
				return;
			}

			Message = Result.Error.empty() ? std::string{} : Result.Error;
			if (OnSucceeded(Result))
			{
				Refresh();
			}
		});
	}
};

class FSourceControlPanel final : public IEditorPanel
{
public:
	explicit FSourceControlPanel(std::shared_ptr<FGitClient> InGit)
	    : Git(std::move(InGit))
	{
	}

	void Draw(IEditorPluginUI& UI) override
	{
		if (!bRequested)
		{
			bRequested = true;
			Git->Refresh();
		}

		const FGitStatus& Status = Git->Status;
		UI.BeginDisabled(Git->IsBusy());
		if (UI.Button("Refresh"))
		{
			Git->Refresh();
		}

		UI.SameLine();
		if (UI.Button("Pull"))
		{
			Git->Change({"pull", "--ff-only"}, NetworkTimeoutMilliseconds, "Pulling");
		}

		UI.SameLine();
		if (UI.Button("Push"))
		{
			Git->Change({"push"}, NetworkTimeoutMilliseconds, "Pushing");
		}

		UI.EndDisabled();
		UI.SameLine();
		if (Git->IsBusy())
		{
			UI.TextDisabled(Git->Running + "...");
		}
		else if (Git->bHasStatus)
		{
			UI.TextDisabled(std::format("{}{}{}", Status.Branch, Status.Ahead > 0 ? std::format("  {} to push", Status.Ahead) : "", Status.Behind > 0 ? std::format("  {} to pull", Status.Behind) : ""));
		}

		if (!Git->Message.empty())
		{
			UI.TextWrapped(Git->Message);
		}

		UI.Separator();
		if (Git->bHasStatus && Status.Changes.empty())
		{
			UI.TextDisabled("No changes.");
		}

		UI.BeginScrollRegion("##Changes", -7.f, false);
		for (const FGitChange& Change : Status.Changes)
		{
			bool bSelected = Git->Selected.contains(Change.Path);
			const std::string Label = std::format("{}{}  {}{}##{}", Change.Staged, Change.Unstaged, Change.Path, Change.OriginalPath.empty() ? "" : std::format("  (from {})", Change.OriginalPath), Change.Path);
			if (UI.Checkbox(Label, bSelected))
			{
				if (bSelected)
				{
					Git->Selected.insert(Change.Path);
				}
				else
				{
					Git->Selected.erase(Change.Path);
				}
			}
		}

		UI.EndScrollRegion();
		UI.BeginDisabled(Git->IsBusy() || Git->Root.empty());
		if (UI.Button("Stage all"))
		{
			Git->Change({"add", "--all"}, LocalTimeoutMilliseconds, "Staging");
		}

		UI.SameLine();
		UI.BeginDisabled(Git->Selected.empty());
		if (UI.Button("Stage selected"))
		{
			Git->Change(WithPaths({"add", "--"}), LocalTimeoutMilliseconds, "Staging");
		}

		UI.SameLine();
		if (UI.Button("Unstage selected"))
		{
			Git->Change(WithPaths({"restore", "--staged", "--"}), LocalTimeoutMilliseconds, "Unstaging");
		}

		UI.EndDisabled();
		UI.InputTextMultiline("##Message", CommitMessage, 3.f);
		const bool bStaged = std::ranges::any_of(Status.Changes, [](const FGitChange& Change)
		{
			return Change.Staged != '.' && Change.Staged != '?';
		});

		UI.BeginDisabled(!bStaged || CommitMessage.find_first_not_of(" \t\r\n") == std::string::npos);
		if (UI.Button("Commit staged"))
		{
			Git->Change({"commit", "--quiet", "-m", CommitMessage}, LocalTimeoutMilliseconds, "Committing");
			CommitMessage.clear();
		}

		UI.EndDisabled();
		UI.EndDisabled();
	}

private:
	[[nodiscard]] std::vector<std::string> WithPaths(std::vector<std::string> Arguments) const
	{
		Arguments.insert(Arguments.end(), Git->Selected.begin(), Git->Selected.end());
		return Arguments;
	}

	std::shared_ptr<FGitClient> Git;
	std::string CommitMessage;
	bool bRequested = false;
};

void Register(IEditorPluginRegistry& Registry)
{
	auto Git = std::make_shared<FGitClient>(Registry.GetHost());
	Registry.AddPanel("Source Control", std::make_unique<FSourceControlPanel>(Git));
	Registry.AddMenuItem("Refresh source control", [Git]
	{
		Git->Refresh();
	});

	// Commands answer from the last status instead of waiting for Git, so automation never stalls the editor; git.refresh asks for a new one.
	Registry.AddCommand("git.status", "Show the branch and changed files from the last Git status", false, [Git](std::span<const std::string_view>) -> FEditorPluginCommandResult
	{
		std::string Text = Git->Describe();
		for (const FGitChange& Change : Git->Status.Changes)
		{
			Text += std::format("\n{}{} {}", Change.Staged, Change.Unstaged, Change.Path);
		}

		return {.bSucceeded = true, .Message = std::move(Text)};
	});

	Registry.AddCommand("git.refresh", "Read the Git status again in the background", false, [Git](std::span<const std::string_view>) -> FEditorPluginCommandResult
	{
		if (Git->IsBusy())
		{
			return {.bSucceeded = false, .Message = std::format("Git is busy: {}", Git->Running)};
		}

		Git->Refresh();
		return {.bSucceeded = true, .Message = "Refreshing; read it with git.status"};
	});
}
}
}

HERTA_EDITOR_PLUGIN(Herta::Register)
