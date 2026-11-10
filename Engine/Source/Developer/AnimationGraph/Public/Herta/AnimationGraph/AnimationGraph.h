#pragma once

#include "Herta/Assets/AnimationStateMachine.h"
#include "Herta/GraphCore/Graph.h"

#include <cstdint>
#include <expected>
#include <string_view>
#include <vector>

namespace Herta
{
// Animation graphs author state machines on the graph canvas. State nodes output a state; a transition node takes the state it leaves in From,
// or every state when From is unlinked, and the state it enters in To; the entry node takes the starting state. Parameter nodes declare the
// floats, bools, and triggers the game drives.
inline constexpr std::string_view AnimationGraphDomain = "animation";
inline constexpr std::uint32_t AnimationGraphVersion = 1;
inline constexpr std::string_view AnimationGraphExtension = ".hanimgraph";

[[nodiscard]] const FGraphSchema& GetAnimationGraphSchema();

struct FAnimationGraphCompilation
{
	FAnimationStateMachineDesc StateMachine{};
	// The node behind each compiled state and transition, for highlighting the active ones during Play.
	std::vector<FGraphNodeId> StateNodes{};
	std::vector<FGraphNodeId> TransitionNodes{};
	std::vector<FGraphDiagnostic> Diagnostics{};
};

// States and parameters compile in node order, and transitions by priority, then node order, which is the order the machine tests them in.
// A state lists its clips as "Clip" or "Clip@Position, Clip@Position" along its blend parameter, and its events as "Name@Phase, ...". A
// transition's conditions are "Parameter" (a bool is true or a trigger fired), "!Parameter" (a bool is false), "Parameter > Value", and
// "Parameter < Value", separated by commas, all of which must hold.
[[nodiscard]] std::expected<FAnimationGraphCompilation, std::vector<FGraphDiagnostic>> CompileAnimationGraph(const FGraph& Graph);

// The graph a state machine compiles from, laid out in a grid, so state machines built in code can move to the canvas.
[[nodiscard]] FGraph MakeAnimationGraph(const FAnimationStateMachineDesc& StateMachine);
}
