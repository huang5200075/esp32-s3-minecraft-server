// ============================================================================
//  monitor.c — 运行时状态监控 + TCP 管理控制台
//
//  端口 25566，支持两种用法：
//
//  1) 状态查询（连上即回一行 JSON，然后断开）
//       $ nc <board-ip> 25566
//       {"uptime_ms":...,"heap_free":...,"temp_c":52.3,"players":1,...}
//
//  2) 管理命令（连上后发送一行文本命令，回车结尾）
//       $ nc <board-ip> 25566
//       list                  -> 列出在线玩家
//       kick <name>           -> 踢出指定玩家
//       say <text>            -> 全服广播
//       status                -> 完整状态（多行文本）
//       help                  -> 命令列表
//
//  实现方式：连上后先等最多 300ms 看客户端是否发来命令；
//  没发就当作状态查询，直接回 JSON。
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_log.h"
#include "driver/temperature_sensor.h"

#include "globals.h"
#include "packets.h"
#include "procedures.h"
#include "monitor.h"
#include "console_cmds.h"

static const char *TAG = "con";

#define CONSOLE_PORT       25566
#define CONSOLE_STACK      6144
#define CONSOLE_PRIO       3
#define CMD_TIMEOUT_MS     300     // 等命令的时长，超时则回状态

static temperature_sensor_handle_t s_tsens = NULL;
static int64_t s_boot_time_us = 0;

// ---------------------------------------------------------------------------
// CPU 占用采样（需要 CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS）
// ---------------------------------------------------------------------------
static uint8_t s_cpu0 = 0, s_cpu1 = 0;

static void sample_cpu (void) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && (configNUM_CORES > 1)
  static uint32_t prev_idle0 = 0, prev_idle1 = 0, prev_total = 0;
  static TaskStatus_t buf[24];

  UBaseType_t n = uxTaskGetSystemState(buf, 24, NULL);
  if (n == 0) return;

  TaskHandle_t idle0h = xTaskGetIdleTaskHandleForCore(0);
  TaskHandle_t idle1h = xTaskGetIdleTaskHandleForCore(1);
  uint32_t idle0 = 0, idle1 = 0, total = 0;

  for (UBaseType_t i = 0; i < n; i ++) {
    if (buf[i].xHandle == idle0h) idle0 = buf[i].ulRunTimeCounter;
    if (buf[i].xHandle == idle1h) idle1 = buf[i].ulRunTimeCounter;
    if (buf[i].ulRunTimeCounter > total) total = buf[i].ulRunTimeCounter;
  }

  if (prev_total != 0 && total > prev_total) {
    uint32_t dt = total - prev_total;
    uint32_t d0 = idle0 - prev_idle0;
    uint32_t d1 = idle1 - prev_idle1;
    if (d0 > dt) d0 = dt;
    if (d1 > dt) d1 = dt;
    s_cpu0 = (uint8_t)(100 - (d0 * 100 / dt));
    s_cpu1 = (uint8_t)(100 - (d1 * 100 / dt));
  }
  prev_idle0 = idle0; prev_idle1 = idle1; prev_total = total;
#endif
}

static float read_temp (void) {
  float t = -1.0f;
  if (s_tsens && temperature_sensor_get_celsius(s_tsens, &t) != ESP_OK) t = -1.0f;
  return t;
}

// ---------------------------------------------------------------------------
// JSON 状态
// ---------------------------------------------------------------------------
static int build_json (char *out, size_t len) {
  uint32_t up = (uint32_t)((esp_timer_get_time() - s_boot_time_us) / 1000);
  size_t heap_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t heap_min  = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  size_t ps_free = 0, ps_total = 0;
#if CONFIG_SPIRAM
  ps_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  ps_total = esp_psram_get_size();
#endif
  return snprintf(out, len,
    "{\"uptime_ms\":%u,\"heap_free\":%u,\"heap_min\":%u,"
    "\"psram_free\":%u,\"psram_total\":%u,"
    "\"cpu_core0\":%u,\"cpu_core1\":%u,\"temp_c\":%.1f,\"players\":%d}\n",
    (unsigned)up, (unsigned)heap_free, (unsigned)heap_min,
    (unsigned)ps_free, (unsigned)ps_total,
    (unsigned)s_cpu0, (unsigned)s_cpu1, read_temp(), monitor_get_players());
}

