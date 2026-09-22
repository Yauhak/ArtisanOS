#!/usr/bin/env python3
"""ARSFS 串口终端：与 RP2040 上的 ArtisanOS 通信、收发文件。

用法：
    python ARSTerm.py [串口] [--baud 115200]
例如：
    python ARSTerm.py COM3
    python ARSTerm.py /dev/ttyACM0

交互命令：
    update <localfile> <dest_name>   上传文件，不存在则创建
    get <dest_name>                  下载并保存为 <随机数>.txt（HEX 文本）
    del <dest_name>                  删除文件
    ls                               列出文件
    occ [text]                       查看 FLASH 占用（空闲绿 / 占用红；text=纯文本）
    ver                              查询固件元数据版本与已装载任务数
    reboot <name>                    重启调度计划里的某一个任务（改了程序后用这个）
    reboot_all                       重置整个堆并按 SCHEDULE 重新装载全部任务
    format                           恢复出厂设置：清空文件系统并铺回出厂示例（会二次确认）
    help / quit

依赖：pyserial      pip install pyserial
"""

import os
import re
import random
import sys

try:
    import serial
except ImportError:
    print("需要 pyserial：pip install pyserial")
    sys.exit(1)

PROMPT = "arsfs> "
MAX_CHUNK = 128
FILE_MAX = 2048
TIMEOUT = 3.0


