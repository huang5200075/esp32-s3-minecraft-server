#!/usr/bin/env python3
"""
ESP32 Minecraft 服务器 — 本地监控 / 管理客户端

用法:
    python3 mc_console.py status            # 单次状态（JSON）
    python3 mc_console.py watch             # 每 2 秒刷新一次状态
    python3 mc_console.py list              # 在线玩家列表
    python3 mc_console.py kick <名字>       # 踢出玩家
    python3 mc_console.py say <消息>        # 全服广播
    python3 mc_console.py cmd <命令>        # 任意命令

可选:
    --host 192.168.1.100    板子 IP（默认从上次使用记录读取）
    --port 25566            监控端口（默认 25566）
"""
import socket
import sys
import time
import json
import argparse
import os

DEFAULT_PORT = 25566
CONF = os.path.expanduser("~/.mc_console_host")


def load_host(cli_host):
    if cli_host:
        return cli_host
    if os.path.exists(CONF):
        return open(CONF).read().strip()
    return "192.168.1.100"


def save_host(host):
    try:
        with open(CONF, "w") as f:
            f.write(host)
    except OSError:
        pass


def send_cmd(host, port, command, timeout=5.0):
    """发送一行命令，返回板子的完整回复文本。"""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect((host, port))
        if command:
            s.sendall((command + "\n").encode())
        # 半关闭，让板子知道我们发完了
        try:
            s.shutdown(socket.SHUT_WR)
        except OSError:
            pass

        chunks = []
        while True:
            try:
                data = s.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
        return b"".join(chunks).decode("utf-8", "replace")
    finally:
        s.close()


def get_status(host, port):
    """不发命令，纯状态查询（板子回一行 JSON）。"""
    raw = send_cmd(host, port, "")
    raw = raw.strip()
    if not raw:
        raise RuntimeError("板子无响应，确认 IP 和端口是否正确")
    return json.loads(raw)


def human_size(n):
    for unit in ("B", "KB", "MB"):
        if n < 1024:
            return f"{n:.0f}{unit}"
        n /= 1024
    return f"{n:.1f}GB"


def print_status(st):
    up = st.get("uptime_ms", 0) / 1000
    h, rem = divmod(int(up), 3600)
    m, s = divmod(rem, 60)

    print("=" * 46)
    print(f"  运行时间   : {h}小时{m}分{s}秒")
    print(f"  在线玩家   : {st.get('players', 0)}")
    print(f"  内存剩余   : {human_size(st.get('heap_free',0))}"
          f"  (历史最低 {human_size(st.get('heap_min',0))})")
    pt = st.get("psram_total", 0)
    if pt:
        print(f"  PSRAM      : {human_size(st.get('psram_free',0))} / {human_size(pt)}")
    print(f"  CPU 核0    : {st.get('cpu_core0',0)}%")
    print(f"  CPU 核1    : {st.get('cpu_core1',0)}%")
    temp = st.get("temp_c", -1)
    if temp and temp > 0:
        bar = "!" if temp > 70 else ""
        print(f"  芯片温度   : {temp:.1f}°C {bar}")
    print("=" * 46)


def main():
    ap = argparse.ArgumentParser(description="ESP32 MC 服务器控制台")
    ap.add_argument("action", nargs="?", default="status",
                    help="status | watch | list | kick | say | cmd")
    ap.add_argument("args", nargs="*", help="动作参数")
    ap.add_argument("--host", help="板子 IP")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--interval", type=float, default=2.0, help="watch 刷新间隔")
    a = ap.parse_args()

    host = load_host(a.host)
    save_host(host)

    try:
        if a.action == "status":
            print_status(get_status(host, a.port))

        elif a.action == "watch":
            print(f"监控 {host}:{a.port}    (Ctrl+C 退出)")
            while True:
                try:
                    st = get_status(host, a.port)
                    os.system("clear" if os.name != "nt" else "cls")
                    print_status(st)
                    print(f"  刷新: {time.strftime('%H:%M:%S')}")
                except Exception as e:
                    print(f"[{time.strftime('%H:%M:%S')}] 连接失败: {e}")
                time.sleep(a.interval)

        elif a.action == "list":
            print(send_cmd(host, a.port, "list"), end="")

        elif a.action == "kick":
            if not a.args:
                sys.exit("用法: kick <玩家名或编号>")
            print(send_cmd(host, a.port, "kick " + " ".join(a.args)), end="")

        elif a.action == "say":
            if not a.args:
                sys.exit("用法: say <消息>")
            print(send_cmd(host, a.port, "say " + " ".join(a.args)), end="")

        elif a.action == "cmd":
            if not a.args:
                sys.exit("用法: cmd <命令>")
            print(send_cmd(host, a.port, " ".join(a.args)), end="")

        else:
            sys.exit(f"未知动作: {a.action}")

    except KeyboardInterrupt:
        print("\n已退出")
    except ConnectionRefusedError:
        sys.exit(f"连接被拒绝：{host}:{a.port}\n"
                 f"  1) 确认板子已联网且 IP 正确\n"
                 f"  2) 确认烧的是 v6 及以上固件（带控制台）")
    except socket.timeout:
        sys.exit(f"连接超时：{host}:{a.port}")
    except json.JSONDecodeError as e:
        sys.exit(f"返回格式错误（固件版本过旧？）: {e}")


if __name__ == "__main__":
    main()
