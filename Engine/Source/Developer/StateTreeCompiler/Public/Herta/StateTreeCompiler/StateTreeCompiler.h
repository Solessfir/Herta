#pragma once

#include "Herta/Assets/StateTree.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace Herta
{
// StateTrees as graphs: the Root and State nodes form the hierarchy through Children into Parent links, and task, condition, transition,
// and consideration nodes attach to a state's Items. A condition on a state guards entering it; one on a transition's Conditions guards
// taking it. Siblings and attached items run in their Order, then node identity, never canvas position. Transitions name their target state.
inline constexpr std::string_view StateTreeDomain = "statetree";
inline constexpr std::uint32_t StateTreeGraphVersion = 1;
inline constexpr std::string_view StateTreeExtension = ".hstatetree";

[[nodiscard]] const FGraphSchema& GetStateTreeSchema();
// The values a text property may take, such as "player" or "home" for a task's target; empty for free text such as names.
[[nodiscard]] std::span<const std::string_view> GetStateTreeChoices(std::string_view NodeType, std::string_view Property);
// Whether a node type is a task, as opposed to the hierarchy, a condition, a transition, or a consideration.
[[nodiscard]] bool IsStateTreeTask(std::string_view NodeType);

struct FStateTreeCompilation
{
	FCookedStateTree Tree{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// Flattens the tree depth first in sibling order, so the same graph always compiles to the same bytes. States not under the root are left
// out with a warning; duplicate state names, missing transition targets or events, too deep or too large a tree, and finishing transitions
// between task-less leaves that would loop forever are errors.
[[nodiscard]] std::expected<FStateTreeCompilation, std::vector<FGraphDiagnostic>> CompileStateTree(const FGraph& Graph);
// A new tree: idle for a while, then wander, then idle again.
[[nodiscard]] FGraph MakeDefaultStateTreeGraph();
}
