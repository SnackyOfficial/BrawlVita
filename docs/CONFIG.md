# config.txt

`ux0:data/BrawlVita/config.txt` holds a single line:

| line | profile |
|---|---|
| `profile=normal` | a new account starting from 0 |
| `profile=modded` | like normal, with gems, coins, power points and star points that never run out |
| `profile=max` | everything unlocked and maxed: all brawlers at rank 35 and power 10 with every gadget and star power, all skins, icons and name colours, Brawl Pass and Trophy Road claimed, max level, infinite resources |

Each profile keeps its own save (`profile_<name>.txt`) and its own bot world (`world_<name>.bin`) in `ux0:data/BrawlVita/`. Without a
`config.txt` the normal profile is used. All other settings are built into the app (`loader/builtin_config.h`).

## Logs

`ux0:data/BrawlVita/log.txt` is written on every start (the previous three runs are kept as `log_1.txt` .. `log_3.txt`). Attach it to bug
reports.
