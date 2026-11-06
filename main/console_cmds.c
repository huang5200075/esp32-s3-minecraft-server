// ============================================================================
//  console_cmds.c — TCP 管理控制台的扩展命令
//
//  实现服务器能力范围内可用的原版风格指令。
//  所有会写玩家 socket 的命令都必须经由主循环执行（见 main.c 的
//  console_pending 机制），不能在此直接调用 —— 否则会与主循环并发写同一个
//  socket 导致玩家掉线。
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "globals.h"
#include "registries.h"
#include "packets.h"
#include "procedures.h"
#include "serialize.h"
#include "console_cmds.h"

// 允许控制台命令在守卫保护下直接调用的写操作：
// 这些命令本身不碰玩家 socket（只改内存/发方块更新），因此安全。
// 需要给玩家发消息/切模式/踢人的命令，仍旧走 console_pending 排队。

static int find_player (const char *name) {
  if (!name || !*name) return -1;
  // 支持按名字或槽位号查找
  int as_idx = -1;
  if (name[0] >= '0' && name[0] <= '9') as_idx = atoi(name);
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    if (as_idx == i) return i;
    if (strcasecmp(player_data[i].name, name) == 0) return i;
  }
  return -1;
}

// ---- give <玩家> <物品ID> [数量] ------------------------------------------
void cc_give (int fd, cc_reply_t reply, const char *arg) {
  char line[128];
  char who[32] = {0};
  int item = -1, count = 1;

  if (!arg || sscanf(arg, "%31s %d %d", who, &item, &count) < 2) {
    reply(fd, "usage: give <player> <itemID> [count]\n");
    return;
  }
  int idx = find_player(who);
  if (idx < 0) { reply(fd, "player not found\n"); return; }
  if (item <= 0 || item > 65535) { reply(fd, "invalid item ID\n"); return; }
  if (count < 1) count = 1;
  if (count > 64) count = 64;

  if (givePlayerItem(&player_data[idx], (uint16_t)item, (uint8_t)count)) {
    snprintf(line, sizeof(line), "failed: inventory full for %s\n", player_data[idx].name);
  } else {
    snprintf(line, sizeof(line), "gave %d x item %d to %s\n", count, item, player_data[idx].name);
  }
  reply(fd, line);
}

// ---- tp <玩家> <x> <y> <z> ------------------------------------------------
void cc_tp (int fd, cc_reply_t reply, const char *arg) {
  char line[128], who[32] = {0};
  double x, y, z;

  if (!arg || sscanf(arg, "%31s %lf %lf %lf", who, &x, &y, &z) < 4) {
    reply(fd, "usage: tp <player> <x> <y> <z>\n");
    return;
  }
  int idx = find_player(who);
  if (idx < 0) { reply(fd, "player not found\n"); return; }
  if (y < 0) y = 0;
  if (y > 255) y = 255;

  player_data[idx].x = (short)x;
  player_data[idx].y = (uint8_t)y;
  player_data[idx].z = (short)z;
  player_data[idx].grounded_y = (uint8_t)y;
  sc_synchronizePlayerPosition(player_data[idx].client_fd, x, y, z, 0, 0);

  snprintf(line, sizeof(line), "teleported %s to %.1f %.1f %.1f\n",
           player_data[idx].name, x, y, z);
  reply(fd, line);
}

// ---- tpall <x> <y> <z> ----------------------------------------------------
void cc_tpall (int fd, cc_reply_t reply, const char *arg) {
  char line[128];
  double x, y, z;
  int n = 0;

  if (!arg || sscanf(arg, "%lf %lf %lf", &x, &y, &z) < 3) {
    reply(fd, "usage: tpall <x> <y> <z>\n");
    return;
  }
  if (y < 0) y = 0;
  if (y > 255) y = 255;

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    player_data[i].x = (short)x;
    player_data[i].y = (uint8_t)y;
    player_data[i].z = (short)z;
    player_data[i].grounded_y = (uint8_t)y;
    sc_synchronizePlayerPosition(player_data[i].client_fd, x, y, z, 0, 0);
    n ++;
  }
  snprintf(line, sizeof(line), "teleported %d player(s)\n", n);
  reply(fd, line);
}

