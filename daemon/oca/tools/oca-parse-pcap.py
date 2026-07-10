#!/usr/bin/env python3
# oca-parse-pcap.py - 离线解析 OCP.1 pcap 抓包
#
# 读 tcpdump 抓的 .pcap,按方向(seq 重组,正确处理重传/乱序)还原 TCP 流,
# 按 OCP.1 PDU 帧化并解码命令/响应,输出按时间排序的可读 transcript。
# 替代在线 MITM:抓包(tcpdump,你跑)与解析(本脚本,我跑,零依赖)分离,
# .pcap 持久可重跑,无代理在数据路径。
#
# 用法:
#   tcpdump -i <iface> -w captures/realtool.pcap port 65037   # 你跑(需 sudo)
#   python3 oca-parse-pcap.py captures/realtool.pcap           # 解析(无依赖)
#
# PDU 帧化逻辑复用 oca-mitm.py 的 parse_pdu(已验证)。支持 DLT_EN10MB
# (Ethernet,LAN 抓包)、DLT_LINUX_SLL(lo 抓包)、DLT_RAW(裸 IP)。
# Command/Response 按结构深度解码;Ntf/KeepAlive 给关键字段 + hex。

import struct
import sys

SYNC = 0x3B

PDU_TYPE = {0: "Command", 1: "CommandRrq", 2: "Ntf1",
            3: "Response", 4: "KeepAlive", 5: "Ntf2"}

STATUS = {0: "OK", 1: "ProtocolVersionError", 2: "DeviceError", 3: "Locked",
          4: "BadFormat", 5: "BadONo", 6: "ParameterError",
          7: "ParameterOutOfRange", 8: "NotImplemented", 9: "InvalidRequest",
          10: "ProcessingFailed", 11: "BadMethod", 12: "PartiallySucceeded",
          13: "Timeout", 14: "BufferOverflow"}

DLT_EN10MB = 1       # Ethernet
DLT_LINUX_SLL = 113  # Linux cooked-capture(lo 抓包常用)
DLT_RAW = 12         # 裸 IP(部分平台用 101/228)


def hexc(data, width=32):
    """紧凑 hex(超 width 字节截断)。"""
    if not data:
        return ""
    h = " ".join(f"{b:02x}" for b in data[:width])
    if len(data) > width:
        h += f" ...(+{len(data) - width}B)"
    return h


def status_name(b):
    return STATUS.get(b, str(b))


# ---- pcap 读取 --------------------------------------------------------------
def read_pcap(path):
    """返回 (linktype, [(ts_sec, ts_frac, frame), ...])。

    pcap 经典格式,微秒/纳秒、大小端自适应。"""
    with open(path, "rb") as f:
        gh = f.read(24)
    if len(gh) < 24:
        raise SystemExit("不是 pcap(全局头不足 24 字节)")
    magic = gh[:4]
    # magic 0xa1b2c3d4:大端机器写 -> 文件字节 a1 b2 c3 d4 -> 用大端读;
    # 小端机器写 -> 文件字节 d4 c3 b2 a1 -> 用小端读。纳秒变体 0xa1b23c4d 同理。
    if magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    elif magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    else:
        raise SystemExit("不是 pcap(magic=%s)" % magic.hex())
    linktype = struct.unpack(endian + "I", gh[20:24])[0]
    frames = []
    with open(path, "rb") as f:
        f.read(24)
        while True:
            ph = f.read(16)
            if len(ph) < 16:
                break
            ts_sec, ts_frac, incl_len, _ = struct.unpack(endian + "IIII", ph)
            frame = f.read(incl_len)
            if len(frame) < incl_len:
                break
            frames.append((ts_sec, ts_frac, frame))
    return linktype, frames


