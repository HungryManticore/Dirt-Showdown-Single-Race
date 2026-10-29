# DiRT Showdown – Single Race

An ASI mod for **DiRT Showdown** that turns the **Split Screen** mode into a
single-player **Single Race**: you pick the event, track and settings as usual,
but race alone on a full, unsplit screen.

## What it does

- Skips the "Player 2, press START" prompt.
- Skips Player 2's car selection.
- Renders the race on one full screen, with the normal single-player HUD.
- Removes Player 2's car from the grid.
- **Vehicle class picker.** The "Player 2 controls" button in the split screen
  lobby now picks a vehicle class (Event default → Race → Demolition →
  Hoonigan). The class you choose applies to both your car list and the AI.
  A message such as *"Vehicle class set to RACE"* briefly appears on screen
  each time you press it.

Supports both `showdown.exe` and `showdown_avx.exe` (current Steam build).
Tested on Linux with Proton/DXVK.

## Installation

1. The mod only works with a DRM-free executable. The unmodified showdown.exe and showdown_avx.exe contain protections against memory tampering, which is what this script does.
2. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
   (x86) into the game folder as `dinput8.dll`.
   On Linux/Proton, add the launch option `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
3. Create a `scripts` folder in the game folder.
4. Copy `release/SplitToSingle.asi` and `release/SplitToSingle.ini` into `scripts`.
5. Start the game and choose **Split Screen** from the menu.

## Configuration

`SplitToSingle.ini` (next to the `.asi`; 1 = on, 0 = off):

| Key | Default | Effect |
|---|---|---|
| `Enabled` | 1 | Master switch |
| `SkipP2PressStart` | 1 | Skip the Player 2 "press START" prompt |
| `SkipP2CarSelect` | 1 | Skip Player 2's car selection |
| `RemoveP2` | 1 | Only Player 1 is a human entrant |
| `SingleScreen` | 1 | Full-screen viewport and single-player HUD |
| `ClassSelect` | 1 | Lobby button becomes the vehicle class picker |
| `LogFlow` | 0 | Log every front-end flow transition (for troubleshooting) |

`SplitToSingle.log` is written next to the `.asi` and includes a crash report
if anything goes wrong. Attach it when reporting issues.

## Building

Needs only `clang`, `lld` and `llvm-dlltool`. No MSVC or Windows SDK
is required, because the mod is freestanding C:

```sh
./build.sh
```

The output is `release/SplitToSingle.asi`.

## Source layout

- `src/SplitToSingle.c` – game hooks (front-end flow redirects, entrant list,
  rendering mode, split screen checks, class override, lobby button label)
- `src/overlay.c` – small D3D11 `Present` hook that draws the on-screen message
- `src/*.def` – import definitions used to generate the import libraries

## AI disclosure
The script was written by Claude and tested by a human being.