// ---- heal <玩家> ----------------------------------------------------------
void cc_heal (int fd, cc_reply_t reply, const char *arg) {
  char line[96];
  if (!arg || !*arg) { reply(fd, "usage: heal <player>\n"); return; }
  int idx = find_player(arg);
  if (idx < 0) { reply(fd, "player not found\n"); return; }

  player_data[idx].health = 20;
  player_data[idx].hunger = 20;
  player_data[idx].saturation = 500;
  sc_setHealth(player_data[idx].client_fd, 20, 20, 500);

  snprintf(line, sizeof(line), "healed %s\n", player_data[idx].name);
  reply(fd, line);
}

// ---- kill <玩家> ----------------------------------------------------------
void cc_kill (int fd, cc_reply_t reply, const char *arg) {
  char line[96];
  if (!arg || !*arg) { reply(fd, "usage: kill <player>\n"); return; }
  int idx = find_player(arg);
  if (idx < 0) { reply(fd, "player not found\n"); return; }

  // 用 generic 伤害打满血，保证致死
  hurtEntity(player_data[idx].client_fd, -1, D_generic, 200);

  snprintf(line, sizeof(line), "killed %s\n", player_data[idx].name);
  reply(fd, line);
}

// ---- clear <玩家> ---------------------------------------------------------
void cc_clear (int fd, cc_reply_t reply, const char *arg) {
  char line[96];
  if (!arg || !*arg) { reply(fd, "usage: clear <player>\n"); return; }
  int idx = find_player(arg);
  if (idx < 0) { reply(fd, "player not found\n"); return; }

  PlayerData *p = &player_data[idx];
  for (int i = 0; i < 41; i ++) {
    p->inventory_items[i] = 0;
    p->inventory_count[i] = 0;
    sc_setContainerSlot(p->client_fd, 0, serverSlotToClientSlot(0, i), 0, 0);
  }
  p->craft_items[0] = 0;
  for (int i = 0; i < 9; i ++) p->craft_count[i] = 0;

  snprintf(line, sizeof(line), "cleared inventory of %s\n", p->name);
  reply(fd, line);
}

// ---- time <day|night|noon|midnight|数字> ----------------------------------
void cc_time (int fd, cc_reply_t reply, const char *arg) {
  char line[96];
  int t = -1;

  if (!arg || !*arg) { reply(fd, "usage: time day|night|noon|midnight|<0-23999>\n"); return; }
  if      (!strcasecmp(arg, "day"))      t = 1000;
  else if (!strcasecmp(arg, "noon"))     t = 6000;
  else if (!strcasecmp(arg, "night"))    t = 13000;
  else if (!strcasecmp(arg, "midnight")) t = 18000;
  else if (arg[0] >= '0' && arg[0] <= '9') t = atoi(arg);

  if (t < 0) { reply(fd, "invalid time\n"); return; }
  if (t > 23999) t = t % 24000;

  world_time = (uint16_t)t;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    sc_updateTime(player_data[i].client_fd, world_time);
  }
  snprintf(line, sizeof(line), "time set to %d\n", t);
  reply(fd, line);
}

// ---- setblock <x> <y> <z> <方块ID> ---------------------------------------
void cc_setblock (int fd, cc_reply_t reply, const char *arg) {
  char line[128];
  int x, y, z, block;

  if (!arg || sscanf(arg, "%d %d %d %d", &x, &y, &z, &block) < 4) {
    reply(fd, "usage: setblock <x> <y> <z> <blockID>\n");
    return;
  }
  if (y < 0 || y > 255) { reply(fd, "y must be 0-255\n"); return; }
  if (block < 0 || block > 255) { reply(fd, "block ID must be 0-255\n"); return; }
  if (x < -32768 || x > 32767 || z < -32768 || z > 32767) {
    reply(fd, "coordinate out of range\n");
    return;
  }

  if (makeBlockChange((short)x, (uint8_t)y, (short)z, (uint8_t)block)) {
    reply(fd, "failed (block change buffer full)\n");
    return;
  }
  snprintf(line, sizeof(line), "set block %d at %d %d %d\n", block, x, y, z);
  reply(fd, line);
}

// ---- fill <x1 y1 z1> <x2 y2 z2> <方块ID> ---------------------------------
// 体积上限保护：避免一次填充几十万方块把内存和 Flash 写爆
#define FILL_MAX_VOLUME 4096

