# Expere

A keyboard launcher for Windows in the spirit of Raycast / Spotlight, built to use as little memory as possible.
One C file, plain Win32, ~145 KB executable, no runtime to install. The window is see-through (blurred acrylic by
default) and follows your Windows light/dark theme.

Press **Alt+Space**, type, press **Enter**.

| Search for | Gets you |
| --- | --- |
| App names, acronyms (`vsc` → Visual Studio Code) | Every app in Start, including Store apps |
| File and folder names | Files and folders from the Windows Search index (or Everything, if it is running), plus folders anywhere on your local drives that Windows does not index, such as `C:\Projects` |
| `2^10`, `sqrt(2)*10`, `15% 4` | Calculator; Enter copies the result |
| `lock`, `sleep`, `restart`, `shut down`, `sign out`, `empty recycle bin` | System commands (power actions ask first) |
| `expere settings` | The settings window |
| Anything | Web search fallback as the last row |

Keys: `↑` `↓` / `Tab` move, `PgUp` `PgDn` jump a page, `Enter` open, `Ctrl+Enter` show in folder, `Esc` clear then
close, `Ctrl+,` settings. When there are more results than rows, scroll with the wheel or touchpad, or drag the
scroll bar on the right; the web search is always the last item.
The search box supports the usual editing keys: `Ctrl+←/→`, `Shift`-selection, `Ctrl+A/C/X/V`, `Ctrl+Backspace`.
Things you open often rank higher (counts are kept in `%APPDATA%\Expere\history.txt`).

Tray icon: left-click opens, right-click has **Settings…** and **Quit**.

## Settings

Open with `Ctrl+,`, the tray menu, or by typing `expere settings`. Saved to `%APPDATA%\Expere\settings.ini`.

| Setting | Options |
| --- | --- |
| Shortcut | Click the shortcut button, then press the keys you want (Esc cancels). It needs Ctrl, Alt or Win with the key, except F-keys, which work alone. Applied on Apply / OK; a shortcut another app already owns is refused with a message. |
| Theme | Follow Windows, Dark, Light |
| Background | **Acrylic** (blurred, like the Start menu), **Transparent** (clear tinted glass), **Solid** |
| Opacity | 20–100 %: how strong the tint is over the blur or glass |
| Visible rows | 3–10 rows on screen; up to 61 results scroll |
| Web search | Google, Bing, DuckDuckGo, Brave Search |
| Include files and folders | Turn file search off to keep results to apps and commands |
| Start with Windows | Adds/removes Expere from `HKCU\…\Run` |
| Keep the last search | Reopen with the previous query selected instead of an empty box |
| Clear usage history | Resets the "opened often" ranking |

## Build

Needs MSYS2 with the UCRT64 toolchain (`pacman -S mingw-w64-ucrt-x86_64-gcc`).

```bash
powershell -File build.ps1
```

## Command line

```bash
expere.exe                 # start (or open the launcher if already running)
expere.exe --background    # start in the tray without a notification
expere.exe --settings      # open the settings window
expere.exe some text       # open the launcher with "some text" typed in
expere.exe --quit          # stop the running instance
```

## Memory

Measured on this machine (Windows 11, Task Manager's "Memory" column = private working set), default acrylic background:

| State | Private working set | Commit |
| --- | --- | --- |
| Idle in tray after start | 0.2 MB | 6 MB |
| Open, after five searches | 5.4 MB | 9.3 MB |
| Hidden again | 0.3–1.7 MB | 8 MB |

Where the savings come from:

1. **No browser engine, no JavaScript.** The UI is one Win32 window painted by our own code.
   A web-tech launcher pays for one or more renderer processes and a JS heap before it shows a single result.
2. **No file index in our process.** File search asks the Windows Search index, which Windows already keeps
   running. Expere connects over OLE DB only while the window is open and disconnects on hide.
   Windows only indexes your user folder, so Expere also keeps the *names* of folders elsewhere on local drives
   (5 levels deep, skipping Windows, Program Files, hidden folders, `node_modules` and dot-folders): about
   700 folders and ~30 KB on this PC, stored as a name plus a parent link rather than full paths.
   Everything is used if its client is *already* running, but Expere never starts it: on this machine
   Everything's full-disk index cost ~245 MB across its client and service, which is the same problem moved elsewhere.
3. **Transparency without a big back buffer.** The blur is done by Windows (DWM). Expere draws the window in
   row-high strips with per-pixel alpha, so even a tall window at 150 % scaling needs well under 1 MB of buffers,
   and those are freed when the window hides.
4. **Apps live in a private heap.** The Start apps list is a few hundred strings in its own `HeapCreate` heap; a refresh
   builds a new heap on a background thread and frees the old one with a single `HeapDestroy`, so nothing fragments.
5. **Icons are borrowed and lazy.** Icons come from the system image list (shared with Explorer) and are looked up only
   for the rows on screen; built-in commands use the Segoe Fluent Icons font that ships with Windows.
6. **Trim on hide.** Closing the window calls `SetProcessWorkingSetSize(-1, -1)` (again 20 s later), returning pages
   to Windows. They sit on the standby list, so reopening is still instant.

The large "Working set" figure you may see in other tools while the window is open (~40 MB) is almost entirely
shared pages of system DLLs such as `shell32.dll` that Explorer has loaded anyway; it is not memory Expere owns.

## How the transparency works

The client area is covered by `DwmExtendFrameIntoClientArea`, so DWM composites the window using the alpha channel
of what Expere paints. Acrylic is the Windows 11 system backdrop (`DWMWA_SYSTEMBACKDROP_TYPE` = transient window,
the Start menu's material), which blurs every window behind it, GPU-drawn browsers included. It only works on a window
with a frame, so the window has `WS_THICKFRAME` and hides it in `WM_NCCALCSIZE`. The Opacity slider paints extra
tint on top. Before Windows 11 22H2 Expere falls back to the undocumented accent policy
(`SetWindowCompositionAttribute`), which reliably blurs only classic GDI windows.
GDI writes no alpha, so text is rendered with ClearType into a mask and blended in from its coverage: over the
**Solid** background every colour channel uses its own coverage (real ClearType); over see-through backgrounds DWM
has only one alpha per pixel, so the subpixel coverages are merged into sharp grayscale text. **For the crispest text,
choose Solid.** Icons get their alpha by drawing them over black and over white, at their native 32/48 px size, and are
then resampled to the display scale instead of being stretched. For the same alpha reason the search box is a small
custom control rather than a Windows edit box; it supports IME input, but screen readers will not see it as a text field.

## Tuning

- Paths hidden from file results: `WS_EXCLUDE` / `EV_EXCLUDE` in `expere.c` (AppData, node_modules, dot-folders, Start Menu).
- Colours for the two themes: `PAL_DARK` / `PAL_LIGHT` in `expere.c`.
