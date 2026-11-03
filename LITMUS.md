# The release litmus test

VidyaGod and the MetaPackageFormat ship when **every game below runs** through them: packaged as data, launched
off-screen, reaching play, with saves and settings kept. Originally-DOS games need their **DOS version** as well as
any modern port (two variants, one package). Built over time, not all at once.

Legend: ✓ packaged and launching · ◐ packaged, open issue · ✗ held (missing or broken data)

## A. Your collection

- `~/Storage/The Vidya/_Recovered Packages` — the 49 first-generation packages that survived the crash.
  ✓ 37 converted, of which 4 dropped by you (2026-10-23: The Shivah, Band of Bugs, Bernband, Gunmetal Arcadia Zero);
  ✓ with the GOG DOS versions (2026-10-22): DOOM II (+ Master Levels), HeXen (+ Deathkings), Strife, Wolfenstein 3D;
  DOOM 64 Retribution dropped, to be superseded by DOOM 64 EX; ✗ held for DOS versions: Heretic, Super 3D Noah's Ark,
  Eye of the Beholder I and II; ✗ Atomic Bomberman (sound data zeroed); ◐ Air Guard ("not compatible with Direct3D 8.1").
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
| 9 | Black & White ✓ (+ Creature Isle) | SafeDisc; crashes on multicore CPUs and modern GPUs |
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
| 1 | Doom ✓ (The Ultimate DOOM; Final DOOM too) | 1993 | DOS · UZDoom |
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
| 16 | Blood ✓ (+ Cryptic Passage) | 1997 | DOS · Raze (NBlood not yet) |
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
| 37 | Myst ✓ (Masterpiece Edition; Riven too) | 1993 | ScummVM |
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

## D. The best LAN party games

Not already in A, B or C. A LAN party game **runs** when at least three friends on the virtual LAN find each other in
the game's own LAN browser (or by a friend's vIP), play a match through, and keep their profiles. The virtual LAN fans out
broadcast and multicast across `10.66.0.0/16`, so these all reach it unmodified:

- LAN browsers on UDP or TCP;
- DirectPlay, with its service providers carried into the prefix;
- IPX, through IPXWrapper on Windows or DOSBox's IPX on DOS;
- Steam LAN, through the Goldberg emulator;
- emulated console LAN.

Played with two peers so far; three or more is untested, and this section is the first to need it.

Here ✓ = a match was played over the virtual LAN · ◐ = packaged, not yet played over it · ✗ = held.

| # | Game | Year | How it networks | Variants to ship |
|---|------|------|-----------------|------------------|
| 1 | Quake III Arena | 1999 | UDP, LAN browser | Windows · ioquake3 |
| 2 | Unreal Tournament 2004 | 2004 | UDP, LAN browser | Windows (3369 patch) |
| 3 | Halo: Combat Evolved ◐ | 2003 | UDP, LAN browser (GameSpy gone) | Windows (+ Custom Edition) |
| 4 | Battlefield 1942 | 2002 | LAN browser | Windows (1.61) |
| 5 | Battlefield 2 | 2005 | LAN, one peer runs the server | Windows (1.5) + dedicated server |
| 6 | Medal of Honor: Allied Assault | 2002 | LAN browser | Windows · OpenMoHAA |
| 7 | Wolfenstein: Enemy Territory | 2003 | LAN browser | Windows · ET: Legacy |
| 8 | Star Wars Jedi Knight: Jedi Academy | 2003 | LAN browser | Windows · OpenJK |
| 9 | Tribes 2 | 2001 | LAN browser | Windows (TribesNext) |
| 10 | Duke Nukem 3D ◐ | 1996 | IPX | DOS (IPX in DOSBox) · EDuke32 |
| 11 | Shadow Warrior ◐ | 1997 | IPX | DOS (IPX in DOSBox) |
| 12 | Rainbow Six 3: Raven Shield | 2003 | LAN browser | Windows (1.60) |
| 13 | SWAT 4 ◐ | 2005 | LAN co-op | Windows (+ The Stetchkov Syndicate) |
| 14 | Left 4 Dead 2 | 2009 | Steam lobby → Goldberg LAN | Windows (Goldberg) |
| 15 | Descent 3 | 1999 | IPX / TCP LAN | Windows · Descent 3 (open-source port) |
| 16 | Soldat | 2002 | LAN browser | Windows · open-source Soldat |
| 17 | Teeworlds | 2007 | LAN browser | Linux native (DDNet) |
| 18 | Age of Empires II ✓ (+ The Conquerors) | 1999 | DirectPlay | Windows (UserPatch); played across the internet 2026-08-28 |
| 19 | Age of Mythology ◐ | 2002 | LAN | Windows (+ The Titans) |
| 20 | Warcraft III ◐ (+ The Frozen Throne) | 2002 | UDP 6112 broadcast | Windows |
| 21 | Total Annihilation | 1997 | DirectPlay (IPX / TCP) | Windows (+ Core Contingency, Battle Tactics) |
| 22 | Command & Conquer | 1995 | IPX | DOS (IPX in DOSBox) · Vanilla Conquer |
| 23 | Command & Conquer: Tiberian Sun (+ Firestorm) | 1999 | IPX | Windows (IPXWrapper) |
| 24 | Empire Earth (+ The Art of Conquest) | 2001 | DirectPlay | Windows |
| 25 | Stronghold Crusader | 2002 | DirectPlay | Windows |
| 26 | Rise of Nations ✗ (No-CD) | 2003 | LAN | Windows (+ Thrones & Patriots) |
| 27 | Warhammer 40,000: Dawn of War | 2004 | LAN | Windows (Dark Crusade · Soulstorm) |
| 28 | Company of Heroes | 2006 | LAN | Windows (+ Opposing Fronts) |
| 29 | Supreme Commander: Forged Alliance | 2007 | LAN | Windows |
| 30 | The Battle for Middle-earth II | 2006 | LAN | Windows (+ Rise of the Witch-king) |
| 31 | Heroes of Might and Magic III ◐ | 1999 | TCP/IP | Windows (HD + HotA) · VCMI |
| 32 | Worms Armageddon ◐ | 1999 | LAN | Windows (3.8) |
| 33 | Wipeout XL ✓ | 1996 | TCP/IP | Windows; played over the virtual LAN |
| 34 | Re-Volt ◐ | 1999 | DirectPlay | Windows · RVGL |
| 35 | Need for Speed: Underground 2 ◐ | 2004 | LAN | Windows |
| 36 | Need for Speed: Most Wanted ◐ | 2005 | LAN | Windows |
| 37 | Midtown Madness 2 | 2000 | DirectPlay | Windows |
| 38 | FlatOut 2 | 2006 | LAN | Windows |
| 39 | TrackMania Nations Forever | 2008 | LAN | Windows |
| 40 | Motocross Madness 2 | 2000 | DirectPlay | Windows |
| 41 | MechWarrior 4: Mercenaries ◐ | 2002 | LAN | Windows |
| 42 | Freelancer ◐ | 2003 | LAN, one peer runs the server | Windows |
| 43 | Minecraft ◐ | 2011 | Open to LAN (multicast announce) | Java, every version |
| 44 | Terraria | 2011 | TCP, join by IP | Windows · Linux native |
| 45 | Factorio | 2020 | UDP, LAN browser | Linux native · Windows |
| 46 | Transport Tycoon Deluxe ◐ | 1995 | IPX | DOS · OpenTTD (LAN) |
| 47 | Neverwinter Nights | 2002 | LAN / direct IP | Windows (Diamond) |
| 48 | Titan Quest (+ Immortal Throne) | 2006 | LAN | Windows |
| 49 | Mario Kart: Double Dash!! (GameCube) | 2003 | Broadband Adapter LAN mode | Dolphin |
| 50 | Monster Hunter Freedom Unite (PSP) | 2008 | ad hoc, one peer runs the ad hoc server | PPSSPP |