// ---------------------------------------------------------------------------
// 发送辅助
// ---------------------------------------------------------------------------
static void reply (int fd, const char *s) {
  size_t len = strlen(s), sent = 0;
  while (sent < len) {
    int n = send(fd, s + sent, len - sent, 0);
    if (n <= 0) return;
    sent += (size_t)n;
  }
}

static int player_online (int i) {
  return player_data[i].client_fd != -1;
}

// ---------------------------------------------------------------------------
// 命令：gm <0|1|2|3>   0=生存 1=创造 2=冒险 3=观察
// [v8] 不再在这里直接写玩家 socket —— 那会与主循环并发操作同一个 fd，
// 导致连接被写坏、玩家掉线。改为设置标记，由主循环下一轮统一下发。
// ---------------------------------------------------------------------------
static void cmd_gm (int fd, const char *arg) {
  static const char *names[] = {"survival", "creative", "adventure", "spectator"};
  char line[96];

  if (!arg || !*arg) {
    snprintf(line, sizeof(line), "current: %d (%s)\nusage: gm 0|1|2|3\n",
             (int)gamemode, names[gamemode & 3]);
    reply(fd, line);
    return;
  }
  if (arg[0] < '0' || arg[0] > '3') {
    reply(fd, "usage: gm 0|1|2|3  (0=survival 1=creative 2=adventure 3=spectator)\n");
    return;
  }

  console_pending_gm = (uint8_t)(arg[0] - '0');
  console_pending |= 0x01;

  snprintf(line, sizeof(line), "gamemode -> %d (%s), queued\n",
           (int)console_pending_gm, names[console_pending_gm & 3]);
  reply(fd, line);
}

// ---------------------------------------------------------------------------
// 命令：list
// ---------------------------------------------------------------------------
static void cmd_list (int fd) {
  char line[128];
  int n = 0;
  reply(fd, "idx  name             x      y    z     hp\n");
  reply(fd, "-----------------------------------------------\n");
  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (!player_online(i)) continue;
    n ++;
    snprintf(line, sizeof(line), "%-4d %-16s %-6d %-4d %-5d %d\n",
             i, player_data[i].name,
             (int)player_data[i].x, (int)player_data[i].y,
             (int)player_data[i].z, (int)player_data[i].health);
    reply(fd, line);
  }
  if (n == 0) reply(fd, "(no players online)\n");
  snprintf(line, sizeof(line), "total: %d\n", n);
  reply(fd, line);
}

// ---------------------------------------------------------------------------
// 命令：kick <name|idx>
// ---------------------------------------------------------------------------
static void cmd_kick (int fd, const char *arg) {
  char line[96];
  if (!arg || !*arg) { reply(fd, "usage: kick <name|index>\n"); return; }

  for (int i = 0; i < MAX_PLAYERS; i ++) {
    if (!player_online(i)) continue;
    if (strcasecmp(player_data[i].name, arg) == 0 ||
        (arg[0] >= '0' && arg[0] <= '9' && atoi(arg) == i)) {
      snprintf(line, sizeof(line), "kick queued: %s (slot %d)\n", player_data[i].name, i);
      reply(fd, line);
      // [v8] 只排队，由主循环执行（避免并发写同一 socket）
      console_pending_kick_idx = (int8_t)i;
      console_pending |= 0x04;
      return;
    }
  }
  reply(fd, "player not found\n");
}

// ---------------------------------------------------------------------------
// 命令：say <text>
// ---------------------------------------------------------------------------
static void cmd_say (int fd, const char *text) {
  if (!text || !*text) { reply(fd, "usage: say <text>\n"); return; }
  // [v8] 拷贝到全局缓冲，由主循环广播
  strncpy((char *)console_pending_text, text, sizeof(console_pending_text) - 1);
  ((char *)console_pending_text)[sizeof(console_pending_text) - 1] = 0;
  console_pending |= 0x02;
  reply(fd, "say queued\n");
}

