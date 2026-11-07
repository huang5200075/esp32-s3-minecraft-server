# NOTES.md — 技术笔记与未解决问题

这份笔记记录本版本在 ESP32-S3 N16R8 上移植、优化时踩过的坑，
以及**目前尚未解决的几个问题**和已有的排查线索。

如果你修好了其中任何一个，欢迎提 Issue 或 PR。

---

## 目录

- [一、编译：从源码到能跑](#一编译从源码到能跑)
- [二、运行时：内存与延迟](#二运行时内存与延迟)
- [三、已知但未解决的问题](#三已知但未解决的问题)
- [四、调试工具与技巧](#四调试工具与技巧)

---

# 一、编译：从源码到能跑

## 1.1 必须自己生成 registries

仓库**不包含** `include/registries.h` 和 `main/registries.c`，
因为里面是 Mojang 的版权数据（方块、物品、注册表 ID）。

生成方法：

```bash
mkdir -p notchian
# 下载 1.21.8 官方 server.jar 放进 notchian/
./extract_registries.sh
```

**依赖**：Java 21+ 和 Node.js（或 bun / deno）。

脚本做的事：

1. `java -DbundlerMainClass=net.minecraft.data.Main -jar server.jar --all`
   —— 导出注册表到 `notchian/generated/`
2. `node build_registries.js` —— 转成 C 数组

生成的 `registries.h` 约 51KB，`registries.c` 约 19KB。

---

## 1.2 `src/` 必须改名为 `main/`

**这是最容易卡住的一步。**

原上游（jeyyprtf/esp32-mcserver）把应用代码放在 `src/` 目录里，
但 **ESP-IDF 只从名为 `main` 的组件里找 `app_main`**。

如果不改名，会出现：

```
undefined reference to `app_main'
```

而且更隐蔽的是：**1051 个编译目标全是 IDF 自己的组件，
你的服务器代码一个文件都没参与编译**。

本仓库已经改好（代码在 `main/`），但如果你基于上游原始代码改，需要注意。

---

## 1.3 子模块必须 pin 到 superproject 记录的 commit

`git clone --recursive` 或 `git submodule update --init` 拉到的是
**各子模块的 HEAD**，而不是 ESP-IDF v5.4.4 需要的那个版本。

最典型的症状：

```
CMake Error at components/mbedtls/CMakeLists.txt:180 (target_link_libraries):
  Cannot specify link libraries for target "mbedcrypto" which is not built
  by this project.
```

原因：mbedtls 子模块被拉到了 4.1.1，而 IDF v5.4 需要 3.6.7。

**正确做法**：按 `git ls-tree` 输出逐个 checkout。

```bash
cd path/to/esp-idf
git ls-tree -r HEAD | awk '$2=="commit"{print $3" "$4}' | while read sha path; do
  (cd "$path" && git fetch --depth 1 origin "$sha" && git checkout -q --detach "$sha")
done
```

> 注意：`cJSON` 和 `micro-ecc` 两个仓库重写过历史，
> 目标 commit 可能取不到，就 `git checkout -f master` 恢复工作区即可（缺这两个也能编过）。

---

## 1.4 工具链是 glibc 版，Alpine 需要 gcompat

如果在 musl 系统（如 Alpine / Termux 容器）里编译，会报：

```
xtensa-esp-elf-gcc: not found
```

原因：Espressif 的工具链是**动态链接到 glibc** 的，
需要 `/lib/ld-linux-aarch64.so.1`。

```bash
apk add gcompat
```

---

## 1.5 下载工具链：绕过 idf_tools.py 的慢速下载

`idf_tools.py` 默认从 `dl.espressif.com` 下载，国内会重定向到一个失效的
`.cn` 域名，实测只有 19 KB/s。

**改用镜像直接下载**，实测 15~17 MB/s：

```bash
export IDF_MIRROR_BASE_URL="https://dl.espressif.cn/github_assets"
```

如果还是慢，可以用 curl 直接抓：

```
https://dl.espressif.cn/github_assets/espressif/crosstool-NG/releases/download/esp-14.2.0_20260121/xtensa-esp-elf-14.2.0_20260121-aarch64-linux-gnu.tar.xz
```

> **注意**：`tools/tools.json` 里记录的 size 是 176559292 字节，
> 但 CDN 上实际是 171504672 字节。**元数据是错的**，
> 用 `xz -t` 校验压缩包完整性，不要信 size 字段。

---

# 二、运行时：内存与延迟

## 2.1 内部 SRAM 是最大瓶颈

原始版本运行时内部 SRAM 只剩约 100KB，
**历史最低到过 547 字节**。

后果：`makeBlockChange()` 返回失败，后面发放物品的代码根本执行不到
—— 表现为「挖了方块拿不到东西」。

**解决**：把 `block_changes[20000]`（120KB）迁到 PSRAM。

```c
// main/globals.c
#ifdef ESP_PLATFORM
EXT_RAM_BSS_ATTR BlockChange block_changes[MAX_BLOCK_CHANGES];
#else
BlockChange block_changes[MAX_BLOCK_CHANGES];
#endif
```

需要同时开启：

```
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
```

**效果**：内部 SRAM 剩余 89KB → 219KB。

### 顺带调小的网络缓冲

原 v4 版本为了降延迟把缓冲调得过大，反而吃光内存：

| 参数 | 过大值 | 调整为 |
|---|---|---|
| `WIFI_STATIC_RX_BUFFER_NUM` | 16 | 8 |
| `TCP_SND_BUF` / `TCP_WND` | 32768 | 8192 |
| `MAX_SOCKETS` | 16 | 10 |

一个 MC 连接原本要占 64KB TCP 缓冲。

---

## 2.2 延迟的根因在代码，不在网络

**原始版本的局域网 ping：185ms 平均 / 393ms 峰值。**

排查下来三个原因：

### ① 每收一个包就 `vTaskDelay(1)`

```c
// main/globals.c 原版
void task_yield () {
  if (time_now - last_yield < TASK_YIELD_INTERVAL) return;
  vTaskDelay(1);        // 1000Hz 下至少 1ms
  last_yield = time_now;
}
```

MC 登录要往返几十个包，每个包多 1~2ms，累积就是秒级延迟。

**改为 `taskYIELD()`**（只让出同优先级，不进入 tick 等待队列）。

### ② 没设 `TCP_NODELAY`

Nagle 算法会攒小包等 ACK，最多延迟 40ms。MC 协议全是小包。

```c
int nodelay = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
```

监听 socket 和每个客户端 socket 都要设。

### ③ 忙等 `MSG_PEEK` 轮询

无数据时空转烧 CPU。加入 `select()` 等待。

**结果**：ping 平均 15.97ms，抖动 4.93ms。

> **踩坑记录**：`select()` 的超时一开始设成 200 微秒，
> 导致主循环每秒空转几千次，CPU 核1 长期 100%。
> 改成 5ms 后正常——有数据时 select 仍然立即返回，不影响延迟。

---

## 2.3 主循环 `continue` 的坑

```c
client_index++;
if (client_index == MAX_PLAYERS) client_index = 0;
if (clients[client_index] == -1) continue;   // ← 这一行跳过了后面所有逻辑
// ... 控制台命令、区块同步、服务器 tick、包处理
```

单玩家时 16 个槽位只有 1 个有连接，**约 94% 的循环轮次什么都没做**。

表现：

- Keep-Alive 每秒只发约 1/16 次 → 客户端断电 30 秒才发现断线
- 摔落伤害（在 `handleServerTick` 里）几乎不生效
- 挖方块的物品发放被饿死
- 游戏内命令因客户端超时看起来「没反应」

**已修复**：tick 和命令处理移到 `continue` 之前。

> 但这四个症状在真机上**仍未完全验证修复成功**，见下一节。

---

# 三、已知但未解决的问题

## 3.1 🔴 `DecoderException: unknown packet id 39`（最主要）

### 现象

客户端进入世界后偶发断线，报：

```
Internal Exception: io.netty.handler.codec.DecoderException:
  Received unknown packet id 39
```

同时可能伴随：**方块渲染成透明、走回去看到虚空**。

### 已确认的事实

- **`39` 是十进制，等于 `0x27`**，即 `Chunk Data and Update Light` 的包 ID
- 客户端说「不认识这个包」，说明**解析位置错位了**
- **包长度计算是对的**（见 `tests/test_mc_logic.c`，35 项测试验证过）

```
chunk_data_size = 91792      声明 == 实际
light_data_size = 53314      声明 == 实际
整包             = 145120    声明 == 实际
```

> 早期曾怀疑「光照段少报 1 字节」，后来写测试逐字节核算，
> **证明新旧公式算出来都是 53314，原实现长度是对的**。
> 这个判断是错的，别再走这条弯路。

### 尚未确认

- **字段顺序是否匹配 1.21.8 的协议定义**
  长度自洽不代表字段布局正确。如果一个 VarInt 的位置错 1 字节，
  总长度可以不变，但客户端会从错误的位置开始解析。

### 建议的查法

1. 拿一份**真实的 1.21.8 协议字段定义**
   - `minecraft.wiki/w/Java_Edition_protocol`（有完整字段顺序）
   - 或 `PrismarineJS/minecraft-data` 的 `data/pc/1.21.8/protocol.json`
2. 逐个字段对比 `main/packets.c` 的 `sc_chunkDataAndUpdateLight()`
3. **最可靠的办法：抓包对比**
   - 本地跑一个原版 1.21.8 服务器
   - Wireshark 抓「进入世界」时的流量
   - 找到客户端收到的 `0x27` 包，看真实字节
   - 和 ESP32 发出的对比

### 尝试过但失败的路径

| 方式 | 结果 |
|---|---|
| Mojang 协议报告（`notchian/generated/reports/packets.json`） | **只有包 ID，没有字段定义** |
| 反编译 `server.jar` | 类名混淆成 `s.class`、`y.class`，无法定位 |
| 从 npm 包 `minecraft-data` 提取 | 包内只有代码，数据在另一个仓库 |

---

## 3.2 区块同步：站着不动会加载不全

### 原实现的问题

```c
short dx = _x - 上一区块x;
while (dx != 0) { ... 发送区块 ... }    // dx == 0 时一次都不执行
```

只有跨越区块边界时才发送。**站着不动时一个区块都不补发。**

加上 `VISITED_HISTORY = 4`（只记最近 4 个区块），
走回旧位置会判定「来过」直接跳过，客户端显示虚空。

### 当前实现

每个玩家维护 `(2r+1)²` 的区块位图，
每帧在 30ms 时间预算内补发缺失区块。

```c
// main/globals.h
#define CHUNK_GRID ((2 * VIEW_DISTANCE + 1) * (2 * VIEW_DISTANCE + 1))
uint32_t chunk_bits[CHUNK_BITMAP_WORDS];
short chunk_center_x, chunk_center_z;
uint8_t chunk_sync_pending;
```

### 仍未验证

改完后**没有在真机上完整跑过**，不知道效果如何。
如果你测了，告诉我结果。

---

## 3.3 游戏内命令可能完全不响应

### 现象

在游戏里按 T 输入 `!give` / `/help` 等，**任何消息都没有反应**。
（控制台 TCP 命令是正常的。）

### 相关代码路径

```c
// main/main.c:177
case 0x08:
  if (state == STATE_PLAY) cs_chat(client_fd);
  break;
```

```c
// main/packets.c:1159
int cs_chat (int client_fd) {
  readStringN(client_fd, 224);   // 只读 Message 字段
  ...
}
```

框架会在 `handlePacket` 末尾统一跳过未处理的字节：

```c
int processed_length = total_bytes_received - bytes_received_start;
if (length > processed_length) {
  discard_all(client_fd, length - processed_length, false);
}
```

理论上安全。

### 可能的原因（未验证）

1. **聊天签名**：1.21.8 强制要求聊天签名。客户端在「仅安全聊天」模式下
   可能拒绝向不支持签名的服务器发送消息
2. **`recv_buffer[0] != '!'` 的判断**：如果 1.21.8 的聊天包在 Message
   之前还有别的字段，这里读到的是错误内容
3. **客户端根本没发包**（最容易验证，但也最容易被忽略）

### 建议的查法

开 `DEV_LOG_VERBOSE` 宏，看串口有没有打印收到的聊天内容：

```c
// main/globals.h
#define DEV_LOG_VERBOSE
```

---

## 3.4 其他未实现的功能

| 缺失 | 说明 |
|---|---|
| 下界 / 末地 | 只有方块 ID 常量，**没有维度切换逻辑**，所以打不了龙 |
| 附魔 / 酿造 / 红石 | 相关方块只有 ID，右键无效果 |
| TNT 引爆 | 只是个装饰方块 |
| 结构化地形 | 没有要塞、村庄、地牢 |
| 经验值 / 药水效果 | 完全没有 |
| 6 种以上生物 | 只有鸡、牛、猪、羊、僵尸 |
| 正版验证 | 任何用户名都能进，**切勿暴露公网** |

---

# 四、调试工具与技巧

## 4.1 本地逻辑测试（不需要硬件）

```bash
cd tests && ./run_tests.sh
```

35 项测试，覆盖：

- 区块包长度计算（**证明长度是对的**，推翻了早期的错误判断）
- VarInt 编解码边界
- 方块 → 物品掉落映射
- 物品堆叠上限与背包槽位分配
- 区块位图尺寸计算

**改代码后先跑这个**，能快速排除纯逻辑错误。

---

## 4.2 TCP 控制台（不需要串口）

端口 **25566**，局域网内直接连：

```bash
# 一行 JSON 状态
nc <板子IP> 25566

# 发命令
echo "list" | nc <板子IP> 25566
echo "status" | nc <板子IP> 25566
```

配合 `mc_console.py`：

```bash
python3 mc_console.py status --host <板子IP>
python3 mc_console.py watch  --host <板子IP>   # 每 2 秒刷新
python3 mc_console.py list
python3 mc_console.py cmd "give <玩家> <物品ID> 64"
```

**注意**：`give` 命令用的是**物品 ID，不是方块 ID**。
两套编号不同，比如草方块 ID 是 30，但物品 ID 是 27。

查看物品 ID：
```bash
grep '#define I_' include/registries.h | head -50
```

---

## 4.3 串口日志级别

```c
// main/globals.h
#define DEV_LOG_VERBOSE      // 恢复详细日志（默认关闭）
```

**默认关闭**是因为：串口 115200 波特率约 1ms/字符，
一行 50 字符的日志就是 **50ms 阻塞**。高频日志会显著拖慢主循环。

---

## 4.4 编译时的内存限制

在内存紧张的机器上编译，gcc 可能被系统 SIGKILL 杀掉。

```bash
ninja -C build -j 4     # 降低并行度
```

全量重编（改 sdkconfig 后）约 **9~10 分钟**，增量编译约 **1~2 分钟**。

---

## 4.5 Flash 模式必须是 DIO

部分 N16R8 板子是两线 SPI 走线。用 QIO 会导致：

```
rst:0x10 (RTCWDT_RTC_RST)
```

bootloader 读不了 Flash，RTC 看门狗无限复位。

本仓库默认已配置为 DIO —— **不要改回 QIO**。

---

# 五、如果你修好了

请开一个 Issue 说明你怎么解决的，或者在 PR 里附上：

- 具体的字段差异（比如「`sc_chunkDataAndUpdateLight` 的第 X 个字段应该是 Y 类型」）
- 测试方法（怎么验证的）
- 受影响的客户端版本

**这些信息对后来者非常有用。**
