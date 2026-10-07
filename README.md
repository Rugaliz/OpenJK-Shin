# OpenJK Shin

OpenJK Shin is a modernized build of the engine behind **Star Wars Jedi Knight II: Jedi Outcast** and
**Star Wars Jedi Knight: Jedi Academy**, based on [OpenJK](https://github.com/JACoders/OpenJK).

The aim is simple: you install the game you own, drop in the programs from this project, and it looks and sounds like
a game made for the computers of today, with no tinkering. The gameplay is not changed: the same levels, the same
rules, the same mods. What is improved is how the game talks to your screen, your speakers and your system.

Shin does what the upstream project deliberately doesn't: it adds new features and drops support for old technology
that nobody needs any more. Fixes that suit upstream are sent back to it as pull requests.

> **No game data is included.** You need your own copy of the game. See [Playing](#playing).

## Supported games

| Game | Single player | Multiplayer |
| - | - | - |
| Jedi Academy | Works, with all the features below | Builds and runs as in OpenJK; none of the new features are in it yet |
| Jedi Outcast | Works, with all the features below | Works (1.04), see [Jedi Outcast multiplayer](#jedi-outcast-multiplayer) |

Shin is developed and tested on Linux. Windows is kept supported by the build system and the platform code, but is not
yet tested by the author. macOS code paths still compile but nothing is packaged for it.

## What is new

### Screen
- **Your screen's own resolution by default**, in a borderless fullscreen window, so there is no mode switch.
  Ultrawide and any other aspect ratio work.
- The **resolution list in the video menu** comes from what your display reports instead of a fixed list of 4:3
  modes, with a "Desktop resolution" entry.
- **Widescreen done properly**: horizontal-plus field of view, menus and HUD that keep their proportions in the
  middle of a wide screen instead of being stretched, and videos shown with black bars instead of stretched.
  The **mouse cursor in the menus goes over the whole screen**, and the **HUD sticks to the corners of the screen**
  (health and armor to the left, force and ammo to the right) instead of staying in the 4:3 area.

### Shadows and draw distance
- **Stencil shadows (`cg_shadows 2`) work properly**: characters are no longer darkened by their own shadow, the
  shadows have soft edges, follow the light, fade with how much directional light there is, and no longer leak through
  walls or reach down to height 0. See [docs/shadows.md](docs/shadows.md).
- **Grass and bushes are drawn about three times further** (`r_surfaceSpriteRange`), widescreen no longer shortens
  that, and characters keep their detailed models twice as far away (`r_lodscale`).

### Sound
- The game **mixes at your sound device's own rate** (usually 48 kHz) instead of 22 or 44.1 kHz, so the system
  doesn't convert the sound again. The old "sound quality" setting is gone from the menu because it no longer means
  anything; the `s_khz` console variable is still there for anyone who wants to force a rate.
- **Better resampling** of the game's sounds (a band-limited windowed-sinc resampler replaces the nearest-neighbour
  one), and a soft limiter instead of hard clipping when many loud sounds play at once.
- **Fixed the crackling in videos**, such as the opening logos, caused by music and video sound fighting over the same
  buffer.
- **Lower and adaptive latency**: sounds start roughly half as long after they happen as they used to.
- **Binaural 3D sound for headphones** (Setup > Sound > Headphone 3D, `s_hrtf`, off by default): sounds are
  positioned around your head, including from behind, above and below. See [docs/hrtf.md](docs/hrtf.md).
- **The sound of the room** (Setup > Sound > Room echo, `s_reverb`, on by default): sounds echo like the room they
  are in, are muffled by walls between you and them, and muffled under water, worked out as you play so it works in
  every level. See [docs/reverb.md](docs/reverb.md).
- The game can keep playing sound when its window isn't in front (`s_muteWhenUnfocused 0`).

### Playing
- **Escape skips cutscenes**, both videos and the ones the game plays itself. Handy for testing and for second playthroughs.

### Under the hood
- The platform layer (window, input, sound) uses **SDL3**. SDL2 is no longer supported. SDL3 is used from the system
  if it is installed, or downloaded and built on the first build.
- **One command builds everything**: `build.sh` on Linux and `build.ps1` on Windows check that you have what is needed,
  tell you how to install what is missing, and build all the games. See [Building](#building).
- The old OpenAL/EAX sound backend, the macOS application bundles and the leftovers of old Visual Studio project
  generation are removed.

## Playing

Shin comes as source code; there are no downloads yet. [Build it](#building), which gives you a folder per game, then:

1. Copy the contents of `dist/JediOutcast/` (or `dist/JediAcademy/`) into the `GameData/` folder of that game, next to
   its `base/` folder. For Steam, that is `<Steam folder>/steamapps/common/Jedi Academy/GameData/` (or `.../Jedi Outcast/GameData/`).
1. Run `openjo_sp.x86_64` (Jedi Outcast single player), `openjo.x86_64` (Jedi Outcast multiplayer),
   `openjk_sp.x86_64` (Jedi Academy single player) or `openjk.x86_64` (Jedi Academy multiplayer). On Windows these are
   `.exe` files. The dedicated servers are `openjoded` and `openjkded`.

If you don't have the game yet, you can buy it from [Steam](https://store.steampowered.com/app/6020/) or
[GOG](https://www.gog.com/game/star_wars_jedi_knight_jedi_academy) (Jedi Academy), and from Steam or GOG
(Jedi Outcast).

On Linux, to download the Windows version of the game data without a Windows machine:

1. Install [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD#Linux).
1. In SteamCMD: `force_install_dir /path/to/install/jka/`, then `@sSteamCmdForcePlatformType windows`, then `app_update 6020`.

## Building

You need CMake, a C++ compiler and the development files of zlib, libpng, libjpeg, OpenGL and SDL3 (3.2.0 or newer,
optional: it is downloaded if missing). You don't have to find that out yourself: the scripts check, and tell you what
is missing and how to install it.

**Linux**

```sh
./build.sh                  # everything: Jedi Outcast SP, Jedi Academy SP and MP
./build.sh --only jo        # only one of: jo, ja-sp, ja-mp (comma separated)
./build.sh --check          # only check what is installed
./build.sh --help           # all the options
```

**Windows** (PowerShell, with Visual Studio or its Build Tools and the "Desktop development with C++" workload)

```powershell
.\build.ps1                 # everything
.\build.ps1 -Only jo,ja-sp  # only some of them
.\build.ps1 -Check          # only check what is installed
Get-Help .\build.ps1 -Detailed
```

The result is copied to `dist/JediOutcast/` and `dist/JediAcademy/`, laid out like the game folders. The build files
go in `build/`. Both folders are ignored by git.

If you would rather call CMake yourself, the options are the usual OpenJK ones (`BuildJK2SPEngine`,
`BuildJK2SPGame`, `BuildJK2SPRdVanilla` for Jedi Outcast; `BuildSP*` for Jedi Academy single player; `BuildMP*` for
multiplayer). Everything is explained at the top of `CMakeLists.txt` and in the scripts. On Windows SDL3 is always
downloaded; on Linux use `-DUseInternalSDL3=ON` to do the same.

More about the code: [renderer architecture](docs/renderer-architecture.md), [library loading](docs/libraries.md),
[save games](docs/save%20games.md), [language strings](docs/language%20strings.md).
Upstream's [debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging) still applies.

## Jedi Outcast multiplayer

`openjo` (client) and `openjoded` (dedicated server) play the multiplayer of Jedi Outcast 1.04 with the game's own
game modules from your copy of the game, so it plays like the original. It is the Jedi Academy multiplayer engine
built for Jedi Outcast (`./build.sh --only jo-mp`); how, and what is not done yet (Outcast 1.02 and 1.03, a widescreen
layout, rend2), is in [docs/jk2-multiplayer.md](docs/jk2-multiplayer.md). It would not exist without
[JK2MV](https://jk2mv.org), the multi-version Outcast engine, which it was written against, and which is still the
way to play on servers of the other versions of the game.

## Contributing

Open an issue or a pull request on this repository. Changes that don't depend on anything Shin-specific are welcome
to go to upstream too: [JACoders/OpenJK](https://github.com/JACoders/OpenJK).

To use this as the base of a new mod, fork it, and change the `JK_VERSION` define in `codemp/qcommon/game_version.h`
from "OpenJK" to your project's name.

## License

OpenJK Shin is free software, licensed under the **GNU General Public License version 2 only**, like OpenJK. See
[LICENSE.txt](LICENSE.txt). You are free to use, modify and redistribute it under those terms; a modified version
must come with its source under the same license.

- The code of Jedi Outcast and Jedi Academy is copyright Activision and Raven Software, and was released by them
  under the GPLv2 in 2013. Their notice is kept in [rv-readme.txt](rv-readme.txt). That code is in turn based on the
  Quake III Arena engine by id Software, also released under the GPL.
- Parts of the code are not covered by the GPL and keep their own licenses, which are included with them in `lib/` and
  the source files: zlib, libpng, libjpeg (the Independent JPEG Group's), minizip. SDL3 is used under the zlib license.
- The HRTF data used for the headphone 3D sound is the KEMAR measurement set by Bill Gardner and Keith Martin, MIT
  Media Lab (Copyright 1994, free to use with attribution), see [docs/hrtf.md](docs/hrtf.md).
- **No game data is included, and none is needed to build.** *Star Wars*, *Jedi Knight*, *Jedi Outcast* and *Jedi
  Academy* are trademarks of Lucasfilm Ltd. / Disney, and the games are the property of their owners. This project is
  not affiliated with or endorsed by Lucasfilm, Disney, Activision, Raven Software, or id Software.

## Credits

OpenJK Shin is made by [Ricardo Fernandes](https://github.com/Rugaliz).

It stands on the work of the [OpenJK](https://github.com/JACoders/OpenJK) project, whose community took the released
source and made it work on modern systems. Its [upstream support channels](https://discord.gg/dPNCfeQ) and
[forum](https://jkhub.org/forums/forum/49-openjk/) are where to ask about things that are not specific to Shin.

OpenJK leads, from the original README (full list: [@JACoders](https://github.com/orgs/JACoders/people)):

- [Ensiform](https://github.com/ensiform)
- [razor](https://github.com/Razish)
- [Xycaleth](https://github.com/xycaleth)

Significant OpenJK contributors ([full list](https://github.com/JACoders/OpenJK/graphs/contributors)):

- [bibendovsky](https://github.com/bibendovsky) (save games, platform support)
- [BobaFett](https://github.com/Lrns123)
- [BSzili](https://github.com/BSzili) (JK2, platform support)
- [Cat](https://github.com/deepy) (infra)
- [Didz](https://github.com/dionrhys)
- [eezstreet](https://github.com/eezstreet)
- exidl (SDL, platform support)
- [ImperatorPrime](https://github.com/ImperatorPrime) (JK2)
- [mrwonko](https://github.com/mrwonko)
- [redsaurus](https://github.com/redsaurus)
- [Scooper](https://github.com/xScooper)
- [Sil](https://github.com/TheSil)
- [smcv](https://github.com/smcv) (debian packaging)
- [Tristamus](https://tristamus.com) (icon)

[JK2MV](https://github.com/mvdevs/jk2mv) (ouned, fau, Daggolin and its other contributors, GPLv2), whose work on running Jedi
Outcast multiplayer on a modern engine the Outcast multiplayer here was written against.

And of course Raven Software and Activision, who made the games and released their source.
