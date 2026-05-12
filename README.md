GitHub description:

A configurable AzerothCore boss-loot module that can add custom item drops to one or more bosses, including repeatable drops, once-per-server drops, custom chances, stack counts, and optional global announcements.


README:

# mod-geddon-binding-shard

AzerothCore module for adding configurable custom item drops to boss corpse loot.

Originally, this module existed to let Baron Geddon drop the legendary Talisman of Binding Shard once per server. It has since been expanded into a more flexible boss-loot module that can add different loot items to different bosses through config alone.

It can still preserve the original Baron Geddon / Talisman of Binding Shard behavior, but it is no longer limited to one boss or one item.

Talisman of Binding Shard reference:

https://wowpedia.fandom.com/wiki/Talisman_of_Binding_Shard

## Features

- Add custom item drops to one or more bosses
- Configure different loot rules per boss
- Add multiple custom items to the same boss
- Set individual drop chances per item
- Configure minimum and maximum stack counts
- Support repeatable drops
- Support once-per-server drops
- Optional global announcements per drop rule
- Optional duplicate prevention if the item already exists in the corpse loot
- Automatic database table creation for once-per-server tracking
- Legacy compatibility with the original GeddonShard config style

## Default Behavior

By default, the module keeps the original behavior:

- Baron Geddon, entry `12056`
- Talisman of Binding Shard, item `17782`
- 1% drop chance
- Once per server
- Global announcement enabled

## Database

The module automatically creates its own tracking table:

```sql
mod_configurable_boss_loot_once
```

This table is used only for once-per-server drops.

Repeatable drops do not need persistence.

The old table:

```sql
mod_geddon_once_drop
```

may still be used for migration/legacy compatibility if your server previously used the original version of the module.

## Configuration

The module is configured through numbered loot rules.

Each rule represents one possible custom drop.

For example:

```ini
BossLoot.Rule.1.*
BossLoot.Rule.2.*
BossLoot.Rule.3.*
```

The number of rules loaded is controlled by:

```ini
BossLoot.RuleCount = 3
```

If you add a new rule, increase `BossLoot.RuleCount`.

For example, if you add `BossLoot.Rule.4.*`, set:

```ini
BossLoot.RuleCount = 4
```

If `BossLoot.RuleCount` is too low, later rules will be ignored.

## Master Settings

```ini
BossLoot.Enable = 1
BossLoot.RuleCount = 1
BossLoot.ResetOnStartup = 0
```

### BossLoot.Enable

Enables or disables the module.

```ini
BossLoot.Enable = 1
```

Use `0` to disable all custom boss loot rules.

### BossLoot.RuleCount

Controls how many numbered rules the module loads.

```ini
BossLoot.RuleCount = 1
```

### BossLoot.ResetOnStartup

Global reset option for once-per-server drops.

```ini
BossLoot.ResetOnStartup = 0
```

Normally this should stay `0`.

Set to `1` only if you intentionally want once-per-server drop memory reset when the worldserver starts.

## Example 1: Original Baron Geddon Talisman Drop

```ini
BossLoot.Enable = 1
BossLoot.RuleCount = 1
BossLoot.ResetOnStartup = 0

BossLoot.Rule.1.Enable = 1
BossLoot.Rule.1.NpcEntry = 12056
BossLoot.Rule.1.ItemEntry = 17782
BossLoot.Rule.1.Chance = 1.0
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 1
BossLoot.Rule.1.AllowRepeat = 0
BossLoot.Rule.1.PreventDuplicate = 1
BossLoot.Rule.1.OnceKey = geddon_17782_once
BossLoot.Rule.1.ResetOnStartup = 0
BossLoot.Rule.1.Announce = 1
BossLoot.Rule.1.AnnounceMessage = {player} has looted the legendary {item} from {boss}!
```

This makes Baron Geddon have a 1% chance to drop Talisman of Binding Shard once per server.

## Example 2: Multiple Bosses With Different Loot

