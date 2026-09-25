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
    start <name>                     手动启动一个存放在 FLASH 中的任务
    kill <name>                      终止所有同名任务（只杀实例，文件与 SCHEDULE 都不动）
    killid <ID>                      按任务 ID 终止一个（ID 就是槽位号，用 tasks 查）
    tasks                            列出当前存活任务与它们的 ID
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
import time

try:
    import serial
except ImportError:
    print("需要 pyserial：pip install pyserial")
    sys.exit(1)

PROMPT = "arsfs> "
MAX_CHUNK = 128
FILE_MAX = 2048
TIMEOUT = 3.0
# 固定的小超时：read_line 只靠"截止时间"控制超时，绝不再中途改 ser.timeout。
# 在 Windows 上给 pyserial 赋 timeout 会触发一次 SetCommState，而 CDC 那边只要
# 还有一次写没落地，SetCommState 就会以 ERROR_SEM_TIMEOUT(121) 失败——
# 表现就是"上传到一半串口炸了、每次错得还不一样"。所有读都固定用它。
POLL_TIMEOUT = 0.02


class ArsfsTerm:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=POLL_TIMEOUT)
        time.sleep(0.2)
        self.drain()
        self.buf = b""

    # ---------- 低层收发 ----------
    def drain(self, quiet=0.05, limit=1.5):
        """把接收缓冲读干净（上一次没读完的应答、被中断的操作留下的数据）。

        每条命令之前都做一次：只要有一点点残留在缓冲里，`read_line` 就会读到
        **上一条命令的应答**，之后每条命令都错开一位——症状是"每次报的错都不一样"。
        """
        """
        t_end = time.time() + limit
        last = time.time()
        while time.time() < t_end:
            if self.ser.read(4096):
                last = time.time()
            elif time.time() - last >= quiet:
                break
        """

    def write_line(self, s: str):
        self.ser.write(s.encode() + b"\n")
        self.ser.flush()

    def read_line(self, timeout=TIMEOUT):
        """按行读，返回去掉行尾的字符串；超时返回 None。"""
        line = b""
        t_end = time.time() + timeout
        while time.time() < t_end:
            ch = self.ser.read(1)
            if not ch:
                continue
            if ch in (b"\n", b"\r"):
                if line:
                    return line.decode(errors="replace")
                continue
            line += ch
        return None

    def wait_for(self, prefixes, timeout=5.0, quiet_report=True):
        """等到一条以 prefixes 里任一前缀开头的应答；路上的杂音跳过并报告。

        返回 (命中行, 跳过的行列表)；超时返回 (None, 跳过的行)。
        """
        skipped = []
        t0 = time.time()
        while time.time() - t0 < timeout:
            l = self.read_line(1.5)
            if l is None:
                break
            if any(l.startswith(p) for p in prefixes):
                return l, skipped
            skipped.append(l)
            if quiet_report and len(skipped) <= 5:
                print("  (跳过 " + l + ")")
        return None, skipped

    def read_exact(self, n, timeout=TIMEOUT):
        """读满 n 字节（或超时）。同样不改 ser.timeout。"""
        got = b""
        t_end = time.time() + timeout
        while len(got) < n and time.time() < t_end:
            chunk = self.ser.read(n - len(got))
            if chunk:
                got += chunk
        return got

    # ---------- 命令 ----------
    def cmd_ls(self):
        self.drain()
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
        self.drain()
        self.write_line(("occ " + extra).strip())
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
        self.drain()
        self.write_line("ver")
        print("  " + (self.read_line() or "超时"))

    def cmd_start(self, name):
        self.drain()
        self.write_line(f"start {name}")
        print("  " + (self.read_line() or "超时"))

    def cmd_kill(self, name):
        self.drain()
        self.write_line(f"kill {name}")
        print("  " + (self.read_line() or "超时"))

    def cmd_killid(self, tid):
        self.drain()
        self.write_line(f"killid {tid}")
        print("  " + (self.read_line() or "超时"))

    def cmd_tasks(self):
        """列出存活任务：TASKS <n> → 每行 "<ID> <名字>" → OK"""
        self.drain()
        self.write_line("tasks")
        while True:
            line = self.read_line()
            if line is None:
                print("  超时")
                return
            if line.startswith("TASKS"):
                print("  当前存活 " + line[5:].strip() + " 个任务：")
                continue
            if line == "OK":
                return
            print("  " + line)

    def cmd_reboot(self, line):
        """把整行原样发过去（reboot <name> 或 reboot_all）"""
        self.drain()
        self.write_line(line)
        print("  " + (self.read_line() or "超时"))

    def cmd_format(self):
        if input("  确认恢复出厂设置？会清空所有文件 (y/N): ").strip().lower() != "y":
            print("  已取消")
            return
        self.drain()
        self.write_line("format")
        print("  " + (self.read_line(15.0) or "超时"))

    def cmd_del(self, name):
        self.drain()
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
        self.drain()
        self.write_line(f"update {dest} {len(data)}")
        # 等 RDY：万一前面还有半条应答没读完，这里把杂音跳过去，不要直接判失败
        line, _ = self.wait_for(["RDY", "ERR"], timeout=3.0)
        if line != "RDY":
            print("  设备未就绪：" + str(line))
            self.drain()
            return
        sent = 0
        while sent < len(data):
            chunk = data[sent : sent + MAX_CHUNK]
            self.ser.write(chunk)
            self.ser.flush()
            ack, _ = self.wait_for(["ACK", "ERR"], timeout=6.0)
            if ack is None:
                print("\n  传输中断：没等到 ACK（宿主这头卡过 5 秒以上）。")
                print("  已清空接收缓冲，重新发一次 command 即可。")
                self.drain(quiet=0.3)
                return
            if ack.startswith("ERR"):
                print("\n  " + ack)
                self.drain(quiet=0.3)
                return
            sent += len(chunk)
            print(f"\r  已发送 {sent}/{len(data)} 字节", end="", flush=True)
        print()
        # 落盘结果：OK / ERR write
        result, _ = self.wait_for(["OK", "ERR"], timeout=10.0)
        print("  " + (result or "超时"))
        if result is None:
            self.drain(quiet=0.3)

    def cmd_get(self, name):
        self.drain()
        self.write_line(f"get {name}")
        line = self.read_line()
        if line is None or not line.startswith("DATA"):
            print("  下载失败：" + str(line))
            self.drain()
            return
        try:
            n = int(line.split()[1])
        except (IndexError, ValueError):
            print("  应答格式错误：" + line)
            self.drain()
            return
        # 读回 n 个字节的 HEX 文本（设备每 32 字节换一次行），读到 OK 为止
        buf = ""
        t0 = time.time()
        while len(buf) < n and time.time() - t0 < 20:
            l = self.read_line(2.0)
            if l is None or l == "OK":
                break
            for i in range(0, int(len(l) / 2)):
                buf += "".join(chr(int(l[i * 2] + l[i * 2 + 1], 16)))
        if len(buf) < n:
            print("  数据不完整，已接收 %d/%d 字节" % (len(buf), n))
            self.drain(quiet=0.3)
        fname = os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            f"{random.randint(0, 0xFFFFFFFF):08X}.txt",
        )
        with open(fname, "w", encoding="utf-8") as f:
            f.write(buf)
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
        return bool(
            k.SetConsoleMode(h, mode.value | 0x0004)
        )  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
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
        print("  · 串口是独占的：Arduino IDE 的串口监视器、别的终端只要开着，这边就打不开。")
        print("    请确认 IDE 的串口监视器已关闭（关了它会自己重启，要连 IDE 一起关）。")
        print("  · 板子如果刚从 BOOTSEL 模式回来，等 2 秒再试。")
        return

    print(f"已连接 {port} @ {baud}，输入 help 查看命令。")
    if not enable_ansi():
        print("提示：当前控制台不支持 ANSI 颜色，`occ` 请用 `occ text` 看纯文本。")
    while True:
        try:
            line = input(PROMPT).strip()
        except KeyboardInterrupt:
            # 中途 Ctrl-C 很容易把一条应答读一半，读干净再继续，否则后面全错位
            print("\n  (已中断，清空接收缓冲)")
            term.drain(quiet=0.4)
            continue
        except EOFError:
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
            elif cmd == "tasks":
                term.cmd_tasks()
            elif cmd == "start" and len(parts) >= 2:
                term.cmd_start(parts[1])
            elif cmd == "kill" and len(parts) >= 2:
                term.cmd_kill(parts[1])
            elif cmd == "killid" and len(parts) >= 2:
                term.cmd_killid(parts[1])
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
