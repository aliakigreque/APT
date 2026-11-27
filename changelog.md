# v1.0.1
- Level fixed : LIMBO, flamewall, Pixelated Kisses, forest of spikes, bell, green bullet, mystic and more.
- The search can now detect far counters that turn obstacle on.
- When obstacles block the way, the search now follow the trigger chain and finds the one that toggles them.
- Item comparison triggers that open the way are now treated as goals
- toggle orbs that deletes obstacles are now found way beforehand.
- various research rules fixed : a portal judged mortal is now priorised if required, speed and gamemode portals are now tried 4 times max, falling into the void is only allowed at the very end of a level with no end trigger, and parts of a level played to the left no longer count as being stuck.
- Creation of "decision zones" when keys, clicks orbs, etc, decide the outcome of a part, the game waits to find the good result before resuming. also added a longer wait time before those kind of zones.
- move triggers are now shared between states that only differ by an item or a counter. for better performance.
- various debug improvements.

# v1.0.0

- First release.
