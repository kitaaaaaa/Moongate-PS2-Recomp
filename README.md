# Moongate-PS2-Recomp
Recomp project for Sega Ages 2500 Series Vol. 31: Dennou Senki Virtual On (SLPM-62767)

## Playing

Windows x64 only. You need a disc image of your own copy of the Japanese release, Sega Ages 2500 Series Vol. 31
Dennou Senki Virtual On (SLPM-62767): a .cue/.bin pair or an .iso. This repository holds no game material: the game
is recompiled from your disc on your PC.

1. Download or clone this repository to a drive with about 8 GB free.
2. Run `setup.bat`, then pick your .cue (or .bin/.iso), or drop it on `setup.bat`.
3. When it finishes, the game starts. From then on, start it with `Moongate.exe`.

`setup.bat` installs its tools into `toolchain\` inside the repository (MSYS2 with clang, a JDK and Ghidra), so
nothing is installed system-wide, and deletes that folder again when it has finished. Next it unpacks your disc into
`moon\iso_jp`, analyses the game program with Ghidra, lifts its VU1 microprogram, and recompiles and builds it
(about 15 minutes of work on a fast PC, plus the downloads: the build libraries are fetched from GitHub while
configuring, which can take an hour or more on a slow connection). It downloads about 1 GB and needs about 8 GB while
it works; afterwards the game takes about 2.5 GB. Run `setup.bat` again after updating the repository to rebuild (it
downloads the tools again).

- Saves are kept in `moon\iso_jp\mc0`.
- The disc image is not needed after setup: the game runs from `moon\iso_jp`.
- Display defaults: the exact GS renderer, with interlacing and the flicker filter off. The environment variables
  `PS2X_GS_INTERLACE=1` and `PS2X_GS_FLICKER_FILTER=1` turn them back on.
- The game's output goes to `moon\logs\run_play.log`; the build logs are in `moon\logs\`.
