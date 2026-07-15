#!/bin/bash
#
# noise-dev.sh - 噪声分析与降噪模块开发脚本(FAKE_DRIVER + out-of-source)
#
# 面向无音频硬件的开发/验证场景:FAKE_DRIVER=ON + WITH_AVAHI=ON,仅 HTTP 控制平面
# (本分支 feature/noise 暂不接入 OCA/AES70 控制协议)。一个入口封装
# build / run / stop / status / clean,不修改原仓库的 build.sh / buildfake.sh /
# cleanup.sh,也不依赖它们。
#
# 后续 noise 开发编译优先使用本脚本做 out-of-source 构建(产物进 daemon/build/,
# 不污染源码,并导出 compile_commands.json 供 clangd 使用)。
#
# 与 noise-daemonctl.sh 的区别:noise-daemonctl.sh 面向真实硬件(加载内核模块 +
# ptp4l);本脚本面向 FAKE_DRIVER,不加载模块、不启 ptp4l,直接跑 daemon 验证
# HTTP/噪声模块控制平面。
#
# Usage:
#   ./noise-dev.sh build [--no-avahi] [--real]  # 构建(默认 AVAHI+FAKE;
#                                              #  --real=真实驱动,另构建 LKM)
#   ./noise-dev.sh run   [-i <iface>] [-p <port>]  # 生成临时配置并后台启动(FAKE)
#   ./noise-dev.sh run-real [-i <iface>] [--ptp-iface <iface>] [--no-ptp] ...
#                                              # 真实驱动整体验证(模块+ptp4l),
#                                              #  委托 noise-daemonctl.sh start
#   ./noise-dev.sh stop                            # 停止后台 daemon(本脚本 + daemonctl 实例)
#   ./noise-dev.sh status                          # 查看运行状态
#   ./noise-dev.sh clean                           # 温和清理(只删构建产物,保留子模块)
#
# Examples:
#   ./noise-dev.sh build
#   ./noise-dev.sh run -i ens160                   # 在 ens160 上跑,mDNS 发布到 LAN
#   ./noise-dev.sh build --real                    # 构建真实驱动二进制 + LKM(需 linux-headers)
#   ./noise-dev.sh run-real -i ens192 --ptp-iface lo  # 真实驱动整体验证(VM 用 lo 跑 ptp4l)
#   ./noise-dev.sh run-real -i ens160,ens192         # ST-2022-7 双网卡冗余(主备)
#   ./noise-dev.sh status
#   ./noise-dev.sh stop
#
# 已入库:本脚本纳入版本库(随 feature/noise 分支提交)。

set -euo pipefail

TOPDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DAEMON_DIR="$TOPDIR/daemon"
DAEMON_CONF="$DAEMON_DIR/daemon.conf"
BUILD_DIR="$DAEMON_DIR/build"   # out-of-source 构建目录,不污染源码
BIN="$BUILD_DIR/aes67-daemon"

# 运行时文件(按接口隔离,避免与主工作区 oca-dev.sh 实例冲突)
RUN_CONF="/tmp/aes67-noise.<iface>.conf"
PIDFILE="/tmp/aes67-noise.pid"
LOGFILE="/tmp/aes67-noise.log"

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
  local with_avahi=ON fake_driver=ON
  while [ $# -gt 0 ]; do
    case "$1" in
      --no-avahi) with_avahi=OFF; shift ;;
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

  # 本分支 feature/noise 暂不接入 OCA(AES70 控制协议),不传 WITH_OCA。
  # noise 模块后续通过 WITH_NOISE 选项纳入(见 docs/noise/architecture-design.md)。
  log "configuring (AVAHI=$with_avahi FAKE_DRIVER=$fake_driver) ..."
  mkdir -p "$BUILD_DIR"
  # out-of-source 构建:源码用 ${CMAKE_SOURCE_DIR}(daemon/),产物进 build/,
  # 不与源码混杂。导出 compile_commands.json 供 clangd 使用。
  cmake -S "$DAEMON_DIR" -B "$BUILD_DIR" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      -DCPP_HTTPLIB_DIR="$TOPDIR/3rdparty/cpp-httplib" \
      -DRAVENNA_ALSA_LKM_DIR="$TOPDIR/3rdparty/ravenna-alsa-lkm" \
      -DENABLE_TESTS=ON \
      -DWITH_AVAHI=$with_avahi \
      -DFAKE_DRIVER=$fake_driver \
      -DWITH_STREAMER=OFF

  log "building aes67-daemon ..."
  cmake --build "$BUILD_DIR" --target aes67-daemon
  log "build done. binary: $BIN  (FAKE_DRIVER=$fake_driver)"
  log "compile_commands.json: $BUILD_DIR/compile_commands.json (供 .clangd)"
  if [ "$fake_driver" = OFF ]; then
    log "run-real:    ./noise-dev.sh run-real -i <LAN网卡> [--ptp-iface lo]"
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

  [ -x "$BIN" ] || die "binary not found, run './noise-dev.sh build' first"

  # -i 支持 ST-2022-7 主备双网卡:"ens160,ens192"(逗号分隔,daemon interface_name
  # 原生支持)。主接口=逗号前第一个,用于临时配置文件名。
  local primary="${iface%%,*}"

  # 生成临时配置:仅覆盖 interface_name,不改原 daemon.conf。
  # 本分支无 OCA,daemon.conf 不含 oca_enabled 字段,故不做 OCA 相关改写。
  local conf="/tmp/aes67-noise.${primary}.conf"
  sed "s/\"interface_name\": \"[^\"]*\"/\"interface_name\": \"$iface\"/" \
      "$DAEMON_CONF" > "$conf"
  if [ -n "$http_port" ]; then
    sed -i "s/\"http_port\": [0-9]*/\"http_port\": $http_port/" "$conf"
  fi

  # 已有本脚本启动的实例在跑则拒绝(避免端口冲突)
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    die "daemon already running (pid $(cat "$PIDFILE")), run './noise-dev.sh stop' first"
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

  log "daemon started (pid $pid)"
  log "  interface: $iface"
  log "  HTTP API:  http://127.0.0.1:${http_port:-8080}/api/config"
  log "  log:       $LOGFILE"
  log "  stop:      ./noise-dev.sh stop"
}

