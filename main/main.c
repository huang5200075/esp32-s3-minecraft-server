#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif

#ifdef ESP_PLATFORM
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "nvs_flash.h"
  #include "esp_wifi.h"
  #include "esp_netif.h"
  #include "esp_event.h"
  #include "esp_timer.h"
  #include "esp_log.h"
  #include "esp_wifi_portal.h"
  #include "lwip/sockets.h"
  #include "lwip/netdb.h"
#else
  #include <sys/types.h>
  #ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
  #else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
  #endif
  #include <unistd.h>
  #include <time.h>
#endif

#include "globals.h"
#include "tools.h"
#include "monitor.h"
#include "varnum.h"
#include "packets.h"
#include "worldgen.h"
#include "registries.h"
#include "procedures.h"
#include "serialize.h"

/**
 * Routes an incoming packet to its packet handler or procedure.
 *
 * Full disclosure, I think this whole thing is a bit of a mess.
 * The packet handlers started out as having proper error checks and
 * handling, but that turned out to be very tedious and space/time
 * consuming, and didn't really help with resolving errors. Not to mention
 * that all those checks likely compound into a non-negligible performance
 * hit on embedded systems.
 *
 * I think the way forward would be to gut the return values of the packet
 * handlers, as most of them only ever return 0, and others aren't checked
 * here. The length discrepancy checks at the bottom already do a good job
 * at preventing this from derailing completely in case of a bad packet,
 * and I think leaning into those is fine.
 *
 * In other words, I think the sc_/cs_ handlers should be of type `void`,
 * and should simply return early when there's a failure that prevents the
 * server from handling a packet. Any data that's left unhandled/unread
 * will be caught by the length discrepancy checks. That's more or less
 * how it already works, just not explicitly.
 *
 * Why have I not done this yet? Well, I'm close to uploading the video,
 * and I don't want to risk refactoring anything this close to release.
 */
