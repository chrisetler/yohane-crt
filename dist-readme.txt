Yohane CRT {VERSION}
====================

A CRT filter for YOHANE THE PARHELION -BLAZE in the DEEPBLUE- that applies only to the level
graphics. The HUD, minimap, menus, dialogue boxes and cutscene art stay sharp.

Project page and source: {URL}


INSTALL
-------
1. In Steam, right-click the game > Manage > Browse local files.
2. Copy everything from this zip into that folder, so D3DCOMPILER_43.dll sits next to game.exe
   with the "crt" folder beside it. (The "source" folder is optional; the game doesn't need it.)
3. Start the game. The crt-geom filter is on by default; F7 switches to Basic.


KEYS
----
F7   Switch filter (crt-geom / Basic). If the filter is off, turns it back on.
F9   Filter on/off.
F8   Reload the filter files after editing them.

Your choice is remembered between launches.


TUNING
------
Each filter is a text file in the "crt" folder with its settings at the top. Edit, save, and
press F8 in game. If an edit has a mistake, the game keeps the last working version and writes
the error to crt\log.txt.


UNINSTALL
---------
Delete D3DCOMPILER_43.dll and the "crt" folder from the game folder.


COMPATIBILITY
-------------
Tested on Steam build 12487807 (game update of 28 April 2024), Windows 11.
If a game update breaks it, the game runs normally without the filter, and crt\log.txt says so.
Steam Deck / Linux (Proton) is untested; try the launch option:
    WINEDLLOVERRIDES="d3dcompiler_43=n,b" %command%


CHECKSUM
--------
SHA-256 of D3DCOMPILER_43.dll in this release:
{SHA256}


CREDITS AND LICENCE
-------------------
crt-geom by cgwg, Themaister and DOLLS (GPL v2 or later), ported from RetroArch.
Mod, Basic filter and HLSL ports 100% vibe coded, at my direction.

Released under the GNU General Public License, version 2 or later (LICENSE.txt). The complete
source is included in the "source" folder.

Not affiliated with or endorsed by the game's developers or publishers.
