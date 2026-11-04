# ESP32-S3 Minecraft Server (N16R8 optimized build)

<p align="right">
<b>English</b> &nbsp;|&nbsp; <a href="https://github.com/huang5200075/esp32-s3-minecraft-server/blob/main/README.zh-CN.md">简体中文</a>
</p>

A playable Minecraft **Java Edition** server running on an **ESP32-S3 N16R8**
(16 MB flash + 8 MB PSRAM), with a **TCP management console** for remote
memory / CPU / temperature monitoring and player management over LAN.

> ⚠️ **Please read this first**
>
> This project was **heavily modified with the assistance of AI tools**, including
> code analysis, performance tuning, feature implementation and documentation.
> All changes are **not fully tested** and there are **many known and unknown bugs**
> — see [Known Issues](#known-issues).
> It is a **learning / demonstration project** and is not suitable as a production server.

---

## Origin

This is a **secondary derivative** of the following projects:

| Layer | Project | Notes |
|---|---|---|
| Original core | [p2r3/bareiron](https://github.com/p2r3/bareiron) | From-scratch Minecraft protocol server by p2r3 |
| Direct upstream | [jeyyprtf/esp32-mcserver](https://github.com/jeyyprtf/esp32-mcserver) | Packages bareiron for ESP32-S3 N16R8 with one-file config |

This project continues from **jeyyprtf/esp32-mcserver** and is released under
**GPL-3.0**, inherited from upstream.

**This is NOT an official release by the upstream authors.** Please do not
blame them for problems in this fork.

---

## What this fork adds / changes

### 1. Build fixes (making it actually run on N16R8)

| Problem | Fix |
|---|---|
| Application sources never compiled; linker could not find `app_main` | Renamed `src/` to `main/` — ESP-IDF only links `app_main` from a component named `main` |
| Build aborted on dozens of indentation warnings | Explicitly disabled `-Werror=misleading-indentation` etc. for the app component |
| Component submodules at wrong revisions; `mbedcrypto` target missing | Pinned every submodule to the commit recorded by the superproject |
| Log level too high to see boot output | Set to `INFO` |

### 2. Memory optimization (biggest win)

The original ran with only ~100 KB of internal SRAM free, dipping to **547 bytes**,
which silently broke block changes and item drops.

| Change | Effect |
|---|---|
| Moved `block_changes[]` (120 KB) to PSRAM via `EXT_RAM_BSS_ATTR` | Internal SRAM usage 73.8% → **35.9%** |
| Reduced WiFi buffers 16/64/64 → 8/16/16 | Freed ~25 KB |
| TCP window 32768 → 8192 | Per-connection cost 64 KB → 16 KB |
| `MAX_SOCKETS` 16 → 10 | Lower baseline overhead |

**Result: free internal SRAM 89 KB → 219 KB.**

### 3. Network latency optimization

Stock build had **185 ms average / 393 ms peak** LAN ping. Profiling showed the
cause was the firmware, not the network:

| Problem | Explanation | Fix |
|---|---|---|
| `vTaskDelay(1)` on every packet received | ≥1 ms at 1000 Hz; login involves dozens of round-trips | Replaced with `taskYIELD()` |
| `TCP_NODELAY` never set | Nagle's algorithm held small packets up to 40 ms | Set on listener and every client socket |
| Busy-wait `MSG_PEEK` polling | Burned CPU while idle | Added `select()` |
| `RX_BA_WIN=6` | Receive block-ack window too small | Raised to 8 |

**Result: 15.97 ms average ping, 4.93 ms jitter (~11× improvement).**

### 4. Chunk synchronization rewritten

The original logic drove chunk sending off player movement deltas, with three flaws:

- `if (dx == 0 && dz == 0) break;` — **no chunks at all** while standing still
- `VISITED_HISTORY = 4` — returning to an old position was treated as "already visited"
  and **never re-sent**, leaving the client looking at void
- Sending all chunks at once blocked the main loop for seconds

**Replaced with:** a per-player `(2r+1)²` chunk bitmap, topping up missing chunks
each frame within a 30 ms budget.

### 5. TCP management console (new)

**Port 25566.** No serial adapter needed.

```
$ nc <board-ip> 25566
{"uptime_ms":123456,"heap_free":180000,"psram_free":7000000,
 "cpu_core0":12,"cpu_core1":8,"temp_c":52.3,"players":1}
```

Send a line of text to run a command:

| Command | Description |
|---|---|
| `list` | List online players (slot, name, coords, health) |
| `kick <name\|slot>` | Disconnect a player (client shows "Kicked by console") |
| `say <text>` | Broadcast a chat message |
| `gm <0\|1\|2\|3>` | Change game mode (0 survival, 1 creative, 2 adventure, 3 spectator) |
| `status` | Multi-line status report |
| `help` | Command list |

#### Bundled client

`mc_console.py` is included:

```bash
python3 mc_console.py status --host 192.168.1.100   # one-shot status
python3 mc_console.py watch  --host 192.168.1.100   # refresh every 2s
python3 mc_console.py list                          # online players
python3 mc_console.py kick Steve                    # kick
python3 mc_console.py say Server restarting in 5m   # broadcast
```

**Design note:** console commands never write to player sockets directly —
that races with the main loop and previously kicked players offline.
Commands that touch sockets only set a flag; the main loop performs the write,
guaranteeing serialized access.

---

## Known Issues

> These problems **still exist** in this build. Contributions welcome.

1. **Blocks may render as transparent**
   The chunk packet (Chunk Data and Update Light) length was under-reported by
   1 byte, causing client decode desync and `DecoderException: packet id 39`.
   The length math has been recomputed and fixed, but **has not been verified
   over long sessions** — other packet lengths may still be wrong.

2. **No Nether or End dimensions**
   Only block ID constants exist (`B_netherrack` etc.); there is **no dimension
   switching logic**. The game is therefore **impossible to finish** — no blaze
   rods, no End, no dragon.

3. **Only 5 mob types**
   Chicken, cow, pig, sheep, zombie. No creeper, enderman, skeleton, etc.

4. **No enchanting / brewing / redstone**
   Those blocks exist as IDs only, with no behavior.

5. **TNT cannot be ignited**
   TNT is a decorative block. No explosion, chain reaction or damage.

6. **No structures** (fortresses, villages, etc.)

7. **No authentication**
   Any username may join. **Never expose this to the internet** — LAN only.

8. **Fragile save compatibility**
   Changing `MAX_BLOCK_CHANGES` makes existing `world.bin` unreadable;
   the flash partition must be erased.

9. **High CPU usage**
   Even after raising the `select()` timeout and enabling run-time stats,
   CPU core 1 may sit near 100%.

---

## Requirements

**Hardware**
- **ESP32-S3 N16R8** (16 MB flash + 8 MB *Octal* PSRAM). Other variants may
  fail to boot due to insufficient memory.
- USB **data** cable
- **2.4 GHz** WiFi (ESP32 cannot use 5 GHz)

**Client**
- Minecraft **Java Edition 1.21.8** (protocol 772). Other versions will not connect.
- Bedrock Edition (mobile / console) is **not** supported.

---

## Building

Requires **ESP-IDF v5.4** (developed against v5.4.4).

```bash
# 1. Generate registries (needs Java 21+ and Node.js, once only)
mkdir -p notchian
# place the 1.21.8 server.jar into notchian/
./extract_registries.sh

# 2. Configure WiFi
cp include/config.h.example include/config.h
# edit include/config.h and set SSID / password

# 3. Build
idf.py set-target esp32s3
idf.py build
```

> Submodules must be pinned to the revisions recorded by the superproject,
> otherwise the build fails with `mbedcrypto not built`.

## Flashing

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 build/esp32-mcserver.bin
```

Or build a single merged image (bootloader + partition table + app):

```bash
esptool.py --chip esp32s3 merge_bin -o full.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32-mcserver.bin

esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 full.bin
```

> **Flash mode must be DIO.** Some N16R8 boards are wired for two-line SPI;
> using QIO makes the bootloader fail to read flash and loop on the RTC watchdog.

## Usage

1. After flashing, power cycle and watch the serial console (115200 baud):
   ```
   I (xxxx) esp_netif_handlers: sta ip: 192.168.1.xxx
   Server listening on port 25565...
   ```
   Note the IP address.

2. On the client, join the same 2.4 GHz network, add a server, enter that IP.

3. The first join takes several seconds while chunks are generated.

### In-game commands

| Command | Description |
|---|---|
| `!gm 0~3` | Change game mode |
| `!tp <x> <y> <z>` | Teleport |
| `!give <item>` | Give item |
| `!time` | Set time |
| `!help` | List all commands |

---

## Layout

```
├── main/              Application code (ESP-IDF requires the name "main")
│   ├── main.c         Main loop, networking, chunk sync
│   ├── monitor.c      TCP management console (added by this fork)
│   ├── packets.c      Protocol encode/decode
│   ├── procedures.c   Game logic
│   ├── worldgen.c     Terrain generation
│   └── serialize.c    Save/load
├── include/
│   ├── config.h.example  Configuration template
│   └── globals.h         Global definitions
├── mc_console.py      Local console client
├── partitions.csv     Partition table
└── sdkconfig.defaults Default build configuration
```

---

## License

**GPL-3.0**, inherited from upstream.

Copyright of the original work belongs to p2r3 (bareiron) and jeyyprtf
(esp32-mcserver). Modifications in this fork are released under the same license.

---

## Credits

- [p2r3/bareiron](https://github.com/p2r3/bareiron) — from-scratch Minecraft protocol core
- [jeyyprtf/esp32-mcserver](https://github.com/jeyyprtf/esp32-mcserver) — packaging and build guide
- Espressif — ESP-IDF and hardware support

---

**Reminder: this project was modified with AI assistance and contains many
unfixed issues. Use it for learning and experimentation only.**
