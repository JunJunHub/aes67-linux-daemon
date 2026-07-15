# RAVENNA ALSA 驱动 — 物理声道分配与映射规则

> 适用范围:`3rdparty/ravenna-alsa-lkm/` 内核模块 + `daemon/` 守护进程 + `webui/` 配置界面
> 目的:讲清楚「声道路由表如何把一条 RTP 流的通道映射到 ALSA 虚拟声卡的物理声道槽位」,作为后续学习与维护的参考。
> 所有结论均带源码位置(`文件:行号`),便于核对。

---

## 0. 一句话结论

- ALSA 虚拟声卡内部有**两套独立的物理声道池**:`playback_buffer`(播放池,给 Source 用)和 `capture_buffer`(录音池,给 Sink 用)。
- Source 永远只占用 playback 池,Sink 永远只占用 capture 池,**两者互不共享、互不抢占**。
- 每条 RTP 流通过一张**路由表** `m_aui32Routing[StreamChannelId] = PhysicalChannelId`,把自己的第 N 个流通道钉到对应方向池里的某个物理槽。
- 编译期容量上限是 128(完整构建)/ 64(精简构建),但运行时默认只激活 64+64,WebUI 也只允许配 0~63。

---

## 1. 两层声道概念:物理池 vs 流通道

必须先分清两个层次的「声道」,这是理解整个映射的钥匙:

| 层次 | 名称 | 容量 | 说明 |
|------|------|------|------|
| 物理层 | 物理声道槽 (PhysicalChannelId) | 每方向 64(默认)/ 128(上限) | ALSA 声卡环形缓冲区里按通道切分的固定槽位,编号 0..N-1 |
| 流层 | 流通道 (StreamChannelId) | 每条 RTP 流最多 64 | 一条 RTP 流内音频帧的通道序号,编号 0..m_byNbOfChannels-1 |

一条 RTP 流(一个 Source 或一个 Sink)最多 64 个流通道;每个流通道通过路由表映射到物理层的一个槽。**多流可以映射到同一物理槽**(后文详述)。

---

## 2. 编译期常量(`common/MergingRAVENNACommon.h`)

```c
// MergingRAVENNACommon.h:62-72
#ifdef AES67_LIMITED_BUILD
    #define MAX_NUMBEROFINPUTS          64
    #define MAX_NUMBEROFOUTPUTS         64
#else
    #define MAX_NUMBEROFINPUTS          128
    #define MAX_NUMBEROFOUTPUTS         128
#endif

// MergingRAVENNACommon.h:81-82  (运行时默认值,与构建类型无关,恒为 64)
#define DEFAULT_NUMBEROFINPUTS          64
#define DEFAULT_NUMBEROFOUTPUTS         64
```

```c
// RTP_stream_info.h:49
#define MAX_CHANNELS_BY_RTP_STREAM 64   // 单条 RTP 流的最大通道数
```

要点:
- `MAX_NUMBEROFINPUTS` / `MAX_NUMBEROFOUTPUTS` 是**缓冲区数组的容量上限**(完整构建 128,精简构建 64)。
- `DEFAULT_NUMBEROFINPUTS` / `DEFAULT_NUMBEROFOUTPUTS` 恒为 **64**,是运行时实际激活的声道数初值。
- `MAX_CHANNELS_BY_RTP_STREAM = 64` 是**单条 RTP 流**的通道上限,与物理池容量无关。

### ⚠️ `MAX_NUMBEROFOUTPUTS` 是 dead code

驱动里把播放/录音两套缓冲的尺寸都绑到 `MAX_NUMBEROFINPUTS`:

```c
// audio_driver.c:62
#define MR_ALSA_NB_CHANNELS_MAX (MAX_NUMBEROFINPUTS)   // 只取 INPUTS

// audio_driver.c:150
void* capture_buffer_channels_map[MR_ALSA_NB_CHANNELS_MAX];

// audio_driver.c:1240 (playback PCM) / 1269 (capture PCM) — 两边都是
.channels_max = MR_ALSA_NB_CHANNELS_MAX,
```

全仓库搜索 `MAX_NUMBEROFOUTPUTS` 只有两处定义,**无任何引用**。即:无论 input/output,物理槽位数组都被切成 `MAX_NUMBEROFINPUTS` 份。这是历史遗留,改 output 容量需要同步改 `MR_ALSA_NB_CHANNELS_MAX` 的定义,不能只改 `MAX_NUMBEROFOUTPUTS`。

---

## 3. 运行时声道数:`m_NumberOfInputs` / `m_NumberOfOutputs`

