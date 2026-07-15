# License Notices and Open Source Compliance Notes

> 本文件基于仓库内现有 `LICENSE`、源码头部、`README.md`、`CMakeLists.txt`
> 和 `webui/package-lock.json` 整理，用于二次开发、打包和对外交付前的合规检查。
> 这不是法律意见；商业产品发布前建议由法务结合实际分发方式复核。

## 总体结论

本仓库主体，包括 AES67 daemon 和 WebUI，按 GNU GPL 第 3 版或后续版本
（`GPL-3.0-or-later`）分发。基于这些模块的二次开发如果对外分发，通常需要：

- 保留原版权声明、许可证文本和免责声明。
- 明确标注修改内容和修改日期。
- 将派生作品整体继续按 GPLv3 或兼容的后续版本授权。
- 向接收方提供完整对应源码，包括构建、安装、运行和修改所需脚本。
- 不对接收方施加额外限制，例如禁止再分发、禁止修改或额外保密义务。
- 如果以设备、固件、镜像或预装产品形式交付，确认 GPLv3 中关于安装信息和反规避限制的义务是否适用。

仅在公司内部修改和使用、没有向第三方分发副本时，通常不会触发 GPL 的源码交付义务。

## 仓库内模块

| 模块 | 许可证 | 适用范围 | 二次开发/分发规范 | 证据 |
| --- | --- | --- | --- | --- |
| AES67 daemon | `GPL-3.0-or-later` | `daemon/` 下 C++ daemon 源码 | 对外分发修改版或二进制时，保留 GPL 文本和版权声明，标注修改，并提供对应源码。 | `README.md` License 段；`daemon/LICENSE`；`daemon/*.hpp` 和 `daemon/*.cpp` 头部 |
| WebUI | `GPL-3.0-or-later` | `webui/src/` 前端源码和构建产物 | WebUI 修改后随产品、固件、安装包或镜像分发时，按 GPLv3-or-later 提供源码和许可说明。 | `README.md` License 段；`webui/LICENSE`；`webui/src/*.jsx` 头部 |
| wavplay_am824 工具 | `GPL-3.0-or-later` | `wavplay_am824/` | 如随产品或工具包分发，保留 Dolby 版权声明和 GPL 条款，并提供对应源码。 | `wavplay_am824/wavplay_am824.cpp`；`wavplay_am824/am824_framer.h` |
| platform latency test 工具 | `GPL-2.0-or-later` | `test/latency.cc` | 如分发该测试工具或其修改版，保留 GPLv2-or-later 头部和源码交付义务。注意与 GPLv3 代码合并时做兼容性复核。 | `test/latency.cc` 源码头部 |
| platform check/createtest 工具 | 未在源码头部发现明确许可证声明 | `test/check.cc`、`test/createtest.cc` | 如需对外分发，建议先向上游确认许可或用项目整体 GPL 许可做保守处理。 | `test/check.cc`；`test/createtest.cc` |

## 第三方源码和子模块

| 依赖/子模块 | 许可证 | 在本项目中的用途 | 合规规范 | 证据 |
| --- | --- | --- | --- | --- |
| Merging Technologies ALSA RAVENNA/AES67 Driver, kernel part | GNU GPL；源码头部多处写明 `GPL-2.0-or-later`，README 使用 GNU GPL 链接描述 | `3rdparty/ravenna-alsa-lkm/driver` 和 `common`，提供 ALSA/RAVENNA/AES67 kernel module 及 daemon 通信用头文件 | 分发内核模块、修改版内核模块或派生源码时，保留 GPL 声明并提供对应源码。README 链接和源码头部的版本表述不完全一致，产品化时应按源码头部逐文件复核。Linux kernel module 的具体派生关系和内核 GPL 义务也应单独复核。 | `3rdparty/ravenna-alsa-lkm/README.md`；`3rdparty/ravenna-alsa-lkm/driver/*.c`；`3rdparty/ravenna-alsa-lkm/common/*.h` |
| Merging Technologies Butler 用户态软件 | 专有 EULA；商业/OEM 集成需另行授权 | `3rdparty/ravenna-alsa-lkm/Butler` | 不要默认把 Butler 当作可自由商用开源组件。EULA 禁止商业目的复制、分发、转售和 OEM 集成，商业产品使用需联系 Merging Technologies 获取商业授权。 | `3rdparty/ravenna-alsa-lkm/README.md`；`3rdparty/ravenna-alsa-lkm/Butler/LICENSE.md` |
| cpp-httplib | MIT | daemon HTTP/REST 服务使用的 header-only HTTP 库 | 分发源码或二进制时保留 MIT 版权声明和许可文本。MIT 不要求派生代码开源，但本项目主体仍受 GPL 约束。 | `3rdparty/cpp-httplib/LICENSE`；`daemon/CMakeLists.txt` |
| Dojo/Rickshaw 等 Butler WebApp 静态资源 | Dojo: AFL 或 modified BSD；Rickshaw: MIT；另有 BSD 片段 | 位于 Butler 用户态软件的 webapp 资源中 | 仅在分发 Butler/webapp 相关内容时适用。需保留对应版权和许可文本，同时仍受 Butler EULA 的额外限制。 | `3rdparty/ravenna-alsa-lkm/Butler/webapp/advanced/dojo/resources/LICENSE`；相关 JS 文件头部 |

## daemon 构建和运行依赖

