# mod-geddon-binding-shard

**AzerothCore module** – Adds a once-per-server drop of **Talisman of Binding Shard (17782)** to a configurable NPC’s corpse loot.

## Features
- Defaults to Baron Geddon (12056)
- Configurable via `worldserver.conf`
- Once-per-server drop memory table (`mod_geddon_once_drop`)
- Global announcement with killer and boss name
- Optional reset and repeat behavior for testing or custom servers

## Configuration (`worldserver.conf`)
```ini
GeddonShard.Enable = 1
GeddonShard.NpcEntry = 12056
GeddonShard.Chance = 1.0
GeddonShard.AllowRepeat = 0
GeddonShard.ResetOnStartup = 0