这是**路由时的边界检查值**,决定「物理槽 0..几 能被路由到」。

```c
// manager.c:163-167  (init 时)
self->m_NumberOfInputs  = DEFAULT_NUMBEROFINPUTS;   // 64
SetNumberOfInputs (self, self->m_NumberOfInputs);
self->m_NumberOfOutputs = DEFAULT_NUMBEROFOUTPUTS;  // 64
SetNumberOfOutputs(self, self->m_NumberOfOutputs);
```

边界检查(决定路由到高槽位会不会失败):

```c
// manager.c:1346-1361  get_live_in_jitter_buffer(Sink 用)
inputBuffer = get_capture_buffer(self->m_pALSAChip);
if (inputBuffer == nullptr || ulChannelId >= self->m_NumberOfInputs)   // >= 64 即拒绝
    return NULL;
inputBuffer += ulChannelId * RINGBUFFERSIZE * get_audio_engine_sample_bytelength(self);

// manager.c:1365-1380  get_live_out_jitter_buffer(Source 用)
outputBuffer = get_playback_buffer(self->m_pALSAChip);
if (outputBuffer == nullptr || ulChannelId >= self->m_NumberOfOutputs) // >= 64 即拒绝
    return NULL;
outputBuffer += ulChannelId * RINGBUFFERSIZE * get_audio_engine_sample_bytelength(self);
```

### 两个值能改吗?谁在改?

- 驱动暴露了 `MT_ALSA_Msg_SetNumberOfInputs/Outputs` 消息(`manager.c:899-928`),`SetNumberOfInputs/Outputs`(`manager.c:561-589`)会重启流并更新计数。
- **但守护进程 `daemon/` 全程只 GET 不 SET**(见 `driver_manager.cpp:240-251` 只有 `get_number_of_inputs/outputs`)。整个仓库没有任何调用方把这两个值提到 64 以上。
- 因此**开箱即用状态下,两个池都只有 64 个可用槽**,即使你编译的是 128 通道完整构建。

### 与 ALSA `channels_max` 的「假矛盾」

ALSA PCM 层上报 `channels_max = MR_ALSA_NB_CHANNELS_MAX`(完整构建=128),所以 `arecord --dump-hw-params` 会显示 `CHANNELS: [1 128]`。这只代表 ALSA 设备**允许以最多 128 通道打开**,跟路由层可用槽数是两回事:

- ALSA 层:`hw_params`(`audio_driver.c:1600-1670`)**不会**回调 `SetNumberOfInputs/Outputs`,打开 128 通道也不影响 `m_NumberOfInputs`。
- 路由层:仍以 `m_NumberOfInputs/Outputs`(默认 64)为准,路由到 64~127 槽会返回 NULL → 流 Init 失败(报 `get_live_in/out_jitter_buffer(N) not available`)。

→ 想真正用上 128 槽,必须自行向驱动发 `SetNumberOfInputs(128)` / `SetNumberOfOutputs(128)`(daemon 当前未暴露此入口,需改代码)。

---

## 4. 物理声道池:两套独立缓冲

```c
// audio_driver.c:149-152 (结构体字段,示意)
void*  playback_buffer_channels_map[MR_ALSA_NB_CHANNELS_MAX];  // 播放池:128 个槽指针
void*  capture_buffer_channels_map [MR_ALSA_NB_CHANNELS_MAX];  // 录音池:128 个槽指针
```

`playback_buffer` 与 `capture_buffer` 是两块**独立的非交错(non-interleaved)环形缓冲区**,每块按通道切成等长子区。驱动把这两块缓冲分别暴露给路由层:

| 路由层接口 | 绑定的物理缓冲 | 方向 | 使用者 |
|-----------|--------------|------|--------|
| `get_live_out_jitter_buffer(ulChannelId)` | **playback_buffer** | aplay 写入 → Source 读出发 RTP | Source |
| `get_live_in_jitter_buffer(ulChannelId)`  | **capture_buffer**  | Sink 写入收到的 RTP → arecord 读出 | Sink |

> **这就是「Source 与 Sink 不共享物理声道」的根本原因**:它们物理上就是两块不同的内存,各自 0..N-1 编号,同名「槽 0」却分属不同缓冲,天然不冲突。所以「Sink 占满所有物理声道后 Source 没槽用」不会发生——它们是两个池。

`RINGBUFFERSIZE`(`MergingRAVENNACommon.h:86-89`)是单通道环形缓冲帧数;每个物理槽占 `RINGBUFFERSIZE × 采样字节` 连续内存。

