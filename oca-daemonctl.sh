#!/bin/bash
#
# AES67 daemon control script for manual verification.
#
# Usage:
#   ./oca-daemonctl.sh start   [-i <iface>] [--ptp-iface <iface>] [--no-ptp]
#                          [-p <port>] [-d <ptp_domain>] [-a <addr>] [--unload-on-stop]
#                          [--oca] [--daemon-bin <path>]
#   ./oca-daemonctl.sh stop    [--unload]
#   ./oca-daemonctl.sh restart [-i <iface>] [--ptp-iface <iface>] [--no-ptp] ...
#   ./oca-daemonctl.sh status
#
# Options:
#   -i <iface>            network interface for the daemon (default: lo).
#                         Single NIC (e.g. ens192) for SAP/mDNS on LAN. Two interfaces
#                         comma-separated (e.g. ens160,ens192) enable ST-2022-7
#                         redundancy: PTP/RTP on primary+secondary, SAP/mDNS on all.
#                         Primary = first (for pidfile/log/ptp4l default).
#   --ptp-iface <iface>   interface for ptp4l (default: same as the PRIMARY -i).
#                         Single ptp4l instance. In VMs the virtual NIC often has no
#                         PTP clock ("failed to create a clock"); use --ptp-iface lo
#                         to run the master on loopback.
#   --no-ptp              skip ptp4l entirely (browse/WebUI OK, but no audio send/recv)
#   -p <port>             HTTP server port (default: 8080, from daemon.conf)
#   -d <domain>           PTPv2 domain 0..127 (default: from daemon.conf)
#   -a <addr>             HTTP server bind address (default: 0.0.0.0)
#   --unload              also rmmod the kernel module on stop
#
# The interface/ptp_domain/port are applied by generating a temporary config
# file (/tmp/aes67-daemon.<iface>.conf) from daemon.conf; the original
# daemon.conf is never modified. ptp4l is started on the SAME interface so
# the PTP slave can lock to it (or to a real grandmaster on that network).
#
# Logs:  /tmp/ptp4l.<ptp-iface>.log, /tmp/aes67-daemon.<iface>.log
# PIDs:  /tmp/aes67-ptp4l.<ptp-iface>.pid, /tmp/aes67-daemon.<iface>.pid
# Conf:  /tmp/aes67-daemon.<iface>.conf  (generated from daemon.conf; original untouched)
#
# Tested on Ubuntu 24.04
#

set -u

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON_BIN="$TOPDIR/daemon/aes67-daemon"
DAEMON_CONF="$TOPDIR/daemon/daemon.conf"
KMOD="$TOPDIR/3rdparty/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko"
PTP4L=/usr/sbin/ptp4l

IFACE="lo"
HTTP_ADDR="0.0.0.0"
HTTP_PORT=""
PTP_DOMAIN=""
PTP_IFACE=""          # interface for ptp4l (default: same as IFACE). Set to lo to
                      # run ptp4l on loopback when the real NIC has no usable clock
                      # (common in VMs). daemon's PTP slave still runs on $IFACE.
NO_PTP=0              # 1 = skip ptp4l entirely (SAP/mDNS browse & WebUI work, but
                      # audio send/recv needs a PTP lock so it won't work)
UNLOAD_ON_STOP=0
OCA_ENABLED=0         # 1 = enable OCA control plane (oca_enabled=true) + use the
                      # OCA-built binary (via --oca / --daemon-bin). The daemon then
                      # also publishes _oca._tcp mDNS for Fitcan/AES70 controllers.
DAEMON_BIN_OVERRIDE="" # --daemon-bin <path>: use a specific daemon binary (e.g. the
                      # out-of-source OCA build from oca-dev.sh build --real).

log()  { echo "[daemonctl] $*"; }
warn() { echo "[daemonctl] WARNING: $*" >&2; }
die()  { echo "[daemonctl] ERROR: $*" >&2; exit 1; }

