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
| Jedi Academy | Works, with all the features below | Works, with the sound improvements below; the other new features are single player only for now |
| Jedi Outcast | Works, with all the features below | Works (1.04), with the sound improvements below, see [Jedi Outcast multiplayer](#jedi-outcast-multiplayer) |

Shin is developed and tested on Linux. Windows is kept supported by the build system and the platform code, but is not
yet tested by the author. macOS code paths still compile but nothing is packaged for it.

## What is new

Everything here works out of the box. Where there is a setting, the menu entry is given, and for the curious the
console variable is in brackets (you can type those in the console, opened with Shift+Escape or the key above Tab).

### Screen
- **The game starts at your screen's own resolution**, filling the screen without switching display modes
  (a borderless window). Ultrawide and any other screen shape work.
- **Setup > Video > Video Mode** lists the resolutions your screen really supports, plus "Desktop resolution".
- **Widescreen done properly**: on a wide screen you see more to the sides (instead of less above and below), the menus
  and the HUD keep their shape instead of being stretched, and videos get black bars instead of being stretched. The
  mouse pointer in the menus reaches the whole screen, and the HUD sits in the corners of the screen (health and
  armour on the left, force and ammo on the right).

### Smooth at any frame rate (single player)
- **The frame rate follows your monitor**: vsync is on, so a 144 Hz monitor gets 144 frames per second with no
  tearing (the line across the picture when the screen and the game are out of step). If a frame comes late, the game
  shows it a little torn instead of dropping to half the frame rate, where the graphics driver allows it. You can
  still set your own limit (`com_maxfps`), for example just under the refresh rate on a G-Sync or FreeSync monitor.
- **The frame limit is exact**: 144 gives 144 frames per second (it used to give 166).
- Old settings files that held the game at 125 frames per second with tearing are corrected once, the first time you
  play.
- **The same game at any frame rate**: footsteps, the bobbing of the view, leaning and a few other things used to run
  faster or slower with the frame rate. They don't any more.
- **Smoother movement**: running had a slight wobble twenty times a second. It's gone.

### Picture quality (single player)
- **Smooth edges**: Setup > Video > **Anti-Aliasing** (Off, 2x, 4x, 8x) takes the jagged "staircase" off the edges of
  everything. The quality presets set it for you. If your graphics card can't do the level you picked, it uses the
  highest it can. With it on, leaves, fences and grates get smooth edges too.
- **Sharper textures at an angle**: Setup > More Video > **Anisotropic Filtering** lets you choose from Off to 16x
  (higher keeps floors and walls sharp when you look along them).
- Textures stay sharper in the distance, because their smaller versions are made with a better filter.
- See [docs/antialiasing.md](docs/antialiasing.md).
- **Optional screen effects** (off by default, with a row for each in Setup > More Video, see
  [docs/post-processing.md](docs/post-processing.md)): `r_ssao 1` adds soft shading in corners and where things meet,
  `r_bloom 1` a soft glow around the brightest lights, `r_smaa 1` a high quality edge smoothing that does not blur the
  textures (unlike FXAA), and `r_dlightGLSL 1` makes the light of lightsabers, blaster bolts and explosions follow the
  shape of what it falls on.

### Shadows and draw distance (single player)
- **"Volumetric" shadows work properly** (Setup > More Video > Shadows): characters are no longer darkened by their
  own shadow, the shadows have soft edges, fall away from the light, are lighter where there is little direct light,
  and no longer show through walls or stretch far down below the character. See [docs/shadows.md](docs/shadows.md).
- **Grass and bushes are drawn about three times further away**, and characters keep their detailed look twice as
  far away.

### Mouse (single player)
- **The sensitivity slider suits today's mice**: it goes from 0.05 to 30 (it used to start at 2, which was already
  too fast for a modern mouse), and it is finer at the slow end where you need it. The default is slower (1.5).
- **Setup > Controls > Mouse/Joystick > Mouse DPI**: tell the game the DPI of your mouse (it is on the box or in the
  mouse's own software) and a sensitivity feels the same on any mouse. Old settings keep working. See
  [docs/mouse.md](docs/mouse.md).

### Sound

The items marked (single player) are only in the single player games; the rest are in multiplayer too.

- **More sounds at once**: up to 128 sounds play together instead of 32, so in a big fight sounds are no longer cut
  off. (In a test with 31 bots fighting in multiplayer, the old limit cut off about 25 sounds every second.)
- **Clearer sound**: the old low quality sound effects are converted carefully so they no longer sound harsh or
  gritty, and when many loud sounds play at once they are gently held back instead of distorting. In single player
  the game also plays at your sound card's own quality (usually 48 kHz) instead of converting everything down and up
  again.
- **Headphone 3D** (single player; Setup > Sound > Headphone 3D, off by default): with headphones you hear sounds
  from behind, above and below, not only from the left and the right. On speakers leave it off.
  See [docs/hrtf.md](docs/hrtf.md).
- **Room echo** (single player; Setup > Sound > Room echo, on by default): sounds echo like the room you are in, a
  big hangar differently from a small corridor, sounds behind a wall are muffled, and everything is muffled under
  water. It is worked out as you play, so it works in every level, mods included. Dialogue in cutscenes stays clear,
  without echo. See [docs/reverb.md](docs/reverb.md).
- **Sounds come sooner** (single player): about half the delay there was between something happening and hearing it.
- **No crackling or stutter**: the crackling when music and a video play together (like the opening logos) is gone.
  When the game freezes for a moment (saving, loading a weapon, a slow disk), the sound carries on smoothly instead of
  repeating the last moment or cutting out.
- **Mouths move with the words**: characters' lip movements follow their dialogue properly again.
- **Combat music changes land on the beat**: the music now joins in at the exact points it was written for.
- **Switching windows** (Alt+Tab): the game goes quiet while it is in the background and comes back cleanly. If you
  want to keep hearing it, set `s_muteWhenUnfocused 0`.
- **Uses about a third of the processor time it did** for mixing, which helps keep the frame rate up in big fights
  (`s_mixaheadAuto`).
- Broken sound files in mods no longer crash the game.
- The old "sound quality" menu entry is gone from single player because the game always uses the best quality now
  (`s_khz` is still there to force a rate).

### Playing
- **Escape skips cutscenes**, both videos and the ones the game plays itself. Handy for a second playthrough.

### Under the hood
- The window, input and sound use **SDL3**. SDL2 is no longer supported. SDL3 is used from the system if it is
  installed, or downloaded and built on the first build.
- **One command builds everything**: `build.sh` on Linux and `build.ps1` on Windows check that you have what is needed,
  tell you how to install what is missing, and build all the games. See [Building](#building).
- The old OpenAL/EAX sound backend, the macOS application bundles and the leftovers of old Visual Studio project
  generation are removed.

## Playing
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
