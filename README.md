# hypr-session-restore

Hyprland plugin. Snapshots open windows on every window event. Relaunches them
after the next Hyprland start.

## Behavior

- Window event (open/close/destroy/title/fullscreen/moveToWorkspace) marks
  state dirty.
- On `tick`, after 3s of quiet and if no `restoring.lock` is held, writes
  `~/.cache/hypr/session.json`. Old file rotates to `session.json.1`.
- On plugin load, waits 5s, then reads the snapshot, counts already-open
  windows per class, and `dispatch exec`s the missing ones.
- `restoring.lock` is held during the restore burst so the snapshot side
  doesn't overwrite the source-of-truth mid-restore.
- 3+ restores in 180s trips the crash-loop guard. `rm ~/.cache/hypr/restore-history`
  to reset.

Snapshot keeps one entry per PID. Multi-window apps (Firefox, Discord, VSCode)
relaunch once; the app's own session restore handles the rest.

Special workspaces (id < 0) excluded. Skip-list of autostart apps
(waybar, hyprpaper, hypridle, mako, polkit agents, etc.) hardcoded in
`SessionManager.hpp`.

## Build

```
make
```

Requires Hyprland 0.55 plugin SDK, `hyprutils`, `hyprlang`, `pixman-1`,
`libdrm`, g++14+ for C++26.

## Install

```
plugin = /path/to/hypr-session-restore.so
```

in `hyprland.conf`. Or via hyprpm:

```
hyprpm add <git-url>
hyprpm enable hypr-session-restore
```

## License

MIT. See `LICENSE`.

## Layout

```
src/main.cpp           PLUGIN_INIT / PLUGIN_EXIT
src/SessionManager.cpp snapshot, restore, debounce, crash-loop guard
src/Json.hpp           JSON reader/writer
```