# ---- parse global options (apply to start/restart) ---------------------------
parse_opts() {
  IFACE="lo"; HTTP_ADDR="0.0.0.0"; HTTP_PORT=""; PTP_DOMAIN=""; PTP_IFACE=""; NO_PTP=0; UNLOAD_ON_STOP=0; OCA_ENABLED=0; DAEMON_BIN_OVERRIDE=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -i) IFACE="${2:-}"; shift 2 ;;
      -p) HTTP_PORT="${2:-}"; shift 2 ;;
      -d) PTP_DOMAIN="${2:-}"; shift 2 ;;
      -a) HTTP_ADDR="${2:-}"; shift 2 ;;
      --ptp-iface) PTP_IFACE="${2:-}"; shift 2 ;;
      --no-ptp) NO_PTP=1; shift ;;
      --unload|--unload-on-stop) UNLOAD_ON_STOP=1; shift ;;
      --oca) OCA_ENABLED=1; shift ;;
      --daemon-bin) DAEMON_BIN_OVERRIDE="${2:-}"; shift 2 ;;
      *) die "unknown option: $1" ;;
    esac
  done
  [ -n "$IFACE" ] || die "interface name cannot be empty"
  # -i 支持 ST-2022-7 主备双网卡:"ens160,ens192"(逗号分隔,daemon interface_name
  # 原生支持)。主接口=逗号前第一个,用于 pidfile/log/ptp4l 默认值。
  PRIMARY_IFACE="${IFACE%%,*}"
  # ptp4l interface defaults to the PRIMARY daemon interface (单实例 ptp4l)
  [ -n "$PTP_IFACE" ] || PTP_IFACE="$PRIMARY_IFACE"
  # --oca implies using the OCA-built binary unless --daemon-bin overrides it
  if [ -n "$DAEMON_BIN_OVERRIDE" ]; then
    DAEMON_BIN="$DAEMON_BIN_OVERRIDE"
  elif [ "$OCA_ENABLED" = 1 ]; then
    # prefer the out-of-source OCA build (oca-dev.sh build --real), fall back to in-source
    if [ -x "$TOPDIR/daemon/build/aes67-daemon" ]; then
      DAEMON_BIN="$TOPDIR/daemon/build/aes67-daemon"
    fi
  fi
}

# 校验 -i 指定的所有接口存在(主备双网卡都校验)。返回非 0 即缺失。
validate_ifaces() {
  local saved_ifs="$IFS"; IFS=','
  local ifc
  for ifc in $IFACE; do
    ip -o link show "$ifc" >/dev/null 2>&1 || { IFS="$saved_ifs"; die "interface '$ifc' does not exist (ip link show)"; }
    ip -br addr show "$ifc" 2>/dev/null | grep -qE 'UP' || warn "interface '$ifc' is not UP"
  done
  IFS="$saved_ifs"
}

# ---- pre-flight --------------------------------------------------------------
preflight() {
  [ -x "$DAEMON_BIN" ] || die "daemon not compiled: $DAEMON_BIN (run build.sh)"
  [ -r "$DAEMON_CONF" ] || die "config not found: $DAEMON_CONF"
  [ -r "$KMOD" ] || die "kernel module not compiled: $KMOD (run build.sh)"
  [ "$NO_PTP" = 1 ] || [ -x "$PTP4L" ] || die "ptp4l not installed at $PTP4L (apt install linuxptp, or use --no-ptp)"
  # validate daemon interface(s) exist (主备双网卡都校验)
  validate_ifaces
  # validate ptp4l interface (if ptp4l will run)
  if [ "$NO_PTP" != 1 ]; then
    ip -o link show "$PTP_IFACE" >/dev/null 2>&1 || die "ptp interface '$PTP_IFACE' does not exist"
  fi
  command -v arecord >/dev/null || warn "arecord not found (alsa-utils) — playback/record tests won't work"
}

