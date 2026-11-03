# OpenJK Shin

**Star Wars Jedi Knight II: Jedi Outcast** and **Star Wars Jedi Knight: Jedi Academy**, rebuilt for the PC on your
desk today.

Your copy of the game, the levels and saber duels you remember, now at your monitor's own resolution and refresh rate,
on a modern Vulkan renderer, with smooth edges, soft shadows, sound that comes from all around you and the echo of
every room you walk into. Drop the files into your game folder and play: there is nothing to tweak.

OpenJK Shin is based on [OpenJK](https://github.com/JACoders/OpenJK), the community project that keeps these games
running on modern systems. OpenJK keeps the games as close to the originals as it can; Shin goes further and adds what a
game made today would have. Everything below is new compared with OpenJK.

> **No game data is included.** You need your own copy of the game (Steam, GOG or the CD). See [Playing](#playing).

## At a glance

- **Native resolution and real widescreen**, ultrawide included: you see more to the sides, nothing is stretched.
- **Vulkan renderer** for the single player games, with automatic fallback to OpenGL.
- **As smooth as your monitor** in single player: the frame rate follows your refresh rate (144, 165, 240 Hz...)
  without tearing.
- **Anti-aliasing, ambient occlusion, bloom, SMAA, per-pixel saber and blaster light**, all from the menus.
- **Shadows that finally work**, grass and detail drawn much further away.
- **Headphone 3D sound and real room echo**, worked out live in every level, mods included.
- **128 sounds at once**, crackle-free, at your sound card's own quality, with less delay.
- **Mouse settings for today's mice**: a Mouse DPI setting and a sensible sensitivity range.
- **Smoother online play**: other players no longer stutter when the connection is uneven.
- **Faster level loading** in single player.
- **Jedi Outcast multiplayer** on a modern engine, which OpenJK doesn't have.

## Supported games

| Game | Single player | Multiplayer |
| - | - | - |
| Jedi Academy | Yes, everything below | Yes, everything below not marked "single player" |
| Jedi Outcast | Yes, everything below | Yes (version 1.04), everything below not marked "single player"; some settings only in the console, see [Jedi Outcast multiplayer](#jedi-outcast-multiplayer) |

Shin is developed and played on Linux. Windows builds are supported by the build scripts and the platform code, but
are not yet tested by the author as much as Linux. macOS code still compiles, but nothing is packaged for it.

## What's new

Everything works out of the box. Where there is a setting, the menu entry is given; for the curious, the console
variable is in brackets (open the console with Shift+Escape or the key above Tab, and type it).

### Your screen, used properly
- **Starts at your screen's own resolution**, filling the screen in a borderless window, so Alt+Tab is instant and
  nothing switches display modes.
- **Setup > Video > Video Mode** lists the resolutions your screen really supports, plus "Desktop resolution".
- **Real widescreen**: on a wide screen you see more to the sides instead of less above and below. Menus and the HUD
  keep their shape instead of being stretched, videos get black bars, the mouse pointer reaches the whole screen, and
  the HUD sits in the corners (health and armour on the left, force and ammo on the right).

### Graphics
- **Vulkan renderer** (single player): a modern graphics backend, used by default. If your system can't run it, the
  game switches to OpenGL by itself and tells you why with a red note on screen. To use OpenGL anyway, set
  `cl_renderer rdsp-vanilla` (Jedi Academy) or `rdjosp-vanilla` (Jedi Outcast) and start the game again. Multiplayer
  uses OpenGL. See
  [docs/vulkan-renderer.md](docs/vulkan-renderer.md).
- **Smooth edges**: Setup > Video > **Anti-Aliasing** (Off, 2x, 4x, 8x) removes the jagged "staircase" from the edges
  of everything, including leaves, fences and grates. The quality presets set it for you, and if your graphics card
  can't do the level you picked, it uses the highest it can.
- **Sharp textures**: Setup > More Video > **Anisotropic Filtering** from Off to 16x keeps floors and walls sharp when
  you look along them, and textures stay sharper in the distance. See [docs/antialiasing.md](docs/antialiasing.md).
- **Screen effects**, each with its own row in Setup > More Video (off by default):
  - **Ambient Occlusion**: soft shading in corners and where things meet, for a sense of depth.
  - **Bloom**: a soft glow around the brightest lights.
  - **SMAA Edge Smoothing**: very clean edges without blurring the textures.
  - **Shader Dynamic Lights**: the light of lightsabers, blaster bolts and explosions follows the shape of what it
    falls on, instead of blocky patches.

  See [docs/post-processing.md](docs/post-processing.md).
- **Shadows that work** (Setup > More Video > Shadows in single player, `cg_shadows 2` in multiplayer): characters are
  no longer darkened by their own shadow, shadows have soft edges, fall away from the light, are lighter where there
  is little direct light, and no longer show through walls. See [docs/shadows.md](docs/shadows.md).
- **See further**: grass and bushes are drawn about three times further away, and characters keep their detailed look
  twice as far.

### Smooth at any frame rate
- **Single player follows your monitor**: vsync is on, so a 144 Hz monitor gets 144 frames per second with no tearing.
  If a frame comes late, it is shown a little torn instead of the game dropping to half the frame rate (where your
  graphics driver allows it). On a G-Sync or FreeSync monitor you can set a limit just under the refresh rate
  (`com_maxfps`).
- **Multiplayer stays at 125 frames per second** with vsync off, as players expect: on most servers how high you jump
  and how fast you run depend on the frame rate, and 125 is the standard. The limit is now exact in every game (144
  used to give 166).
- **The same game at any frame rate** (single player): footsteps, the bobbing of the view, leaning and other things
  no longer speed up or slow down with the frame rate, and the slight wobble when running is gone.
- **Effects keep their density at any frame rate**: at high frame rates some effects (saber sparks, the glow under
  pickups) used to multiply until other things stopped being drawn in big fights.
- **Faster loading** (single player): the work of loading a level is shared between your processor's cores, and the
  loading screen no longer waits for the monitor. In tests a big level loaded in 1.45 seconds instead of 2.1. See
  [docs/parallel-work.md](docs/parallel-work.md).
- Old settings files that held the game at 125 frames per second with tearing are corrected once, the first time you
  play.

### Sound
- **Headphone 3D** (Setup > Sound > Headphone 3D, off by default): with headphones you hear sounds from behind, above
  and below, not only left and right. On speakers leave it off. See [docs/hrtf.md](docs/hrtf.md).
- **Room echo** (Setup > Sound > Room Echo, on by default): a hangar rings, a corridor doesn't, sounds behind a wall
  are muffled, and everything is muffled under water. It is worked out from the level as you play, so it works in
  every level, mods included. Dialogue in cutscenes stays clear. See [docs/reverb.md](docs/reverb.md).
- **128 sounds at once** instead of 32, so big fights no longer cut sounds off (with 31 bots fighting, the old limit
  dropped about 25 sounds every second).
- **Clearer sound**: the old low quality sound effects are converted carefully so they no longer sound harsh, loud
  moments are gently held back instead of distorting, and everything plays at your sound card's own quality (usually
  48 kHz). The old Sound Quality setting is gone: it is always the best.
- **Sounds come sooner**: about half the delay between something happening and hearing it.
- **No crackling or stutter**: the crackle when music and a video play together is gone, and when the game freezes for
  a moment (saving, a slow disk) the sound carries on smoothly instead of repeating or cutting out.
- **Mouths move with the words** again, and combat music changes land on the beat (single player).
- **Alt+Tab**: the game goes quiet in the background and comes back cleanly (`s_muteWhenUnfocused 0` keeps it
  playing).
- **A third of the processor time** for mixing sound, which helps the frame rate in big fights.
- Broken sound files in mods no longer crash the game.

### Mouse
- **A sensitivity slider for today's mice**: from 0.05 to 30, finer at the slow end where you need it (it used to start
  at 2, already too fast for a modern mouse). The default is 1.5.
- **Setup > Controls > Mouse/Joystick > Mouse DPI**: tell the game your mouse's DPI (on the box or in the mouse's
  software) and a sensitivity feels the same on any mouse. See [docs/mouse.md](docs/mouse.md).

