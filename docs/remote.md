# Remote commands

The plugin registers these game commands (`src/dll/remote/RemoteCommands.cpp`). They work in the
cheat box (no reply shown), in Lua test scripts (`-LuaScript`) and over the game's command socket.
The socket is off by default: start the game with `-NetCommandGenerator:enabled`, then connect to
port 50020, send one command with a newline (`SeamlessStatus\n`) and read the reply until the game
closes the connection. One command per connection, and always send the newline (see
[sc4-game-command-documentation](https://github.com/0xC0000054/sc4-game-command-documentation)).
The socket accepts connections from the network: block SimCity 4 in the firewall if needed.

| Command | Does |
|---|---|
| `SeamlessStatus` | One line: view, city, region render state (`done`, `failed`, `left`, current city, phase, tiles), city render phase and ETA, date, zoom |
| `SeamlessRenderRegion [force] [transport]` | Starts the region render (as the Batch button, without its confirmation). `force` renders the cities that are up to date too. `transport` adds the transport layer to the main renders that lack it, keeping their normal levels. In the region view it renders every city and ends there |
| `SeamlessResumeRegion` | Continues the unfinished region render of `batch.json` (after a crash or a quit), as the panel's Continue button |
| `SeamlessRenderCity` | Renders the open city |
| `SeamlessCancel` | Stops the region render after its current step, or the city render |
| `SeamlessSet <key> <value>` | Sets one INI setting for this session, with the INI's rules (`SeamlessSet CaptureAtDate true`) |
| `SeamlessGet <key>` | Reads one setting |
| `SeamlessSaveSettings` | Writes the settings to the INI |

The game's own commands do the rest, for example `LoadRegion <name>`, `GetAppState` (1 region view,
2 city view) and `QuitGame true`.

A summer render of a whole region, from the region view:

```
LoadRegion Kanto Tokai
SeamlessSet CaptureAtDate true
SeamlessSet CaptureMonth 7
SeamlessSet CaptureDay 15
SeamlessRenderRegion force
SeamlessStatus            (repeat until batch=idle)
```
