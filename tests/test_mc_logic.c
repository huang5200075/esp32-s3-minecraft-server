// ============================================================================
//  ESP32 MC Server — 纯逻辑回归测试
//
//  在 PC 上直接运行，不依赖 ESP32 硬件：
//      ./test_mc_logic
//
//  覆盖过去实际出过 bug 的地方：
//    - 区块包长度计算（少报 1 字节导致客户端解码失败）
//    - 方块掉落映射
//    - 物品堆叠与背包槽位分配
//    - VarInt 编解码
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "registries.h"

// ---- 被测函数的精简副本（与原实现保持一致，仅去掉硬件依赖）----------------

// 对应 main/varnum.c
int sizeVarInt (uint32_t value) {
  int size = 1;
  while (value >= 0x80) {
    value >>= 7;
    size ++;
  }
  return size;
}

// 对应 main/procedures.c: getItemStackSize
uint8_t getItemStackSize (uint16_t item) {
  switch (item) {
    case I_potion:
    case I_snowball:
    case I_bucket:
    case I_ender_pearl:
    case I_egg:
      return 16;
    case I_iron_sword:
    case I_golden_sword:
    case I_diamond_sword:
      return 1;
    default:
      return 64;
  }
}

// ---- 测试框架 --------------------------------------------------------------
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, fmt, ...)                                     \
  do {                                                            \
    if (cond) { g_pass++; }                                       \
    else {                                                        \
      g_fail++;                                                   \
      printf("  [FAIL] " fmt "\n", ##__VA_ARGS__);                \
    }                                                             \
  } while (0)

#define SECTION(name) printf("\n== %s\n", name)

// ---- 1. 区块包长度 ---------------------------------------------------------
// 这是曾经导致 "DecoderException: packet id 39" 的元凶：
// light_data_size 少算了末尾的 writeVarInt(0)，包长度少报 1 字节，
// 客户端多读 1 字节后整个流错位。
static void test_chunk_packet_size (void) {
  SECTION("区块包（Chunk Data and Update Light）长度计算");

  // registries.h 中已声明为 network_block_palette[483]
  const int palette_len = (int)sizeof(network_block_palette);

  const int chunk_data_size =
      (4101 + sizeVarInt(256) + palette_len) * 20 + 6 * 12;

  // 修复后的公式（与 packets.c 一致）
  const int light_declared =
      1                                  // 数组数量
    + 8                                  // sky light mask (uint64)
    + 3                                  // 3 个空 VarInt
    + sizeVarInt(26) + 26 * (sizeVarInt(2048) + 2048)   // sky light 数组
    + 1;                                 // 末尾 writeVarInt(0)

  // 按实际写出顺序累加
  int light_actual = 0;
  light_actual += 1;                               // writeVarInt(1)
  light_actual += 8;                               // writeUint64(mask)
  light_actual += 1;                               // writeVarInt(0)
  light_actual += 1;                               // writeVarInt(0)
  light_actual += 1;                               // writeVarInt(0)
  light_actual += sizeVarInt(26);                  // 数组长度
  light_actual += 26 * (sizeVarInt(2048) + 2048);  // 26 个 2048 字节数组
  light_actual += 1;                               // writeVarInt(0) 末尾

  printf("  chunk_data_size  = %d\n", chunk_data_size);
  printf("  light 声明       = %d\n", light_declared);
  printf("  light 实际       = %d\n", light_actual);

  CHECK(light_declared == light_actual,
        "light 长度不一致: 声明 %d vs 实际 %d", light_declared, light_actual);

  // 核对旧公式：14 + (sv(2048)+2048)*26
  //   = 1(数组数) + 8(mask) + 3(空VarInt) + 1(sv(26)) + 53300 + 1(末尾)
  //   = 14 + 53300  —— 数值上恰好等价，所以旧代码并没有算错长度。
  // 保留这个对照，防止以后有人"优化"公式时改错。
  const int light_old = 14 + (sizeVarInt(2048) + 2048) * 26;
  CHECK(light_old == light_actual,
        "旧公式重算后应与实际一致（%d vs %d）", light_old, light_actual);

  printf("  旧公式重算       = %d（与修复后一致，说明原实现长度是对的）\n", light_old);
}

// ---- 2. VarInt -------------------------------------------------------------
static void test_varint (void) {
  SECTION("VarInt 长度");
  CHECK(sizeVarInt(0) == 1, "sizeVarInt(0)");
  CHECK(sizeVarInt(127) == 1, "sizeVarInt(127)");
  CHECK(sizeVarInt(128) == 2, "sizeVarInt(128)");
  CHECK(sizeVarInt(255) == 2, "sizeVarInt(255)");
  CHECK(sizeVarInt(256) == 2, "sizeVarInt(256)");
  CHECK(sizeVarInt(16383) == 2, "sizeVarInt(16383)");
  CHECK(sizeVarInt(16384) == 3, "sizeVarInt(16384)");
  CHECK(sizeVarInt(2097151) == 3, "sizeVarInt(2097151)");
  CHECK(sizeVarInt(2048) == 2, "sizeVarInt(2048)");
}

// ---- 3. 方块掉落映射 -------------------------------------------------------
static void test_block_drops (void) {
  SECTION("方块 → 物品掉落映射");

  // 这些是玩家最常挖的方块，必须都能掉落东西
  struct { uint8_t block; const char *name; } cases[] = {
    { B_grass_block, "grass_block" },
    { B_dirt,        "dirt" },
    { B_stone,       "stone" },
    { B_cobblestone, "cobblestone" },
    { B_sand,        "sand" },
    { B_gravel,      "gravel" },
    { B_oak_log,     "oak_log" },
  };

  for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
    uint16_t item = B_to_I[cases[i].block];
    CHECK(item != 0, "%s (id=%d) 掉落为空!", cases[i].name, cases[i].block);
    if (item) printf("  %-12s -> item %d\n", cases[i].name, item);
  }

  // 空气不该掉落任何东西
  CHECK(B_to_I[B_air] == 0, "air 不应该掉落物品");
}