void handlePacket (int client_fd, int length, int packet_id, int state) {

  // Count the amount of bytes received to catch length discrepancies
  uint64_t bytes_received_start = total_bytes_received;

  switch (packet_id) {

    case 0x00:
      if (state == STATE_NONE) {
        if (cs_handshake(client_fd)) break;
      } else if (state == STATE_STATUS) {
        if (sc_statusResponse(client_fd)) break;
      } if (state == STATE_LOGIN) {
        uint8_t uuid[16];
        char name[16];
        if (cs_loginStart(client_fd, uuid, name)) break;
        if (reservePlayerData(client_fd, uuid, name)) {
          recv_count = 0;
          return;
        }
        if (sc_loginSuccess(client_fd, uuid, name)) break;
      } else if (state == STATE_CONFIGURATION) {
        if (cs_clientInformation(client_fd)) break;
        if (sc_knownPacks(client_fd)) break;
        if (sc_registries(client_fd)) break;

        #ifdef SEND_BRAND
        if (sc_sendPluginMessage(client_fd, "minecraft:brand", (uint8_t *)brand, brand_len)) break;
        #endif
      }
      break;

    case 0x01:
      // Handle status ping
      if (state == STATE_STATUS) {
        // No need for a packet handler, just echo back the long verbatim
        writeByte(client_fd, 9);
        writeByte(client_fd, 0x01);
        writeUint64(client_fd, readUint64(client_fd));
        // Close connection after this
        recv_count = 0;
        return;
      }
      break;

    case 0x02:
      if (state == STATE_CONFIGURATION) cs_pluginMessage(client_fd);
      break;

    case 0x03:
      if (state == STATE_LOGIN) {
        LOGV("Client Acknowledged Login\n\n");
        setClientState(client_fd, STATE_CONFIGURATION);
      } else if (state == STATE_CONFIGURATION) {
        LOGV("Client Acknowledged Configuration\n\n");

        // Enter client into "play" state
        setClientState(client_fd, STATE_PLAY);
        sc_loginPlay(client_fd);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        // Send full client spawn sequence
        spawnPlayer(player);

        // Register all existing players and spawn their entities
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          // Note that this will also filter out the joining player
          if (player_data[i].flags & 0x20) continue;
          sc_playerInfoUpdateAddPlayer(client_fd, player_data[i]);
          sc_spawnEntityPlayer(client_fd, player_data[i]);
        }

        // Send information about all other entities (mobs):
        // Use a random number for the first half of the UUID
        uint8_t uuid[16];
        uint32_t r = fast_rand();
        memcpy(uuid, &r, 4);
        // Send allocated living mobs, use ID for second half of UUID
        for (int i = 0; i < MAX_MOBS; i ++) {
          if (mob_data[i].type == 0) continue;
          if ((mob_data[i].data & 31) == 0) continue;
          memcpy(uuid + 4, &i, 4);
          // For more info on the arguments here, see the spawnMob function
          sc_spawnEntity(
            client_fd, -2 - i, uuid,
            mob_data[i].type, mob_data[i].x, mob_data[i].y, mob_data[i].z,
            0, 0
          );
          broadcastMobMetadata(client_fd, -2 - i);
        }

      }
      break;

    case 0x07:
      if (state == STATE_CONFIGURATION) {
        LOGV("Received Client's Known Packs\n");
        LOGV("  Finishing configuration\n\n");
        sc_finishConfiguration(client_fd);
      }
      break;

    case 0x08:
      if (state == STATE_PLAY) cs_chat(client_fd);
      break;

    case 0x0B:
      if (state == STATE_PLAY) cs_clientStatus(client_fd);
      break;

    case 0x0C: // Client tick (ignored)
      break;

    case 0x11:
      if (state == STATE_PLAY) cs_clickContainer(client_fd);
      break;

    case 0x12:
      if (state == STATE_PLAY) cs_closeContainer(client_fd);
      break;

    case 0x1B:
      if (state == STATE_PLAY) {
        // Serverbound keep-alive (ignored)
        discard_all(client_fd, length, false);
      }
      break;

    case 0x19:
      if (state == STATE_PLAY) cs_interact(client_fd);
      break;

    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
      if (state == STATE_PLAY) {

        double x, y, z;
        float yaw, pitch;
        uint8_t on_ground;

        // Read player position (and rotation)
        if (packet_id == 0x1D) cs_setPlayerPosition(client_fd, &x, &y, &z, &on_ground);
        else if (packet_id == 0x1F) cs_setPlayerRotation (client_fd, &yaw, &pitch, &on_ground);
        else if (packet_id == 0x20) cs_setPlayerMovementFlags (client_fd, &on_ground);
        else cs_setPlayerPositionAndRotation(client_fd, &x, &y, &z, &yaw, &pitch, &on_ground);

        PlayerData *player;
        if (getPlayerData(client_fd, &player)) break;

        uint8_t block_feet = getBlockAt(player->x, player->y, player->z);
        uint8_t swimming = block_feet >= B_water && block_feet < B_water + 8;

        // Handle fall damage
        if (on_ground) {
          int16_t damage = player->grounded_y - player->y - 3;
          if (damage > 0 && (GAMEMODE == 0 || GAMEMODE == 2) && !swimming) {
            hurtEntity(client_fd, -1, D_fall, damage);
          }
          player->grounded_y = player->y;
        } else if (swimming) {
          player->grounded_y = player->y;
        }

        // Don't continue if all we got were flags
        if (packet_id == 0x20) break;

        // Update rotation in player data (if applicable)
        if (packet_id != 0x1D) {
          player->yaw = ((short)(yaw + 540) % 360 - 180) * 127 / 180;
          player->pitch = pitch / 90.0f * 127.0f;
        }

        // Whether to broadcast player position to other players
        uint8_t should_broadcast = true;

        #ifndef BROADCAST_ALL_MOVEMENT
          // If applicable, tie movement updates to the tickrate by using
          // a flag that gets reset on every tick. It might sound better
          // to just make the tick handler broadcast position updates, but
          // then we lose precision. While position is stored using integers,
          // here the client gives us doubles and floats directly.
          should_broadcast = !(player->flags & 0x40);
          if (should_broadcast) player->flags |= 0x40;
        #endif

        #ifdef SCALE_MOVEMENT_UPDATES_TO_PLAYER_COUNT
          // If applicable, broadcast only every client_count-th movement update
          if (++player->packets_since_update < client_count) {
            should_broadcast = false;
          } else {
            // Note that this does not explicitly set should_broadcast to true
            // This allows the above BROADCAST_ALL_MOVEMENT check to compound
            // Whether that's ever favorable is up for debate
            player->packets_since_update = 0;
          }
        #endif

        if (should_broadcast) {
          // If the packet had no rotation data, calculate it from player data
          if (packet_id == 0x1D) {
            yaw = player->yaw * 180 / 127;
            pitch = player->pitch * 90 / 127;
          }
          // Send current position data to all connected players
          for (int i = 0; i < MAX_PLAYERS; i ++) {
            if (player_data[i].client_fd == -1) continue;
            if (player_data[i].flags & 0x20) continue;
            if (player_data[i].client_fd == client_fd) continue;
            if (packet_id == 0x1F) {
              sc_updateEntityRotation(player_data[i].client_fd, client_fd, player->yaw, player->pitch);
            } else {
              sc_teleportEntity(player_data[i].client_fd, client_fd, x, y, z, yaw, pitch);
            }
            sc_setHeadRotation(player_data[i].client_fd, client_fd, player->yaw);
          }
        }

        // Don't continue if all we got was rotation data
        if (packet_id == 0x1F) break;

        // Players send movement packets roughly 20 times per second when
        // moving, and much less frequently when standing still. We can
        // use this correlation between actions and packet count to cheaply
        // simulate hunger with a timer-based system, where the timer ticks
        // down with each position packet. The timer value itself then
        // naturally works as a substitute for saturation.
        if (player->saturation == 0) {
          if (player->hunger > 0) player->hunger--;
          player->saturation = 200;
          sc_setHealth(client_fd, player->health, player->hunger, player->saturation);
        } else if (player->flags & 0x08) {
          player->saturation -= 1;
        }

        // Cast the values to short to get integer position
        short cx = x, cy = y, cz = z;
        if (x < 0) cx -= 1;
        if (z < 0) cz -= 1;
        // Determine the player's chunk coordinates
        short _x = (cx < 0 ? cx - 16 : cx) / 16, _z = (cz < 0 ? cz - 16 : cz) / 16;
        // Calculate distance between previous and current chunk coordinates
        short dx = _x - (player->x < 0 ? player->x - 16 : player->x) / 16;
        short dz = _z - (player->z < 0 ? player->z - 16 : player->z) / 16;

        // Prevent players from leaving the world
        if (cy < 0) {
          cy = 0;
          player->grounded_y = 0;
          sc_synchronizePlayerPosition(client_fd, cx, 0, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        } else if (cy > 255) {
          cy = 255;
          sc_synchronizePlayerPosition(client_fd, cx, 255, cz, player->yaw * 180 / 127, player->pitch * 90 / 127);
        }

        // Update position in player data
        player->x = cx;
        player->y = cy;
        player->z = cz;

        // Exit early if no chunk borders were crossed
        // [v8] 原来这里直接 break，导致玩家站着不动时永远走不到下面的区块同步逻辑，
        // 结果只加载进服那一瞬间的几个区块。现在改为：未跨区块时跳过"怪物生成"，
        // 但仍然继续执行区块同步。
        int crossed_chunk_border = !(dx == 0 && dz == 0);

        // Check if the player has recently been in this chunk
        int found = false;
        for (int i = 0; i < VISITED_HISTORY; i ++) {
          if (player->visited_x[i] == _x && player->visited_z[i] == _z) {
            found = true;
            break;
          }
        }
        // [v8] 原来这里 break，会连带跳过下面的区块同步。
        // 只跳过"怪物生成"部分，区块补齐继续执行。
        int should_spawn_mob = crossed_chunk_border && !found;

        if (should_spawn_mob) {
          // Update player's recently visited chunks
          for (int i = 0; i < VISITED_HISTORY - 1; i ++) {
            player->visited_x[i] = player->visited_x[i + 1];
            player->visited_z[i] = player->visited_z[i + 1];
          }
          player->visited_x[VISITED_HISTORY - 1] = _x;
          player->visited_z[VISITED_HISTORY - 1] = _z;

          uint32_t r = fast_rand();
          // One in every 4 new chunks spawns a mob
          if ((r & 3) == 0) {
          // The mob is placed in the middle of the new chunk row,
          // at a random position within the chunk
          short mob_x = (_x + dx * VIEW_DISTANCE) * 16 + ((r >> 4) & 15);
          short mob_z = (_z + dz * VIEW_DISTANCE) * 16 + ((r >> 8) & 15);
          // Start at the Y coordinate of the spawning player and move upward
          // until a valid space is found
          uint8_t mob_y = cy - 8;
          uint8_t b_low = getBlockAt(mob_x, mob_y - 1, mob_z);
          uint8_t b_mid = getBlockAt(mob_x, mob_y, mob_z);
          uint8_t b_top = getBlockAt(mob_x, mob_y + 1, mob_z);
          while (mob_y < 255) {
            if ( // Solid block below, non-solid(spawnable) at feet and above
              !isPassableBlock(b_low) &&
              isPassableSpawnBlock(b_mid) &&
              isPassableSpawnBlock(b_top)
            ) break;
            b_low = b_mid;
            b_mid = b_top;
            b_top = getBlockAt(mob_x, mob_y + 2, mob_z);
            mob_y ++;
          }
          if (mob_y != 255) {
            // Spawn passive mobs above ground during the day,
            // or hostiles underground and during the night
            if ((world_time < 13000 || world_time > 23460) && mob_y > 48) {
              uint32_t mob_choice = (r >> 12) & 3;
              if (mob_choice == 0) spawnMob(25, mob_x, mob_y, mob_z, 4); // Chicken
              else if (mob_choice == 1) spawnMob(28, mob_x, mob_y, mob_z, 10); // Cow
              else if (mob_choice == 2) spawnMob(95, mob_x, mob_y, mob_z, 10); // Pig
              else if (mob_choice == 3) spawnMob(106, mob_x, mob_y, mob_z, 8); // Sheep
            } else {
              spawnMob(145, mob_x, mob_y, mob_z, 20); // Zombie
            }
          }
          }   // end if ((r & 3) == 0)
        }     // end if (should_spawn_mob)

        // [v8] 区块同步：不再用"移动增量 delta"驱动。
        // 原逻辑 while (dx != 0) 只在跨区块边界时发送，站着不动一个区块都不发，
        // 走回旧位置也不补发（VISITED_HISTORY 判定"来过"），导致大面积虚空。
        // 新逻辑：以玩家区块为中心建立 (2r+1)^2 网格位图，每帧补发缺失的区块，
        // 带时间预算，不会长时间阻塞主循环。
        if (player->chunk_center_x != _x || player->chunk_center_z != _z) {
          // 中心变了，位图作废重新同步
          player->chunk_center_x = _x;
          player->chunk_center_z = _z;
          for (int w = 0; w < CHUNK_BITMAP_WORDS; w ++) player->chunk_bits[w] = 0;
          player->chunk_sync_pending = 1;
          player->chunks_sent_total = 0;
          sc_setCenterChunk(client_fd, _x, _z);
        }
        (void)dx; (void)dz;

      }
      break;

    case 0x29:
      if (state == STATE_PLAY) cs_playerCommand(client_fd);
      break;

    case 0x2A:
      if (state == STATE_PLAY) cs_playerInput(client_fd);
      break;

    case 0x2B:
      if (state == STATE_PLAY) cs_playerLoaded(client_fd);
      break;

    case 0x34:
      if (state == STATE_PLAY) cs_setHeldItem(client_fd);
      break;
	
    case 0x3C:
      if (state == STATE_PLAY) cs_swingArm(client_fd);
      break;

    case 0x28:
      if (state == STATE_PLAY) cs_playerAction(client_fd);
      break;

    case 0x3F:
      if (state == STATE_PLAY) cs_useItemOn(client_fd);
      break;

    case 0x40:
      if (state == STATE_PLAY) cs_useItem(client_fd);
      break;

    default:
      #ifdef DEV_LOG_UNKNOWN_PACKETS
        printf("Unknown packet: 0x");
        if (packet_id < 16) printf("0");
        printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
      #endif
      discard_all(client_fd, length, false);
      break;

  }

  // Detect and fix incorrectly parsed packets
  int processed_length = total_bytes_received - bytes_received_start;
  if (processed_length == length) return;

  if (length > processed_length) {
    discard_all(client_fd, length - processed_length, false);
  }

  #ifdef DEV_LOG_LENGTH_DISCREPANCY
  if (processed_length != 0) {
    printf("WARNING: Packet 0x");
    if (packet_id < 16) printf("0");
    printf("%X parsed incorrectly!\n  Expected: %d, parsed: %d\n\n", packet_id, length, processed_length);
  }
  #endif
  #ifdef DEV_LOG_UNKNOWN_PACKETS
  if (processed_length == 0) {
    printf("Unknown packet: 0x");
    if (packet_id < 16) printf("0");
    printf("%X, length: %d, state: %d\n\n", packet_id, length, state);
  }
  #endif

}