### Online play
- **Other players move smoothly on any connection**: when a server update comes late, the original game had to guess
  where everyone went, and they stuttered and jumped when the update came. The game now measures your connection
  and waits just long enough for the next update: in tests it went from guessing in 4 to 13% of frames to under 1%,
  with no extra delay on a steady connection (`cl_timeSync`). See [docs/network.md](docs/network.md).
- **Network settings for today's connections**: your inputs go out every frame, the server may send you up to
  100000 bytes a second, and the modem-era Data Rate choices are gone. Old settings files are updated once.

### Gameplay (single player)
- **Realistic saber combat is on by default**: more precise saber hits and higher damage. To play with the original
  rules: `g_saberMoreRealistic 0` (Jedi Academy) or `g_saberRealisticCombat 0`
  (Jedi Outcast).
- **Escape skips cutscenes**, both videos and the ones the game plays itself. Handy on a second playthrough.

### Jedi Outcast multiplayer is back
- **Play Jedi Outcast multiplayer (1.04) on a modern engine**, with the game's own modules from your copy of the game,
  so it plays exactly like the original, with the screen, graphics, sound and mouse improvements above. Because it
  runs the game's original menus, a few settings are console only: Anti-Aliasing (`r_ext_multisample 4`), the screen
  effects (`r_ssao 1`, `r_bloom 1`, `r_smaa 1`, `r_dlightGLSL 1`), Headphone 3D (`s_hrtf 1`) and Mouse DPI
  (`in_mouseDPI`); its Video Mode list is the original one (it still starts at your desktop resolution), and its old
  Sound Quality row is still there but every choice gives the best quality. Its HUD stays in the middle of the screen,
  while the view is widened all the same. See [Jedi Outcast multiplayer](#jedi-outcast-multiplayer).

### Behind the scenes
- **Settings and saves always have a home**: if your user folder can't be written (a locked down Windows account, for
  example), the game says why and uses the game folder instead of failing to start.