# ---- paths derived from PRIMARY_IFACE / PTP_IFACE ----------------------------
# 用主接口名(逗号前)做文件名,避免逗号进 pidfile/log/conf 文件名。
ptp_log()       { echo "/tmp/ptp4l.$PTP_IFACE.log"; }
ptp_pidfile()   { echo "/tmp/aes67-ptp4l.$PTP_IFACE.pid"; }
daemon_log()    { echo "/tmp/aes67-daemon.$PRIMARY_IFACE.log"; }
daemon_pidfile(){ echo "/tmp/aes67-daemon.$PRIMARY_IFACE.pid"; }
conf_file()     { echo "/tmp/aes67-daemon.$PRIMARY_IFACE.conf"; }

iface_ip() {
  # print the first IPv4 of the PRIMARY interface, or empty
  ip -4 -o addr show "$PRIMARY_IFACE" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1
}

# ---- helpers -----------------------------------------------------------------
is_running() {  # <pidfile>
  local pf="$1" pid
  [ -f "$pf" ] || return 1
  pid="$(cat "$pf" 2>/dev/null)"
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null
}

kill_pidfile() {  # <pidfile>
  local pf="$1" pid
  [ -f "$pf" ] || return 0
  pid="$(cat "$pf" 2>/dev/null)"
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    kill -TERM "$pid" 2>/dev/null
    for _ in $(seq 1 10); do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$pid" 2>/dev/null; then
      warn "process $pid did not exit, sending KILL"
      kill -9 "$pid" 2>/dev/null
    fi
  fi
  rm -f "$pf"
}

# ---- generate per-interface config (does NOT touch daemon.conf) --------------
gen_config() {
  local conf; conf="$(conf_file)"
  # start from the stock config and override fields via python (robust JSON edit).
  # --oca: also set oca_enabled=true so the real driver exposes the OCA control
  # plane (port 65037 + _oca._tcp mDNS with Fitcan-style TXT) for controller verify.
  python3 - "$DAEMON_CONF" "$conf" "$IFACE" "${PTP_DOMAIN:-}" "${HTTP_PORT:-}" "$OCA_ENABLED" <<'PY' || die "failed to generate config"
import json, sys
src, dst, iface, domain, port, oca = sys.argv[1:7]
with open(src) as f:
    cfg = json.load(f)
cfg["interface_name"] = iface
if domain != "":
    cfg["ptp_domain"] = int(domain)
if port != "":
    cfg["http_port"] = int(port)
if oca == "1":
    cfg["oca_enabled"] = True
with open(dst, "w") as f:
    json.dump(cfg, f, indent=2)
PY
  echo "$conf"
}

