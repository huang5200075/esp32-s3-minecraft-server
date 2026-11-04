# ESP32-S3 我的世界服务器（N16R8 优化版）

<p align="right">
<a href="https://github.com/huang5200075/esp32-s3-minecraft-server/blob/main/README.en.md">English</a> &nbsp;|&nbsp; <b>简体中文</b>
</p>

在 **ESP32-S3 N16R8**（16MB Flash + 8MB PSRAM）上运行一个可玩的 Minecraft Java 版服务器，
带 **TCP 管理控制台**，可通过局域网远程查看内存 / CPU / 温度并管理玩家。

> ⚠️ **重要声明，请务必先读**
>
> 本项目**大量使用 AI 工具辅助修改**，包括代码分析、性能优化、功能实现与文档撰写。
> 所有改动尚未经过完整测试，**存在多处已知与未知的 bug**，详见下方[已知问题](#已知问题)。
> 它是**技术演示与学习项目**，不适合作为正式生存服务器。

---

## 项目来源

本项目是以下两个项目的**二次修改版本**：

| 层级 | 项目 | 说明 |
|---|---|---|
| 原始内核 | [p2r3/bareiron](https://github.com/p2r3/bareiron) | 从零实现的 Minecraft 协议服务器，作者 p2r3 |
| 直接上游 | [jeyyprtf/esp32-mcserver](https://github.com/jeyyprtf/esp32-mcserver) | 把 bareiron 打包到 ESP32-S3 N16R8，提供一键配置与构建说明 |

本项目在 **jeyyprtf/esp32-mcserver** 基础上继续修改，遵循原项目 **GPL-3.0** 许可。

**这不是原作者的官方版本**，原项目的问题请勿归咎于上游作者。

---

## 相对上游新增 / 修改的内容

### 一、可用性修复（让它在 N16R8 上真正跑起来）

| 问题 | 修改 |
|---|---|
| 应用代码不参与编译，链接时找不到 `app_main` | 将 `src/` 更名为 `main/`，ESP-IDF 只识别名为 `main` 的组件 |
| 编译被大量缩进告警打断 | 对应用组件显式关闭 `-Werror=misleading-indentation` 等 |
| 组件子模块版本错配，`mbedcrypto` 目标不存在 | 按 superproject 记录的 commit 逐个 pin |
| 日志级别过高看不到启动过程 | 调整为 `INFO` |

### 二、内存优化（最大收益项）

原始版本内部 SRAM 只剩约 100KB，最低到过 **547 字节**，直接导致方块改动失败、物品发放失效。

| 改动 | 效果 |
|---|---|
| `block_changes[]`（120KB）迁到 PSRAM（`EXT_RAM_BSS_ATTR`） | 内部 SRAM 占用 73.8% → **35.9%** |
| WiFi 缓冲从 16/64/64 降到 8/16/16 | 释放约 25KB |
| TCP 窗口 32768 → 8192 | 每个连接从占 64KB 降到 16KB |
| `MAX_SOCKETS` 16 → 10 | 减少常驻开销 |

**结果：内部 SRAM 剩余 89KB → 219KB。**

### 三、网络延迟优化

原始版本局域网 ping 高达 **185ms 平均、393ms 峰值**，实测根因在代码而非网络：

| 问题 | 说明 | 修改 |
|---|---|---|
| 每收一个包就 `vTaskDelay(1)` | 1000Hz 下至少 1ms，登录要往返几十个包 | 改用 `taskYIELD()` |
| 未设 `TCP_NODELAY` | Nagle 算法攒包等 ACK，最多延迟 40ms | 对监听与每个客户端 socket 设置 |
| 忙等 `MSG_PEEK` 轮询 | 无数据时空转烧 CPU | 加入 `select()` 等待 |
| `RX_BA_WIN=6` | 接收聚合窗口过小 | 调整为 8 |

**结果：ping 平均 15.97ms、抖动 4.93ms（降约 11 倍）。**

### 四、区块同步重写

原始逻辑用「玩家移动增量」驱动区块发送，存在三个缺陷：

- `if (dx == 0 && dz == 0) break;` —— 站着不动时**一个区块都不发送**
- `VISITED_HISTORY = 4` —— 走回旧位置判定为「来过」，**不补发**，客户端显示虚空
- 一次性发送全部区块，阻塞主循环数秒

**修改为**：每个玩家维护 `(2r+1)²` 的区块位图，每帧在 30ms 时间预算内补发缺失区块。

### 五、TCP 管理控制台（新增功能）

**端口 25566**，无需在设备上接串口即可远程管理。

```
$ nc <板子IP> 25566
{"uptime_ms":123456,"heap_free":180000,"psram_free":7000000,
 "cpu_core0":12,"cpu_core1":8,"temp_c":52.3,"players":1}
```

发送文本命令（以换行结束）：

| 命令 | 作用 |
|---|---|
| `list` | 列出在线玩家（编号、名字、坐标、血量） |
| `kick <名字\|编号>` | 踢出玩家，客户端显示 "Kicked by console" |
| `say <文本>` | 全服广播 |
| `gm <0\|1\|2\|3>` | 切换游戏模式（0生存/1创造/2冒险/3观察） |
| `status` | 多行状态报告 |
| `help` | 命令列表 |

#### 配套本地客户端

仓库内提供 `mc_console.py`：

```bash
python3 mc_console.py status --host 192.168.1.100   # 单次状态
python3 mc_console.py watch  --host 192.168.1.100   # 每 2 秒刷新
python3 mc_console.py list                          # 在线玩家
python3 mc_console.py kick Steve                    # 踢人
python3 mc_console.py say 服务器5分钟后重启          # 广播
```

**设计说明**：控制台命令不会直接操作玩家 socket（那样会与主循环并发冲突，
曾导致玩家被误踢下线）。所有会写 socket 的命令都只设置标记，
由主循环统一执行，保证串行访问。

---

## 已知问题

> 以下问题在本版本中**依然存在**，欢迎有能力者继续完善。

1. **方块可能显示为透明**
   区块包（Chunk Data and Update Light）的长度计算曾少报 1 字节，
   导致客户端解码错位、抛 `DecoderException: packet id 39`。
   已按实际写出顺序重算修正，但**尚未经过长时间验证**，可能仍存在其他包长度偏差。

2. **没有下界与末地维度**
   代码中只有 `B_netherrack` 之类的方块 ID 常量，**没有任何维度切换逻辑**。
   因此**永远无法通关**：拿不到烈焰棒、也去不了末地打龙。

3. **生物只有 5 种**
   仅鸡、牛、猪、羊、僵尸。没有苦力怕、末影人、骷髅等。

4. **没有附魔 / 酿造 / 红石逻辑**
   相关方块只有 ID 定义，右键无任何效果。

5. **TNT 无法点燃**
   TNT 只是个装饰方块，没有爆炸、连锁破坏、伤害计算。

6. **没有要塞、村庄等地形结构**

7. **不支持正版验证**
   任何用户名都可进入。**切勿暴露到公网**，只适合可信的局域网。

8. **存档兼容性脆弱**
   修改 `MAX_BLOCK_CHANGES` 会导致旧 `world.bin` 无法读取，需擦除 Flash。

9. **CPU 占用偏高**
   主循环的 `select()` 超时加上性能统计后，实测核1 仍可能长期接近 100%。

---

## 硬件要求

- **ESP32-S3 N16R8**（16MB Flash + 8MB Octal PSRAM）—— 其他型号可能因内存不足无法运行
- USB 数据线
- **2.4GHz** WiFi（ESP32 不支持 5GHz）

## 客户端要求

- Minecraft **Java 版 1.21.8**（协议 772），其他版本无法连接
- 不支持基岩版（手机 / 主机版）

---

## 构建

需要 **ESP-IDF v5.4**（实测 v5.4.4）。

```bash
# 1. 生成注册表（需要 Java 21+ 和 Node.js，只需一次）
mkdir -p notchian
# 把 1.21.8 的 server.jar 放进 notchian/
./extract_registries.sh

# 2. 配置 WiFi
cp include/config.h.example include/config.h
# 编辑 include/config.h 填入 SSID 和密码

# 3. 编译
idf.py set-target esp32s3
idf.py build
```

> 子模块必须 pin 到 superproject 记录的 commit，否则会出现 `mbedcrypto not built` 错误。

## 烧录

```bash
esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 build/esp32-mcserver.bin
```

若使用合并固件（bootloader + 分区表 + 应用）：

```bash
esptool.py --chip esp32s3 merge_bin -o full.bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/esp32-mcserver.bin

esptool.py --chip esp32s3 -p /dev/ttyACM0 write_flash 0x0 full.bin
```

> **Flash 模式必须是 DIO**。部分 N16R8 板子走线为两线，用 QIO 会导致
> bootloader 读取 Flash 失败、RTC 看门狗无限复位。

## 使用

1. 烧录后重新上电，串口（115200）会打印：
   ```
   I (xxxx) esp_netif_handlers: sta ip: 192.168.1.xxx
   Server listening on port 25565...
   ```
   记下这个 IP。

2. 客户端用同一 2.4GHz WiFi，添加服务器，地址填该 IP。

3. 首次进入会等待数秒（世界按区块生成）。

### 游戏内命令

| 命令 | 作用 |
|---|---|
| `!gm 0~3` | 切换游戏模式 |
| `!tp <x> <y> <z>` | 传送 |
| `!give <物品>` | 给予物品 |
| `!time` | 设置时间 |
| `!help` | 查看全部 |

---

## 目录结构

```
├── main/              应用代码（ESP-IDF 要求组件名为 main）
│   ├── main.c         主循环、网络、区块同步
│   ├── monitor.c      TCP 管理控制台（本项目新增）
│   ├── packets.c      协议收发包
│   ├── procedures.c   游戏逻辑
│   ├── worldgen.c     地形生成
│   └── serialize.c    存档读写
├── include/
│   ├── config.h.example  配置模板
│   └── globals.h         全局定义
├── mc_console.py      本地控制台客户端
├── partitions.csv     分区表
└── sdkconfig.defaults 默认构建配置
```

---

## 许可证

**GPL-3.0**，继承自上游项目。

原项目版权归 p2r3（bareiron）与 jeyyprtf（esp32-mcserver）所有。
本项目的修改部分同样以 GPL-3.0 发布。

---

## 致谢

- [p2r3/bareiron](https://github.com/p2r3/bareiron) —— 从零实现的 Minecraft 协议内核
- [jeyyprtf/esp32-mcserver](https://github.com/jeyyprtf/esp32-mcserver) —— 打包与构建说明
- Espressif —— ESP-IDF 与硬件支持

---

**再次提醒：本项目由 AI 协助修改，存在大量未修复问题，仅适合学习与实验。**
