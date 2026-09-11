# Gains Tracker

A native client mod for [Ugaris](https://ugaris.com) that shows you exactly
what you are earning:

- **Experience lines** — every exp gain (or loss) is printed to chat with the
  exact amount and how much is left to your next level:
  `+5,000 exp (26,362,375 to level 188)`
- **Gold lines** — every change to your purse: `+2.50G`, `-35S`
- **Drop announcements** — when an item appears on the ground around you
  (a monster drops loot, another player discards something), you get a chat
  line with its position and distance, so nothing is missed under a corpse.
- **Session overlay** — a small HUD chip with session time, exp gained,
  exp/hour, gold delta and drop count. Drag it anywhere on the screen;
  the position is remembered.

## Commands

| Command | Effect |
|---------|--------|
| `#track` | Show status and help |
| `#track exp` | Toggle experience lines |
| `#track gold` | Toggle gold lines |
| `#track drops` | Toggle drop announcements |
| `#track overlay` | Toggle the session overlay |
| `#track reset` | Restart the session counters |

Settings persist in `<client config dir>/tracker_mod.cfg`.

## Installing

Install from the Ugaris Launcher: **Mods → Browse → Gains Tracker**, or via
*Install from URL* with `Ugaris/tracker-mod`.

## Notes

- Everything is computed client-side from data the server already sends;
  the mod sends nothing to the server.
- Ground items carry no names on the client, so drops are announced by
  position, not by name.
- Requires client v1.2.35 or newer.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Produces `tracker.so` / `tracker.dylib` / `tracker.dll` for the current platform.
On Windows you must first copy `lib/moac.a` (and `moac.lib`) from the client
release's `mod-sdk.zip` into `lib/` — mods link against the client's import
library there. Drop the library into its own folder under the game's user
directory, beside a `mod.json` (the launcher does this for you):

```
<userdir>/mods/Ugaris-tracker-mod/{mod.json,tracker.so}
```

Released binaries are built by [GitHub Actions](.github/workflows/build.yml)
from tags; no binaries are committed to this repository.

## License

MIT