| 依赖 | 许可证 | 启用条件/用途 | 合规规范 | 证据 |
| --- | --- | --- | --- | --- |
| Boost libraries | Boost Software License 1.0 | 必需；`thread`、`filesystem`、`log`、`program_options` | 分发时保留 Boost Software License 文本。该许可证 permissive，不强制开源调用方代码。 | `README.md`；`daemon/CMakeLists.txt` |
| Avahi common/client libraries | LGPL | `WITH_AVAHI=ON` 时用于 mDNS 支持 | 动态链接时通常需要允许用户替换/重新链接 LGPL 库，并随分发提供 LGPL 文本和库源码获取方式。若静态链接，义务更重，需法务复核。 | `README.md`；`daemon/CMakeLists.txt` |
| Freeware Advanced Audio Coder, libfaac | LGPL | `WITH_STREAMER=ON` 时用于 AAC streamer | 按 LGPL 保留许可文本，提供库源码获取方式，并确保用户可以替换/重新链接 LGPL 组件。还需单独评估 AAC 编码相关专利/地区风险。 | `README.md`；`daemon/CMakeLists.txt` |
| ALSA library, libasound | LGPL | `WITH_STREAMER=ON` 时用于音频采集/播放接口 | 按 LGPL 处理动态链接和许可文本；避免把 LGPL 库静态合入不可替换的闭源产物。 | `daemon/CMakeLists.txt` |
| systemd library, libsystemd | LGPL | `WITH_SYSTEMD=ON` 时用于 notify/watchdog | 按 LGPL 处理动态链接、许可文本和源码获取方式。 | `daemon/CMakeLists.txt` |

## WebUI npm 依赖

WebUI 的直接依赖和开发依赖记录在 `webui/package.json` 与
`webui/package-lock.json`。当前直接依赖的锁文件许可证如下：

| npm 包 | 类型 | 许可证 | 合规规范 |
| --- | --- | --- | --- |
| `react` | runtime | MIT | 保留 MIT 版权和许可文本。 |
| `react-dom` | runtime | MIT | 保留 MIT 版权和许可文本。 |
| `react-modal` | runtime | MIT | 保留 MIT 版权和许可文本。 |
| `react-router-dom` | runtime | MIT | 保留 MIT 版权和许可文本。 |
| `react-toastify` | runtime | MIT | 保留 MIT 版权和许可文本。 |
| `@vitejs/plugin-react-refresh` | dev | MIT | 开发依赖，若不随产品分发一般不进入交付包；若分发工具链，保留 MIT 文本。 |
| `vite` | dev | MIT | 开发依赖，若不随产品分发一般不进入交付包；若分发工具链，保留 MIT 文本。 |

`package-lock.json` 中的传递依赖包含 MIT、Apache-2.0、BSD-3-Clause、ISC、
CC-BY-4.0 等许可证。每次升级前端依赖或发布包含 `webui/dist` 的产品包前，应从
锁文件重新生成第三方清单或 SBOM，并随交付包提供对应 NOTICE/许可证文本。

## 其他工具依赖

| 依赖 | 许可证 | 适用范围 | 合规规范 | 证据 |
| --- | --- | --- | --- | --- |
| PortAudio | PortAudio License，MIT 风格 | `wavplay_am824` 构建依赖 | 如分发链接后的工具，保留 PortAudio 版权和许可文本。 | `wavplay_am824/CMakeLists.txt` |
| libsndfile | LGPL | `wavplay_am824` 在 Linux/Unix 下读取音频文件 | 按 LGPL 处理动态链接、许可文本和源码获取方式。 | `wavplay_am824/CMakeLists.txt` |
| pthread | 系统库 | `test/` 平台测试工具 | 一般作为系统库处理。 | `test/Makefile` |
| ALSA library, libasound | LGPL | `test/latency` 平台测试工具 | 如分发该测试工具，按 LGPL 保留许可文本并提供库源码获取方式。 | `test/Makefile` |

## 对外交付检查清单

发布二次开发版本、设备镜像、安装包、容器镜像或 SDK 前，至少检查：

1. 交付包中包含 `LICENSE`、`daemon/LICENSE`、`webui/LICENSE` 和本文件。
2. 对所有修改过的 GPL 文件保留原头部，并添加清晰的修改说明和日期。
3. 对外交付的二进制有对应源码包，源码包能重建相同版本的 daemon、WebUI、驱动和工具。
4. 源码包包含构建脚本、补丁、配置模板、子模块版本和生成 WebUI 的依赖锁文件。
5. 若启用了 Avahi、FAAC、ALSA、libsndfile 或 systemd，提供 LGPL 许可证文本和库源码获取方式。
6. 若分发 `3rdparty/ravenna-alsa-lkm/Butler` 或其 webapp，先确认 EULA 和商业/OEM 授权。
7. 若产品锁定设备启动链或禁止用户安装修改版，单独复核 GPLv3 的安装信息要求。
8. 为 WebUI 前端产物生成 npm 依赖 SBOM，覆盖 `package-lock.json` 中所有传递依赖。
9. 在产品 About、WebUI 法律信息页、CLI `--version` 输出或随包文档中提供开源许可证入口。
10. 不在 EULA、销售合同或交付条款中加入与 GPL/LGPL 权利冲突的限制。

## 建议的交付目录结构

```text
licenses/
  LICENSE_NOTICES.MD
  GPL-3.0.txt
  GPL-2.0.txt
  LGPL-2.1.txt
  LGPL-3.0.txt
  MIT-cpp-httplib.txt
  Boost-1.0.txt
  npm-third-party-notices.txt
source/
  aes67-linux-daemon/
  build-instructions.md
  submodule-versions.txt
```

## 维护规则

- 升级 `3rdparty/` 子模块后，重新检查其 `LICENSE`、`README` 和源码头部。
- 修改 `daemon/CMakeLists.txt`、`wavplay_am824/CMakeLists.txt`、`test/Makefile`
  或 `webui/package-lock.json` 后，同步更新本文件。
- 新增可选功能依赖时，记录许可证、链接方式、启用条件和分发义务。
- 发布前用锁文件或 SBOM 工具生成机器可读清单，避免遗漏传递依赖。