### Steam games, over Goldberg

The Goldberg emulator replaces `steam_api(64).dll`. It finds the other players by UDP broadcast on port 47584 and carries
Steam lobbies and Steam peer-to-peer networking over the LAN, so any game whose multiplayer is only those (no backend of its own,
no Easy Anti-Cheat, no Epic Online Services) plays over the virtual LAN like a LAN game. Each player's Goldberg name and
Steam ID are their own (kept per user, never shipped in the package); the emulator is one shared library node.

| # | Game | Year | How it networks | Variants to ship |
|---|------|------|-----------------|------------------|
| 51 | Portal 2 (co-op) | 2011 | Steam lobby → Goldberg | Windows · Linux native |
| 52 | Left 4 Dead | 2008 | Steam lobby → Goldberg | Windows |
| 53 | Team Fortress 2 | 2007 | Source LAN browser + Goldberg | Windows · Linux native |
| 54 | Counter-Strike: Source | 2004 | Source LAN browser + Goldberg | Windows · Linux native |
| 55 | Garry's Mod | 2006 | Source LAN browser + Goldberg | Windows · Linux native |
| 56 | Killing Floor | 2009 | LAN browser + Goldberg | Windows |
| 57 | Borderlands 2 | 2012 | Steam lobby → Goldberg | Windows |
| 58 | Payday 2 | 2013 | Steam lobby → Goldberg | Windows |
| 59 | Civilization V | 2010 | Steam lobby → Goldberg | Windows |
| 60 | Magicka | 2011 | Steam lobby → Goldberg | Windows |
| 61 | Castle Crashers | 2012 | Steam lobby → Goldberg | Windows |
| 62 | Dungeon Defenders | 2011 | Steam lobby → Goldberg | Windows |
| 63 | Risk of Rain 2 | 2020 | Steam lobby → Goldberg | Windows |
| 64 | Deep Rock Galactic | 2020 | Steam lobby → Goldberg | Windows |
| 65 | Overcooked! 2 | 2018 | Steam lobby → Goldberg | Windows |
| 66 | Human: Fall Flat | 2016 | Steam lobby → Goldberg | Windows |
| 67 | Stick Fight: The Game | 2017 | Steam lobby → Goldberg | Windows |
| 68 | The Forest | 2018 | Steam lobby → Goldberg | Windows |
| 69 | Raft | 2022 | Steam lobby → Goldberg | Windows |
| 70 | Lethal Company | 2023 | Steam lobby → Goldberg | Windows |
| 71 | Valheim | 2021 | Steam lobby → Goldberg, or join by IP | Windows · Linux native |
| 72 | Stardew Valley | 2016 | Steam lobby → Goldberg, or join by IP | Windows · Linux native |
