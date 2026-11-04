# Network play (multiplayer)

The multiplayer of both games is Quake III's: the server runs the game and sends each player snapshots of it (40 a
second by default), and the client draws the moment between the last two snapshots it has, so other players move
smoothly. Your own movement is predicted by your client, so it reacts at once whatever your ping. Nothing here
changes the protocol: Shin plays on any server, with any other client.

## Smooth movement on any connection (`cl_timeSync`)

To draw between two snapshots, the client needs the next one before the moment it draws reaches the newest one it
has. If the next one is late, the client has to guess where everything went (extrapolate), and other players stutter
and jump when the snapshot comes. How late snapshots come depends on the connection: on a steady one they come like
clockwork, on Wi-Fi or a busy line some come several milliseconds late.

The original timing keeps the drawn moment right at the newest snapshot: it moves it 1 msec later with every
snapshot, and 2 msec earlier when it had to extrapolate. So it extrapolates regularly by design, and more the more
uneven the connection is; the only cure was `cl_timeNudge`, by hand, at most 30 msec.

`cl_timeSync 1` (the default) measures instead. For every snapshot it remembers how long after its time it could first
be used, and the time between snapshots. The drawn moment is kept just far enough behind the newest snapshot that the
latest of the snapshots of the last 3 seconds would still have come in time for the next frame, leaving out the
latest 2 in 100 (those and lost snapshots are extrapolated over, as before). It moves there gently, running up to 5%
slower or 2% faster than real time, so there are no jumps.

Measured with a local server, 6 bots and a relay that delays the packets by 20 msec plus a random part (Jedi Academy
multiplayer, 125 frames a second, 40 snapshots a second):

| Connection | Original: behind / frames extrapolated | `cl_timeSync 1`: behind / frames extrapolated |
| - | - | - |
| Steady | 13.5 msec / 4.0% | 13.9 msec / 0.2% |
| Up to 10 msec uneven | 14.5 msec / 4.2% | 17.3 msec / 0.3% |
| Up to 30 msec uneven | 13.5 msec / 13.3% | 25.4 msec / 0.6% |
| Up to 10 msec uneven, 2% lost | 14.2 msec / 5.4% | 15.7 msec / 2.7% |

Jedi Outcast multiplayer, up to 10 msec uneven: 14.5 msec / 4.1% before, 16.5 msec / 0.4% after.

`cl_timeSync 0` brings back the original timing. Demos and `timescale` always use it. `cl_timeNudge` still works on
top of either.

`cl_showTimeDelta 2` prints, once a second, how far behind the newest snapshot the drawn moment was, how many frames
had to extrapolate, how many snapshots came, the snapshot interval and how uneven their arrival was (the spread).

The code is `codemp/client/cl_cgame_time.h`, shared by the client of both games.

## Lag compensation (`g_lagCompensation`, Jedi Academy multiplayer servers)

You see the other players where they were a little while ago: their positions took as long as the network does to
reach you, and your shot takes as long again to reach the server. Without lag compensation the server decides the
shot against where everybody is when it arrives, so with a ping of 100 a running player has moved on by about a body's
width and you have to aim ahead of what you see.

Every command a client sends says which moment of the game the player was looking at. The server keeps where every
player was at the end of each of its frames for the last second and, for a shot, moves the others back to where the
shooter saw them, decides the shot, and puts them back.

| Setting | Default | What it does |
| - | - | - |
| `g_lagCompensation` | 2 | 0 off; 1 the weapons that hit at once (the disruptor, the beam of the concussion rifle, the stun baton, punches); 2 also projectiles: a new bolt or rocket starts as far along its path as the shooter's command was behind (at most 150 msec), and its first stretch is decided against the players where the shooter saw them |
| `g_lagCompensationMax` | 300 | msec, how far back it goes at most, so a very high ping can't shoot somebody who has long since got away |

Players are moved whole (their box, position and the way they face, which places the model the accurate hit tests
use); the pose of the model is the current one. Sabers are not compensated: both fighters' blades and poses would have
to go back, and a hit the defender saw blocked could land. Bots need none.

It is in the server's game module (`codemp/game/g_lagcomp.c`), so it works on servers running Shin's server with any
client, and not on servers running other mods. Jedi Outcast multiplayer runs the game modules of the original game,
so it doesn't have it.

Measured with a local server, 5 bots and a relay adding 50 msec each way (the shots reached the server 110 to 160 msec
after the moment the shooter saw), comparing where the shooter's client drew each player when firing with where the
server decided the shot:

| Weapon | Rewound position vs drawn (median / 90%) | Without (median / 90%) |
| - | - | - |
| Disruptor | 2.0 / 2.8 units | 31.0 / 42.9 units |
| Repeater (projectiles, first stretch) | 0.2 / 2.0 units | 18.6 / 40.8 units |

A player is about 30 units wide.

## Bandwidth

| Setting | Default | Was | What it does |
| - | - | - | - |
| `rate` | 100000 | 25000 | Bytes per second the server may send you. 25000 is about 600 bytes a snapshot at 40 a second; when a snapshot is bigger, servers that respect it skip the next ones, exactly when most is happening. In a test fight with 15 bots the snapshots stayed under 450 bytes, so this is headroom for full servers and mods with much more in view. Servers that cap it lower (most cap at 90000) still do. |
| `cl_maxpackets` | 125 | 63 | Packets with your commands sent per second: every frame at 125 frames a second, so your input leaves at once. Roughly 10 KB/s up (an estimate). |
| `sv_clientRate` (server) | 100000 | 50000 | The rate the server gives every client (`sv_ratePolicy 1`, the default of OpenJK servers). |

The Data Rate row of the server browser (56K, ISDN, LAN/Cable) is gone from the Jedi Academy multiplayer menus. The
menus of Jedi Outcast multiplayer can't be changed, so the engine sends its values (4000, 5000, 25000) as the default
instead, and the `cl_maxpackets` those menus set with them (15, 30) counts as the default too.

Settings files of older versions are changed once (`com_configVersion` 1): a `rate` of 4000, 5000 or 25000, a
`cl_maxpackets` of 15, 30 or 63 and a server's `sv_clientRate` of 50000 were old defaults or menu presets and become
the new defaults. Other values stay as they are.
