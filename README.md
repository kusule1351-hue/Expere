# Expere

A keyboard launcher for Windows in the spirit of Raycast / Spotlight, built to use as little memory as possible.
One C file, plain Win32, 112 KB executable, no runtime to install.

Press **Alt+Space**, type, press **Enter**.

| Search for | Gets you |
| --- | --- |
| App names, acronyms (`vsc` → Visual Studio Code) | Every app in Start, including Store apps |
| File and folder names | Results from the Windows Search index (or Everything, if it is running) |
| `2^10`, `sqrt(2)*10`, `15% 4` | Calculator; Enter copies the result |
| `lock`, `sleep`, `restart`, `shut down`, `sign out`, `empty recycle bin` | System commands (power actions ask first) |
| Anything | Web search fallback as the last row |

Keys: `↑` `↓` / `Tab` move, `Enter` open, `Ctrl+Enter` show in folder, `Esc` clear then close, `Ctrl+Backspace` delete word.
Things you open often rank higher (counts are kept in `%APPDATA%\Expere\history.txt`).

Tray icon: left-click opens, right-click has **Start with Windows** and **Quit**.

## Build

Needs MSYS2 with the UCRT64 toolchain (`pacman -S mingw-w64-ucrt-x86_64-gcc`).

```bash
powershell -File build.ps1
```

## Command line

```bash
expere.exe                 # start (or open the launcher if already running)
expere.exe --background    # start in the tray without a notification
expere.exe some text       # open the launcher with "some text" typed in
expere.exe --quit          # stop the running instance
```

## Memory

Measured on this machine (Windows 11, Task Manager's "Memory" column = private working set):

| State | Private working set | Commit |
| --- | --- | --- |
| Idle in tray after start | 0.2 MB | 6 MB |
| Open, after five searches | 4.9 MB | 8.8 MB |
| Hidden again | 0.5–1.7 MB | 8 MB |

Where the savings come from:

1. **No browser engine, no JavaScript.** The UI is one Win32 window with a native edit control, painted with GDI.
   A web-tech launcher pays for one or more renderer processes and a JS heap before it shows a single result.
2. **No file index in our process.** File search asks the Windows Search index, which Windows already keeps
   running. Expere connects over OLE DB only while the window is open and disconnects on hide.
   Everything is used if its client is *already* running, but Expere never starts it: on this machine
   Everything's full-disk index cost ~245 MB across its client and service, which is the same problem moved elsewhere.
3. **Apps live in a private heap.** The Start apps list is a few hundred strings in its own `HeapCreate` heap; a refresh
   builds a new heap on a background thread and frees the old one with a single `HeapDestroy`, so nothing fragments.
4. **Icons are borrowed and lazy.** Icons come from the system image list (shared with Explorer) and are looked up only
   for the eight rows on screen.
5. **Fixed-size results.** The candidate lists are capped at eight entries and kept sorted on insert; no growing arrays.
6. **Trim on hide.** Closing the window calls `SetProcessWorkingSetSize(-1, -1)` (again 20 s later), returning pages
   to Windows. They sit on the standby list, so reopening is still instant.

The large "Working set" figure you may see in other tools while the window is open (~40 MB) is almost entirely
shared pages of system DLLs such as `shell32.dll` that Explorer has loaded anyway; it is not memory Expere owns.

## Tuning

- Paths hidden from file results: `WS_EXCLUDE` / `EV_EXCLUDE` in `expere.c` (AppData, node_modules, dot-folders, Start Menu).
- Hotkey: `RegisterHotKey(... MOD_ALT, VK_SPACE)` in `wWinMain`.
- Web search engine: the URL in `OnQueryChanged`.