# ---- actions -----------------------------------------------------------------
start() {
  preflight

  # 校验每个接口有 IPv4(主备双网卡都需 IP,SAP/mDNS 跑所有接口)。
  local ipaddr; ipaddr="$(iface_ip)"   # 主接口 IP(用于状态显示)
  if [ "$IFACE" != "lo" ]; then
    local saved_ifs="$IFS"; IFS=','
    local ifc
    for ifc in $IFACE; do
      local ip4; ip4="$(ip -4 -o addr show "$ifc" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)"
      [ -n "$ip4" ] || die "interface '$ifc' has no IPv4 address — daemon cannot bind SAP/mDNS/PTP"
    done
    IFS="$saved_ifs"
  fi

  # 1. kernel module
  if lsmod | grep -q '^MergingRavennaALSA '; then
    log "kernel module already loaded"
  else
    log "loading kernel module ..."
    sudo insmod "$KMOD" || die "insmod failed (check: dmesg | tail)"
    sleep 1
    lsmod | grep -q '^MergingRavennaALSA ' || die "module load reported ok but not in lsmod"
    log "kernel module loaded"
  fi

  # 2. sysctl tuning (same as run_test.sh)
  log "applying sysctl tuning ..."
  sudo sysctl -w net/ipv4/igmp_max_memberships=66 >/dev/null 2>&1
  sudo sysctl -w kernel/perf_cpu_time_max_percent=0 >/dev/null 2>&1
  sudo sysctl -w kernel/sched_rt_runtime_us=1000000 >/dev/null 2>&1

  # 3. generate per-interface config
  local conf; conf="$(gen_config)"
  log "generated config: $conf (interface_name=$IFACE)"

  # 4. ptp4l — provides a PTP grandmaster for the daemon's slave to lock to.
  #    Skip entirely with --no-ptp (SAP/mDNS browse & WebUI still work, but
  #    audio send/recv needs a lock). In VMs the virtual NIC often has no usable
  #    PTP clock ("failed to create a clock"); use --ptp-iface lo to run ptp4l
  #    on loopback instead.
  if [ "$NO_PTP" = 1 ]; then
    log "ptp4l skipped (--no-ptp) — PTP will stay unlocked; browse/WebUI OK, audio send/recv will NOT work"
  elif is_running "$(ptp_pidfile)"; then
    log "ptp4l already running (pid $(cat "$(ptp_pidfile)")) on $PTP_IFACE"
  else
    log "starting ptp4l on $PTP_IFACE ..."
    sudo "$PTP4L" -i "$PTP_IFACE" -l7 -E -S > "$(ptp_log)" 2>&1 &
    echo $! > "$(ptp_pidfile)"
    sleep 1
    if is_running "$(ptp_pidfile)"; then
      log "ptp4l started (pid $(cat "$(ptp_pidfile)")) on $PTP_IFACE"
    else
      # ptp4l died — likely "failed to create a clock" on a NIC without a usable
      # timestamp. Don't hard-fail: tell the user how to proceed.
      warn "ptp4l failed to start on $PTP_IFACE, see $(ptp_log)"
      warn "  common VM cause: NIC has no PTP clock. Remedies:"
      warn "    1) ./oca-daemonctl.sh start -i $IFACE --ptp-iface lo   # run ptp4l on loopback"
      warn "    2) ./oca-daemonctl.sh start -i $IFACE --no-ptp         # skip ptp4l (browse only, no audio)"
      rm -f "$(ptp_pidfile)"
      die "ptp4l start failed"
    fi
  fi

  # 5. daemon (current user, on the chosen interface via config)
  if is_running "$(daemon_pidfile)"; then
    log "daemon already running (pid $(cat "$(daemon_pidfile)"))"
  else
    log "starting aes67-daemon on $IFACE ..."
    # cd daemon/ 保持相对路径(status.json 等)正确,但用 $DAEMON_BIN(可能是
    # out-of-source 的 OCA 构建,而非 in-source)。--oca 默认选 build/aes67-daemon。
    # exec 让子 shell 替换为 daemon,使 $! 记录真实 daemon pid(否则记录的是
    # nohup wrapper pid,stop/status 会误判)。
    ( cd "$TOPDIR/daemon" && exec nohup "$DAEMON_BIN" -c "$conf" -a "$HTTP_ADDR" > "$(daemon_log)" 2>&1 ) &
    echo $! > "$(daemon_pidfile)"
    sleep 2
    is_running "$(daemon_pidfile)" || die "daemon failed to start, see $(daemon_log)"
    log "daemon started (pid $(cat "$(daemon_pidfile)"))"
  fi

  # 6. wait for PTP lock (skipped under --no-ptp)
  local port="${HTTP_PORT:-$(python3 -c "import json;print(json.load(open('$DAEMON_CONF'))['http_port'])")}"
  if [ "$NO_PTP" = 1 ]; then
    log "skipping PTP lock wait (--no-ptp)"
  else
    log "waiting for PTP lock (up to 30s) ..."
    local i locked=0
    for i in $(seq 1 30); do
      if curl -s "http://127.0.0.1:$port/api/ptp/status" 2>/dev/null | grep -q '"locked"'; then
        locked=1; break
      fi
      sleep 1
    done
    if [ "$locked" = 1 ]; then
      log "PTP LOCKED ✓ (daemon on $IFACE, master on $PTP_IFACE, after ${i}s)"
    else
      warn "PTP not locked after 30s — SAP/mDNS browse still works, but audio send/recv needs a lock"
      warn "  check $(ptp_log); if $PTP_IFACE lacks a PTP clock, try --ptp-iface lo or --no-ptp"
    fi
  fi

  echo
  log "interface:  $IFACE  $([ -n "$ipaddr" ] && echo "($ipaddr)")"
  log "WebUI:      http://localhost:$port"
  log "API:        http://localhost:$port/api/config"
  log "browse SAP: http://localhost:$port/api/browse/sources/sap"
  if [ "$OCA_ENABLED" = 1 ]; then
    log "OCA:        enabled (port 65037 OCP.1 + _oca._tcp mDNS, binary $DAEMON_BIN)"
  fi
  log "Logs:       $(daemon_log)  /  $(ptp_log)"
  log "Stop:       ./oca-daemonctl.sh stop"
}

