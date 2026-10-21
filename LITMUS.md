# The release litmus test

VidyaGod and the MetaPackageFormat ship when **every game below runs** through them: packaged as data, launched
off-screen, reaching play, with saves and settings kept. Originally-DOS games need their **DOS version** as well as
any modern port (two variants, one package). Built over time, not all at once.

Legend: ✓ packaged and launching · ◐ packaged, open issue · ✗ held (missing or broken data)

## A. Your collection

- `~/Storage/The Vidya/_Recovered Packages` — the 49 first-generation packages that survived the crash.
  ✓ 37 converted; ✗ held for DOS versions: DOOM II, Heretic, HeXen, Strife, Duke Nukem 3D (+3 add-ons), Wolfenstein 3D,
  Super 3D Noah's Ark, Eye of the Beholder I and II, DOOM 64 Retribution (needs DOOM II); ✗ Atomic Bomberman (sound data
  zeroed); ◐ Air Guard ("not compatible with Direct3D 8.1").
- `~/Storage/The Vidya/_TO-DO` — every game in the folder, and every title in its `TO-DO.txt` and `PROBLEMATIC.txt`
  (the pre-crash library list).
- ✓ Demon's Souls (PS3, RPCS3).

## B. The 50 notoriously hard to run

| # | Game | Why it is hard |
|---|------|----------------|
| 1 | Grand Theft Auto IV (2008 disc) | Games for Windows Live + SecuROM; stuttering port |
| 2 | Dark Souls: Prepare to Die Edition | Games for Windows Live; 1024×720 render without DSfix |
| 3 | Fable III | Games for Windows Live activation |
| 4 | Halo 2 for Windows Vista | Games for Windows Live |
| 5 | Gears of War (2007 PC) | its signing certificate expired in January 2009 |
| 6 | Universe at War: Earth Assault | Games for Windows Live, never patched out |
| 7 | Splinter Cell: Chaos Theory (retail) | StarForce driver |
| 8 | Silent Hunter III (retail) | StarForce |
| 9 | Black & White | SafeDisc; crashes on multicore CPUs and modern GPUs |
| 10 | The Sims 2 with expansions | disc DRM per expansion; graphics rules miss modern GPUs |
| 11 | C&C Generals: Zero Hour (retail) | SafeDisc; GameSpy gone |
| 12 | Battlefield 2142 | EA servers gone; community master server |
| 13 | Star Wars: Battlefront II (2005) | disc DRM; GameSpy gone |
| 14 | Splinter Cell (2002) | shadows vanish on modern GPUs |
| 15 | Final Fantasy VII (1998 PC) | renderer and Yamaha MIDI; needs FFNx |
| 16 | Final Fantasy VIII (2000 PC) | the same |
| 17 | Need for Speed II SE | right only with 3dfx Glide |
| 18 | Need for Speed III / High Stakes | Direct3D path breaks on modern drivers |
| 19 | Carmageddon II | Glide |
| 20 | Star Wars: Rogue Squadron 3D | Glide |
| 21 | Interstate '76 | Win95 era; Nitro pack and patches |
| 22 | Outcast (1999) | voxel software renderer tied to old CPU timing |
| 23 | Messiah | crashes on modern systems |
| 24 | LEGO Island | crashes; decompilation port exists |
| 25 | Sonic R (PC) | Direct3D modes broken |
| 26 | Ultima IX: Ascension | buggy, Glide-first renderer |
| 27 | Archimedean Dynasty | early Direct3D, sound hardware |
| 28 | Dungeon Keeper 2 | DirectX 6 issues |
| 29 | Heavy Gear 2 | early Direct3D/Glide |
| 30 | Prince of Persia: The Sands of Time (PC) | lighting broken on newer GPUs |
| 31 | Silent Hill 3 (PC) | resolution and crashes; needs enhancements |
| 32 | Silent Hill: Homecoming (PC) | crashes or will not start |
| 33 | Resident Evil 4 (2007 PC) | missing effects, broken mouse |
| 34 | Deadly Premonition (2013 PC) | barely runs; needs DP Fix |
| 35 | Guilty Gear X2 #Reload (PC) | port quirks |
| 36 | Sonic Adventure DX (2004 PC) | needs the mod loader and fixes |
| 37 | Vampire: The Masquerade – Bloodlines | needs the unofficial patch |
| 38 | System Shock 2 | needs NewDark |
| 39 | Gothic (2001) | modern Windows fixes |
| 40 | Driver (1999) | crashes on modern CPUs |
| 41 | Hitman: Codename 47 | crashes on modern Windows |
| 42 | Sacrifice | Direct3D 7 crashes |
| 43 | Grim Fandango (1998) | Win9x installer; engine re-implementation |
| 44 | Battlecruiser 3000AD | infamous |
| 45 | MechWarrior 2 ✓ | a dozen builds (DOS, 3dfx, Titanium, Matrox…) |
| 46 | Wing Commander IV | DOS and Win95 versions, movie playback |
| 47 | King's Quest: Mask of Eternity | Win9x Direct3D; patch |
| 48 | EarthSiege 2 | Win3.1/95 WinG |
| 49 | Star Wars Rogue Squadron III: Rebel Strike (GameCube) | Dolphin's hardest game |
| 50 | Panzer Dragoon Saga (Saturn) | Saturn emulation |