class ArsfsTerm:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.buf = b""

    # ---------- 低层收发 ----------
    def write_line(self, s: str):
        self.ser.write(s.encode() + b"\n")
        self.ser.flush()

    def read_line(self, timeout=TIMEOUT):
        """按行读，返回去掉行尾的字符串；超时返回 None。"""
        line = b""
        deadline = self.ser.timeout
        self.ser.timeout = 0.2
        waited = 0.0
        while True:
            ch = self.ser.read(1)
            if ch:
                if ch in (b"\n", b"\r"):
                    if line:
                        self.ser.timeout = deadline
                        return line.decode(errors="replace")
                    continue
                line += ch
            else:
                waited += 0.2
                if waited >= timeout:
                    self.ser.timeout = deadline
                    return None

    def read_exact(self, n, timeout=TIMEOUT):
        got = b""
        waited = 0.0
        old = self.ser.timeout
        self.ser.timeout = 0.2
        while len(got) < n and waited < timeout:
            chunk = self.ser.read(n - len(got))
            if chunk:
                got += chunk
                waited = 0.0
            else:
                waited += 0.2
        self.ser.timeout = old
        return got

    # ---------- 命令 ----------
    def cmd_ls(self):
        self.write_line("ls")
        while True:
            line = self.read_line()
            if line is None:
                print("  超时")
                return
            if line == "LS":
                continue
            if line == "OK":
                return
            print("  " + line)

    def cmd_occ(self, extra=""):
        self.write_line(("occ " + extra).strip())
        color = False
        while True:
            line = self.read_line()
            if line is None:
                print("  超时")
                return
            if line == "OK":
                return
            # 把 ANSI 颜色码翻译成终端上的着色输出
            out = ""
            i = 0
            while i < len(line):
                m = re.match(r"\x1b\[(\d+)m", line[i:])
                if m:
                    code = int(m.group(1))
                    if code == 31:
                        out += "\x1b[31m"
                    elif code == 32:
                        out += "\x1b[32m"
                    elif code == 0:
                        out += "\x1b[0m"
                    i += m.end()
                else:
                    out += line[i]
                    i += 1
            print("  " + out + "\x1b[0m")

    def cmd_ver(self):
        self.write_line("ver")
        print("  " + (self.read_line() or "超时"))

    def cmd_reboot(self, line):
        """把整行原样发过去（reboot <name> 或 reboot_all）"""
        self.write_line(line)
        print("  " + (self.read_line() or "超时"))

    def cmd_format(self):
        if input("  确认恢复出厂设置？会清空所有文件 (y/N): ").strip().lower() != "y":
            print("  已取消")
            return
        self.write_line("format")
        print("  " + (self.read_line(15.0) or "超时"))

    def cmd_del(self, name):
        self.write_line(f"del {name}")
        line = self.read_line()
        print("  " + (line or "超时"))

    def cmd_update(self, local, dest):
        if not os.path.isfile(local):
            print(f"  本地文件不存在：{local}")
            return
        data = open(local, "rb").read()
        if len(data) > FILE_MAX:
            print(f"  文件过大（{len(data)} > {FILE_MAX} 字节）")
            return
        self.write_line(f"update {dest} {len(data)}")
        line = self.read_line()
        if line != "RDY":
            print("  设备未就绪：" + str(line))
            return
        sent = 0
        while sent < len(data):
            chunk = data[sent:sent + MAX_CHUNK]
            self.ser.write(chunk)
            self.ser.flush()
            ack = self.read_line()
            if ack is None or not ack.startswith("ACK"):
                print("  传输中断：" + str(ack))
                return
            sent += len(chunk)
            print(f"\r  已发送 {sent}/{len(data)} 字节", end="", flush=True)
        print()
        result = self.read_line()
        print("  " + (result or "超时"))

    def cmd_get(self, name):
        self.write_line(f"get {name}")
        line = self.read_line()
        if line is None or not line.startswith("DATA"):
            print("  下载失败：" + str(line))
            return
        try:
            n = int(line.split()[1])
        except (IndexError, ValueError):
            print("  应答格式错误：" + line)
            return
        # 读回 n 个字节的 HEX 文本（设备每 32 字节换一次行）
        hexbuf = ""
        while len(hexbuf) < n * 2:
            chunk = self.read_exact(64)
            if not chunk:
                break
            hexbuf += "".join(ch for ch in chunk.decode(errors="replace") if ch in "0123456789abcdefABCDEF")
        if len(hexbuf) < n * 2:
            print("  数据不完整，已接收 %d/%d 字节" % (len(hexbuf) // 2, n))
        raw = bytes.fromhex(hexbuf[: n * 2])
        fname = f"{random.randint(0, 0xFFFFFFFF):08X}.txt"
        with open(fname, "w", encoding="utf-8") as f:
            f.write(raw.hex().upper())
        print(f"  已保存 {n} 字节到 {fname}")


def enable_ansi():
    """Windows 的传统控制台默认不解析 ANSI 转义序列，需要手动打开 VT 处理，
    否则 `occ` 的彩色输出会被当成普通文本显示成 ←[31m 之类。"""
    if os.name != "nt":
        return True
    try:
        import ctypes
        k = ctypes.windll.kernel32
        h = k.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not k.GetConsoleMode(h, ctypes.byref(mode)):
            return False
        return bool(k.SetConsoleMode(h, mode.value | 0x0004))  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        return False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    port = args[0] if args else None
    baud = 115200
    if "--baud" in sys.argv:
        try:
            baud = int(sys.argv[sys.argv.index("--baud") + 1])
        except (IndexError, ValueError):
            pass
    if not port:
        try:
            from serial.tools import list_ports
            ports = list(list_ports.comports())
        except ImportError:
            ports = []
        if not ports:
            print("未指定串口，且未发现可用设备。用法：python ARSTerm.py COM3")
            return
        print("可用串口：")
        for p in ports:
            print(f"  {p.device}  {p.description}")
        port = input("请选择：").strip()

    try:
        term = ArsfsTerm(port, baud)
    except serial.SerialException as e:
        print("打开串口失败：" + str(e))
        return

    print(f"已连接 {port} @ {baud}，输入 help 查看命令。")
    if not enable_ansi():
        print("提示：当前控制台不支持 ANSI 颜色，`occ` 请用 `occ text` 看纯文本。")
    while True:
        try:
            line = input(PROMPT).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break
        if not line:
            continue
        parts = line.split()
        cmd = parts[0].lower()
        try:
            if cmd == "help":
                print(__doc__)
            elif cmd == "ls":
                term.cmd_ls()
            elif cmd == "occ":
                term.cmd_occ(" ".join(parts[1:]))
            elif cmd == "ver":
                term.cmd_ver()
            elif cmd in ("reboot", "reboot_all", "rebootall", "restart", "restart_all"):
                term.cmd_reboot(line)
            elif cmd == "format":
                term.cmd_format()
            elif cmd == "del" and len(parts) >= 2:
                term.cmd_del(parts[1])
            elif cmd == "update" and len(parts) >= 3:
                term.cmd_update(parts[1], parts[2])
            elif cmd == "get" and len(parts) >= 2:
                term.cmd_get(parts[1])
            elif cmd in ("quit", "exit", "q"):
                break
            else:
                print("  未知命令，输入 help 查看用法")
        except serial.SerialException as e:
            print("  串口错误：" + str(e))
    term.ser.close()


if __name__ == "__main__":
    main()
