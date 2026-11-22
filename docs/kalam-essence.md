# Kalam’s essence

Kalam is a lightweight, performance-first game engine for people and agents building games together. It gives a project a renderer, a way to load and reload data while the game runs, and an editor that can turn that data into useful controls. The project owns its rules, state, and gameplay code.

## JSON is the shared editing format

A game can register a JSON document with a typed loader and validator. Kalam can watch the file, expose its fields in a generated inspector, and serialize edits back to readable JSON. A form edit, a manual text edit, and an agent’s change on disk all go through the same validation and reload path. Text and form views work on the same document. The project can add a custom panel when a workflow needs more than a generated form.

Kalam reports invalid drafts and keeps the last valid data running. The game decides when to adopt a valid change. Lighting exposure can update on the next frame. An attack definition can take effect on the next swing so a swing already in progress finishes with the parameters it started with.

Parse and validate JSON at load or reload. Simulation and rendering consume typed runtime data, with flat storage and explicit lifetimes where they fit the workload.

## A small foundation with replaceable modules

The rendering front end gives projects a starting point for drawing a game. Lighting, physics, animation, and FX can ship as optional modules that a game can use, replace, or leave out. Keep each interface small enough to use on its own. The editor is a development tool that the shipped game can leave out.

Projects write their own systems and open the panels relevant to those systems. A puzzle game can expose puzzle rules, while an action game can expose attack timing and enemy tuning. An agent can work directly with the project’s code and JSON, and a person can review the diff, open the relevant panel, and see the result in the running game.

## Sange as the reference

Gin’s Sange workflow involved playing the game, opening the editor to adjust an attack or its FX, and relaunching the game to retest. Kalam should make that a continuous session. Play an encounter, pause, tweak the attack or its effect in a panel beside the viewport, and replay it without closing the game. Use the same effect simulation for preview and play, and make it possible to replay fixed inputs and inspect the result.

Sange owns its enemy roster, combat, talents, and tuning panels. Kalam owns the shared tools that let Sange and other games load, inspect, edit, and test their own data.