# ---- 链路层 / IP / TCP 剥离 -------------------------------------------------
def strip_link(frame, linktype):
    """返回 (src_ip, src_port, dst_ip, dst_port, seq, tcp_payload) 或 None。"""
    if linktype == DLT_EN10MB:
        if len(frame) < 14:
            return None
        ethertype = struct.unpack(">H", frame[12:14])[0]
        ip = frame[14:]
    elif linktype == DLT_LINUX_SLL:
        if len(frame) < 16:
            return None
        ethertype = struct.unpack(">H", frame[14:16])[0]
        ip = frame[16:]
    elif linktype in (DLT_RAW, 101, 228):
        ip = frame
        ethertype = 0x0800
    else:
        return None
    if ethertype != 0x0800 or len(ip) < 20:
        return None
    ihl = (ip[0] & 0x0F) * 4
    if ip[9] != 6:  # 非 TCP
        return None
    total_len = struct.unpack(">H", ip[2:4])[0]
    src_ip = ".".join(str(b) for b in ip[12:16])
    dst_ip = ".".join(str(b) for b in ip[16:20])
    tcp = ip[ihl:total_len]
    if len(tcp) < 20:
        return None
    src_port, dst_port, seq = struct.unpack(">HHI", tcp[0:8])
    payload = tcp[(tcp[12] >> 4) * 4:]
    return src_ip, src_port, dst_ip, dst_port, seq, payload


# ---- TCP 流重组(按方向,seq 填充,处理重传/乱序)---------------------------
class Stream:
    def __init__(self, label):
        self.label = label
        self.segs = []  # (seq, payload, ts)

    def add(self, seq, payload, ts):
        if payload:
            self.segs.append((seq, payload, ts))

    def reassemble(self):
        """返回 (data, marks)。marks=[(offset, ts)] 每段起始处,用于 PDU 取时间戳。"""
        if not self.segs:
            return b"", []
        base = min(s for s, _, _ in self.segs)
        result = bytearray()
        marks = []  # (offset, ts):每段在重组流中的起始偏移
        for seq, payload, ts in sorted(self.segs, key=lambda x: x[0]):
            off = (seq - base) & 0xFFFFFFFF
            end = off + len(payload)
            if end <= len(result):
                continue  # 已被覆盖(重传)
            if off > len(result):
                result.extend(b"\x00" * (off - len(result)))  # 缺口(完整抓包不应出现)
            marks.append((len(result), ts))
            start = max(0, len(result) - off)
            result.extend(payload[start:])
        return bytes(result), marks


def ts_at(marks, offset):
    """offset 所属段的起始时间戳。"""
    lo = 0
    for i, (o, _) in enumerate(marks):
        if o <= offset:
            lo = i
        else:
            break
    return marks[lo][1] if marks else (0, 0)


# ---- OCP.1 PDU 帧化 + 解码 --------------------------------------------------
def frame_pdus(buf):
    """从字节流帧化 PDU。生成 (pdu_type, msg_count, start_offset, pdu_bytes)。"""
    i = 0
    n = len(buf)
    while i < n:
        if buf[i] != SYNC:
            i += 1
            continue
        if i + 10 > n:
            break
        pdu_size = struct.unpack(">I", buf[i + 3:i + 7])[0]
        total = 1 + pdu_size
        if total < 10:
            i += 1
            continue
        if i + total > n:
            break  # 不完整,等更多(流已重组完则丢弃尾部)
        pdu_type = buf[i + 7]
        msg_count = struct.unpack(">H", buf[i + 8:i + 10])[0]
        yield pdu_type, msg_count, i, buf[i:i + total]
        i += total


def decode_command(body):
    out = []
    p, n = 0, len(body)
    while p + 17 <= n:
        cmd_size = struct.unpack(">I", body[p:p + 4])[0]
        if cmd_size < 17 or p + cmd_size > n:
            out.append(f"    [命令尺寸异常 cmdSize={cmd_size} @off={p}]")
            break
        handle, target = struct.unpack(">II", body[p + 4:p + 12])
        def_level, method_index = struct.unpack(">HH", body[p + 12:p + 16])
        nr_params = body[p + 16]
        params = body[p + 17:p + cmd_size]
        out.append(f"    COMMAND handle={handle} target={target} "
                   f"method={{{def_level},{method_index}}} nrParams={nr_params} "
                   f"params=[{hexc(params)}]")
        p += cmd_size
    return out


def decode_response(body):
    out = []
    p, n = 0, len(body)
    while p + 10 <= n:
        rsp_size = struct.unpack(">I", body[p:p + 4])[0]
        if rsp_size < 10 or p + rsp_size > n:
            out.append(f"    [响应尺寸异常 rspSize={rsp_size} @off={p}]")
            break
        handle = struct.unpack(">I", body[p + 4:p + 8])[0]
        status = body[p + 8]
        nr_params = body[p + 9]
        params = body[p + 10:p + rsp_size]
        out.append(f"    RESPONSE handle={handle} status={status_name(status)}"
                   f"({status}) nrParams={nr_params} "
                   f"params=[{hexc(params)}]")
        p += rsp_size
    return out


