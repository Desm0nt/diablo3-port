# Installing Diablo III Mobile — step by step

Русская версия: [INSTALL.ru.md](INSTALL.ru.md).

Diablo III Mobile runs **your own copy** of Diablo III: Eternal Collection for Nintendo Switch on an Android phone. This
project contains no game files and never shares game files or console keys: you dump them from a Switch you own. A setup
program on your PC turns your dump into a game folder (~16 GB, +5–6 GB per language pack), you copy that folder to the
phone and install the app.

The whole thing takes about an hour, most of it waiting.

## What you need

**From your Switch** (see step 1):

| What | Details |
|---|---|
| Diablo III base game | title id `01001B300B9BE000`, from your cartridge or eShop copy |
| Diablo III update **v1441792** | title id `01001B300B9BE800` (game patch 2.7.7). Only this exact update works (the port is made for its code). Dump tools usually put it in the file name, e.g. `...[01001B300B9BE800][v1441792].nsp` |
| `prod.keys` | the keys of **your own** console |
| *optional:* a language pack | only for a language other than English: e.g. the Russian Language Pack `01001B300B9BF00A`, the UK English pack `01001B300B9BF001` |

The game and the update can be **NSP, NSZ, XCI or XCZ**, in any combination. Split dumps from a FAT32 SD card work too
(a folder with parts `00`, `01`, … or `name.xc0`, `name.xc1`, …). File names do not matter: setup recognises the files
by their content.

**A PC** with Windows 10/11 (64-bit), Linux or macOS:

- about **20 GB of free disk space** while preparing (+5–6 GB per language pack; up to ~35 GB with NSZ / XCZ input);
- internet on the first run: setup downloads a few helper tools (Python packages ~40 MB, Microsoft .NET ~300 MB, and
  Python itself ~15 MB on Windows if you have none). It asks before each download and puts them only inside its own
  folder. Later runs work offline.

**A phone:**

- Android 12 or newer, 64-bit (Android 10 and 11: the separate `-android10` APK, Step 6);
- **Snapdragon with an Adreno GPU recommended** (tested on Snapdragon 8 Gen 3; Mali / PowerVR / Xclipse phones use their
  own driver with engine fallbacks and are untested);
- 8 GB of RAM or more;
- about **17 GB free** in internal storage (+5–6 GB per language pack);
- a USB cable (or another way to copy the folder to the phone).

## Step 1. Get the game files and keys from your Switch

