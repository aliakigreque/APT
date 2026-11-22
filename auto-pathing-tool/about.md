# Auto Pathing Tool

Auto Pathing Tool finds a way through a level by itself. It does not copy anyone's inputs: it carries its own
simulator of Geometry Dash's physics, searches it at millions of ticks per second for a sequence of clicks that beats
the level, and plays those clicks in the real game. When it goes somewhere you don't want, you can stop it, fix its
path tick by tick, and let it go on from there.

The mod is free. If it saves you a few hundred attempts, you can support it on
[Ko-fi](https://ko-fi.com/aliaky). Questions, bugs and ideas: [Discord](https://discord.gg/S4QzXYf65G).

## Requirements

- Geometry Dash **2.2081** on Windows, with Geode **5.11.2**.
- A kinda good computer

## Keys

These are the default keys. Every key can be changed in the menu or in the mod's settings. the menu and the
messages in game always show the keys you chose.

- **F1 - Menu.**

- **F2 - Find the path while playing.** The attempt restarts from the beginning and the bot plays it while the search
  runs alongside. The game only waits if the search falls behind. Press again to stop.

- **F3 - Edit the path.** While the bot plays (F2) or a macro plays: the level stops. **Left / A** goes back one tick,
  **Right / D** plays one tick; hold them to go faster. The jump button held while a tick plays is written in the
  path (released: no jump on that tick); the jump is whatever you set for player 1 and player 2 in the game (keys,
  mouse, controller, keybind mods), and in 2 player levels both players' jumps are written. The music stops while
  you edit and starts again at the right time when the level moves on. Press **F3** again: the bot goes on from there with your change, and
  searches the rest of the level from that point.

- **F4 - Find the whole path first.** The game waits while the bot solves the whole level, then plays it. Press again
  to stop.

- **F5 - Play the macro.** Plays the current macro from the start (the last path found, edited, recorded or loaded).

- **F6 - Save the macro.** Saves it as a GDR file in `macros/`. A level can have as many
  macros as you want: a new save never replaces an older one.

- **F7 - Record the game (bug reports).** Writes the exact player state tick by tick in `recordings/` and the level in
  `levels/`. Attach these files to a bug report.

## Progress and leaderboards

With **Safe mode** on, an attempt the bot played any part of count for nothing, like an attempt from a
start position: no completion, no new best, no attempts or jumps added to your stats. A macro playback never counts
as a completion either way.

If you turn Safe mode off, a live completion (F2) counts like a normal one. Auto Pathing Tool is made for showcases and
playtesting, not for stars or leaderboards: bot completions break RobTop's leaderboard guidelines, and scores can be
removed or accounts banned.

## Bug reports

On [Discord](https://discord.gg/S4QzXYf65G): send `auto-pathing-tool.log`, the level from `levels/` (exported when the
search hit a dead end), and if you have one, an F7 recording of the part where it goes wrong.
