# BrawlVita: Brawl Stars v29 for PS Vita

**Version 0.1 beta** - an unofficial, fully **offline** port of Brawl Stars v29 (Android) to the PlayStation Vita.

BrawlVita is a **native port**, not an emulator: the game's own ARMv7 code runs directly on the Vita's CPU. A loader (in the style of
the Vita "so-loader" ports) loads the Android game library, connects it to the Vita's graphics, sound, touch screen and buttons, and a
small server built into the app answers the game so it runs without any network.

> This repository contains **only our own code**: no game files, libraries, art, music or text. You need your own copies of two v29 APKs
> (see [Requirements](#requirements)). BrawlVita is not affiliated with, endorsed or sponsored by Supercell. Brawl Stars is a trademark of
> Supercell.

## What BrawlVita adds

The offline v29 APK this port starts from only had a maxed account and bot battles. BrawlVita adds:

- **3 profiles**, each its own save: **Normal** (a new account from 0), **Modded** (normal, with infinite gems, coins, power points and
  star points), **Max** (every brawler at rank 35 and power 10 with every gadget and star power, all skins, icons and name colours, Brawl Pass
  and Trophy Road claimed, max level, infinite resources)
- **Real progression**: trophies per brawler, trophy league resets with star points, Trophy Road rewards, experience and levels
- **Shop with a daily rotation**: special offers, 6 daily deals, 6 skins, seasonal skins only in their season (Halloween, Christmas, Lunar),
  purchases that are kept
- **Brawl Boxes, Big Boxes and Mega Boxes** with 2020-style drops: brawlers by rarity, power points, gadgets, star powers
- **Brawl Pass** (free and premium track) and **quests** (daily and season)
- **Events** with the map rotation of the time and Boss Fight on weekends
- **A living world**: 1200 generated bot players and 60 bot clubs that keep playing while the Vita is off
  - global and local **leaderboards** (players, clubs, every brawler)
  - **matchmaking against bots near your trophies**, each with its own name, brawlers and power levels; their strength follows their rank
  - **clubs**: join one, see who is online, chat with the Vita keyboard (club mates answer), members join, leave and get promoted
  - tap any bot for its **profile**
- **Brawl TV**: watch bot matches
- **Battle log**, result screen with trophies and experience, name colour and skin choices that are kept
- **Vita controls**: touch screen and buttons (left stick move, right stick aim, R / Cross attack, L super, Square gadget)
- Faster loading and battles tuned for the Vita (simpler shadows and weather effects)

## Known issues (0.1 beta)

- Some maps run at about 16-20 fps; 30 fps is the next goal.
- The first start takes about 1-2 minutes, and a match can freeze for a few seconds at its start while it loads.
- Friendly battles do not work.
- Mega Boxes give less than they should.
- Boxes give a fixed amount of power points to each brawler.
- Special offer boxes do not give the right amount of boxes
- The battle log shows results but not the other players or replays.
- Some skin previews or Brawl Pass pictures can stay blank if the picture download failed.
- New worlds start with every bot at 0 trophies and one brawler, so early opponents mostly use the same brawler until bots unlock more.
- Not done yet: club invites, "wants to play" requests from bots, kicking or promoting members yourself, bot battle logs, Power Play.
  Asking to join an invite-only club gets no answer.
- Saving the bot world after a battle can cause a short hitch.
<!-- add new known issues above this line -->

## Requirements

- A PS Vita on firmware 3.60-3.74 with **HENkaku / h-encore / Enso** and **VitaShell** (tested on a PS Vita 1000, firmware 3.65)
- The **kubridge** plugin by TheOfficialFloW (the installer can add it over FTP; or by hand, see below)
- About 350 MB free on `ux0:`
- Your own copies of:
  - the **offline v29 APK** - its libraries must match these SHA-256 hashes (the loader is made for this exact build):

    | file | SHA-256 |
    |---|---|
    | `lib/armeabi-v7a/libg.so` | `a96de7c77ef6fdf850eab24fdcf35c3ab93d0b52135b15dc82c8e23669e0574d` |
    | `lib/armeabi-v7a/libfmod.so` | `a7d6bf28b1f89a672197a081a4c80742813c4e3e33014a84859dac7192201c00` |

  - the **original v29 APK** (29.270) - only Colette's 3D model is taken from it, because the offline APK's copy is wrong
- A Windows PC for the installer (Linux / macOS: run `installer/brawlvita_installer.py` with Python 3 + Pillow; folder and FTP modes work)

## Install

1. Download **BrawlVitaInstaller.exe** from the [Releases](../../releases) page and start it.
2. Choose your **offline v29 APK** and your **original v29 APK**.
3. Choose a **profile**: Normal, Modded or Max.
4. Optional: choose your own pictures for the **icon**, the **LiveArea background** and the **start screen**. They are resized and converted
   to the Vita's format automatically. Without pictures you get black covers with a white "BRAWLVITA".
5. Keep "Download the high-resolution pictures" on (needs internet): these skin pictures were never inside the APK and come from the game's
   asset server.
6. Choose where the files go:
   - **USB**: in VitaShell press START, choose USB mode, connect the cable, pick the Vita's drive
   - **FTP**: in VitaShell press SELECT, type the IP address it shows (you can also let the installer add kubridge)
   - **Folder**: then copy `ux0_data_BrawlVita` to the Vita as `ux0:data/BrawlVita` yourself
7. Press **Install**. Then on the Vita install `ux0:BrawlVita.vpk` with VitaShell and start **BrawlVita** from the LiveArea.

### kubridge by hand

Download `kubridge.skprx` from github.com/TheOfficialFloW/kubridge/releases, copy it to `ur0:tai/`, add the line `ur0:tai/kubridge.skprx`
under `*KERNEL` in `ur0:tai/config.txt` (or `ux0:tai/config.txt` if you use that one) and reboot.

## Profiles and settings

`ux0:data/BrawlVita/config.txt` has one line: `profile=normal`, `profile=modded` or `profile=max`. Change it with VitaShell to switch.
Each profile has its own save (`profile_<name>.txt`) and its own bot world (`world_<name>.bin`); delete those files to start a profile over.
Everything else is built in.

## Build from source

With Docker Desktop running: `build.bat` (Windows) or `./build.sh` (Linux / macOS) builds `BrawlVita.vpk`. `installer/build_exe.bat`
then packs the installer. Details: [docs/BUILD.md](docs/BUILD.md).

## How it works

- `loader/` - ELF loader for the Android library, code patches, the offline server (home data, shop, boxes, Brawl Pass, quests, battles,
  bot world, clubs), saved profiles
- `shims/` - bionic libc, pthread, file and asset access, sockets, GLES 2 on vitaGL, EGL
- `jni/` - a small fake Java VM for the calls the game makes into Java (including the keyboard)
- `gfx/`, `audio/` - texture decoding, GL setup, the Vita keyboard dialog, audio output
- `installer/` - the Windows installer
- `tests/` - tests for the pure-logic parts that run on a PC

## Legal

BrawlVita is a loader and an offline server written from observed behaviour; it contains no code, data or assets of the game. Use it only
with copies of the game you own, on your own console, and do not share the files the installer writes. Running a modified game client may
break the publisher's terms of service; BrawlVita has no online features. See [LEGAL.md](LEGAL.md).

## License

Our code is released under the [MIT License](LICENSE).

## Credits

- [S.B.](https://github.com/PeterHackz) - Original Offline V29
- [vitaSDK](https://vitasdk.org) and [vitaGL](https://github.com/Rinnegatamante/vitaGL) by Rinnegatamante
- [kubridge](https://github.com/TheOfficialFloW/kubridge) by TheOfficialFloW
- [VitaShell](https://github.com/TheOfficialFloW/VitaShell) by TheOfficialFloW
- **Claude** (Anthropic) - code, reverse-engineering notes and tests
