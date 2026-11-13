# OpenJK Shin

OpenJK Shin is an updated engine for **Star Wars Jedi Knight II: Jedi Outcast** and **Star Wars Jedi Knight: Jedi
Academy**. It replaces the game's program files and keeps everything else (the levels, characters, story and
gameplay) as it was. You need your own copy of the games.

What it changes, in short: the games run at your screen's resolution and refresh rate, look cleaner (smoother edges,
better shadows, optional lighting effects), sound better (3D sound on headphones, room echo), and handle modern mice
and connections properly. Every visual addition can be turned off, and most are off until you turn them on, so out of
the box the games look as they always did, only sharper.

Shin is based on [OpenJK](https://github.com/JACoders/OpenJK), the community project that keeps these games running on
modern systems. OpenJK aims to stay as close to the originals as possible; Shin adds improvements on top of it.

> **No game data is included.** You need Jedi Outcast and/or Jedi Academy from Steam, GOG or the original CDs.

## Contents

- [Installing](#installing)
- [What is different](#what-is-different)
- [Graphics settings](#graphics-settings)
- [Supported games and systems](#supported-games-and-systems)
- [Jedi Outcast multiplayer](#jedi-outcast-multiplayer)
- [Building from source](#building-from-source)
- [License](#license) and [Credits](#credits)

## Installing

1. Download the archive for your system from the
   [Releases page](https://github.com/Rugaliz/OpenJK-Shin/releases): `windows` or `linux`.
1. Unpack it. It has a `JediOutcast` folder and a `JediAcademy` folder.
1. Copy the **contents** of `JediOutcast` into the `GameData` folder of Jedi Outcast, next to its `base` folder, and
   the contents of `JediAcademy` into the `GameData` folder of Jedi Academy. With Steam these are
   `<Steam folder>/steamapps/common/Jedi Outcast/GameData/` and `.../Jedi Academy/GameData/`.
   Nothing of the original game is overwritten, so to go back to the original, start its own program again.
1. Start the game with one of these:

   | | Single player | Multiplayer | Dedicated server |
   | - | - | - | - |
   | Jedi Outcast | `openjo_sp` | `openjo` | `openjoded` |
   | Jedi Academy | `openjk_sp` | `openjk` | `openjkded` |

   On Windows the files end in `.x86_64.exe`, on Linux in `.x86_64`. With Steam you can add them to your library as
   non-Steam games.

Your settings and saves are kept apart from the original game's: in `Documents\My Games\OpenJO` and
`Documents\My Games\OpenJK` on Windows, and `~/.local/share/openjo` and `~/.local/share/openjk` on Linux.

Don't have the games? Jedi Academy is on [Steam](https://store.steampowered.com/app/6020/) and
[GOG](https://www.gog.com/game/star_wars_jedi_knight_jedi_academy), Jedi Outcast on
[Steam](https://store.steampowered.com/app/6030/) and [GOG](https://www.gog.com/game/star_wars_jedi_knight_ii_jedi_outcast).

## What is different

Settings are named as they appear in the game's menus. The console command is given in brackets for those who want it
(open the console with Shift+Escape or the key above Tab).

### Screen and resolution
- The game **starts at your screen's own resolution**, in a borderless window that covers the screen, so switching to
  other programs with Alt+Tab is instant.
- **Setup > Video > Video Mode** lists the resolutions your screen supports, plus "Desktop resolution".
- **Widescreen and ultrawide screens** show more to the sides, as they should, instead of cutting off the top and
  bottom. Menus and the HUD keep their proportions, videos get black bars, and the HUD sits in the corners of the
  screen.
- **Linux (X11):** in fullscreen the desktop's compositor keeps drawing the screen, as it does for a window. Letting
  the game draw it directly froze the whole PC with some X server and driver versions (XLibre 25.1.11 with KDE, in
  November 2026). Where that works, `r_bypassCompositor 1` turns it back on.

### Frame rate
- **Single player runs at your monitor's refresh rate** (60, 144, 240 Hz...) without tearing. On a G-Sync or FreeSync
  monitor you can set a frame rate limit just under the refresh rate (`com_maxfps`).
- **The game behaves the same at any frame rate**: in the original, footsteps, the bobbing of the view and some other
  movements sped up or slowed down with the frame rate.
- **Multiplayer stays at 125 frames per second**, the rate the multiplayer community plays at, because on most
  servers how far you jump depends on it. The limit is now exact (asking for 144 used to give 166).
- **Big fights are drawn completely**: a scene can hold four times as many things (players, weapons, effects) as
  before. The original stopped drawing whatever went over its limit, and at high frame rates some effects multiplied
  until it did.
- **Levels load faster** in single player, using all of your processor's cores.

### Graphics
- **Vulkan renderer** in single player: a modern graphics interface, used automatically. If your system can't run it,
  the game uses OpenGL instead and shows a short note saying why. Multiplayer uses OpenGL.
- **Anti-aliasing and texture filtering** for clean edges and sharp textures at an angle, from the menus.
- **Smoother level lighting**: the lighting baked into the levels is stored coarsely and showed steps along the edges
  of shadows; it is now smoothed as the level loads, without changing the lighting itself.
- **Grass and small details are drawn further away**, and characters keep their detailed models twice as far.
- **The Brightness slider works at once** and in a window too, and the original's brighter-than-white lighting
  (`r_overBrightBits 1`) works again.
- **Better shadows** for the characters: they no longer darken the character itself or show through walls, and a new
  **Soft** setting gives shadows the character's shape.
- **Ray tracing** on graphics cards that support it: exact character shadows, and light from sabers, blasters and
  explosions that stops at walls and casts shadows.
- **Optional effects**: ambient occlusion, bloom, edge smoothing, more detailed saber and blaster light, light shafts
  and eye adaptation.

All of these are described in [Graphics settings](#graphics-settings) below.

### Sound
- **Headphone 3D** (Setup > Sound > Headphone 3D, off by default): with headphones, sounds come from behind, above and
  below you, not only from the left and right. Leave it off with speakers.
- **Room echo** (Setup > Sound > Room Echo, on by default): large halls ring, small rooms don't, sounds behind a wall
  are muffled, and so is everything under water. It is worked out from the level as you play, so it works in mods too.
  Dialogue in cutscenes stays clear.
- **Up to 128 sounds at once** instead of 32, so sounds are no longer cut off in big fights.
- **Cleaner sound**: played at your sound card's own quality, with the harshness of the old low-quality sound files
  removed and loud moments kept from distorting. The crackling some systems had when music and a video played together
  is fixed. The old Sound Quality setting is gone, as it is always at the best quality.
- **Less delay** between something happening and hearing it (about half of the original).
- When you switch to another program the game goes quiet, and the sound comes back cleanly when you return
  (`s_muteWhenUnfocused 0` keeps it playing).
- Characters' mouths move with their words again, and the combat music changes on the beat (single player).

### Mouse
- **Sensitivity range for modern mice**: from 0.05 to 30 (the original started at 2, already too fast for most of
  today's mice). The default is 1.5.
- **Setup > Controls > Mouse/Joystick > Mouse DPI**: enter your mouse's DPI and the same sensitivity feels the same on
  any mouse.

### Online play
- **Other players move smoothly** even on an uneven connection. The original had to guess where players went whenever
  an update from the server came late, and they jumped when it arrived; the game now waits just long enough for the
  next update instead. On a steady connection this adds no delay.
- **Lag compensation** (Jedi Academy multiplayer, on servers running Shin): the server checks your shots against where
  the other players were on your screen when you fired, so you no longer need to aim ahead of moving players to make up
  for your ping. It covers guns and blaster bolts; saber combat is unchanged.
- **Network settings suited to today's connections**: the old modem-era Data Rate choices are replaced by settings that
  suit broadband, and old settings files are updated once.

### Single player
- **More realistic saber combat is on by default**: more precise saber hits and higher damage. **Setup > Options >
  Realistic Saber Combat** turns it off for the original rules (`g_saberRealisticCombat` in Jedi Outcast,
  `g_saberMoreRealistic` in Jedi Academy). Model Dismemberment is on by default too.
- **Bodies stay**: the original removed them about 10 seconds after a death, and the limbs a saber cut off soon after.
  Now they stay until there are more than 128 (`g_corpseLimit`, 0 for no limit); then the ones furthest from you go
  first, out of your sight, with their limbs. If the level runs short of room for new things (it holds 1024 in all),
  bodies make room first. `g_corpseRemovalTime` brings back removal after that many seconds. Characters the level's
  own scripts take away still go.
- **Escape skips cutscenes**, both the videos and the scenes played in the game engine.

### Other
- If your user folder can't be written to (a locked-down Windows account, for example), the game keeps your settings
  and saves in the game folder instead of failing to start, and says so.
- Broken sound files in mods no longer crash the game.

## Graphics settings

The graphics settings are in **Setup > Video** and **Setup > More Video**. A row only appears if your graphics card
and the renderer in use support it. In single player More Video has more rows than fit: scroll them with the bar beside
them, the mouse wheel, or the arrow keys.

| Setting | Where | What it does |
| - | - | - |
| Anti-Aliasing (Off, 2x, 4x, 8x) | Video | Smooths the jagged edges of everything, including leaves, fences and grates. Higher is smoother and slower. The quality presets set it for you. |
| Anisotropic Filtering (Off to 16x) | More Video | Keeps floors and walls sharp when seen at a slant. Costs almost nothing on modern cards. |
| Shadows (None, Simple, Volumetric, Soft) | More Video | The characters' shadows. Simple is a round patch under the feet. Volumetric has the character's shape, on the ground. **Soft** also falls on walls and steps, with edges that blur further from the character. |
| Ray Tracing (Off, Medium, High) | More Video | Needs a graphics card with ray tracing, in single player. See below. |
| Ambient Occlusion | More Video | Soft shading in corners and where objects meet, which adds depth. |
| Bloom | More Video | A soft glow around the brightest lights. |
| SMAA Edge Smoothing | More Video | Smooths edges without blurring textures; works with or without Anti-Aliasing. |
| Shader Dynamic Lights (Off, On, Detailed) | More Video | The light of sabers, blaster bolts and explosions follows the shape of what it falls on instead of making square patches. Sabers light along the whole blade, and blaster bolts light the walls they pass and flash where they hit. **Detailed** also brings out the bumps of rock, brick and tiles. |
| Light Shafts | More Video | Rays of sunlight through windows and gratings and around roof edges, on levels with a sun. |
| Eye Adaptation | More Video | Dark places slowly brighten as if your eyes were adjusting, without washing out the bright parts. |
| Lamp Lighting (Off, With Ray Tracing, Always) | More Video | Characters are lit from the level's own lamps, each from its side, in single player (the Jedi Outcast levels have them). |
| Soft Particles | More Video | Smoke, sparks and glows fade where they meet walls and floors instead of ending in a hard line. Vulkan only. |

### Ray Tracing

Ray tracing works out light and shadow by tracing the path of light through the level, which makes shadows exact. It
needs the Vulkan renderer (the default in single player) and a graphics card with ray tracing: AMD Radeon RX 6000 or
newer, NVIDIA GeForce RTX, or Intel Arc. It is not available in multiplayer.

With Ray Tracing on:

- **The characters' shadows are exact**, down to the fingers and the hilt of a saber, whenever Shadows is set to
  Volumetric or Soft.
- **Light no longer passes through walls.** In the original, a saber in the next room lit the floor on your side of
  the wall, and an explosion upstairs lit the room below. Now pillars, crates and doors cast shadows from sabers,
  blaster bolts and explosions, and grates and leaves cast their pattern.
- **Characters block that light too**, including the one holding the saber, so a duel throws the fighters' shadows
  onto the walls and floor around them.

**Medium** is the efficient setting: on a Radeon RX 7800 XT at 3440x1440 it costs less than the Soft shadows do.
**High** traces more rays and works at the full resolution of the screen, for smoother shadow edges with less grain,
at a higher cost; it traces its extra rays only where they make a difference, along the edges of shadows.

More detail on the effects and what each costs: [docs/post-processing.md](docs/post-processing.md),
[docs/shadows.md](docs/shadows.md), [docs/antialiasing.md](docs/antialiasing.md).

## Supported games and systems

| Game | Single player | Multiplayer |
| - | - | - |
| Jedi Academy | Yes | Yes |
| Jedi Outcast | Yes | Yes (version 1.04), see [Jedi Outcast multiplayer](#jedi-outcast-multiplayer) |

- **Windows** 10 or 11, 64-bit.
- **Linux**, 64-bit (x86_64). Shin is developed and played on Linux, so it is the most tested.
- **macOS** is not supported.
- **Graphics**: any card with OpenGL works. The Vulkan renderer, used by default in single player, needs Vulkan 1.2,
  which most cards from the last ten years have with current drivers. Ray tracing needs a card with ray tracing
  (see above).

The Windows version is built and tested less often than the Linux one. If something doesn't work, please
[open an issue](https://github.com/Rugaliz/OpenJK-Shin/issues).

## Jedi Outcast multiplayer

OpenJK only plays Jedi Outcast single player. Shin adds Jedi Outcast multiplayer (version 1.04), using the game
modules from your own copy of the game, so it plays the same as the original. You get the screen, graphics, sound and
mouse improvements above, with a few differences, because it runs the game's original menus:

- Some settings are only available from the console: Anti-Aliasing (`r_ext_multisample 4`), the effects (`r_ssao 1`,
  `r_bloom 1`, `r_smaa 1`, `r_dlightGLSL 1` or `2`), Headphone 3D (`s_hrtf 1`) and Mouse DPI (`in_mouseDPI`).
- The Video Mode list is the original one (the game still starts at your desktop resolution), and the old Sound
  Quality row is still there, though every choice gives the best quality.
- The HUD stays in the middle of the screen, while the view itself is widened.

To play on servers running other versions of Jedi Outcast (1.02, 1.03), use [JK2MV](https://jk2mv.org). How Jedi
Outcast multiplayer works here: [docs/jk2-multiplayer.md](docs/jk2-multiplayer.md).

## Building from source

Most players don't need this: the [Releases page](https://github.com/Rugaliz/OpenJK-Shin/releases) has ready-made
downloads. To build the games yourself, the build scripts check what is installed, say what is missing and how to
install it, build all four games and copy them to `dist/`, laid out like the game folders.

### Linux

What you need (the script checks all of it):

- CMake 3.16 or newer, a C++ compiler (GCC or Clang), pkg-config, and Ninja (optional, faster)
- the development files of zlib, libpng, libjpeg and OpenGL
- SDL3 3.2.0 or newer (optional: if it isn't installed it is downloaded and built the first time, which needs an
  internet connection and the X11/Wayland development files)
- the Vulkan headers and loader (optional: without them the single player games have no Vulkan renderer and use
  OpenGL)

For example:

```sh
# Debian, Ubuntu, Mint
sudo apt install build-essential cmake ninja-build pkg-config zlib1g-dev libpng-dev libjpeg-dev libgl-dev libsdl3-dev libvulkan-dev
# Fedora
sudo dnf install gcc-c++ make cmake ninja-build pkgconf-pkg-config zlib-devel libpng-devel libjpeg-turbo-devel mesa-libGL-devel SDL3-devel vulkan-headers vulkan-loader-devel
# Arch, Manjaro
sudo pacman -S --needed base-devel cmake ninja pkgconf zlib libpng libjpeg-turbo libglvnd sdl3 vulkan-headers vulkan-icd-loader
```

If your distribution has no SDL3 package yet, leave it out: the build downloads it, and `./build.sh --check` tells you
the few extra packages that needs.

Then, from the folder of this repository:

```sh
./build.sh                  # everything
./build.sh --only jo,ja-sp  # only some: jo, jo-mp, ja-sp, ja-mp (Jedi Outcast/Academy single player/multiplayer)
./build.sh --check          # only check what is installed
./build.sh --help           # all the options
```

### Windows

What you need (the script checks all of it):

- Microsoft's C++ compiler. The free **Build Tools for Visual Studio** (2019, 2022 or 2026) are enough: you only need
  the **"Desktop development with C++"** workload and a Windows 10/11 SDK. One command installs them:
  `winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
  An existing Visual Studio (Community is free) with that workload works too.
- CMake 3.16 or newer (`winget install Kitware.CMake`)
- an internet connection the first time (SDL3 is downloaded and built)
- the [Vulkan SDK](https://vulkan.lunarg.com/) (optional: without it the single player games have no Vulkan renderer
  and use OpenGL)

Then, in PowerShell, from the folder of this repository:

```powershell
.\build.ps1                 # everything
.\build.ps1 -Only jo,ja-sp  # only some: jo, jo-mp, ja-sp, ja-mp
.\build.ps1 -Check          # only check what is installed
Get-Help .\build.ps1 -Detailed
```

If scripts are blocked on your system: `powershell -ExecutionPolicy Bypass -File .\build.ps1`.

### The Windows version, built on Linux

The MinGW compiler on Linux can build the Windows version, including the Vulkan renderer: it needs only the Vulkan
headers, as the import library for Windows' Vulkan loader is made during the build. Shin is tested with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (unpack it and put its `bin` folder on your `PATH`). The
MinGW of your distribution should work too: `mingw-w64-gcc` on Arch, `mingw64-gcc-c++` on Fedora, and
`g++-mingw-w64-x86-64` on Debian/Ubuntu, where you also have to pick its "posix" variant
(`sudo update-alternatives --config x86_64-w64-mingw32-g++`, and the same for `x86_64-w64-mingw32-gcc`). zlib, libpng,
libjpeg and SDL3 are built from source; SDL3 is downloaded the first time.

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/x86_64-w64-mingw32.cmake \
  -DBuildJK2SPEngine=ON -DBuildJK2SPGame=ON -DBuildJK2SPRdVanilla=ON \
  -DBuildJK2MPEngine=ON -DBuildJK2MPDed=ON -DBuildJK2MPRdVanilla=ON -DBuildMPRend2=OFF
cmake --build build-win
cmake --install build-win --prefix dist/windows
```

The games end up in `dist/windows/JediOutcast/` and `dist/windows/JediAcademy/`. (The experimental rend2 renderer is
left out: building it runs a Windows tool, which needs Wine.)

### Result

The games are copied to `dist/JediOutcast/` and `dist/JediAcademy/`; the build files go in `build/`. Running the same
command again after updating the source only compiles what changed.

If you would rather call CMake yourself, the options are the usual OpenJK ones (`BuildJK2SP*` for Jedi Outcast single
player, `BuildSP*` for Jedi Academy single player, `BuildMP*` and `BuildJK2MP*` for multiplayer), plus
`BuildSPRdVulkan` and `BuildJK2SPRdVulkan` for the Vulkan renderers. They are explained at the top of
`CMakeLists.txt`. On Windows SDL3 is always downloaded; on Linux `-DUseInternalSDL3=ON` does the same.

More about the code: [Vulkan renderer](docs/vulkan-renderer.md), [renderer architecture](docs/renderer-architecture.md),
[library loading](docs/libraries.md), [save games](docs/save%20games.md),
[language strings](docs/language%20strings.md), [sound: HRTF](docs/hrtf.md) and [reverb](docs/reverb.md),
[mouse](docs/mouse.md), [network](docs/network.md), [parallel loading](docs/parallel-work.md). Upstream's
[debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging) still applies.

## Contributing

Open an issue or a pull request on this repository. Changes that don't depend on anything specific to Shin are welcome
upstream too: [JACoders/OpenJK](https://github.com/JACoders/OpenJK).

To use this as the base of a new mod, fork it, and change the `JK_VERSION` define in `codemp/qcommon/game_version.h`
from "OpenJK" to your project's name.

## License

OpenJK Shin is free software, licensed under the **GNU General Public License version 2 only**, like OpenJK. See
[LICENSE.txt](LICENSE.txt). You may use, modify and redistribute it under those terms; a modified version must come
with its source code under the same license.

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

OpenJK Shin is made by [Rugaliz](https://github.com/Rugaliz).

It builds on the work of the [OpenJK](https://github.com/JACoders/OpenJK) project, whose community took the released
source and made it work on modern systems. Its [support channels](https://discord.gg/dPNCfeQ) and
[forum](https://jkhub.org/forums/forum/49-openjk/) are the place to ask about things that are not specific to Shin.

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

[JK2MV](https://github.com/mvdevs/jk2mv) (ouned, fau, Daggolin and its other contributors, GPLv2), whose work on running
Jedi Outcast multiplayer on a modern engine the Outcast multiplayer here was written against.

And Raven Software and Activision, who made the games and released their source code.
