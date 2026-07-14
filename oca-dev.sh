#!/bin/bash
#
# oca-dev.sh - AES67 daemon 自用开发脚本(FAKE_DRIVER + OCA 路径)
#
# 面向无音频硬件的开发/验证场景:FAKE_DRIVER=ON + WITH_OCA=ON + WITH_AVAHI=ON。
# 一个入口封装 build / run / stop / status / test / clean,不修改原仓库的
# build.sh / buildfake.sh / cleanup.sh,也不依赖它们。
#
# 与 daemonctl.sh 的区别:daemonctl.sh 面向真实硬件(加载内核模块 + ptp4l);
# 本脚本面向 FAKE_DRIVER,不加载模块、不启 ptp4l,直接跑 daemon 验证
# OCA/HTTP/mDNS 控制平面。
#
# Usage:
#   ./oca-dev.sh build [--no-avahi] [--no-oca] [--real]  # 构建(默认 OCA+AVAHI+FAKE;
#                                              # --real=真实驱动+OCA,另构建 LKM)
#   ./oca-dev.sh run   [-i <iface>] [-p <port>]  # 生成临时配置并后台启动(FAKE)
#   ./oca-dev.sh run-real [-i <iface>] [--ptp-iface <iface>] [--no-ptp] ...
#                                              # 真实驱动整体验证(模块+ptp4l+OCA),
#                                              # 委托 oca-daemonctl.sh start --oca
#   ./oca-dev.sh stop                            # 停止后台 daemon(本脚本 + daemonctl 实例)
#   ./oca-dev.sh status                          # 查看运行状态 + OCA 端口
#   ./oca-dev.sh test                            # 跑 oca-test 全量
#   ./oca-dev.sh probe [host] [port] [--no-sub]  # 跑 OCP.1 探测客户端
#   ./oca-dev.sh clean                           # 温和清理(只删构建产物,保留子模块)
#
# Examples:
#   ./oca-dev.sh build
#   ./oca-dev.sh run -i ens160                   # 在 ens160 上跑,OCA+mDNS 发布到 LAN
#   ./oca-dev.sh build --real                    # 构建真实驱动二进制 + LKM(需 linux-headers)
#   ./oca-dev.sh run-real -i ens192 --ptp-iface lo  # 真实驱动+OCA 整体验证(VM 用 lo 跑 ptp4l)
#   ./oca-dev.sh run-real -i ens160,ens192         # ST-2022-7 双网卡冗余(主备)
#   ./oca-dev.sh status
#   ./oca-dev.sh test
#   ./oca-dev.sh probe 172.16.1.198 65037        # 探测指定地址的 OCA 设备
#   ./oca-dev.sh stop
#
# 不入库:本脚本为本地自用,放仓库根,不纳入版本库(见 .gitignore)。

set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON_DIR="$TOPDIR/daemon"
DAEMON_CONF="$DAEMON_DIR/daemon.conf"
BUILD_DIR="$DAEMON_DIR/build"   # out-of-source 构建目录,不污染源码
BIN="$BUILD_DIR/aes67-daemon"
OCATEST="$BUILD_DIR/tests/oca-test"
PROBE="$BUILD_DIR/oca-probe"

# 运行时文件(按接口隔离,避免与 daemonctl.sh 冲突)
RUN_CONF="/tmp/aes67-dev.<iface>.conf"
PIDFILE="/tmp/aes67-dev.pid"
LOGFILE="/tmp/aes67-dev.log"

# ---- 颜色 --------------------------------------------------------------------
if [ -t 1 ]; then
  C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_RED=$'\033[31m'; C_OFF=$'\033[0m'
else
  C_GREEN=""; C_YELLOW=""; C_RED=""; C_OFF=""
fi
log()  { echo "${C_GREEN}[dev]${C_OFF} $*"; }
warn() { echo "${C_YELLOW}[dev]${C_OFF} WARN: $*" >&2; }
die()  { echo "${C_RED}[dev]${C_OFF} ERROR: $*" >&2; exit 1; }

