# Jedi Outcast multiplayer

`openjo` (client) and `openjoded` (dedicated server) play the multiplayer of **Jedi Knight II: Jedi Outcast 1.04**
(network protocol 16), with the game's own game logic. They are the multiplayer engine of Jedi Academy (`codemp/`)
built a second time with `JK2_MODE` defined, the same way `openjo_sp` is `openjk_sp` built with `JK2_MODE`.

```
./build.sh --only jo-mp        # (or the CMake options BuildJK2MPEngine, BuildJK2MPDed, BuildJK2MPRdVanilla)
```

You need the Jedi Outcast game data: the engine runs the game, cgame and ui modules that are in the game's own pk3
files (`vm/jk2mpgame.qvm`, `vm/cgame.qvm`, `vm/ui.qvm` in `assets5.pk3` for 1.04), they are not part of this project.
Put the programs next to the game's `base/` folder and run `openjoded.x86_64 +set dedicated 1 +map ffa_bespin`, then
`openjo.x86_64 +connect <address>`.

## How it is built

* **The structures.** Jedi Outcast and Jedi Academy do not share `entityState_t`, `playerState_t`, `trace_t`, the
  limits (`MAX_MODELS` 256, `MAX_CONFIGSTRINGS` 1400, ...) or the sound channels. `codemp/qcommon/q_shared.h` has the
  Jedi Outcast ones under `#ifdef JK2_MODE`; the network field tables of protocol 16 are in `msg.cpp` under the same
  switch. The rest of the engine (network, files, collision, sound, effects, bots) is the same code for both games.
* **The modules.** The Jedi Outcast modules are bytecode (QVM), run by an interpreter (`codemp/jk2/qcommon/vm*.cpp`,
  there is no compiler for the bytecode). What they ask of the engine (the system calls) and what the engine asks of
  them are different from Jedi Academy's, so the three interfaces are of their own: `codemp/jk2/server/sv_game.cpp`,
  `codemp/jk2/client/cl_cgame.cpp` and `cl_ui.cpp`, with the headers of the module interface in `codemp/jk2/game`,
  `cgame` and `ui` (these take the place of the Jedi Academy ones for the Jedi Outcast programs, through the order of
  the include directories). Pointers that a QVM passes are checked against its memory; Ghoul2 instances are handed
  to the modules as integer handles (`codemp/jk2/qcommon/g2_vmhandles.*`).
* **The strings.** The texts of the menus and the game are in `strip/*.sp` files (`codemp/jk2/qcommon/strip.*`).
* **The renderer.** `rdjomp-vanilla` is `codemp/rd-vanilla` built with `JK2_MODE`: the models of Jedi Outcast use
  their own skeleton (the remapping of the bones that Jedi Academy needs for old models is off) and keep the `_off`
  of their surface names.

## Where it comes from

The parts of this that come from other projects are under the GPL version 2, like the rest: the QVM interpreter, the
string package code and the module interface headers are from the Quake III Arena and Jedi Outcast source code
(id Software, Raven Software, Activision), by way of [JK2MV](https://github.com/mvdevs/jk2mv) (its authors: ouned,
fau, Daggolin and others), which this was written against, and the Jedi Outcast source code released by Raven
(github.com/grayj/Jedi-Outcast).

## Status

Works: the dedicated server loads the retail 1.04 game module, runs maps with bots; the client connects, loads the
menus and the cgame module and the level, and plays.

Not done yet: Jedi Outcast 1.02 and 1.03 (protocol 15), the widescreen layout of the 2D parts (they are stretched on
wide screens as the 4:3 original was), the rend2 renderer for Jedi Outcast, support for native (non-QVM) modules,
and the JIT for the bytecode.
