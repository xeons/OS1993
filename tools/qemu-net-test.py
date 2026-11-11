#!/usr/bin/env python3
"""The network: NE2000, TCP/IP, WSOCK.DLL, PING and IPCONFIG.

QEMU's user-mode network gives the NE2000 an address by DHCP and reaches
the host as 10.0.2.2, where this script runs TCP and UDP echo servers.
NETTEST.BIN checks loopback UDP and TCP (with a server thread), refused
connections, ICMP port unreachable, timeouts, non-blocking sockets and
select, then echoes through the host. PING pings the host, the loopback,
and an address nobody has; IPCONFIG shows the lease. Last, NETTEST serves
on port 7 and this script connects to it through a forwarded port.
"""
import os
import random
import re
import socket
import sys
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qemu_harness import Checker, Machine, pixel  # noqa: E402

check = Checker()


def tcp_echo(server):
    while True:
        try:
            c, _ = server.accept()
        except OSError:
            return
        def serve(c=c):
            with c:
                while data := c.recv(65536):
                    c.sendall(data)
        threading.Thread(target=serve, daemon=True).start()


def udp_echo(sock):
    while True:
        try:
            data, who = sock.recvfrom(2048)
        except OSError:
            return
        sock.sendto(data, who)


def run(m, command):
    opened = m.log().count("GUI: run dialog")
    for k in ("ctrl-esc", "r"):
        m.key(k)
    m.wait_log("GUI: run dialog", opened + 1)
    m.type(command + "\n")


def close_console(m, prog, n):
    """The console window stays after the program: close it (Alt+F4)."""
    m.key("alt-f4")
    return m.wait_log(f"USER: {prog} exited", n, timeout=15)


def main():
    tcp = socket.socket()
    tcp.bind(("127.0.0.1", 0))
    tcp.listen(4)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    threading.Thread(target=tcp_echo, args=(tcp,), daemon=True).start()
    threading.Thread(target=udp_echo, args=(udp,), daemon=True).start()
    fwd = random.randint(20000, 40000)

    with Machine(hostfwd=(f"tcp:127.0.0.1:{fwd}-:7",)) as m:
        if not m.wait_log("PS/2 mouse"):
            raise SystemExit("  FAIL  VMM did not boot")
        check(m.wait_log("NET: NE2000 at 300h, IRQ 9, MAC 52:54:00:12:34:56"), "the NE2000 is found")
        check(m.wait_log("NET: DHCP: 10.0.2.15/255.255.255.0, gateway 10.0.2.2, DNS 10.0.2.3", timeout=20),
              "DHCP: 10.0.2.15, gateway 10.0.2.2, DNS 10.0.2.3")
        check(m.start_gui(), "the shell started")

        run(m, f"nettest.bin all {tcp.getsockname()[1]} {udp.getsockname()[1]}")
        m.wait_log("USER: NETTEST.BIN exited", timeout=90)
        log = m.log()
        for f in re.findall(r"NET: FAIL .*", log):
            print("        " + f)
        line = re.search(r"NET: (\d+) of (\d+) checks passed", log)
        check(line and line.group(1) == line.group(2) and int(line.group(2)) >= 20,
              line.group(0) if line else "NETTEST: no result")
        speed = re.search(r"NET: 64 KB each way in (\d+) ms", log)
        if speed:
            print(f"        (64 KB each way through the NE2000 in {speed.group(1)} ms)")

        run(m, "ping.bin -n 3 10.0.2.2")
        check(m.wait_log("GUI: console Ping"), "PING opens a console window")
        m.wait_log("Packets: Sent = 3", timeout=30)
        log = m.log()
        check(log.count("Reply from 10.0.2.2: bytes=32") == 3 and "Received = 3, Lost = 0 (0% loss)" in log,
              "ping 10.0.2.2: three replies")
        shot = m.screenshot("net-ping")
        dark = sum(1 for y in range(100, 280, 2) for x in range(30, 500, 2) if sum(pixel(shot, x, y)) < 120)
        check(dark > 300, f"... and its window shows them ({dark} dark pixels of text)")
        check(close_console(m, "PING.BIN", 1), "... closing its window ends it")

        run(m, "ping.bin -n 2 127.0.0.1")
        m.wait_log("Packets: Sent = 2", 2, timeout=30)
        check(m.log().count("Reply from 127.0.0.1: bytes=32") == 2, "ping 127.0.0.1: the loopback answers")
        close_console(m, "PING.BIN", 2)

        run(m, "ping.bin -n 2 -w 500 10.0.2.99")
        m.wait_log("Packets: Sent = 2", 3, timeout=30)
        log = m.log()
        check(log.count("Request timed out.") == 2 and "Received = 0, Lost = 2 (100% loss)" in log,
              "ping an address nobody has: timed out")
        close_console(m, "PING.BIN", 3)

        run(m, "ipconfig.bin")
        m.wait_log("Packets", 4, timeout=20)
        log = m.log()
        check(re.search(r"IP Address\s+: 10\.0\.2\.15", log) and re.search(r"Default Gateway\s+: 10\.0\.2\.2", log)
              and "52-54-00-12-34-56" in log, "IPCONFIG shows the lease and the MAC")
        close_console(m, "IPCONFIG.BIN", 1)

        run(m, "nettest.bin serve")
        check(m.wait_log("NET: listening on port 7", timeout=20), "NETTEST listens on port 7")
        data = bytes(random.getrandbits(8) for _ in range(20000))
        echoed = b""
        try:
            with socket.create_connection(("127.0.0.1", fwd), timeout=10) as c:
                c.sendall(data)
                c.shutdown(socket.SHUT_WR)
                while chunk := c.recv(65536):
                    echoed += chunk
        except OSError as e:
            print(f"        host connect: {e}")
        check(echoed == data, "the host connects in (a forwarded port) and gets its 20000 bytes back")
        check(m.wait_log("NET: served 20000 bytes", timeout=20), "... and the guest saw them all")

        check(m.shut_down(), "the GUI shuts down")
    return 1 if check.failures else 0


if __name__ == "__main__":
    sys.exit(main())
