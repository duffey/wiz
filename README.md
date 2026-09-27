# wiz

A hotkey app switcher for Windows 11, in one C file. Bind an F-key to a
program from the tray menu, and that key jumps to the program's window or
launches it.

## Build

Requires CMake 3.20+, Ninja, and a C compiler. The included preset uses clang
from llvm-mingw:

```
cmake --preset default
cmake --build --preset default
```

The executable is `build/wiz.exe`.

With Visual Studio 2022 installed instead, the plain commands also work:

```
cmake -S . -B build
cmake --build build --config Release
```

which puts the executable at `build/Release/wiz.exe`.

## Using it

wiz lives in the notification area (system tray). Click its icon to open the
menu:

- **F1 … F12** — each row shows the bound program (with its icon) and opens a
  submenu listing every program that currently has a window open. Pick one to
  bind it to that key, or pick *Unassigned* to release the key. The current
  choice is checked.
- **Run at startup** — launches wiz at sign-in, via the per-user Run entry in
  the registry. The entry points at the exe's current location, so toggle it
  off and on again after moving the exe.
- **Quit** — exits wiz.

Pressing a bound F-key focuses that program's window. If the program has
several windows, repeated presses cycle through them. If it has none open, the
program is launched from the path it was running from when you bound it.

Bindings are saved to `%LOCALAPPDATA%\wiz\wiz.ini` and reloaded on start.

A bound F-key is grabbed system-wide, so the focused program no longer sees
it (F1 will not open help, and so on). Unassigned keys are left alone. To use
Alt+F1 etc. instead, change `KEYMOD` at the top of `wiz.c` to
`MOD_ALT | MOD_NOREPEAT`; Alt+F4 closes windows, so leave F4 unbound then.

The window classes wiz ignores (taskbar, desktop, and other shell surfaces)
are listed at the top of `wiz.c` too. Edit and rebuild.

## How it works

- When a key is pressed or the menu opens, wiz enumerates top-level windows
  and keeps every one that is visible, titled, not a tool window, not owned by
  another window, and not cloaked (hidden on another virtual desktop).
- Each window is matched to its program by the executable that owns it.
- Windows of the same program are cycled in the order they were first seen,
  so repeated presses visit each one in turn.
- A minimized window is restored when focused.
- Menu icons come from the window itself (so Store apps work too), falling
  back to the executable's shell icon.

## Notes

- Only one instance runs at a time.
- If a bound key is already taken by another program, wiz warns at startup and
  the remaining keys still work.
- Launching a Microsoft Store app from its exe path can fail, since those live
  under a protected folder. Focusing an already-running one works fine.