```ini
BossLoot.Enable = 1
BossLoot.RuleCount = 3
BossLoot.ResetOnStartup = 0

# Baron Geddon -> Talisman of Binding Shard, once per server
BossLoot.Rule.1.Enable = 1
BossLoot.Rule.1.NpcEntry = 12056
BossLoot.Rule.1.ItemEntry = 17782
BossLoot.Rule.1.Chance = 1.0
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 1
BossLoot.Rule.1.AllowRepeat = 0
BossLoot.Rule.1.PreventDuplicate = 1
BossLoot.Rule.1.OnceKey = geddon_17782_once
BossLoot.Rule.1.ResetOnStartup = 0
BossLoot.Rule.1.Announce = 1
BossLoot.Rule.1.AnnounceMessage = {player} has looted the legendary {item} from {boss}!

# Onyxia -> repeatable rare item
BossLoot.Rule.2.Enable = 1
BossLoot.Rule.2.NpcEntry = 10184
BossLoot.Rule.2.ItemEntry = 19019
BossLoot.Rule.2.Chance = 0.25
BossLoot.Rule.2.MinCount = 1
BossLoot.Rule.2.MaxCount = 1
BossLoot.Rule.2.AllowRepeat = 1
BossLoot.Rule.2.PreventDuplicate = 1
BossLoot.Rule.2.OnceKey =
BossLoot.Rule.2.ResetOnStartup = 0
BossLoot.Rule.2.Announce = 1
BossLoot.Rule.2.AnnounceMessage = {player} has looted {item} from {boss}!

# Ragnaros -> custom once-per-server item
BossLoot.Rule.3.Enable = 1
BossLoot.Rule.3.NpcEntry = 11502
BossLoot.Rule.3.ItemEntry = 900001
BossLoot.Rule.3.Chance = 5.0
BossLoot.Rule.3.MinCount = 1
BossLoot.Rule.3.MaxCount = 1
BossLoot.Rule.3.AllowRepeat = 0
BossLoot.Rule.3.PreventDuplicate = 1
BossLoot.Rule.3.OnceKey = ragnaros_900001_once
BossLoot.Rule.3.ResetOnStartup = 0
BossLoot.Rule.3.Announce = 1
BossLoot.Rule.3.AnnounceMessage = {player} has looted {item} from {boss}!
```

## Example 3: Multiple Items on the Same Boss

Use the same `NpcEntry` in multiple rules.

```ini
BossLoot.Enable = 1
BossLoot.RuleCount = 2

# Ragnaros custom item 1
BossLoot.Rule.1.Enable = 1
BossLoot.Rule.1.NpcEntry = 11502
BossLoot.Rule.1.ItemEntry = 900001
BossLoot.Rule.1.Chance = 5.0
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 1
BossLoot.Rule.1.AllowRepeat = 0
BossLoot.Rule.1.PreventDuplicate = 1
BossLoot.Rule.1.OnceKey = ragnaros_900001_once
BossLoot.Rule.1.ResetOnStartup = 0
BossLoot.Rule.1.Announce = 1
BossLoot.Rule.1.AnnounceMessage = {player} has looted {item} from {boss}!

# Ragnaros custom item 2
BossLoot.Rule.2.Enable = 1
BossLoot.Rule.2.NpcEntry = 11502
BossLoot.Rule.2.ItemEntry = 900002
BossLoot.Rule.2.Chance = 10.0
BossLoot.Rule.2.MinCount = 1
BossLoot.Rule.2.MaxCount = 1
BossLoot.Rule.2.AllowRepeat = 1
BossLoot.Rule.2.PreventDuplicate = 1
BossLoot.Rule.2.OnceKey =
BossLoot.Rule.2.ResetOnStartup = 0
BossLoot.Rule.2.Announce = 0
BossLoot.Rule.2.AnnounceMessage =
```

## Rule Options

### Enable

Enables or disables this specific loot rule.

```ini
BossLoot.Rule.1.Enable = 1
```

Use `0` to disable the rule without deleting it.

### NpcEntry

The creature template entry of the boss.

```ini
BossLoot.Rule.1.NpcEntry = 12056
```

Example boss entries:

```ini
12056 = Baron Geddon
10184 = Onyxia
11502 = Ragnaros
```

### ItemEntry

The item template entry to add to the boss loot.

```ini
BossLoot.Rule.1.ItemEntry = 17782
```

### Chance

Drop chance as a percentage.

```ini
BossLoot.Rule.1.Chance = 1.0
```

Examples:

```ini
BossLoot.Rule.1.Chance = 100.0
BossLoot.Rule.1.Chance = 25.0
BossLoot.Rule.1.Chance = 5.0
BossLoot.Rule.1.Chance = 1.0
BossLoot.Rule.1.Chance = 0.25
```