def decode_pdu(pdu_type, msg_count, pdu):
    body = pdu[10:]  # 跳过 sync(1)+header(9)
    name = PDU_TYPE.get(pdu_type, str(pdu_type))
    lines = [f"  PDU type={name} msgCount={msg_count} "
             f"pduSize={len(pdu) - 1} bytes={len(pdu)}"]
    if pdu_type in (0, 1):
        lines.extend(decode_command(body))
    elif pdu_type == 3:
        lines.extend(decode_response(body))
    elif pdu_type == 4:
        hb = struct.unpack(">H", body[:2])[0] if len(body) >= 2 else 0
        lines.append(f"    KEEPALIVE heartbeat={hb}s")
    elif pdu_type in (2, 5):
        lines.append(f"    NOTIFICATION bytes={len(body)} data=[{hexc(body)}]")
    return lines


def response_statuses(body):
    """提取 Response 消息体里的 status 字节(用于汇总)。"""
    sts = []
    p, n = 0, len(body)
    while p + 10 <= n:
        rsp_size = struct.unpack(">I", body[p:p + 4])[0]
        if rsp_size < 10 or p + rsp_size > n:
            break
        sts.append(body[p + 8])
        p += rsp_size
    return sts


# ---- 主流程 -----------------------------------------------------------------
def main():
    if len(sys.argv) < 2:
        raise SystemExit("用法: oca-parse-pcap.py <file.pcap>")
    path = sys.argv[1]
    linktype, frames = read_pcap(path)

    lk_name = {DLT_EN10MB: "Ethernet", DLT_LINUX_SLL: "SLL"}.get(
        linktype, "raw" if linktype in (DLT_RAW, 101, 228) else f"type={linktype}")
    print(f"# pcap: {path}")
    print(f"# linktype={linktype} ({lk_name})  frames={len(frames)}\n")

    # 按 4-tuple(单方向)分流
    streams = {}
    for ts_sec, ts_frac, frame in frames:
        r = strip_link(frame, linktype)
        if not r:
            continue
        s_ip, s_port, d_ip, d_port, seq, payload = r
        key = (s_ip, s_port, d_ip, d_port)
        st = streams.setdefault(key, Stream(f"{s_ip}:{s_port}->{d_ip}:{d_port}"))
        st.add(seq, payload, (ts_sec, ts_frac))

    # 每条流重组 + 帧化,PDU 绑定首字节时间戳
    events = []  # (ts_sec, ts_frac, label, dir_guess, pdu_type, msg_count, pdu)
    for key, st in streams.items():
        data, marks = st.reassemble()
        if not data:
            continue
        s_ip, s_port, d_ip, d_port = key
        label = f"{s_ip}:{s_port}->{d_ip}:{d_port}"
        for pdu_type, msg_count, off, pdu in frame_pdus(data):
            ts = ts_at(marks, off)
            dir_guess = ("C->S" if pdu_type in (0, 1)
                         else "S->C" if pdu_type in (2, 3, 5) else "---")
            events.append((ts[0], ts[1], label, dir_guess,
                           pdu_type, msg_count, pdu))

    events.sort(key=lambda e: (e[0], e[1]))

    n_cmd = n_rsp = n_ntf = n_ka = 0
    status_hist = {}
    for ts_sec, ts_frac, label, d, pdu_type, msg_count, pdu in events:
        tstr = f"{ts_sec}.{ts_frac:06d}"
        print(f"[t={tstr}] {d} {label}")
        for line in decode_pdu(pdu_type, msg_count, pdu):
            print(line)
        if pdu_type in (0, 1):
            n_cmd += msg_count
        elif pdu_type == 3:
            n_rsp += msg_count
            for st in response_statuses(pdu[10:]):
                nm = status_name(st)
                status_hist[nm] = status_hist.get(nm, 0) + 1
        elif pdu_type in (2, 5):
            n_ntf += msg_count
        elif pdu_type == 4:
            n_ka += 1
        print()

    print("=== 汇总 ===")
    print(f"commands={n_cmd} responses={n_rsp} "
          f"notifications={n_ntf} keepalive={n_ka}")
    if status_hist:
        print("response 状态分布: " + ", ".join(
            f"{k}={v}" for k, v in sorted(status_hist.items(),
                                          key=lambda x: -x[1])))


if __name__ == "__main__":
    main()