- Window, input and sound run on **SDL3**.
- **One command builds everything**, after checking that your system has what it needs and telling you how to install
  what is missing. See [Building](#building).
- The old OpenAL/EAX sound backend, the macOS application bundles and the leftovers of old Visual Studio projects are
  removed.

## Playing

Shin comes as source code; there are no ready-made downloads yet. [Build it](#building) (one command), which gives you
a folder per game, then:

1. Copy everything in `dist/JediOutcast/` (or `dist/JediAcademy/`) into the `GameData/` folder of that game, next to
   its `base/` folder. With Steam that is `<Steam folder>/steamapps/common/Jedi Academy/GameData/` (or
   `.../Jedi Outcast/GameData/`).
1. Start the game you want:

   | | Single player | Multiplayer | Dedicated server |
   | - | - | - | - |
   | Jedi Outcast | `openjo_sp` | `openjo` | `openjoded` |
   | Jedi Academy | `openjk_sp` | `openjk` | `openjkded` |

   On Linux the files end in `.x86_64`, on Windows in `.x86_64.exe`.

Your settings and saves go to `~/.local/share/openjo` and `~/.local/share/openjk` on Linux, and to
`Documents\My Games\OpenJO` and `Documents\My Games\OpenJK` on Windows.

Don't have the games? Jedi Academy is on [Steam](https://store.steampowered.com/app/6020/) and
[GOG](https://www.gog.com/game/star_wars_jedi_knight_jedi_academy), Jedi Outcast on
[Steam](https://store.steampowered.com/app/6030/) and [GOG](https://www.gog.com/game/star_wars_jedi_knight_ii_jedi_outcast).

On Linux, to download the Windows version of the game data without a Windows machine:

1. Install [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD#Linux).
1. In SteamCMD: `force_install_dir /path/to/install/jka/`, then `@sSteamCmdForcePlatformType windows`, then
   `login <your Steam user>`, then `app_update 6020` (Jedi Academy) or `app_update 6030` (Jedi Outcast).

## Building

The build scripts do the work: they check what is installed, say exactly what is missing and how to install it, build
all four games and copy them to `dist/`, laid out like the game folders.

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

- Microsoft's C++ compiler. The free **Build Tools for Visual Studio** (2019, 2022 or 2026) are enough: you don't
  need the Visual Studio editor, only its **"Desktop development with C++"** workload and a Windows 10/11 SDK. One
  command installs them:
  `winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"`.
  If you already have Visual Studio (Community is free) with that workload, it is used.
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

### Result

The games are copied to `dist/JediOutcast/` and `dist/JediAcademy/`; the build files go in `build/`. To build again
after updating the source, run the same command: only what changed is compiled.

If you would rather call CMake yourself, the options are the usual OpenJK ones (`BuildJK2SP*` for Jedi Outcast single
player, `BuildSP*` for Jedi Academy single player, `BuildMP*` and `BuildJK2MP*` for multiplayer), plus
`BuildSPRdVulkan` and `BuildJK2SPRdVulkan` for the Vulkan renderers. They are explained at the top of
`CMakeLists.txt`. On Windows SDL3 is always downloaded; on Linux `-DUseInternalSDL3=ON` does the same.

### The Windows version, built on Linux

No Windows machine and no Microsoft compiler needed: the MinGW compiler on Linux builds the Windows programs. They
need nothing but Windows itself (10 or newer) to run. They have no Vulkan renderer: the single player games use
OpenGL.

What you need: CMake, Ninja and a MinGW-w64 compiler for 64-bit Windows. Shin is tested with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases): unpack it and put its `bin` folder on your `PATH`.
The MinGW of your distribution should work too: `mingw-w64-gcc` on Arch, `mingw64-gcc-c++` on Fedora, and
`g++-mingw-w64-x86-64` on Debian/Ubuntu, where you also have to pick its "posix" variant (the default one can't build
Shin's worker threads): `sudo update-alternatives --config x86_64-w64-mingw32-g++` and the same for
`x86_64-w64-mingw32-gcc`. zlib, libpng, libjpeg and SDL3 are built from source; SDL3 is downloaded the first time.

```sh
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchains/x86_64-w64-mingw32.cmake \
  -DBuildJK2SPEngine=ON -DBuildJK2SPGame=ON -DBuildJK2SPRdVanilla=ON \
  -DBuildJK2MPEngine=ON -DBuildJK2MPDed=ON -DBuildJK2MPRdVanilla=ON -DBuildMPRend2=OFF
cmake --build build-win
cmake --install build-win --prefix dist/windows
```

The games end up in `dist/windows/JediOutcast/` and `dist/windows/JediAcademy/`, laid out like the game folders. (The
experimental rend2 renderer is left out: building it runs a Windows tool, which needs Wine.)

More about the code: [renderer architecture](docs/renderer-architecture.md), [library loading](docs/libraries.md),
[save games](docs/save%20games.md), [language strings](docs/language%20strings.md). Upstream's
[debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging) still applies.

## Jedi Outcast multiplayer

`openjo` (client) and `openjoded` (dedicated server) play the multiplayer of Jedi Outcast 1.04 with the game's own
game modules from your copy of the game, so it plays like the original. It is the Jedi Academy multiplayer engine
built for Jedi Outcast (`./build.sh --only jo-mp`); how, and what is not done yet (Outcast 1.02 and 1.03, rend2), is
in [docs/jk2-multiplayer.md](docs/jk2-multiplayer.md). It would not exist without
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