---

## 5. 路由表 `m_aui32Routing`

```c
// RTP_stream_info.h:99,106
unsigned char m_byNbOfChannels;                              // 本流的流通道数(≤64)
uint32_t      m_aui32Routing[MAX_CHANNELS_BY_RTP_STREAM];    // [StreamChannelId] = PhysicalChannelId; ~0 表示未用

// RTP_stream_info.c:299,308  (访问器)
rtp_stream_info->m_aui32Routing[ui32StreamChannelId] = ui32PhysicalChannelId;  // set
return rtp_stream_info->m_aui32Routing[ui32StreamChannelId];                   // get
```

语义:**流通道 i → 物理槽 m_aui32Routing[i]**。`~0`(0xFFFFFFFF)表示该流通道未接任何物理槽(Sink 允许部分通道悬空)。

合法性校验(`RTP_stream_info.c:202-211`):`m_byNbOfChannels` 必须 > 0 且 ≤ `MAX_CHANNELS_BY_RTP_STREAM`(64)。

### 路由表在流初始化时被消费(`RTP_audio_stream.c`)

**Source(`m_bSource == true`,第 114-130 行)**:逐个流通道 `us`,取出物理槽 `ulChannelId = get_routing(us)`,把 `live_out`(playback_buffer)对应槽的指针存进 `m_pvLivesOutCircularBuffer[us]`。此后 Source 打包 RTP 时,从这些指针读音频发出去。

```c
// RTP_audio_stream.c:117-129
for (us = 0; us < pRTP_stream_info->m_byNbOfChannels; us++) {
    ulChannelId = get_routing(pRTP_stream_info, us);
    if (!pManager->get_live_out_jitter_buffer(pManager->user, ulChannelId)) {
        // 物理槽不可用(>= m_NumberOfOutputs)→ Init 失败
        return 0;
    }
    self->m_pvLivesOutCircularBuffer[us] = pManager->get_live_out_jitter_buffer(pManager->user, ulChannelId);
}
```

**Sink(`m_bSource == false`,第 247-264 行)**:同样取物理槽,但绑到 `live_in`(capture_buffer);`~0` 的流通道置 NULL(静音/跳过)。

```c
// RTP_audio_stream.c:247-263
for (us = 0; us < pRTP_stream_info->m_byNbOfChannels; us++) {
    ulChannelId = get_routing(pRTP_stream_info, us);
    if (ulChannelId == (unsigned)~0) {
        self->m_pvLivesInCircularBuffer[us] = NULL;        // 未用通道
    } else if (!pManager->get_live_in_jitter_buffer(pManager->user, ulChannelId)) {
        return 0;                                           // 物理槽不可用(>= m_NumberOfInputs)
    } else {
        self->m_pvLivesInCircularBuffer[us] = pManager->get_live_in_jitter_buffer(pManager->user, ulChannelId);
    }
}
```

---

## 6. 守护进程如何把 WebUI 的 map 写进路由表

WebUI 上配 Source/Sink 时的「channel map」是一个整数数组,就是物理槽 ID 列表。守护进程把它拷进 `m_aui32Routing`,并据此设 `m_byNbOfChannels`:

```c
// session_manager.cpp —— Source (add_source_, 写入方向: map → 路由表)
info.stream[0].m_byNbOfChannels = source.map.size();          // 561
std::copy(source.map.begin(), source.map.end(),               // 601
          info.stream[0].m_aui32Routing);

// session_manager.cpp —— Sink (add_sink_, 写入方向: map → 路由表)
info.stream[0].m_bSource = 0;  // sink                          915
info.stream[0].m_byNbOfChannels = sink.map.size();            // 921
std::copy(sink.map.begin(), sink.map.end(),                    // 922
          info.stream[0].m_aui32Routing);

// session_manager.cpp —— 反向读回(路由表 → map DTO,供 API/WebUI 响应)
// get_source_:408-409 / get_sink_:421-422 构造 {m_aui32Routing[0..m_byNbOfChannels-1]}
```

即:`map = [0, 1]` 表示流通道 0→物理槽 0,流通道 1→物理槽 1。
`map = [5, 7, 9]` 表示流通道 0→槽 5,1→槽 7,2→槽 9。

> 注意:`m_byNbOfChannels` 是 `unsigned char`(8 位),且 ≤ 64,所以 `map` 长度也受此限。

---

## 7. 完整数据流向

