#include <stdio.h>
#include <stdint.h>
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
#endif
#include <unistd.h>

#include "globals.h"

#ifdef ESP_PLATFORM
  #include "esp_attr.h"
  #include "esp_task_wdt.h"
  #include "esp_timer.h"

  // [OPT-B] 原来每 1 秒就让出一次 CPU，且调用点是"每个网络包"。
  // vTaskDelay(1) 在 1000Hz 下至少 1ms，实测常到 1~2ms，
  // MC 登录要往返几十个包，逐个叠加就是秒级延迟。
  // 改为 taskYIELD()：只让出同优先级任务，不进入 tick 等待队列。
  //
  // 注意：本项目 sdkconfig 里 CONFIG_ESP_TASK_WDT_INIT=n（看门狗未初始化），
  // 所以这里绝对不能调用 esp_task_wdt_reset()，否则每 200ms 刷一条
  // "TWDT was never initialized" 错误，串口输出本身就会拖慢主循环。
  #define TASK_YIELD_INTERVAL (1000 * 1000)

  int64_t last_yield = 0;

  void task_yield () {
    int64_t time_now = esp_timer_get_time();

    // 每秒才真正让出一次 CPU，且用 taskYIELD 而非 vTaskDelay，
    // 不进入 tick 等待队列，避免每包 1ms 的额外延迟
    if (time_now - last_yield > TASK_YIELD_INTERVAL) {
      taskYIELD();
      last_yield = time_now;
    }
  }
#endif

ssize_t recv_count;
uint8_t recv_buffer[MAX_RECV_BUF_LEN] = {0};

uint32_t world_seed = INITIAL_WORLD_SEED;
uint32_t rng_seed = INITIAL_RNG_SEED;

uint16_t world_time = 0;
uint32_t server_ticks = 0;

char motd[] = { SERVER_MOTD };
uint8_t motd_len = sizeof(motd) - 1;

#ifdef SEND_BRAND
  char brand[] = { "bareiron" };
  uint8_t brand_len = sizeof(brand) - 1;
#endif

uint16_t client_count;

// [v8] 控制台跨任务命令标记（见 globals.h 说明）
volatile uint8_t console_pending = 0;
volatile uint8_t console_pending_gm = 0;
volatile char console_pending_text[128] = {0};
volatile int8_t console_pending_kick_idx = -1;

// Server game mode, changeable at runtime via the !gm command (see GAMEMODE).
uint8_t gamemode = INITIAL_GAMEMODE;

// [v7] block_changes 占 120KB，原本在内部 SRAM（总量仅 512KB，静态已占 252KB）。
// 这是冷数据：只在生成区块和落盘时访问，放 PSRAM 不影响性能。
#ifdef ESP_PLATFORM
EXT_RAM_BSS_ATTR BlockChange block_changes[MAX_BLOCK_CHANGES];
#else
BlockChange block_changes[MAX_BLOCK_CHANGES];
#endif
int block_changes_count = 0;

PlayerData player_data[MAX_PLAYERS];
int player_data_count = 0;

MobData mob_data[MAX_MOBS];