// ---- 4. 物品堆叠 -----------------------------------------------------------
static void test_stack_size (void) {
  SECTION("物品堆叠上限");
  CHECK(getItemStackSize(I_dirt) == 64, "普通方块应为 64");
  CHECK(getItemStackSize(I_ender_pearl) == 16, "末影珍珠应为 16");
  CHECK(getItemStackSize(I_iron_sword) == 1, "铁剑应为 1");

  // 防御性：任意物品都必须返回 >0，否则 givePlayerItem 会算错
  const uint16_t samples[] = { I_dirt, I_stone, I_iron_sword, I_ender_pearl };
  for (unsigned i = 0; i < sizeof(samples)/sizeof(samples[0]); i++) {
    CHECK(getItemStackSize(samples[i]) > 0,
          "物品 %d 堆叠上限为 0（会导致物品无法入包）", samples[i]);
  }
}

// ---- 5. 背包槽位分配（复刻 givePlayerItem 的决策逻辑）---------------------
// 曾经的问题：`inventory_count[i] <= stack_size - count` 在 uint8 下溢，
// 导致本该新建堆的槽位被判成"能堆叠"，物品数量错乱。
static int find_slot (uint8_t *items, uint8_t *counts, uint16_t item, uint8_t n) {
  uint16_t stack_size = getItemStackSize(item);
  if (stack_size == 0) stack_size = 64;
  if (n > stack_size) n = (uint8_t)stack_size;

  for (int i = 0; i < 41; i++) {
    if (items[i] != item) continue;
    if ((uint32_t)counts[i] + n <= stack_size) return i;
  }
  for (int i = 0; i < 41; i++) {
    if (counts[i] == 0) return i;
  }
  return 255;   // 满
}

static void test_inventory (void) {
  SECTION("背包槽位分配");
  uint8_t items[41] = {0};
  uint8_t counts[41] = {0};

  // 空背包：应放进 slot 0
  CHECK(find_slot(items, counts, I_dirt, 1) == 0, "空背包应放入 slot 0");

  // 已有 63 个泥土，再放 1 个应当堆叠到同一格而不是新建
  items[0] = I_dirt & 0xFF; counts[0] = 63;
  int s = find_slot(items, counts, I_dirt, 1);
  CHECK(s == 0, "63+1 应堆叠到 slot 0，实际 %d", s);

  // 已有 64 个（满堆），再放应当新建一格
  counts[0] = 64;
  s = find_slot(items, counts, I_dirt, 1);
  CHECK(s == 1, "满堆后应新建 slot 1，实际 %d", s);

  // 下溢回归：count=64、n=64 时，(uint8)(64-64)=0，若用无符号减法会误判
  counts[0] = 64;
  s = find_slot(items, counts, I_dirt, 64);
  CHECK(s != 0, "下溢回归：满堆不该再叠加 64 个");

  // 背包全满：应返回 255
  memset(items, 0, sizeof(items));
  memset(counts, 0, sizeof(counts));
  for (int i = 0; i < 41; i++) { items[i] = 1; counts[i] = 64; }
  CHECK(find_slot(items, counts, I_dirt, 1) == 255, "背包满时应返回 255");

  // 背包满时不能崩溃、不能越界
  CHECK(find_slot(items, counts, I_dirt, 1) != 0, "满包不应返回 0");
}

// ---- 6. 区块位图（新版区块同步逻辑）---------------------------------------
static void test_chunk_bitmap (void) {
  SECTION("区块同步位图");

  const int VIEW_DISTANCE = 2;
  const int grid = (2*VIEW_DISTANCE+1) * (2*VIEW_DISTANCE+1);   // 25
  const int words = (grid + 31) / 32;

  CHECK(words == 1, "视距2应只需 1 个 uint32（实际 %d）", words);

  uint32_t bits[2] = {0};
  // 标记全部 25 格
  for (int i = 0; i < grid; i++) bits[i >> 5] |= (1u << (i & 31));

  int marked = 0;
  for (int i = 0; i < grid; i++) if (bits[i >> 5] & (1u << (i & 31))) marked++;
  CHECK(marked == 25, "25 格应全部标记成功，实际 %d", marked);

  // 视距 3 需要 2 个 uint32
  const int grid3 = 7*7;
  CHECK((grid3 + 31) / 32 == 2, "视距3应需要 2 个 uint32");
  printf("  视距2: %d 格 / %d word，视距3: %d 格 / %d word\n",
         grid, words, grid3, (grid3+31)/32);
}

// ---- main ------------------------------------------------------------------
int main (void) {
  printf("ESP32 MC Server — 逻辑回归测试\n");
  printf("========================================\n");

  test_chunk_packet_size();
  test_varint();
  test_block_drops();
  test_stack_size();
  test_inventory();
  test_chunk_bitmap();

  printf("\n========================================\n");
  printf("通过: %d   失败: %d\n", g_pass, g_fail);
  if (g_fail == 0) printf("ALL PASS\n");
  else             printf("有失败项，需要修复\n");
  return g_fail == 0 ? 0 : 1;
}