## C. The 50 must-have PC games, 1990s–2000s

Not already in A or B. DOS games pair with their modern ports.

| # | Game | Year | Variants to ship |
|---|------|------|------------------|
| 1 | Doom | 1993 | DOS · UZDoom |
| 2 | Quake | 1996 | DOS · vkQuake / Ironwail |
| 3 | Quake II | 1997 | Windows · Yamagi Quake II |
| 4 | Half-Life | 1998 | Windows (retail) · Xash3D FWGS |
| 5 | Counter-Strike 1.6 | 2000 | Windows · Xash3D FWGS |
| 6 | Half-Life 2 | 2004 | Windows |
| 7 | Unreal | 1998 | Windows (OldUnreal 227) |
| 8 | Unreal Tournament | 1999 | Windows (OldUnreal 469) |
| 9 | Deus Ex | 2000 | Windows |
| 10 | Thief: The Dark Project | 1998 | Windows (NewDark) |
| 11 | Max Payne | 2001 | Windows |
| 12 | F.E.A.R. | 2005 | Windows |
| 13 | Far Cry | 2004 | Windows |
| 14 | Call of Duty | 2003 | Windows |
| 15 | Serious Sam: The First Encounter | 2001 | Windows |
| 16 | Blood | 1997 | DOS · NBlood |
| 17 | Star Wars: Dark Forces | 1995 | DOS · The Force Engine |
| 18 | Descent | 1995 | DOS · DXX-Rebirth |
| 19 | S.T.A.L.K.E.R.: Shadow of Chernobyl | 2007 | Windows |
| 20 | BioShock | 2007 | Windows |
| 21 | StarCraft + Brood War | 1998 | Windows |
| 22 | Warcraft II | 1995 | DOS · Windows (Battle.net Edition) |
| 23 | Command & Conquer: Red Alert | 1996 | DOS · Vanilla Conquer / OpenRA |
| 24 | Command & Conquer: Red Alert 2 | 2000 | Windows |
| 25 | Age of Empires | 1997 | Windows |
| 26 | Civilization IV | 2005 | Windows |
| 27 | Sid Meier's Alpha Centauri | 1999 | Windows |
| 28 | Master of Orion II | 1996 | DOS |
| 29 | X-COM: UFO Defense | 1994 | DOS · OpenXcom |
| 30 | Jagged Alliance 2 | 1999 | Windows · JA2 Stracciatella |
| 31 | Commandos: Behind Enemy Lines | 1998 | Windows |
| 32 | The Settlers II | 1996 | DOS · Return to the Roots |
| 33 | Theme Hospital | 1997 | DOS · CorsixTH |
| 34 | Diablo | 1996 | Windows · DevilutionX |
| 35 | Diablo II + Lord of Destruction | 2000 | Windows |
| 36 | Arcanum | 2001 | Windows |
| 37 | Myst | 1993 | ScummVM |
| 38 | Day of the Tentacle | 1993 | DOS · ScummVM |
| 39 | Full Throttle | 1995 | DOS · ScummVM |
| 40 | The Curse of Monkey Island | 1997 | Windows · ScummVM |
| 41 | SimCity 4 | 2003 | Windows |
| 42 | RollerCoaster Tycoon 2 | 2002 | Windows · OpenRCT2 |
| 43 | Tomb Raider | 1996 | DOS · TR1X |
| 44 | Grand Theft Auto: Vice City | 2002 | Windows (SilentPatch) |
| 45 | Grand Theft Auto: San Andreas | 2005 | Windows |
| 46 | Mafia | 2002 | Windows |
| 47 | Hitman 2: Silent Assassin | 2002 | Windows |
| 48 | FreeSpace 2 | 1999 | Windows · FreeSpace Open |
| 49 | Star Wars: TIE Fighter | 1994 | DOS (Collector's CD) · Windows |
| 50 | Carmageddon | 1997 | DOS · Windows (Splat Pack) |