### MinCount and MaxCount

Controls how many of the item can drop.

```ini
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 1
```

For a random stack count:

```ini
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 3
```

### AllowRepeat

Controls whether the item can drop more than once.

```ini
BossLoot.Rule.1.AllowRepeat = 1
```

Use `1` for repeatable drops.

Use `0` for once-per-server drops.

```ini
BossLoot.Rule.1.AllowRepeat = 0
```

### PreventDuplicate

Prevents the module from adding the item if the corpse loot already contains it.

```ini
BossLoot.Rule.1.PreventDuplicate = 1
```

Usually this should stay enabled.

### OnceKey

Unique tracking key for once-per-server drops.

```ini
BossLoot.Rule.1.OnceKey = geddon_17782_once
```

This matters only when:

```ini
BossLoot.Rule.1.AllowRepeat = 0
```

Good examples:

```ini
BossLoot.Rule.1.OnceKey = geddon_17782_once
BossLoot.Rule.2.OnceKey = ragnaros_900001_once
BossLoot.Rule.3.OnceKey = illidan_901234_once
```

Do not reuse the same `OnceKey` for unrelated drops unless you deliberately want them to share the same once-per-server lockout.

### ResetOnStartup

Resets this rule's once-per-server state when the worldserver starts.

```ini
BossLoot.Rule.1.ResetOnStartup = 0
```

Normally this should stay `0`.

Use `1` only for testing.

### Announce

Controls whether the module sends a global announcement when the item is looted.

```ini
BossLoot.Rule.1.Announce = 1
```

To disable the announcement:

```ini
BossLoot.Rule.1.Announce = 0
```

### AnnounceMessage

Custom global announcement message.

```ini
BossLoot.Rule.1.AnnounceMessage = {player} has looted {item} from {boss}!
```

Available placeholders:

```ini
{player} = player who looted the item
{boss}   = boss or loot source name
{item}   = item name
{count}  = item count
```

Example:

```ini
BossLoot.Rule.1.AnnounceMessage = {player} has torn {item} from the smoking corpse of {boss}!
```

If `Announce = 0`, the message is ignored.

## Common Patterns

### Repeatable 5% Drop

```ini
BossLoot.Rule.1.Chance = 5.0
BossLoot.Rule.1.AllowRepeat = 1
BossLoot.Rule.1.OnceKey =
```

### Once-Per-Server Legendary Drop

```ini
BossLoot.Rule.1.Chance = 1.0
BossLoot.Rule.1.AllowRepeat = 0
BossLoot.Rule.1.OnceKey = boss_item_once
BossLoot.Rule.1.Announce = 1
```

### Guaranteed Token Drop

```ini
BossLoot.Rule.1.Chance = 100.0
BossLoot.Rule.1.MinCount = 1
BossLoot.Rule.1.MaxCount = 3
BossLoot.Rule.1.AllowRepeat = 1
```

### Silent Drop

```ini
BossLoot.Rule.1.Announce = 0
BossLoot.Rule.1.AnnounceMessage =
```

## Legacy Configuration

The original configuration style is still available for compatibility.

These settings are used only when:

```ini
BossLoot.RuleCount = 0
```

Legacy settings:

```ini
GeddonShard.Enable         = 1
GeddonShard.NpcEntry       = 12056
GeddonShard.Chance         = 1.0
GeddonShard.AllowRepeat    = 0
GeddonShard.ResetOnStartup = 0
```

If `BossLoot.RuleCount` is `1` or higher, the `GeddonShard.*` settings are ignored.

## Installation

1. Place the module in your AzerothCore `modules` directory.

2. Re-run CMake.

3. Rebuild the worldserver.

4. Copy the config file into your server config/modules config location.

5. Edit the config to define your loot rules.

6. Start the worldserver.

The module will create its database table automatically if the world database user has permission to create tables.

## Notes

Once-per-server drops are tracked by `OnceKey`, not by rule number.

This means you can reorder rules later without resetting a drop, as long as the `OnceKey` stays the same.

For example, this key:

```ini
BossLoot.Rule.1.OnceKey = geddon_17782_once
```

should remain the same forever if you want to preserve whether that specific drop already happened.

Changing the `OnceKey` makes the module treat the drop as a new once-per-server drop.