// ---------------------------------------------------------------------------
// 命令：status（多行文本）
// ---------------------------------------------------------------------------
static void cmd_status (int fd) {
  char buf[512];
  uint32_t up = (uint32_t)((esp_timer_get_time() - s_boot_time_us) / 1000);
  snprintf(buf, sizeof(buf),
    "uptime    : %u s\n"
    "players   : %d\n"
    "heap free : %u KB (min %u KB)\n"
    "psram free: %u KB / %u KB\n"
    "cpu core0 : %u%%\n"
    "cpu core1 : %u%%\n"
    "temp      : %.1f C\n",
    (unsigned)(up / 1000), monitor_get_players(),
    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
    (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
#if CONFIG_SPIRAM
    (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
    (unsigned)(esp_psram_get_size() / 1024),
#else
    0u, 0u,
#endif
    (unsigned)s_cpu0, (unsigned)s_cpu1, read_temp());
  reply(fd, buf);
}

static void cmd_help (int fd) {
  reply(fd,
    "== players ==\n"
    "  list                 online player list\n"
    "  players              detailed player info (mode/hp/food/pos)\n"
    "  kick <name|idx>      disconnect a player\n"
    "  say <text>           broadcast chat message\n"
    "  gm <0|1|2|3>         gamemode (0 survival / 1 creative / 2 adventure / 3 spectator)\n"
    "\n== players (action) ==\n"
    "  give <player> <itemID> [n]   give items (1-64)\n"
    "  tp <player> <x> <y> <z>      teleport one player\n"
    "  tpall <x> <y> <z>            teleport everyone\n"
    "  heal <player>                restore health and hunger\n"
    "  kill <player>                kill a player\n"
    "  clear <player>               empty inventory\n"
    "\n== world ==\n"
    "  time <day|noon|night|midnight|0-23999>   set world time\n"
    "  setblock <x> <y> <z> <blockID>           place one block\n"
    "  fill <x1 y1 z1> <x2 y2 z2> <blockID>     fill a region (max 4096)\n"
    "  spawn <mobID> [n]                        spawn mobs (25 chicken/28 cow/\n"
    "                                           95 pig/106 sheep/145 zombie)\n"
    "  save                                     flush world + players to flash\n"
    "\n== server ==\n"
    "  status               memory / cpu / temperature\n"
    "  help                 this text\n"
    "\n(connect without sending anything to get one-line JSON status)\n"
    "\nNot available on this server: xp, enchant, effect, execute,\n"
    "gamerule, dimension switching (no Nether/End).\n");
}

// ---------------------------------------------------------------------------
// 解析并执行命令
// ---------------------------------------------------------------------------
static void run_command (int fd, char *cmd) {
  // 去掉尾部换行/回车
  size_t n = strlen(cmd);
  while (n > 0 && (cmd[n-1] == '\n' || cmd[n-1] == '\r' || cmd[n-1] == ' ')) cmd[--n] = 0;

  char *arg = strchr(cmd, ' ');
  if (arg) { *arg = 0; arg ++; while (*arg == ' ') arg ++; }

  cc_reply_t rep = (cc_reply_t)reply;

  if      (!strcasecmp(cmd, "list"))    cmd_list(fd);
  else if (!strcasecmp(cmd, "players")) cc_players(fd, rep, arg);
  else if (!strcasecmp(cmd, "kick"))    cmd_kick(fd, arg);
  else if (!strcasecmp(cmd, "say"))     cmd_say(fd, arg);
  else if (!strcasecmp(cmd, "gm"))      cmd_gm(fd, arg);
  else if (!strcasecmp(cmd, "give"))    cc_give(fd, rep, arg);
  else if (!strcasecmp(cmd, "tp"))      cc_tp(fd, rep, arg);
  else if (!strcasecmp(cmd, "tpall"))   cc_tpall(fd, rep, arg);
  else if (!strcasecmp(cmd, "heal"))    cc_heal(fd, rep, arg);
  else if (!strcasecmp(cmd, "kill"))    cc_kill(fd, rep, arg);
  else if (!strcasecmp(cmd, "clear"))   cc_clear(fd, rep, arg);
  else if (!strcasecmp(cmd, "time"))    cc_time(fd, rep, arg);
  else if (!strcasecmp(cmd, "setblock"))cc_setblock(fd, rep, arg);
  else if (!strcasecmp(cmd, "fill"))    cc_fill(fd, rep, arg);
  else if (!strcasecmp(cmd, "spawn"))   cc_spawn(fd, rep, arg);
  else if (!strcasecmp(cmd, "save"))    cc_save(fd, rep, arg);
  else if (!strcasecmp(cmd, "status"))  cmd_status(fd);
  else if (!strcasecmp(cmd, "info"))    cmd_status(fd);
  else if (!strcasecmp(cmd, "help"))    cmd_help(fd);
  else if (!strcasecmp(cmd, ""))        { char j[384]; build_json(j, sizeof(j)); reply(fd, j); }
  else {
    reply(fd, "unknown command, try 'help'\n");
  }
}

// ---------------------------------------------------------------------------
// 监听任务
// ---------------------------------------------------------------------------
static void console_task (void *arg) {
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) { ESP_LOGE(TAG, "socket: %d", errno); vTaskDelete(NULL); return; }

  int opt = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(CONSOLE_PORT);

  if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    ESP_LOGE(TAG, "bind %d: %d", CONSOLE_PORT, errno);
    close(listen_fd); vTaskDelete(NULL); return;
  }
  listen(listen_fd, 2);
  ESP_LOGI(TAG, "console on port %d (JSON status + commands)", CONSOLE_PORT);

  char buf[256];
  while (1) {
    struct sockaddr_in cli;
    socklen_t clen = sizeof(cli);
    int fd = accept(listen_fd, (struct sockaddr *)&cli, &clen);
    if (fd < 0) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    // 读超时：保证 recv 不会永久阻塞，同时让"连上就查状态"不用干等太久
    struct timeval rtv = { .tv_sec = 0, .tv_usec = 40000 };   // 40ms
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    struct timeval stv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

    // 最多等 CMD_TIMEOUT_MS 收集一行命令
    int got = 0;
    int waited = 0;
    while (waited < CMD_TIMEOUT_MS && got < (int)sizeof(buf) - 1) {
      int r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
      if (r > 0) {
        got += r;
        if (memchr(buf, '\n', got)) break;    // 收到完整一行，立刻执行
        continue;                             // 继续读，可能还有
      }
      if (r == 0) break;                      // 对端关闭（nc 常见）
      // r < 0：区分超时与其他错误
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
        continue;
      }
      break;                                  // 真正的错误，放弃这次连接
    }
    buf[got] = 0;

    sample_cpu();
    if (got > 0) {
      run_command(fd, buf);
    } else {
      char json[384];
      build_json(json, sizeof(json));
      reply(fd, json);
    }

    // 收尾：先半关闭写端，再把残留数据读掉，最后关闭。
    // 用 SO_LINGER=0 避免在 TIME_WAIT 里占用 socket（板子只有 10 个）
    shutdown(fd, SHUT_WR);
    {
      char sink[64];
      int guard = 0;
      while (guard ++ < 4) {
        if (recv(fd, sink, sizeof(sink), 0) <= 0) break;
      }
    }
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    close(fd);
    vTaskDelay(pdMS_TO_TICKS(5));   // 给 lwIP 一点时间回收，避免下次 accept 拿到脏 fd
  }
}

// ---------------------------------------------------------------------------
// 初始化
// ---------------------------------------------------------------------------
void monitor_start (void) {
  s_boot_time_us = esp_timer_get_time();

#if CONFIG_SOC_TEMP_SENSOR_SUPPORTED
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
  if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) {
    if (temperature_sensor_enable(s_tsens) != ESP_OK) s_tsens = NULL;
  }
#endif

  xTaskCreatePinnedToCore(console_task, "console",
                          CONSOLE_STACK, NULL, CONSOLE_PRIO, NULL, 0);
}
