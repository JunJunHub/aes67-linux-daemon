#!/usr/bin/env python3
# oca-mitm.py - OCP.1 中间人抓包代理
#
# 监听 LISTEN_PORT,把连接转发到 daemon 的 TARGET_HOST:TARGET_PORT,
# 双向 dump 每个 OCP.1 PDU 的原始字节(含 sync)。用于抓官方合规工具实际
# 发出的命令字节,裁决 Ocp1Command 格式(paramCount 字段之争)。
#
# 用法:
#   python3 oca-mitm.py                 # 默认 65038 -> 127.0.0.1:65037
#   python3 oca-mitm.py 65038 127.0.0.1 65037
#
# 抓到的 PDU 按方向标注 [C->S](客户端发) / [S->C](daemon 回),十六进制 dump。

import socket
import select
import sys
import threading


def hexdump(data, prefix="    "):
    """可读十六进制 dump,每行 16 字节 + ASCII。"""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexpart = " ".join(f"{b:02x}" for b in chunk)
        asciipart = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{prefix}{i:04x}  {hexpart:<48}  {asciipart}")
    return "\n".join(lines)


def parse_pdu(buf):
    """尝试从 buf 头部解析一个完整 OCP.1 PDU。返回 (pdu_bytes, consumed) 或 (None, 0)。"""
    if len(buf) < 10:  # sync + 9 头
        return None, 0
    if buf[0] != 0x3B:
        return None, 0  # 未同步,交给调用方丢字节
    proto = (buf[1] << 8) | buf[2]
    pdu_size = (buf[3] << 24) | (buf[4] << 16) | (buf[5] << 8) | buf[6]
    pdu_type = buf[7]
    msg_count = (buf[8] << 8) | buf[9]
    total = 1 + pdu_size  # sync + (header+payload)
    if len(buf) < total:
        return None, 0  # 不完整,等更多数据
    return buf[:total], total, proto, pdu_size, pdu_type, msg_count


PDU_TYPE_NAMES = {0: "Command", 1: "CommandRrq", 2: "Ntf1",
                  3: "Response", 4: "KeepAlive", 5: "Ntf2"}


def relay(src, dst, direction, label):
    """把 src 收到的数据转发到 dst,并 dump 每个 PDU。"""
    buf = b""
    while True:
        try:
            data = src.recv(4096)
        except OSError:
            break
        if not data:
            break
        buf += data
        # 尝试逐个 PDU 解析
        while len(buf) >= 10 and buf[0] == 0x3B:
            proto = (buf[1] << 8) | buf[2]
            pdu_size = (buf[3] << 24) | (buf[4] << 16) | (buf[5] << 8) | buf[6]
            pdu_type = buf[7]
            msg_count = (buf[8] << 8) | buf[9]
            total = 1 + pdu_size
            if len(buf) < total:
                break  # 等更多数据
            pdu = buf[:total]
            buf = buf[total:]
            # 打印
            print(f"\n[{label}] {direction} PDU ({len(pdu)} bytes) "
                  f"type={PDU_TYPE_NAMES.get(pdu_type, pdu_type)} "
                  f"msgCount={msg_count} pduSize={pdu_size} proto={proto}")
            print(hexdump(pdu))
            sys.stdout.flush()
            # 转发原始字节
            try:
                dst.sendall(pdu)
            except OSError:
                return
        # 若 buffer 头部未同步(非 0x3B),丢一个字节继续(理论上不应发生)
        if buf and buf[0] != 0x3B:
            # 找下一个 sync
            idx = buf.find(b"\x3b")
            if idx < 0:
                buf = b""
            else:
                buf = buf[idx:]
    try:
        dst.shutdown(socket.SHUT_WR)
    except OSError:
        pass


def handle_client(client, addr, target_host, target_port):
    label = f"conn@{addr[0]}:{addr[1]}"
    print(f"\n=== 新连接 {label} -> {target_host}:{target_port} ===")
    sys.stdout.flush()
    try:
        upstream = socket.create_connection((target_host, target_port))
    except OSError as e:
        print(f"[{label}] 连接 daemon 失败: {e}")
        client.close()
        return
    t1 = threading.Thread(target=relay, args=(client, upstream, "C->S", label),
                          daemon=True)
    t2 = threading.Thread(target=relay, args=(upstream, client, "S->C", label),
                          daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    client.close()
    upstream.close()
    print(f"=== {label} 断开 ===")


def main():
    listen_port = int(sys.argv[1]) if len(sys.argv) > 1 else 65038
    target_host = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    target_port = int(sys.argv[3]) if len(sys.argv) > 3 else 65037

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", listen_port))
    srv.listen(8)
    print(f"OCP.1 MITM 抓包:监听 0.0.0.0:{listen_port} -> "
          f"{target_host}:{target_port}")
    print("官方工具连此机器的 IP:{listen_port}".format(listen_port=listen_port))
    print("Ctrl-C 退出\n")
    sys.stdout.flush()
    try:
        while True:
            client, addr = srv.accept()
            threading.Thread(target=handle_client,
                             args=(client, addr, target_host, target_port),
                             daemon=True).start()
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        srv.close()


if __name__ == "__main__":
    main()