stop() {
  # stop any instance we may have started (try all known pidfiles)
  log "stopping daemon ..."
  local killed=0 pf
  for pf in /tmp/aes67-daemon.*.pid; do
    [ -f "$pf" ] || continue
    kill_pidfile "$pf"; killed=1
  done
  log "stopping ptp4l ..."
  for pf in /tmp/aes67-ptp4l.*.pid; do
    [ -f "$pf" ] || continue
    kill_pidfile "$pf"; killed=1
  done
  # catch orphans by name
  pkill -x aes67-daemon 2>/dev/null
  sudo pkill -x ptp4l 2>/dev/null
  sleep 1
  [ "$killed" = 1 ] && log "stopped." || log "nothing was running."

  if [ "$UNLOAD_ON_STOP" = 1 ]; then
    if lsmod | grep -q '^MergingRavennaALSA '; then
      log "unloading kernel module (--unload) ..."
      sudo rmmod MergingRavennaALSA && log "module unloaded" || warn "rmmod failed (may be in use)"
    else
      log "kernel module not loaded, nothing to unload"
    fi
  else
    log "kernel module left loaded (use --unload to rmmod)"
  fi
}

status() {
  local found=0
  echo "=== aes67-daemon instances ==="
  local pf pid iface port
  for pf in /tmp/aes67-daemon.*.pid; do
    [ -f "$pf" ] || continue
    iface="$(basename "$pf" .pid)"; iface="${iface#aes67-daemon.}"
    if is_running "$pf"; then
      pid="$(cat "$pf")"
      found=1
      echo "  [$iface] running pid=$pid"
      ps -o pid,rss,etime,cmd -p "$pid" 2>/dev/null | tail -1 | sed 's/^/      /'
    else
      echo "  [$iface] stale pidfile (not running)"
    fi
  done
  [ "$found" = 0 ] && echo "  none running"

  echo "=== ptp4l instances ==="
  found=0
  for pf in /tmp/aes67-ptp4l.*.pid; do
    [ -f "$pf" ] || continue
    iface="$(basename "$pf" .pid)"; iface="${iface#aes67-ptp4l.}"
    if is_running "$pf"; then
      echo "  [$iface] running pid=$(cat "$pf")"; found=1
    fi
  done
  [ "$found" = 0 ] && echo "  none running"

  echo "=== PTP status / browse (per running daemon) ==="
  for pf in /tmp/aes67-daemon.*.pid; do
    [ -f "$pf" ] || continue
    iface="$(basename "$pf" .pid)"; iface="${iface#aes67-daemon.}"
    is_running "$pf" || continue
    local port; port="$(python3 -c "import json;print(json.load(open('$(conf_file_for "$iface")'))['http_port'])" 2>/dev/null || echo 8080)"
    echo "  [$iface] port=$port"
    echo -n "    ptp: "; curl -s "http://127.0.0.1:$port/api/ptp/status" 2>/dev/null || echo "(no response)"
    echo
    echo -n "    sap sources: "; curl -s "http://127.0.0.1:$port/api/browse/sources/sap" 2>/dev/null | head -c 300; echo
  done

  echo "=== RAVENNA soundcard ==="
  if aplay -l 2>/dev/null | grep -qi ravenna; then
    aplay -l 2>/dev/null | grep -i ravenna | sed 's/^/  /'
  else
    echo "  NOT present (module not loaded?)"
  fi

  echo "=== kernel module ==="
  lsmod | grep -q '^MergingRavennaALSA ' && echo "  loaded" || echo "  NOT loaded"
}