int main () {
  #ifdef _WIN32 //initialize windows socket
    WSADATA wsa;
      if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        exit(EXIT_FAILURE);
      }
  #endif

  // Hash the seeds to ensure they're random enough
  world_seed = splitmix64(world_seed);
  printf("World seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((world_seed >> (8 * i)) & 255));

  rng_seed = splitmix64(rng_seed);
  printf("\nRNG seed (hashed): ");
  for (int i = 3; i >= 0; i --) printf("%X", (unsigned int)((rng_seed >> (8 * i)) & 255));
  printf("\n\n");

  // Initialize block changes entries as unallocated
  for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
    block_changes[i].block = 0xFF;
  }

  // Start the disk/flash serializer (if applicable)
  if (initSerializer()) exit(EXIT_FAILURE);

  // Initialize all file descriptor references to -1 (unallocated)
  int clients[MAX_PLAYERS], client_index = 0;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    clients[i] = -1;
    client_states[i * 2] = -1;
    player_data[i].client_fd = -1;
  }

  // Create server TCP socket
  int server_fd, opt = 1;
  struct sockaddr_in server_addr, client_addr;
  socklen_t addr_len = sizeof(client_addr);

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    perror("socket failed");
    exit(EXIT_FAILURE);
  }
#ifdef _WIN32
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
      (const char*)&opt, sizeof(opt)) < 0) {
#else
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
#endif    
    perror("socket options failed");
    exit(EXIT_FAILURE);
  }

  // [OPT-A] 关闭 Nagle 算法。MC 协议全是小包，Nagle 会攒包等 ACK，
  // 单个包最多被延迟 40ms，几十个握手包叠加就是秒级卡顿。
  int nodelay = 1;
  setsockopt(server_fd, IPPROTO_TCP, TCP_NODELAY,
             (const char*)&nodelay, sizeof(nodelay));

  // Bind socket to IP/port
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(PORT);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    perror("bind failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }

  // Listen for incoming connections
  if (listen(server_fd, 5) < 0) {
    perror("listen failed");
    close(server_fd);
    exit(EXIT_FAILURE);
  }
  printf("Server listening on port %d...\n", PORT);

  // Make the socket non-blocking
  // This is necessary to not starve the idle task during slow connections
  #ifdef _WIN32
    u_long mode = 1;  // 1 = non-blocking
    if (ioctlsocket(server_fd, FIONBIO, &mode) != 0) {
      fprintf(stderr, "Failed to set non-blocking mode\n");
      exit(EXIT_FAILURE);
    }
  #else
  int flags = fcntl(server_fd, F_GETFL, 0);
  fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
  #endif

  // Track time of last server tick (in microseconds)
  int64_t last_tick_time = get_program_time();

  /**
   * Cycles through all connected clients, handling one packet at a time
   * from each player. With every iteration, attempts to accept a new
   * client connection.
   */
  while (true) {
    // Check if it's time to yield to the idle task
    task_yield();

    // [OPT-C] 用 select() 等待数据，取代忙等 MSG_PEEK 轮询。
    // 原来每轮对每个客户端都做一次 recv(MSG_PEEK)，无数据时纯空转烧 CPU；
    // 现在挂起等待，有数据立刻返回，最长只等 5ms 以便处理新连接和 tick。
    // 这样 CPU 大幅空闲，网络响应也更及时。
    {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(server_fd, &readfds);
      int maxfd = server_fd;
      for (int i = 0; i < MAX_PLAYERS; i ++) {
        if (clients[i] == -1) continue;
        FD_SET(clients[i], &readfds);
        if (clients[i] > maxfd) maxfd = clients[i];
      }
      // 200us 超时：足够新包进入，又不至于空转烧 CPU
      struct timeval tv;
      tv.tv_sec  = 0;
      // [v8] 原来是 200 微秒：没数据时几乎立刻返回，主循环每秒空转上千次，
      // 实测 CPU 核1 长期 100%，挤占了世界生成和包处理的算力。
      // 改成 5ms：有数据时 select 立即返回（不影响延迟），
      // 没数据时才真正让出 CPU。
      tv.tv_usec = 5000;
      select(maxfd + 1, &readfds, NULL, NULL, &tv);
    }

    // Attempt to accept a new connection
    for (int i = 0; i < MAX_PLAYERS; i ++) {
      if (clients[i] != -1) continue;
      clients[i] = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
      // If the accept was successful, make the client non-blocking too
      if (clients[i] != -1) {
        LOGV("New client, fd: %d\n", clients[i]);
      #ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(clients[i], FIONBIO, &mode);
      #else
        int flags = fcntl(clients[i], F_GETFL, 0);
        fcntl(clients[i], F_SETFL, flags | O_NONBLOCK);
      #endif
        // [OPT-A] 每个客户端连接同样关闭 Nagle
        {
          int nd = 1;
          setsockopt(clients[i], IPPROTO_TCP, TCP_NODELAY,
                     (const char*)&nd, sizeof(nd));
        }
        client_count ++;
      }
      break;
    }

    // Look for valid connected clients
    client_index ++;
    if (client_index == MAX_PLAYERS) client_index = 0;

    // [v19] 关键修复：原来下一行是
    //     if (clients[client_index] == -1) continue;
    // 它会跳过后面的「控制台命令 / 区块同步 / 服务器 tick / 包处理」。
    // 玩家只占 16 个槽位中的 1 个时，87% 的循环轮次什么都没做：
    //   - Keep-Alive 每秒只发约 1/16 次 -> 客户端断电 30 秒才发现断线
    //   - 摔落/岩浆等 tick 伤害几乎不生效
    //   - 方块改动与物品发放被严重拖慢
    // 现在把 tick 和控制台命令移到 continue 之前，保证每轮都执行。

    // Handle periodic events (server ticks)
    {
      int64_t time_since_last_tick = get_program_time() - last_tick_time;
      if (time_since_last_tick > TIME_BETWEEN_TICKS) {
        handleServerTick(time_since_last_tick);
        last_tick_time = get_program_time();
      }
    }

    // 执行控制台排队的命令（必须在主循环里做，保证串行访问 socket）
    if (console_pending) {
      uint8_t pend = console_pending;
      console_pending = 0;

      if (pend & 0x01) {            // 切换游戏模式
        gamemode = console_pending_gm;
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          sc_setGamemode(player_data[i].client_fd, gamemode);
        }
      }
      if (pend & 0x02) {            // 全服广播
        uint8_t len = (uint8_t)strlen((char *)console_pending_text);
        for (int i = 0; i < MAX_PLAYERS; i ++) {
          if (player_data[i].client_fd == -1) continue;
          sc_systemChat(player_data[i].client_fd, (char *)console_pending_text, len);
        }
      }
      if (pend & 0x04) {            // 踢出玩家
        int8_t k = console_pending_kick_idx;
        console_pending_kick_idx = -1;
        if (k >= 0 && k < MAX_PLAYERS && player_data[k].client_fd != -1) {
          int dead_fd = player_data[k].client_fd;
          sc_disconnectPlay(dead_fd, "Kicked by console");
          disconnectClient(&player_data[k].client_fd, 99);
          // [v8] clients[] 是主循环 select/recv 用的独立数组，下标与 player_data[]
          // 并不是一一对应的（前者按 accept 顺序，后者按 UUID 分配），
          // 所以必须按 fd 值找到对应槽位再清理，否则会留下已 close 的僵尸 fd，
          // 表现为踢人后重连"连接已丢失"。
          for (int ci = 0; ci < MAX_PLAYERS; ci ++) {
            if (clients[ci] == dead_fd) clients[ci] = -1;
          }
          player_data[k].flags = 0;
        }
      }
    }

    // [v8] 每帧补发缺失的区块（带时间预算，避免阻塞主循环）
    // 放在 tick 之后、处理本客户端之前，保证新玩家一进来就能开始加载。
    for (int pi = 0; pi < MAX_PLAYERS; pi ++) {
      PlayerData *pp = &player_data[pi];
      if (pp->client_fd == -1 || !pp->chunk_sync_pending) continue;

      int64_t budget_start = get_program_time();
      const int r = VIEW_DISTANCE;
      const int w = 2 * r + 1;
      int all_done = 1;

      for (int gz = 0; gz < w; gz ++) {
        for (int gx = 0; gx < w; gx ++) {
          int bit = gz * w + gx;
          if (pp->chunk_bits[bit >> 5] & (1u << (bit & 31))) continue;

          // 这一格还没发
          all_done = 0;

          // 时间预算：每帧最多用 30ms 发区块
          if (get_program_time() - budget_start > 30000) goto next_player;

          short cx = pp->chunk_center_x + (gx - r);
          short cz = pp->chunk_center_z + (gz - r);
          sc_chunkDataAndUpdateLight(pp->client_fd, cx, cz);
          pp->chunk_bits[bit >> 5] |= (1u << (bit & 31));
          pp->chunks_sent_total ++;
        }
      }

      if (all_done) pp->chunk_sync_pending = 0;
    next_player: ;
    }

    // [v19] 到这里才判断当前槽位是否有客户端要处理数据。
    // 上面的 tick / 控制台命令 / 区块同步每轮都会执行，不受这里影响。
    if (clients[client_index] == -1) continue;

    // Handle this individual client
    int client_fd = clients[client_index];

    // Check if at least 2 bytes are available for reading
    #ifdef _WIN32
    recv_count = recv(client_fd, recv_buffer, 2, MSG_PEEK);
    if (recv_count == 0) {
      disconnectClient(&clients[client_index], 1);
      continue;
    }
    if (recv_count == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAEWOULDBLOCK) {
        continue; // no data yet, keep client alive
      } else {
        disconnectClient(&clients[client_index], 1);
        continue;
      }
    }
    #else
    recv_count = recv(client_fd, &recv_buffer, 2, MSG_PEEK);
    if (recv_count < 2) {
      if (recv_count == 0 || (recv_count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        disconnectClient(&clients[client_index], 1);
      }
      continue;
    }
    #endif
    // Handle 0xBEEF and 0xFEED packets for dumping/uploading world data
    #ifdef DEV_ENABLE_BEEF_DUMPS
    // Received BEEF packet, dump world data and disconnect
    if (recv_buffer[0] == 0xBE && recv_buffer[1] == 0xEF && getClientState(client_fd) == STATE_NONE) {
      // Send block changes and player data back to back
      // The client is expected to know (or calculate) the size of these buffers
      send_all(client_fd, block_changes, sizeof(block_changes));
      send_all(client_fd, player_data, sizeof(player_data));
      // Flush the socket and receive everything left on the wire
      shutdown(client_fd, SHUT_WR);
      recv_all(client_fd, recv_buffer, sizeof(recv_buffer), false);
      // Kick the client
      disconnectClient(&clients[client_index], 6);
      continue;
    }
    // Received FEED packet, load world data from socket and disconnect
    if (recv_buffer[0] == 0xFE && recv_buffer[1] == 0xED && getClientState(client_fd) == STATE_NONE) {
      // Consume 0xFEED bytes (previous read was just a peek)
      recv_all(client_fd, recv_buffer, 2, false);
      // Write full buffers straight into memory
      recv_all(client_fd, block_changes, sizeof(block_changes), false);
      recv_all(client_fd, player_data, sizeof(player_data), false);
      // Recover block_changes_count
      for (int i = 0; i < MAX_BLOCK_CHANGES; i ++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].block == B_chest) i += 14;
        if (i >= block_changes_count) block_changes_count = i + 1;
      }
      // Update data on disk
      writeBlockChangesToDisk(0, block_changes_count);
      writePlayerDataToDisk();
      // Kick the client
      disconnectClient(&clients[client_index], 7);
      continue;
    }
    #endif

    // Read packet length
    int length = readVarInt(client_fd);
    if (length == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 2);
      continue;
    }
    // Read packet ID
    int packet_id = readVarInt(client_fd);
    if (packet_id == VARNUM_ERROR) {
      disconnectClient(&clients[client_index], 3);
      continue;
    }
    // Get client connection state
    int state = getClientState(client_fd);
    // Disconnect on legacy server list ping
    if (state == STATE_NONE && length == 254 && packet_id == 122) {
      disconnectClient(&clients[client_index], 5);
      continue;
    }
    // Handle packet data
    handlePacket(client_fd, length - sizeVarInt(packet_id), packet_id, state);
    if (recv_count == 0 || (recv_count == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
      disconnectClient(&clients[client_index], 4);
      continue;
    }

  }

  close(server_fd);
 
  #ifdef _WIN32 //cleanup windows socket
    WSACleanup();
  #endif

  printf("Server closed.\n");

}