```
                ┌──────── ALSA 虚拟声卡 RAVENNA (双缓冲) ────────┐
   aplay ──写──▶│  playback_buffer (播放池, 默认 64 槽)           │
                │   槽0 槽1 ... 槽63                              │
                │      ↑ get_live_out_jitter_buffer(槽id)         │
                │      └── Source 流 m_pvLivesOutCircularBuffer   │──打包──▶ RTP 多播发出去
                │                                                  │
   arecord◀─读──│  capture_buffer (录音池, 默认 64 槽)            │
                │   槽0 槽1 ... 槽63                              │
                │      ↑ get_live_in_jitter_buffer(槽id)          │
                │      └── Sink 流 m_pvLivesInCircularBuffer      │◀──解包── RTP 多播收进来
                └──────────────────────────────────────────────────┘
```

- **Source 链路**:`aplay -D plughw:RAVENNA -c N` 把 PCM 写进 playback_buffer → 驱动按 `m_pvLivesOutCircularBuffer[us]` 指针读对应槽 → 打包 RTP 发到 `m_ui32DestIP:5004`。
- **Sink 链路**:驱动从 RTP 收到音频 → 按 `m_pvLivesInCircularBuffer[us]` 指针写进 capture_buffer 对应槽 → `arecord -D plughw:RAVENNA -c N` 从 capture_buffer 读出 PCM。

PTP SAC(采样绝对计数)用于在环形缓冲里定位读写偏移(`get_live_in/out_jitter_buffer_offset`,`manager.c:1397-1441`);Sink 若超时未收到包,`IsLivesInMustBeMuted()`(`RTP_audio_stream.c:1017-1022`)会对该槽写静音,避免读到陈旧数据。

---

## 8. WebUI 的硬性约束(`webui/src/`)

WebUI 在前端就把范围卡死在 64,与运行时默认池大小对齐:

| 文件:行 | 约束 |
|---------|------|
| `SourceEdit.jsx:214` / `SinkEdit.jsx:214` | 通道数输入框 `max='64'` |
| `SourceEdit.jsx:78,155` / `SinkEdit.jsx:68,123` | 起始槽循环 `v <= (64 - channels)` |
| `SourceEdit.jsx:136` | `maxChannels > 64 ? 64 : maxChannels`(再受包大小限制) |
| `Sources.jsx:125` | Source 总数 `< 64` |
| `Sources.jsx:243` | 默认 map `[(id*2)%64, (id*2+1)%64]` —— **物理槽 ID 对 64 取模** |

最后一条尤其关键:即使你手改构建到 128,WebUI 默认生成的 map 也会用 `% 64` 把槽 ID 拉回 0..63。要配 64~127 槽,得绕过 WebUI 直接调 HTTP API。

---

## 9. 实际可用槽位与「占满」问题

回到最常见的问题:**「配很多 Sink 占满物理声道,Source 还有得用吗?」**

**不会受影响。** 因为:

1. Sink 占的是 `capture_buffer` 的 0..63 槽(受 `m_NumberOfInputs=64` 限制)。
2. Source 用的是 `playback_buffer` 的 0..63 槽(受 `m_NumberOfOutputs=64` 限制)。
3. 两块缓冲物理独立,编号各自从 0 起,互不干涉。

所以可以同时:64 路 Sink(占满 capture 池)+ 64 路 Source(占满 playback 池),共 128 路并发,各方向不抢。

**真正的限制是单方向 64 槽**:一个方向(比如 Sink)配了 64 路单声道流、把 capture 池 0..63 全占满后,第 65 路 Sink 就要复用已有槽(多流映射到同一物理槽 → 后到的流会覆盖,这是配置错误,不是池容量问题),除非把 `m_NumberOfInputs` 提到 128。

### 多流映射到同一物理槽会怎样?

代码**不做互斥检查**——路由表只是「流通道 → 槽指针」的查表。两条 Sink 流都映射到 capture 槽 0,两者会交替/竞争写同一块内存,产生交错杂音。同理两条 Source 流映射到 playback 槽 0,会竞争读。**正确做法:同一方向内,每个物理槽只给一条流用。**

---

## 10. 通道映射的几条规则速查

1. **方向隔离**:Source 只走 playback 池,Sink 只走 capture 池,绝不交叉。
2. **池容量**:每方向默认 64 槽,上限 128(完整构建 + 手动 `SetNumberOfInputs/Outputs`)。
3. **单流上限**:每条 RTP 流 ≤ 64 个流通道(`MAX_CHANNELS_BY_RTP_STREAM`)。
4. **映射粒度**:流通道 → 物理槽,一对一由 `m_aui32Routing` 决定,`~0` 表示悬空(仅 Sink 有用)。
5. **槽位复用**:代码不阻止同向多流共占一槽,但会互相覆盖,属配置错误。
6. **越界即失败**:物理槽 ≥ `m_NumberOfInputs/Outputs` 时流 Init 失败(日志 `get_live_in/out_jitter_buffer(N) not available`)。
7. **WebUI 天花板**:前端限死 0..63,改 128 须走 HTTP API + 改驱动计数。