# ---- build -------------------------------------------------------------------
cmd_build() {
  local with_avahi=ON with_oca=ON fake_driver=ON
  while [ $# -gt 0 ]; do
    case "$1" in
      --no-avahi) with_avahi=OFF; shift ;;
      --no-oca)   with_oca=OFF;   shift ;;
      --real)     fake_driver=OFF; shift ;;
      *) die "build: unknown option: $1" ;;
    esac
  done

  # 确保子模块就绪(若被 cleanup.sh 误删,这里自动恢复,不报错)
  if [ ! -f "$TOPDIR/3rdparty/cpp-httplib/httplib.h" ] || \
     [ ! -f "$TOPDIR/3rdparty/ravenna-alsa-lkm/common/MergingRAVENNACommon.h" ]; then
    log "submodules missing, initializing ..."
    git -C "$TOPDIR" submodule update --init 3rdparty/cpp-httplib 3rdparty/ravenna-alsa-lkm
    git -C "$TOPDIR/3rdparty/ravenna-alsa-lkm" checkout aes67-daemon >/dev/null 2>&1 || true
  fi

  # --real: 真实驱动需先构建 LKM(ravenna-alsa-lkm 必须在 aes67-daemon 分支)
  if [ "$fake_driver" = OFF ]; then
    log "building RAVENNA/AES67 kernel module ..."
    git -C "$TOPDIR/3rdparty/ravenna-alsa-lkm" checkout aes67-daemon >/dev/null 2>&1 || true
    ( cd "$TOPDIR/3rdparty/ravenna-alsa-lkm/driver" && make ) || die "LKM build failed (need linux-headers-$(uname -r)?)"
    [ -f "$TOPDIR/3rdparty/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko" ] || die "LKM build produced no .ko"
    log "kernel module built: 3rdparty/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko"
  fi

  log "configuring (OCA=$with_oca AVAHI=$with_avahi FAKE_DRIVER=$fake_driver) ..."
  mkdir -p "$BUILD_DIR"
  # out-of-source 构建:源码用 ${CMAKE_SOURCE_DIR}(daemon/),产物进 build/,
  # 不与源码混杂。导出 compile_commands.json 供 clangd 使用。
  cmake -S "$DAEMON_DIR" -B "$BUILD_DIR" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCPP_HTTPLIB_DIR="$TOPDIR/3rdparty/cpp-httplib" \
      -DRAVENNA_ALSA_LKM_DIR="$TOPDIR/3rdparty/ravenna-alsa-lkm" \
      -DENABLE_TESTS=ON \
      -DWITH_OCA=$with_oca \
      -DWITH_AVAHI=$with_avahi \
      -DFAKE_DRIVER=$fake_driver \
      -DWITH_STREAMER=OFF

  log "building aes67-daemon + oca-test ..."
  cmake --build "$BUILD_DIR" --target aes67-daemon oca-test oca-probe
  log "build done. binary: $BIN  (FAKE_DRIVER=$fake_driver)"
  log "probe:      $BUILD_DIR/oca-probe (OCP.1 探测客户端)"
  log "compile_commands.json: $BUILD_DIR/compile_commands.json (供 .clangd)"
  if [ "$fake_driver" = OFF ]; then
    log "run-real:    ./oca-dev.sh run-real -i <LAN网卡> [--ptp-iface lo]"
  fi
}

# ---- run ---------------------------------------------------------------------
cmd_run() {
  local iface="lo" http_port=""
  while [ $# -gt 0 ]; do
    case "$1" in
      -i) iface="$2"; shift 2 ;;
      -p) http_port="$2"; shift 2 ;;
      *) die "run: unknown option: $1" ;;
    esac
  done

  [ -x "$BIN" ] || die "binary not found, run './oca-dev.sh build' first"

  # -i 支持 ST-2022-7 主备双网卡:"ens160,ens192"(逗号分隔,daemon interface_name
  # 原生支持)。主接口=逗号前第一个,用于临时配置文件名。
  local primary="${iface%%,*}"

  # 生成临时配置:interface_name(完整双网卡值) + oca_enabled=true,不改原 daemon.conf
  local conf="/tmp/aes67-dev.${primary}.conf"
  sed -e "s/\"interface_name\": \"[^\"]*\"/\"interface_name\": \"$iface\"/" \
      -e 's/"oca_enabled": false/"oca_enabled": true/' \
      "$DAEMON_CONF" > "$conf"
  if [ -n "$http_port" ]; then
    sed -i "s/\"http_port\": [0-9]*/\"http_port\": $http_port/" "$conf"
  fi

  # 已有本脚本启动的实例在跑则拒绝(避免端口冲突)
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    die "daemon already running (pid $(cat "$PIDFILE")), run './oca-dev.sh stop' first"
  fi

  log "starting aes67-daemon on $iface (config: $conf) ..."
  # 记录配置路径到 pidfile 旁,供 stop 校验(daemon 可能 fork,$! 不准)
  echo "$conf" > "${PIDFILE}.conf"
  nohup "$BIN" -c "$conf" > "$LOGFILE" 2>&1 &
  sleep 2

  # 用精确进程名取真正的 daemon PID(pgrep -f 会误匹配本脚本的命令行参数)
  local pid; pid="$(pgrep -x aes67-daemon | head -1 || true)"
  if [ -z "$pid" ]; then
    warn "daemon exited early, log tail:"
    tail -15 "$LOGFILE" >&2
    rm -f "$PIDFILE" "${PIDFILE}.conf"
    die "daemon failed to start"
  fi
  echo "$pid" > "$PIDFILE"

  local oca_port; oca_port="$(grep -o '"oca_port": [0-9]*' "$conf" | grep -o '[0-9]*')"
  log "daemon started (pid $pid)"
  log "  interface: $iface"
  log "  HTTP API:  http://127.0.0.1:${http_port:-8080}/api/config"
  log "  OCA port:  $oca_port  (OCP.1 TCP)"
  log "  OCA mDNS:  _oca._tcp  (需 WITH_AVAHI=ON 构建 + avahi-daemon 运行)"
  log "  log:       $LOGFILE"
  log "  stop:      ./oca-dev.sh stop"
}

