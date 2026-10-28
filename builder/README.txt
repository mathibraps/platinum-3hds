Platinum 3HDS builder
=====================

Makes Platinum 3HDS, the native 3DS port of Pokémon Platinum, from your own
Pokémon Platinum (USA) ROM (.nds). The game is not included: the builder
takes its files from your ROM, checks them, and writes two files next to it:

  Platinum3HDS.cia    install with FBI; it appears on the HOME menu
  Platinum3HDS.3dsx   or copy to the SD card's /3ds folder and start it from
                      the Homebrew Launcher

Both use the same save file, sdmc:/3ds/pokeplat/pokeplat.sav.

Using it
--------

Windows: drag your .nds file onto platinum3hds-builder.exe, or double-click
  the builder and drag the ROM into its window. If SmartScreen warns about an
  unknown app, choose "More info", then "Run anyway".

macOS: double-click platinum3hds-builder and drag the ROM into the Terminal
  window it opens, then press Return. The first time, macOS may refuse to
  open it because it is not from the App Store: open System Settings, Privacy
  & Security, and choose "Open Anyway" (or right-click the builder, Open).

Linux: run it in a terminal:  ./platinum3hds-builder path/to/rom.nds

Options: --out <folder> to write the files elsewhere, --force to build from a
ROM whose files differ from the original (it may not run).

The ROM must be Pokémon Platinum (USA). Rev 1 is checked file by file;
trimmed ROMs work too.