---

## 11. 源码位置索引(维护速查)

| 主题 | 文件:行 |
|------|---------|
| 编译期容量宏 | `common/MergingRAVENNACommon.h:62-82` |
| 单流通道上限 | `driver/RTP_stream_info.h:49` |
| 驱动声道数宏(只取 INPUTS) | `driver/audio_driver.c:62` |
| 物理 buffers 数组 | `driver/audio_driver.c:149-152` |
| PCM channels_max | `driver/audio_driver.c:1240,1269` |
| 运行时计数初值 | `driver/manager.c:163-167` |
| SetNumberOfInputs/Outputs | `driver/manager.c:561-589` |
| 消息分发 Set/Get | `driver/manager.c:899-944` |
| live_in 绑 capture_buffer + 边界 | `driver/manager.c:1346-1361` |
| live_out 绑 playback_buffer + 边界 | `driver/manager.c:1365-1380` |
| 路由表定义 + set/get | `driver/RTP_stream_info.h:106`;`driver/RTP_stream_info.c:299,308` |
| 路由合法性校验 | `driver/RTP_stream_info.c:202-211` |
| Source 消费路由 → live_out | `driver/RTP_audio_stream.c:114-130` |
| Sink 消费路由 → live_in | `driver/RTP_audio_stream.c:247-264` |
| SAC 偏移定位 | `driver/manager.c:1397-1441` |
| Sink 静音判定 | `driver/RTP_audio_stream.c:1017-1022` |
| daemon: map→路由表(Source 写入) | `daemon/session_manager.cpp:561,601` |
| daemon: map→路由表(Sink 写入) | `daemon/session_manager.cpp:915-922` |
| daemon: 路由表→map DTO(读回) | `daemon/session_manager.cpp:408-409`(source),`421-422`(sink) |
| daemon: get 计数(无 set) | `daemon/driver_manager.cpp:240-251` |
| daemon: Source SDP `a=recvonly` | `daemon/session_manager.cpp:788`(get_source_sdp_) |
| WebUI: 64 上限 | `webui/src/SourceEdit.jsx:78,136,155,214`;`SinkEdit.jsx:68,123,214`;`Sources.jsx:125,243` |

---

## 12. 常见误区

1. **「128 物理声道池 source/sink 共享」** —— 错。是 playback/capture 两个独立池,各 64(默认)/ 128(上限),按方向分不按数量分。
2. **「完整构建就能用 128 通道路由」** —— 错。构建只决定缓冲数组尺寸和 ALSA `channels_max`;路由层可用槽由 `m_NumberOfInputs/Outputs`(默认 64)决定,且 daemon 不改它。
3. **「arecord 报 [1 128] 就能录 128 路网络音频」** —— 错。那是 ALSA 设备能力,跟路由可用槽无关;路由到 64~127 会失败。
4. **「`MAX_NUMBEROFOUTPUTS` 控制播放池大小」** —— 错。它是 dead code,播放池实际由 `MR_ALSA_NB_CHANNELS_MAX = MAX_NUMBEROFINPUTS` 决定。
5. **「Source 和 Sink 都用 map=[0] 会冲突」** —— 错。Source 的槽 0 在 playback_buffer,Sink 的槽 0 在 capture_buffer,不是同一块内存。
6. **「多 Sink 映射到同一槽会自动混音」** —— 错。会互相覆盖产生杂音,需手动避免。

---

## 附:验证命令

```bash
# 看 ALSA 设备能力(注意:这是设备能力,不是路由可用槽)
arecord -D plughw:RAVENNA --dump-hw-params -d 1 /dev/null 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'
aplay    -D plughw:RAVENNA --dump-hw-params -d 1 /dev/null 2>&1 | grep -E 'CHANNELS|RATE|FORMAT'

# 看运行时实际计数(daemon HTTP)
curl -s http://127.0.0.1:8080/api/ptp/status | jq
# 查 dmesg 里 SetNumberOfInputs/Outputs 的打印(确认是否仍是 64)
dmesg | grep -E 'SetNumberOfInputs|SetNumberOfOutputs'
```
