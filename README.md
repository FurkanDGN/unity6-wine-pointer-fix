# Unity 6 mouse fix for Wine on macOS (Whisky / CrossOver / plain Wine)

A drop-in `version.dll` that makes mouse clicks work in **Unity 6** games
running under **Wine builds older than 11.3**, such as the stock Whisky
engine (Wine 11.0) or CrossOver 25.

If you landed here searching for one of these, this is for you:

- Unity 6 mouse not working Wine
- `EnableMouseInPointer failed with the following error: Call not implemented.`
- Whisky Unity clicks not working / cursor flickers / cursor hidden
- CrossOver Unity game menu does not react to clicks, keyboard works

## Do you need this?

**Only on Wine < 11.3.** Upstream Wine implemented `EnableMouseInPointer`
and mouse-to-`WM_POINTER` translation in Wine 11.3 (February 2026, `win32u`).
On a newer runtime the game works without any shim:

| Runtime | Needs this shim? |
| --- | --- |
| Whisky stock engine v3.1.1 (Wine 11.0) | yes |
| Whisky beta engines v4.5.105+ (Wine 11.15+) | no |
| CrossOver 25.x | yes (or see kiku-jw's patch below) |
| Wine 11.3 or newer | no |

The symptom: keyboard works, the mouse moves but clicks never register,
and the game's `Player.log` contains

```
EnableMouseInPointer failed with the following error: Call not implemented.
```

## Download and verify

No binary is committed to this repository. `version.dll` is built from this
source by GitHub Actions (see `.github/workflows/build.yml`) and attached to
each release together with its SHA-256 and a signed build provenance
attestation, so you can check that the file you downloaded came out of this
exact source and workflow:

```bash
gh attestation verify version.dll --owner FurkanDGN
sha256sum -c SHA256SUMS
```

Releases: https://github.com/FurkanDGN/unity6-wine-pointer-fix/releases

If you would rather not trust any download, build it yourself (one command,
see below) — the source is about 550 lines of plain C.

## Install

1. Copy `version.dll` into the game folder, next to `UnityPlayer.dll`.
2. Set the DLL override `version=n,b` for the game.
   - Whisky: bottle → program settings → environment variables →
     `WINEDLLOVERRIDES` = `version=n,b`. If you launch through Steam, set it
     on `steam.exe` too, so the game process inherits it.
   - Plain Wine: `WINEDLLOVERRIDES="version=n,b" wine Game.exe`
3. Start the game. Clicks should work immediately.

Optional debug log: set `UNITY6_PTRFIX_LOG=1` and a `unity6_ptrfix.log`
file appears in the game folder.

## How it works

Unity 6's input backend calls `EnableMouseInPointer(TRUE)` and then only
consumes `WM_POINTER*` messages. Old Wine stubs that call
([Wine bug 53847](https://bugs.winehq.org/show_bug.cgi?id=53847)), so the
game never sees pointer input and ignores the legacy mouse messages Wine
does deliver.

This shim is loaded as the game's `version.dll` and:

- makes `EnableMouseInPointer` / `IsMouseInPointerEnabled` report success;
- mirrors every `WM_MOUSE*` message the game retrieves as a synthesized
  `WM_POINTER*` message sent to the window procedure (screen coordinates,
  button state tracked for drags);
- serves `GetPointerType` / `GetPointerInfo` / `GetPointerFrameInfo` from
  the last mouse frame;
- wraps `GetProcAddress` so dynamic lookups get the same hooks;
- forwards the real `version.dll` API to the builtin in `system32`.

Interception is done **only by patching the import tables (IAT)** of
`UnityPlayer.dll` and the game executable. No Wine binaries are modified,
no Direct3D hooks, nothing is written outside the game folder. Removing the
file and the override restores the original state.

## Build

Cross-compile with MinGW-w64 (`brew install mingw-w64` on macOS):

```bash
make
# or
x86_64-w64-mingw32-gcc -shared -O2 -Wall -o version.dll \
    version_ptrfix.c version.def -static-libgcc -luser32
```

## Tested with

- Beta Kafe: Write Your Love Story (Unity 6000.0.68f1), Whisky (frankea
  fork) 3.7.0, stock engine v3.1.1 (Wine 11.0), DXMT, Apple Silicon.

## Prior art and credits

- [kiku-jw/peak-crossover-mouse-fix](https://github.com/kiku-jw/peak-crossover-mouse-fix):
  patches CrossOver's `user32` / `win32u` with the same mouse-to-pointer
  emulation. This shim reimplements that message translation in user space
  so no Wine files have to be replaced.
- [programmeruser517/unity6-wine-macos](https://github.com/programmeruser517/unity6-wine-macos):
  an earlier `version.dll` shim with a different hooking approach (also
  stubs `ID3D11Device5::CreateFence` for a separate startup crash).
- Rémi Bernon's original emulation patch attached to
  [Wine bug 53847](https://bugs.winehq.org/show_bug.cgi?id=53847), and the
  upstream implementation that made all of this unnecessary on Wine 11.3+.

This is a user-space workaround, not a fix. The real fix is Wine's own
`WM_POINTER` support; upgrade your Wine runtime when you can.

## License

MIT, see `LICENSE`.
