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

For terminal windows (`kitty`, `foot`, `alacritty`, `wezterm`, `ghostty`,
`xterm`, `st`), the plugin walks the descendant process tree and records the
foreground program name as `[<terminal>, <-e or -->, <argv0>]`. **Only argv[0]
is captured — all arguments are dropped.** Programs whose argv commonly carries
secrets (`ssh`, `gpg`, `aws`, `gh`, `op`, `pass`, `curl`, `psql`, `kubectl`,
`vault`, `terraform`, `sudo`, etc. — see `SENSITIVE_BASENAMES` in
`SessionManager.cpp`) are not captured at all; the terminal restores empty.
Idle terminals (running only a shell) restore as the bare terminal.

Periodic re-snapshot every 30s catches foreground-program changes that don't
fire any Hyprland window event (e.g. you launched a terminal with an explicit
program; the program forks too late for the debounced post-open snapshot).

## Security notes

- File mode `0600`, cache dir `0700`, owner-only. Open via `O_NOFOLLOW`,
  refuses non-regular or non-owner-owned files.
- Captured data persists on disk — exclude `~/.cache/hypr/` from any backup
  tool (restic/borg/rclone/etc.) before adding one.
- `/proc/<pid>/cmdline` is world-readable already; the plugin's only added
  exposure is **persistence**. Shell scrub (`explicit_bzero`) is applied to
  intermediate cmdline buffers; `posix_fadvise(DONTNEED)` is hinted on the
  written file. Argv minimization (drop everything past argv[0]) is the actual
  load-bearing mitigation.
- Encryption-at-rest is **not** implemented: any keyring-backed key would
  either race the 5s startup window or fail-open silently, and a key-on-disk
  scheme is theatre. The defensible posture is "do not write secrets to disk
  in the first place" — hence the SENSITIVE_BASENAMES denylist + argv stripping.

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