# ---- run-real (真实驱动整体验证:模块+ptp4l+OCA+真实 RTP) ---------------------
# 委托 oca-daemonctl.sh 的模块加载/ptp4l/daemon 编排,但启用 OCA(oca_enabled=true)
# 并使用 OCA 构建产物(daemon/build/aes67-daemon)。用于真实音频硬件下的 OCA 整体验证。
cmd_run_real() {
  local daemonctl="$TOPDIR/oca-daemonctl.sh"
  [ -x "$daemonctl" ] || die "oca-daemonctl.sh not found at $daemonctl"

  # 验证真实驱动 + LKM 已构建(build --real)
  [ -x "$BIN" ] || die "binary not found, run './oca-dev.sh build --real' first"
  grep -q "FAKE_DRIVER:BOOL=OFF" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null \
    || die "current build is FAKE_DRIVER; run './oca-dev.sh build --real' first"
  [ -f "$TOPDIR/3rdparty/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko" ] \
    || die "kernel module not built, run './oca-dev.sh build --real' first"

  # 先停本脚本的 FAKE 实例(避免与真实驱动实例抢 65037 端口)
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    log "stopping FAKE daemon instance to free port 65037 ..."
    cmd_stop
  fi

  log "delegating to oca-daemonctl.sh (real driver + OCA) ..."
  # --oca 启用 OCA 控制平面;--daemon-bin 指定 OCA 构建产物(含 Fitcan TXT 修复)。
  # 透传其余选项(-i / --ptp-iface / --no-ptp 等)给 oca-daemonctl.sh。
  exec "$daemonctl" start --oca --daemon-bin "$BIN" "$@"
}

# ---- stop --------------------------------------------------------------------
cmd_stop() {
  # 用精确进程名匹配所有 daemon 实例(pgrep -f 会误匹配本脚本)
  local pids; pids="$(pgrep -x aes67-daemon || true)"

  # 退化:pidfile 里的 PID(若 pgrep 未命中但 pidfile 存在)
  if [ -z "$pids" ] && [ -f "$PIDFILE" ]; then
    pids="$(cat "$PIDFILE")"
  fi

  if [ -z "$pids" ]; then
    log "not running (no pidfile/match)"
    rm -f "$PIDFILE" "${PIDFILE}.conf"
    return
  fi

  local any_killed=0
  for pid in $pids; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      local i
      for i in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.1
      done
      kill -0 "$pid" 2>/dev/null && { warn "pid $pid graceful timeout, KILL"; kill -9 "$pid" 2>/dev/null || true; }
      any_killed=1
    fi
  done
  [ "$any_killed" = 1 ] && log "stopped (pids: $pids)" || log "no live process found"
  rm -f "$PIDFILE" "${PIDFILE}.conf"

  # 若有 oca-daemonctl.sh 启动的实例(含 ptp4l),委托其 stop 清理(不 unload 模块)
  local daemonctl="$TOPDIR/oca-daemonctl.sh"
  if [ -x "$daemonctl" ] && ls /tmp/aes67-daemon.*.pid /tmp/aes67-ptp4l.*.pid >/dev/null 2>&1; then
    log "also stopping oca-daemonctl instances (daemon + ptp4l) ..."
    "$daemonctl" stop >/dev/null 2>&1 || true
  fi
}

