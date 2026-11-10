# AI

Agents perceive the player, remember what they sensed, and act out StateTrees through their bodies. Three pieces:

- **StateTreeCompiler** (developer) turns `.hstatetree` graphs into compiled programs, cooked as assets.
- **AI** (runtime) runs programs (`FStateTreeExecution`), and owns perception, working memory, and scheduling (`FAiWorld`).
- **GameFramework** gives Character entities with an **AI Agent** component a body (`FGameNpc`) that walks the navigation crowd, animates, and fights.

## StateTrees

A StateTree is a hierarchy of states. A state with children picks one to enter, either the first whose conditions hold (`first`) or the one with the highest utility score (`utility`); its tasks run while it and its children are active. A state finishes when all its tasks succeed, or fails when one fails; an empty leaf finishes at once and a container runs as long as its children do.

Transitions live on states and are checked from the active leaf up to the root, the first that holds winning:

- `completed`, `succeeded`, and `failed` when the state's tasks finish.
- `tick` every time the agent thinks. A tick transition into a state that is already active waits until that state ends, so it never restarts a running state.
- `event` when a named event arrives: `Spotted` and `Lost` from sight, `Noise` from hearing, and `Hit` when the agent is struck.

A transition goes to a named state anywhere in the tree, or finishes its parent as succeeded or failed so the parent's own transitions decide. When the leaf finishes and nothing handles it, the tree starts over from the root. A tick takes at most eight transitions, so states that finish as soon as they start cannot loop forever; tick transitions only start a tick, and each event is taken once.

Built-in tasks: Wait, Move To, Wander, Flee, Face, Attack, Animate, Set Flag, and Forget. Conditions: sees or remembers the player, heard a noise, distance to the player, health, a random chance, time in the state, and flags. Utility considerations map distance, health, and seconds since the player was seen or heard into scores.

### Authoring

`.hstatetree` files are graphs in the GraphCore format, edited on the shared graph canvas. The Root and State nodes form the hierarchy through Children into Parent links; task, condition, transition, and consideration nodes attach to a state's Items, and a transition's own conditions attach to its Conditions. Siblings and attached items run in their **Order**, then node identity, never canvas position. Text properties with fixed values, such as a task's target or a transition's trigger, are choices.

The StateTree document's sidebar lists the tree as agents run it. During Play it follows one of the agents running the tree, picked from a list, and lights its active states in the outline and on the canvas; hovering a state lists its transitions. **New StateTree** in the Content Browser starts from idle and wander states that hand over to each other. `statetree.compile <tree.hstatetree>` checks a tree headless.

### Compiled programs

The compiler flattens the tree depth first in sibling order, so the same graph always compiles to the same bytes. States sit in one array where a state's first child follows it and `NextSibling` jumps past its subtree; tasks, conditions, transitions, and considerations are flat arrays that states index by range, and transitions name their target by index. Names of flags, triggers, and events are a table that tasks and transitions index. Each state records its graph node for diagnostics and highlighting.

The compiler rejects duplicate state names, missing transition targets or events, trees over 1024 states or 16 deep, and finishing transitions between task-less leaves that would hand over to each other forever; states not under the root are left out with a warning. Cooked programs are validated again on load.

One program is shared by every agent that runs it. An agent keeps only its active path, its queued events, and a slot in a pool of task data sized for the program's busiest path.

## Perception and memory

An agent sees the player within its sight range and half angle when a physics sphere cast from its eyes reaches the player's head, or within 1.5 m whichever way it faces. It hears noises within its hearing range: impacts, louder the harder they hit, breaking destructibles, and the player's sprinting footfalls. A hit tells it where its attacker is. It remembers where it last saw the player and where it last heard a noise for eight seconds.

## Scheduling

Agents look around five times a second and think ten times a second, taking turns over each interval so they do not all think in the same step. Agents more than 40 m from the player think twice a second. An event wakes an agent on the next step. Everything runs in the session's fixed step in agent order with each agent's own seeded random numbers, so a run is the same every time; tests compare the trace of a cooked and reloaded program with the original's.

## Agents in play

A Character entity with an **AI Agent** component names its StateTree and its sight, hearing, walking, and running speeds. In Play and the game it joins the navigation crowd, walks where the crowd moves it, turns toward where it walks or faces, animates at its walking speed, and carries the player's sword. A swing that starts moving with the player in reach and in front knocks the player down into a ragdoll, who gets back up after 2.5 seconds. Each cut costs a quarter of its health, and losing its head or chest kills it and takes it off the navigation. Restarting puts every agent back where it was placed with nothing remembered. Without navigation agents stand still and every move fails.

The Sandbox's guard tree flees when wounded, fights the player it sees, searches where it lost them, investigates noises, and otherwise patrols; see [Playground](Playground.md).