#ifdef ESP_PLATFORM

void bareiron_main (void *pvParameters) {
  main();
  vTaskDelete(NULL);
}

static void wifi_event_handler (void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  // [v10] STA 连接/断开的重连逻辑交给 esp_wifi_portal 组件处理。
  // [v15] 关键：只有真正拿到 IP（= 连上 WiFi）之后，才启动 MC 服务器和控制台。
  // 配网阶段（只有 softAP、没有 STA 连接）完全不启动这些服务，
  // 把 socket / CPU / 内存全部留给配网页，避免抢占导致页面打不开。
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    static int started = 0;
    if (started) return;
    started = 1;

    const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)event_data;
    printf("Got IP: " IPSTR ", starting server...\n\n", IP2STR(&ev->ip_info.ip));

    xTaskCreate(bareiron_main, "bareiron", 4096, NULL, 5, NULL);
    monitor_start();
  }
}

void wifi_init () {
  // [v11] NVS 必须正确处理初始化失败：
  //  - 分区满 / 版本不匹配  -> 擦除后重新初始化（旧 WiFi 配置会丢失，从头配网）
  //  - 已经初始化过        -> 忽略 ESP_ERR_INVALID_STATE
  // 原来直接调用不检查返回值，一旦出错 esp_wifi_init() 拿不到 NVS 就会崩溃。
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW("wifi", "NVS needs erase (%s), erasing...", esp_err_to_name(err));
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE("wifi", "nvs_flash_init failed: %s", esp_err_to_name(err));
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  // [v10] 接入 esp_wifi_portal 组件（第三方开源，Apache-2.0）：
  //   - NVS 里已有 WiFi 配置 -> 直连，连上后启动 MC 服务器
  //   - 没有配置 / 连不上   -> 自动开启热点，手机连上后弹配网页面
  //   - 配网失败或断线      -> 自动重开热点
  // 不再需要在 config.h 里硬编码 SSID / 密码。
  ESP_ERROR_CHECK(esp_wifi_portal_init());

  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                      &wifi_event_handler, NULL, NULL);
}

void app_main () {
  esp_timer_early_init();
  wifi_init();
  // [v15] 控制台/监控服务改为"连上 WiFi 后才启动"（见 wifi_event_handler）。
  // 配网阶段不占用 socket、CPU 和内存，把资源都留给配网页。
}

// [v6] 供 monitor.c 查询的运行时信息
int monitor_get_players (void) { return (int)client_count; }

int monitor_get_sockets (void) {
  // 监听 socket + 每个在线客户端各占一个
  return 1 + (int)client_count;
}

#endif