# ---- status ------------------------------------------------------------------
cmd_status() {
  # OCA 端口监听状态(无论哪个实例起的)
  local oca_listen=""
  if command -v ss >/dev/null 2>&1; then
    oca_listen="$(ss -tlnp 2>/dev/null | grep ':65037 ' || true)"
  fi

  echo "=== dev daemon (FAKE, ./oca-dev.sh run) ==="
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    local pid; pid="$(cat "$PIDFILE")"
    log "running (pid $pid)"
    echo "  log: $LOGFILE"
    grep -m1 "OCA server listening" "$LOGFILE" 2>/dev/null | sed 's/^/  /' || true
  else
    log "not running"
  fi

  # oca-daemonctl.sh 启动的实例(真实驱动 ./oca-dev.sh run-real)
  local daemonctl_pids=""
  for pf in /tmp/aes67-daemon.*.pid; do
    [ -f "$pf" ] || continue
    local p; p="$(cat "$pf" 2>/dev/null)"
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
      local iface; iface="$(basename "$pf" .pid)"; iface="${iface#aes67-daemon.}"
      daemonctl_pids="$daemonctl_pids $p"
      echo "  [$iface] real-driver pid=$p ($(readlink /proc/$p/exe 2>/dev/null || echo '?'))"
      grep -m1 "OCA server listening" "/tmp/aes67-daemon.$iface.log" 2>/dev/null | sed 's/^/    /' || true
    fi
  done
  if [ -n "$daemonctl_pids" ]; then
    echo "  (以上为 ./oca-dev.sh run-real 启动的真实驱动实例)"
  else
    log "no real-driver instances (run-real)"
  fi

  # OCA 端口总览
  if [ -n "$oca_listen" ]; then
    echo "=== OCA 65037 ==="
    echo "  LISTEN ✓"
  fi

  # mDNS 发布检查(若 avahi-browse 可用)
  if command -v avahi-browse >/dev/null 2>&1; then
    echo "=== mDNS _oca._tcp (avahi-browse) ==="
    timeout 5 avahi-browse -rtp _oca._tcp 2>/dev/null | head -20 || echo "  (no response)"
  else
    echo "=== mDNS ==="
    echo "  avahi-browse 未安装(sudo apt install -y avahi-utils)"
  fi
}

# ---- test --------------------------------------------------------------------
cmd_test() {
  [ -x "$OCATEST" ] || die "oca-test not found, run './oca-dev.sh build' first"
  log "running oca-test ..."
  "$OCATEST" -p
}

# ---- probe -------------------------------------------------------------------
# OCP.1 探测客户端:作为外部陌生控制器连真实 daemon,独立验证控制平面。
# 不带参数时默认探测本机 65037;可指定 host/port 探测远端设备。
cmd_probe() {
  [ -x "$PROBE" ] || die "oca-probe not found, run './oca-dev.sh build' first"
  log "running oca-probe ..."
  "$PROBE" "$@"
}

# ---- clean (温和:只删构建产物,保留子模块) ----------------------------------
cmd_clean() {
  log "cleaning build artifacts (keeping submodules) ..."
  # out-of-source 构建产物全在 build/(主要清理目标)
  rm -rf "$BUILD_DIR"
  # 兼容清理:之前 in-source 构建可能遗留的散落文件
  rm -f  "$DAEMON_DIR"/CMakeCache.txt "$DAEMON_DIR"/Makefile \
         "$DAEMON_DIR"/cmake_install.cmake "$DAEMON_DIR"/CTestTestfile.cmake \
         "$DAEMON_DIR"/aes67-daemon "$DAEMON_DIR"/status.json
  rm -rf "$DAEMON_DIR"/CMakeFiles "$DAEMON_DIR"/Testing \
         "$DAEMON_DIR"/tests/CMakeFiles "$DAEMON_DIR"/tests/Testing
  rm -f  "$DAEMON_DIR"/tests/Makefile "$DAEMON_DIR"/tests/CMakeCache.txt \
         "$DAEMON_DIR"/tests/cmake_install.cmake "$DAEMON_DIR"/tests/CTestTestfile.cmake \
         "$DAEMON_DIR"/tests/oca-test
  log "done. submodules preserved; rebuild with './oca-dev.sh build'."
}

# ---- main --------------------------------------------------------------------
usage() {
  sed -n '2,/^$/p' "$0" | sed 's/^# \?//' >&2
  exit 1
}

ACTION="${1:-}"
[ -n "$ACTION" ] || usage
shift || true
case "$ACTION" in
  build)     cmd_build     "$@" ;;
  run)       cmd_run       "$@" ;;
  run-real)  cmd_run_real  "$@" ;;
  stop)      cmd_stop      "$@" ;;
  status)    cmd_status    ;;
  test)      cmd_test      ;;
  probe)     cmd_probe     "$@" ;;
  clean)     cmd_clean     ;;
  -h|--help) usage ;;
  *)      usage ;;
esac