# helper: conf path for a given iface (used by status)
conf_file_for() { echo "/tmp/aes67-daemon.$1.conf"; }

usage() {
  cat >&2 <<EOF
Usage: $0 {start|stop|restart|status} [options]

  start [-i <iface>] [-p <port>] [-d <domain>] [-a <addr>]
        load module, start ptp4l + daemon on <iface>, wait for PTP lock
  stop  [--unload]
        stop all daemon + ptp4l instances (module left loaded unless --unload)
  restart [same options as start]
  status
        show all running instances, PTP status, discovered SAP sources

Options:
  -i <iface>      network interface for the daemon (default: lo). Single NIC
                  (e.g. ens192) for SAP/mDNS on LAN. Two interfaces comma-separated
                  (e.g. ens160,ens192) enable ST-2022-7 redundancy (PTP/RTP on
                  primary+secondary, SAP/mDNS on all). Primary = first.
  -p <port>       HTTP server port (default: from daemon.conf = 8080)
  -d <domain>     PTPv2 domain 0..127 (default: from daemon.conf)
  -a <addr>       HTTP bind address (default: 0.0.0.0)
  --ptp-iface <iface>  run ptp4l on a different interface than the daemon.
                  In VMs the virtual NIC often has no PTP clock; use
                  --ptp-iface lo to run the PTP master on loopback while the
                  daemon stays on the real NIC.
  --no-ptp        skip ptp4l entirely. SAP/mDNS browse & WebUI still work, but
                  PTP stays unlocked so audio send/recv will NOT work.
  --unload        rmmod kernel module on stop
  --oca           enable the OCA control plane (oca_enabled=true) on the real
                  driver: OCP.1 port 65037 + _oca._tcp mDNS (Fitcan-style TXT)
                  for AES70/Fitcan controller verify. Uses the out-of-source
                  OCA build (daemon/build/aes67-daemon) if present.
  --daemon-bin <path>  use a specific daemon binary (overrides --oca auto-select).
                  Point at the OCA build from './oca-dev.sh build --real'.

Examples:
  $0 start -i ens192                        # run on ens192, ptp4l on ens192 too
  $0 start -i ens160,ens192 --ptp-iface lo   # ST-2022-7 dual-NIC, ptp4l on lo (VM)
  $0 start -i ens192 --ptp-iface lo         # daemon on ens192, ptp4l on lo (VM)
  $0 start -i ens192 --no-ptp               # browse only, no PTP master
  $0 start -i ens192 --oca                  # real driver + OCA for Fitcan verify
  $0 start -i ens192 -p 8081                # custom HTTP port
  $0 stop --unload                          # stop everything and unload module
  $0 status                                 # show what's running
EOF
  exit 1
}

# ---- main --------------------------------------------------------------------
ACTION="${1:-}"
[ -n "$ACTION" ] || usage
shift || true

case "$ACTION" in
  start)   parse_opts "$@"; start ;;
  restart) parse_opts "$@"; stop;  echo; start ;;
  stop)    # stop ignores -i etc, only honors --unload
           for a in "$@"; do [ "$a" = "--unload" ] && UNLOAD_ON_STOP=1; done
           stop ;;
  status)  status ;;
  -h|--help) usage ;;
  *)       usage ;;
esac