**Dumping them yourself.** This needs a Switch that can run homebrew (how to get there is not covered here).
1. Install the game, the latest update (v1441792) and, if you want, the language pack on the Switch.
2. Dump them with [nxdumptool](https://github.com/DarkMatterCore/nxdumptool): installed or eShop copy → *NSP*;
   cartridge → *XCI* (the installed update and DLC are dumped separately as NSP). For NSP dumps turn on
   **"remove console specific data"** — otherwise the update cannot be read on a PC.
3. Dump the console keys with [Lockpick_RCM](https://github.com/shchmue/Lockpick_RCM). You get `prod.keys` on the SD card
   in `switch/`.
4. Copy the files to your PC.

Do not download game files or keys from the internet, and do not share yours.

## Step 2. Get Diablo III Mobile setup

Download this repository as a ZIP (green **Code** button → **Download ZIP**) or clone it, and extract it to a disk with
enough free space, into a short path such as `D:\D3-setup`. You get a folder with `setup.bat`, `setup.sh` and a folder
`put-game-files-here`.

## Step 3. Put your files into `put-game-files-here`

```
<setup folder>/
├── setup.bat
├── setup.sh
└── put-game-files-here/
    ├── Diablo III ... [01001B300B9BE000] ... .nsp              (or .nsz / .xci / .xcz — the base game)
    ├── Diablo III ... [01001B300B9BE800][v1441792] ... .nsp    (the update)
    ├── Diablo III ... [DLC Russian Language Pack] [01001B300B9BF00A] ... .nsp   (optional)
    └── prod.keys
```

You may also put the folder from your dump tool inside it as it is (setup looks two folders deep).

## Step 4. Run setup

### Windows

1. **Double-click `setup.bat`.** If Windows warns about a file from the internet, choose **Run** (or **More info → Run anyway**).
2. Answer the questions with **Y** and Enter (Enter alone also means yes): *Download a private Python?* (only when your PC
   has no Python 3.10+), *Install the Python packages?*, *Download and install the .NET 10 SDK?* Everything goes into
   `tools\.runtime` inside the setup folder. Nothing is installed system-wide, no admin rights are needed.
3. **Wait.** Setup shows eight numbered stages with progress:

   | Stage | Usually takes |
   |---|---|
   | 1. Finding and checking your game files | seconds |
   | 2. Checking the tools and the free space | seconds (first time: the .NET download) |
   | 3. Extracting the game program | under a minute |
   | 4. Patching the program for Android | under a minute |
   | 5. Checking the patch | 1–5 min |
   | 6. Extracting the game data (~16 GB) | 15–45 min |
   | 7. Translating the shaders | 2–10 min |
   | 8. Language packs (optional) | a few minutes each |

   It checks the free space before it starts and stops early with a clear message when something is wrong. If you
   close the window or the PC turns off, just run setup again: finished stages are skipped.
4. At the end it shows where the finished folder is, the adb commands to copy it, and what to do next.

Instead of using `put-game-files-here` you can also drag the files onto `setup.bat`.

### Linux / macOS

1. You need Python 3.10 or newer with its `venv` module (Debian / Ubuntu: `sudo apt install python3 python3-venv python3-pip`;
   Fedora: `sudo dnf install python3 python3-pip`; Arch: `sudo pacman -S python python-pip`; macOS: `brew install python@3.12`).
2. Open a terminal in the setup folder and run `bash setup.sh`. Answer the questions with **Y** and wait (same stages
   as on Windows). Python packages go into `tools/.runtime/venv`, .NET into `tools/.runtime/dotnet`.

You can also give the files directly, in any order: `bash setup.sh ~/dumps/d3-base.nsp ~/dumps/d3-update.nsp ~/prod.keys`.

### Not sure what is missing?

Run `setup.bat check` (Windows, in a terminal in the setup folder) or `bash setup.sh check`. It lists the tools, the
game files it finds, what is already prepared and the next step, and changes nothing.

## Step 5. Copy the game folder to the phone

The finished folder is **`work/Diablo3Mobile`** inside the setup folder.

- **USB file transfer:** connect the phone, choose **File transfer**, open the phone's **internal storage** →
  **Download** and copy the whole **`Diablo3Mobile`** folder there. It must end up exactly like this:

  ```
  Internal storage/Download/Diablo3Mobile/romfs/...
  Internal storage/Download/Diablo3Mobile/dm/out/...
  Internal storage/Download/Diablo3Mobile/dm/shaders/...
  Internal storage/Download/Diablo3Mobile/aoc/10/...      (only with a language pack)
  Internal storage/Download/Diablo3Mobile/astc/...        (the ASTC texture cache: phones with Mali, PowerVR or Xclipse)
  ```

  The `astc` folder (~6 GB, made by setup's last step, 10-30 min, or on the phone itself: pause menu → "Prepare textures") lets phones whose GPU has no BC textures (Mali,
  PowerVR, Samsung Xclipse) keep the textures about as small as on the Switch instead of 4-8x larger: on 6-8 GB phones it
  is what keeps Android from closing the game. Adreno (Snapdragon) phones do not need it. If you already have the game
  folder, `setup.bat astc` / `bash setup.sh astc` builds only this folder.

- **adb (USB debugging):** `setup.bat push` / `bash setup.sh push` copies only what is missing and resumes after an
  interruption; setup also prints the plain `adb push` commands.

## Step 6. Install the app

Install the APK from this repository's Releases page, or build it yourself (`build.bat`, see *Building* in the
[README](../README.md)). Android asks you to allow installing apps from your browser or file manager the first time.

- Android 12 or newer: `Diablo3-Mobile-v<version>-arm64.apk` from the [latest release](https://github.com/Desm0nt/diablo3-port/releases/latest) (`D3-port-debug.apk` when you build it yourself).
- Android 10 or 11: `Diablo3-Mobile-v<version>-arm64-android10.apk` (the same game built for older Android; it asks for the storage
  permission instead of *All files access*; not yet tested on a real Android 10 device).

## Step 7. First launch

1. Start **Diablo III Mobile**.
2. It asks for **All files access** (it needs it to read `Download/Diablo3Mobile`). Turn it on and go back.
3. The first start and the first loads take longer than later ones. The gear button opens the port's settings
   (language, picture shape / widescreen, frame rate limit, driver, controls). For another language, pick it under
   *Game language* (its language pack must be in `aoc/<index>`) and restart.

Saves and settings stay in the app's own storage. Installing a newer APK over the old one keeps them; the pause menu can
export and import saves.

## Troubleshooting / FAQ

**"your Diablo III update is v… ; only v1441792 is supported"** — dump update v1441792 (game patch 2.7.7).

**"the Diablo III base game / the update … is not among the files"** — setup lists every file it found with its title id
and version; check that you copied the right files into `put-game-files-here`.

**"prod.keys not found" / "your prod.keys has no …" / "does not decrypt with this prod.keys"** — use the keys of the
console you dumped from, made with an up-to-date Lockpick_RCM. `title.keys` is a different file.

**"personalized ticket" / "no ticket for rights id"** — dump the NSP again with nxdumptool's *remove console specific
data* option turned on (and keep the ticket).

**Setup stops or shows `!!` at stage 8/8 (DLC)** — language and item packs are optional: a pack that fails is skipped and the game folder is still finished (the game then runs in English). `setup.bat --no-dlc` / `bash setup.sh --no-dlc` ignores all DLC. Every run is logged to `work/setup.log`: attach it to an issue.

**"not enough free space"** — free some space, or put the work folder on another disk: `setup.bat --work D:\d3-work`
(Windows) or `bash setup.sh --work /mnt/big/d3-work`.

**The game stays in English** — the language pack for that language must be in `Download/Diablo3Mobile/aoc/<index>` and
selected under *Game language* in the gear dialog; restart the game afterwards.

**Black screen or the app closes** — open the gear dialog → *Vulkan driver* → try *System*, restart the app. Then open an
issue on this repository with the phone model, Android version and what happened, and attach
`Download/Diablo3Mobile/logs/crash.txt`: after a crash the next start writes it there (with the log of the crashed run)
and says so on screen. The same folder holds `boot.log` and `logcat.txt` of the current run and `boot.prev.log` /
`logcat.prev.txt` of the one before, written line by line while the game runs (so they are there even when Android kills
the game): attach the whole `logs` folder.

Technical details of the preparation (what each step does, running the tools by hand): [d3/GAME_FILES.md](d3/GAME_FILES.md).