void cc_fill (int fd, cc_reply_t reply, const char *arg) {
  char line[160];
  int x1, y1, z1, x2, y2, z2, block;

  if (!arg || sscanf(arg, "%d %d %d %d %d %d %d",
                     &x1, &y1, &z1, &x2, &y2, &z2, &block) < 7) {
    reply(fd, "usage: fill <x1> <y1> <z1> <x2> <y2> <z2> <blockID>\n");
    return;
  }
  if (block < 0 || block > 255) { reply(fd, "block ID must be 0-255\n"); return; }

  if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
  if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
  if (z1 > z2) { int t = z1; z1 = z2; z2 = t; }

  if (y1 < 0) y1 = 0;
  if (y2 > 255) y2 = 255;

  long volume = (long)(x2 - x1 + 1) * (y2 - y1 + 1) * (z2 - z1 + 1);
  if (volume > FILL_MAX_VOLUME) {
    snprintf(line, sizeof(line),
             "volume too large: %ld blocks (max %d). Split into smaller areas.\n",
             volume, FILL_MAX_VOLUME);
    reply(fd, line);
    return;
  }

  int done = 0, failed = 0;
  for (int y = y1; y <= y2; y ++)
    for (int z = z1; z <= z2; z ++)
      for (int x = x1; x <= x2; x ++) {
        if (makeBlockChange((short)x, (uint8_t)y, (short)z, (uint8_t)block)) failed ++;
        else done ++;
      }

  snprintf(line, sizeof(line), "filled %d block(s), %d failed\n", done, failed);
  reply(fd, line);
}

// ---- save ----------------------------------------------------------------
void cc_save (int fd, cc_reply_t reply, const char *arg) {
  (void)arg;
  writeBlockChangesToDisk(0, block_changes_count);
  writePlayerDataToDisk();
  reply(fd, "world and player data saved\n");
}

// ---- players（详细列表）---------------------------------------------------
void cc_players (int fd, cc_reply_t reply, const char *arg) {
  (void)arg;
  static const char *gmn[] = {"survival", "creative", "adventure", "spectator"};
  char line[160];
  int n = 0;

  reply(fd, "idx name             gamemode   hp food x    y    z\n");
  reply(fd, "--------------------------------------------------------\n");
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd == -1) continue;
    n ++;
    snprintf(line, sizeof(line), "%-3d %-16s %-10s %-2d %-4d %-4d %-4d %d\n",
             i, player_data[i].name,
             gmn[gamemode & 3],
             player_data[i].health, player_data[i].hunger,
             (int)player_data[i].x, (int)player_data[i].y, (int)player_data[i].z);
    reply(fd, line);
  }
  if (!n) reply(fd, "(no players online)\n");
  snprintf(line, sizeof(line), "total: %d\n", n);
  reply(fd, line);
}

// ---- spawn <生物> [数量] --------------------------------------------------
// 生物 ID：25 鸡 / 28 牛 / 95 猪 / 106 羊 / 145 僵尸
void cc_spawn (int fd, cc_reply_t reply, const char *arg) {
  char line[128];
  int type = 0, count = 1;

  if (!arg || sscanf(arg, "%d %d", &type, &count) < 1) {
    reply(fd, "usage: spawn <mobID> [count]\n"
              "  mobs: 25=chicken 28=cow 95=pig 106=sheep 145=zombie\n");
    return;
  }
  if (count < 1) count = 1;
  if (count > 10) count = 10;

  // 在第一个在线玩家周围生成
  int host = -1;
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (player_data[i].client_fd != -1) { host = i; break; }
  }
  if (host < 0) { reply(fd, "no player online\n"); return; }

  int made = 0;
  for (int i = 0; i < count; i ++) {
    short mx = player_data[host].x + (i % 3) - 1;
    short mz = player_data[host].z + (i / 3) - 1;
    uint8_t my = player_data[host].y;
    spawnMob((uint8_t)type, mx, my, mz, 20);
    made ++;
  }
  snprintf(line, sizeof(line), "spawned %d mob(s) of type %d near %s\n",
           made, type, player_data[host].name);
  reply(fd, line);
}
