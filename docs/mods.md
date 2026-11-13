# Mods

Everything the original games load still loads the same way: `.pk3` files put in `base`, and a mod folder next to
`base` chosen in **Setup > Mods** or with `+set fs_game <folder>`. On top of that there is a simpler way to install,
switch and order mods, and the game tells you when two mods change the same files.

## Installing a mod: the `mods` folder

Put the mod in a folder named `mods`, next to `base` (in the game's folder: for Jedi Academy that is `GameData/mods`)
or in your user folder (`~/.local/share/openjk/mods` for Jedi Academy, `~/.local/share/openjo/mods` for Jedi Outcast
on Linux). It can be:

- **the `.zip` exactly as you downloaded it.** The game finds the `.pk3` files inside it, at any depth, copies them once
  into `base/modcache` in your user folder and loads them from there; the readme and screenshots stay in the zip. A
  zip that holds the game's folders (`models`, `textures`, `sound`...) instead of `.pk3` files is loaded as one.
- **a `.pk3` file.**
- **a folder** with `.pk3` files in it, or with the game's folders (`models`, `textures`...) directly in it.

The next time the game starts, the mod is on. Nothing is copied into `base`, and nothing in `base` is changed: to
remove a mod, delete it from `mods` (or switch it off).

Mods in `mods` are loaded after the `.pk3` files of `base`, and before a mod folder (`fs_game`), which still comes last.

## Switching mods on and off, and their order

**Setup > Mods** lists the mods of the `mods` folders first, then the mod folders:

- **On / Off** switches the chosen mod.
- **Move Up** and **Move Down** change the order. Where two mods change the same file, the **lower one in the list
  wins**.
- **Apply** reloads the game's files with the list as it is now. With a mod folder chosen, it also loads that folder,
  as the old **Load Mod** button did.

Hovering over the list shows the chosen mod's name, author and description, whether it changes files of the game or
of other mods, and which mods change its files.

The list is kept in `base/mods.txt` in your user folder, which you can also edit by hand:

```
bloodplus.zip
-DT_Maul.zip
MovieSabers.zip [sp]
```

One mod per line, top to bottom. A `-` in front switches it off. `[sp]` after the name loads it only in single
player, `[mp]` only in multiplayer (the single and multiplayer games of one game share the list). New mods are added
at the end, switched on; mods that are gone are taken off.

## When two mods change the same file

In the console:

- `fs_mods` lists the mods of the `mods` folders, in order, with what they say about themselves.
- `fs_conflicts` lists the `.pk3` files that replace files of another mod, and which one wins, for example:
  `zzzMovieSabers (MovieSabers.zip) replaces 2 files of HS_anakin (hs_anakin.zip), e.g. models/weapons2/...`.
  `fs_conflicts all` also lists what replaces the game's own files, which most mods do on purpose.
- `which <file>` (as before) says where a file is read from.

This covers `.pk3` files in `base` too, so it helps with mods installed the old way.

**Text is merged, not replaced.** Many mods carry a copy of one of the game's text tables (`strings/english/menus.str`
in Jedi Academy, `strip/*.sp` in Jedi Outcast) to add or rename a few things. Before, the last such copy replaced the
whole table: the other mods' text was lost, and a translated game lost its language in every menu. Now every copy is
read, the game's first, then the mods' in the order they load, and each one only changes the lines it has.

**Menus replaced by a mod.** This engine adds rows to the Setup menu (the More Video effects, Realistic Saber Combat,
the Mods buttons). A mod that replaces the Setup menu with its own leaves them out; the console then says which mod
did it and what is missing.

## For mod makers: `modinfo.txt`

A mod can describe itself with a `modinfo.txt` at the top of its zip, folder or `.pk3`. It is optional: mods without
one load the same way, named after their file (or the "Title:" / "Name:" line of their readme).

```
name: Movie Lightsabers
version: 1.0
author: Circa
description: The sabers of the films, for single player.
game: ja
scope: sp
requires: hs_anakin
```

| Key | Meaning |
| - | - |
| `name`, `version`, `author`, `description` | Shown in Setup > Mods and by `fs_mods`. |
| `game` | `jo` (Jedi Outcast), `ja` (Jedi Academy) or `both`. A mod for the other game is listed but not loaded. |
| `scope` | `sp`, `mp` or `both`: loaded only in single player, only in multiplayer, or in both. A scope written in `mods.txt` wins. |
| `requires` | Other mods it needs, by their file or folder name (with or without `.zip`), or their name, separated by commas. If one is not loaded before it, the console says so. |

Multiplayer: mods in the `mods` folder count as `.pk3` files of `base`. A pure server only lets clients use the files it
has itself, as before, and its downloads are served from `base`, so a server should keep downloadable content there.

## What is not supported

Mods that replace the game's executable or inject their own code into it (Movie Duels, for example) cannot work with
this engine. Mods with their own game code (a `jagamex86.dll` and similar) still load from their mod folder as before,
but only on the system they were built for: a Windows `.dll` does not run on Linux.