# ---- run-real (真实驱动整体验证:模块+ptp4l+真实 RTP) -------------------------
# 委托 noise-daemonctl.sh 的模块加载/ptp4l/daemon 编排,使用本脚本构建的产物
# (daemon/build/aes67-daemon)。用于真实音频硬件下的整体验证。不带 --oca:
# 本分支无 OCA 控制平面,只验证 HTTP/噪声模块。
cmd_run_real() {
  local daemonctl="$TOPDIR/noise-daemonctl.sh"
  [ -x "$daemonctl" ] || die "noise-daemonctl.sh not found at $daemonctl"

  # 验证真实驱动 + LKM 已构建(build --real)
  [ -x "$BIN" ] || die "binary not found, run './noise-dev.sh build --real' first"
  grep -q "FAKE_DRIVER:BOOL=OFF" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null \
    || die "current build is FAKE_DRIVER; run './noise-dev.sh build --real' first"
  [ -f "$TOPDIR/3rdparty/ravenna-alsa-lkm/driver/MergingRavennaALSA.ko" ] \
    || die "kernel module not built, run './noise-dev.sh build --real' first"

  # 先停本脚本的 FAKE 实例(避免与真实驱动实例抢同一 HTTP 端口)
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    log "stopping FAKE daemon instance to free HTTP port ..."
    cmd_stop
  fi

  log "delegating to noise-daemonctl.sh (real driver, HTTP only) ..."
  # --daemon-bin 指定本脚本构建的产物(FAKE_DRIVER=OFF)。不带 --oca。
  # 透传其余选项(-i / --ptp-iface / --no-ptp 等)给 noise-daemonctl.sh。
  exec "$daemonctl" start --daemon-bin "$BIN" "$@"
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

  # 若有 noise-daemonctl.sh 启动的实例(含 ptp4l),委托其 stop 清理(不 unload 模块)
  local daemonctl="$TOPDIR/noise-daemonctl.sh"
  if [ -x "$daemonctl" ] && ls /tmp/aes67-daemon.*.pid /tmp/aes67-ptp4l.*.pid >/dev/null 2>&1; then
    log "also stopping noise-daemonctl instances (daemon + ptp4l) ..."
    "$daemonctl" stop >/dev/null 2>&1 || true
  fi
}

# ---- status ------------------------------------------------------------------
cmd_status() {
  echo "=== dev daemon (FAKE, ./noise-dev.sh run) ==="
  if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null; then
    local pid; pid="$(cat "$PIDFILE")"
    log "running (pid $pid)"
    echo "  log: $LOGFILE"
    grep -m1 "session_manager\|HTTP server\|started" "$LOGFILE" 2>/dev/null | sed 's/^/  /' || true
  else
    log "not running"
  fi

  # noise-daemonctl.sh 启动的实例(真实驱动 ./noise-dev.sh run-real)
  local daemonctl_pids=""
  for pf in /tmp/aes67-daemon.*.pid; do
    [ -f "$pf" ] || continue
    local p; p="$(cat "$pf" 2>/dev/null)"
    if [ -n "$p" ] && kill -0 "$p" 2>/dev/null; then
      local iface; iface="$(basename "$pf" .pid)"; iface="${iface#aes67-daemon.}"
      daemonctl_pids="$daemonctl_pids $p"
      echo "  [$iface] real-driver pid=$p ($(readlink /proc/$p/exe 2>/dev/null || echo '?'))"
      grep -m1 "session_manager\|HTTP server\|started" "/tmp/aes67-daemon.$iface.log" 2>/dev/null | sed 's/^/    /' || true
    fi
  done
  if [ -n "$daemonctl_pids" ]; then
    echo "  (以上为 ./noise-dev.sh run-real 启动的真实驱动实例)"
  else
    log "no real-driver instances (run-real)"
  fi

  # mDNS 发布检查(若 avahi-browse 可用):noise daemon 发布 _http._tcp(含
  # _ravenna._sub._http._tcp 子类型),不走 OCA 的 _oca._tcp。
  if command -v avahi-browse >/dev/null 2>&1; then
    echo "=== mDNS _http._tcp (avahi-browse) ==="
    timeout 5 avahi-browse -rtp _http._tcp 2>/dev/null | head -20 || echo "  (no response)"
  else
    echo "=== mDNS ==="
    echo "  avahi-browse 未安装(sudo apt install -y avahi-utils)"
  fi
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
         "$DAEMON_DIR"/tests/cmake_install.cmake "$DAEMON_DIR"/tests/CTestTestfile.cmake
  log "done. submodules preserved; rebuild with './noise-dev.sh build'."
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
  clean)     cmd_clean     ;;
  -h|--help) usage ;;
  *)      usage ;;
esac
