# CamMicLink 麦克风驱动

**一个小巧、MIT 许可的 macOS 虚拟麦克风驱动。**

[English](README.md) | 简体中文

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform: macOS 12+](https://img.shields.io/badge/platform-macOS%2012%2B-lightgrey)
![Type: AudioServerPlugIn](https://img.shields.io/badge/type-AudioServerPlugIn-informational)

CamMicLink 麦克风是一个 AudioServerPlugIn（用户态 HAL 插件），为所有
CoreAudio 应用提供一个系统级虚拟麦克风。它不需要内核扩展（kext），不需要常驻
特权守护进程，也不依赖任何第三方运行时——只是在
`/Library/Audio/Plug-Ins/HAL` 里放一个 `.driver` 包。

本驱动为[镜麦传（CamMicLink）](https://oscarzhou.com/cammiclink/) App 开发，现独立
成库，以 MIT 协议开源。

## 为什么再造一个虚拟麦克风驱动？

macOS 上，我一开始考虑使用的开源虚拟声卡是 BlackHole，采用 GPL-3.0。copyleft
对某些项目是优点，但它不适合闭源应用和 App Store 分发：一旦打包或链接 GPL 代码，
整个应用就需要以 GPL 开源。

我需要一个能装进闭源商业应用里的驱动，于是参考 Apple 官方示例 NullAudio，在 GPT
和 DeepSeek 的辅助下自己动手写了一个，并以 MIT 协议发布。任何人都可以使用、
修改、嵌入商业产品，甚至直接销售，而没有开源自己代码的义务。这也是我对开源社区
的回馈——这个项目从开源生态中受益，希望这份实现能帮到更多人。

驱动是独立实现，没有拷贝 BlackHole 或其他第三方虚拟声卡的代码。CFPlugIn 骨架
沿用 Apple 以 MIT 许可发布的 *NullAudio* 示例（原样收录在 `reference/`），设备
拓扑、时钟和音频引擎均为独立重写。

## 它能做什么

驱动对外提供两个相互关联的设备：

```text
  音频来源：手机桥接、网络接收、文件播放、DAW……
        │  按采样时间写入 PCM
        ▼
  ┌─────────────────────────────────────────────────────┐
  │ CamMicLink Private Writer（隐藏，仅输出）            │
  │ UID com.oscarzhou.cammiclink.writer.v3              │
  └──────────────────────────┬──────────────────────────┘
                             │  按时间戳寻址的 PCM 环形缓冲
                             ▼
  ┌─────────────────────────────────────────────────────┐
  │ CamMicLink Microphone（公开，仅输入）                │
  │ UID com.oscarzhou.cammiclink.microphone.v3          │
  └───────┬───────────────────────────┬─────────────────┘
          ▼                           ▼
   微信 / Telegram / 腾讯会议      OBS / QuickTime / ……
```

- **CamMicLink Microphone** 是录音应用看到的设备。它只有输入流，**没有输出流**，
  所以会议软件不可能误写麦克风、把真正的供音端覆盖掉。
- **CamMicLink Private Writer** 是仅输出的设备，`kAudioDevicePropertyIsHidden = 1`。
  它不会出现在普通设备枚举里，也不能被设为默认输出；供音端可按 UID 找到它并写入。
- 两个设备共用一条连续的 48 kHz 采样时钟。音频按**绝对采样时间戳**定位，而不是
  按 FIFO 顺序，因此多个客户端可以同时录同一个虚拟麦克风而互不消费；晚加入的客户端
  在“过去”的时间段拿到静音，而不是别人残留的旧声音。

## 特性

- **无 kext、无常驻助手。** 一个 AudioServerPlugIn 包，由 `coreaudiod` 加载。
- **公开端仅输入。** 录音客户端只有读取权限。
- **隐藏写入端。** 设备列表保持干净，永远不会变成默认输出。
- **按时间戳寻址。** 65,536 帧环形缓冲，每帧携带绝对采样时间。未写入、已被覆盖或
  超出范围的帧读出为零，旧音频不会被重播。
- **支持多读者。** 读取不消费数据，多个客户端可同时采集同一时间段；同一时刻只允许
  一个写入者。
- **实时安全的 IO。** 音频路径不分配内存、不加锁、不做文件 IO、不写日志。双声道
  样本通过一次 64 位无锁原子操作发布。
- **连续时钟。** 客户端接入/退出不会重置时钟或清空缓冲；长时间空档按真实经过时间
  计算。
- **有界处理。** 异常高频的回调风暴会被限流保护拦住；降级时输出确定的静音，而不是
  未初始化的数据。
- **固定、可预期的格式。** 48 kHz、双声道、交织 Float32。
- **有测试。** 离线属性/时序回归，以及真机 HAL 探针。
- **MIT 许可。** 开源软件和商业闭源软件都可以用。

## 设备与标识符

| 对象 | UID | 流 | 应用可见性 |
| --- | --- | --- | --- |
| CamMicLink Microphone | `com.oscarzhou.cammiclink.microphone.v3` | 仅输入 | 可见，可选中/设为默认输入 |
| CamMicLink Private Writer | `com.oscarzhou.cammiclink.writer.v3` | 仅输出 | 隐藏（`IsHidden=1`），可按 UID 访问 |
| CamMicLink Box | `com.oscarzhou.cammiclink.box.v3` | — | 仅作为容器对象 |

插件包标识符为 `com.oscarzhou.cammiclink.audiodriver`，安装后的路径是
`/Library/Audio/Plug-Ins/HAL/CamMicLinkMic.driver`。

驱动刻意不提供音量、静音、数据源、EQ、自动增益或降噪控件。它只搬运采样。

## 环境要求

- macOS 12 Monterey 或更高版本。
- Xcode Command Line Tools（`xcode-select --install`）。
- 仅在安装时需要管理员权限。

通用二进制包含 `arm64` 和 `x86_64` 两个架构。本项目在 macOS 27.0 / Apple Silicon
上构建并测试；Intel 架构会编译，但未在 Intel 真机上运行过。

## 构建

```bash
./build.sh
```

产物：

| 路径 | 内容 |
| --- | --- |
| `build/CamMicLinkMic.driver` | 驱动包 |
| `build/hal_probe` | 拓扑/供音/录音探针 |
| `build/test_driver` | 离线回归测试程序 |
| `artifacts/CamMicLink-Microphone-0.3.0.pkg` | 安装包 |
| `artifacts/Uninstall-CamMicLink-Microphone.pkg` | 卸载包 |
| `artifacts/offline-test-results.txt` | 最近一次离线测试日志 |

默认使用 **ad-hoc 签名**，本机使用足够。要对外分发，请使用自己的 Developer ID：

```bash
CML_SIGN_ID="Developer ID Application: Your Name (TEAMID)" \
CML_INSTALLER_ID="Developer ID Installer: Your Name (TEAMID)" \
./build.sh
```

仓库中没有硬编码任何证书或团队标识。

## 安装与卸载

```bash
sudo ./scripts/install.sh     # 复制已构建的驱动并重启 coreaudiod
sudo ./scripts/uninstall.sh   # 只移除 CamMicLinkMic.driver
```

`install.sh` 只会替换 `CamMicLinkMic.driver`，不会动其他音频插件。重启
`coreaudiod` 会让系统音频短暂中断。

也可以直接安装 `artifacts/CamMicLink-Microphone-0.3.0.pkg`。除非设置了
`CML_INSTALLER_ID`，安装包默认未签名，在其他 Mac 上会被 Gatekeeper 拦截。

## 快速上手

安装后，先检查拓扑并跑一次回环测试。运行前请先停掉任何正在向隐藏写入端供音的
应用——同一时刻只允许一个写入者。

```bash
./build/hal_probe inspect          # 必须输出 "Topology: PASS"

# 终端 1：向隐藏写入端送入 440 Hz 正弦波
./build/hal_probe tone 15 512

# 终端 2：从公开麦克风录音
./build/hal_probe record 10 512
```

如果录到的声音静音、削波、不连续或频率偏离 440 Hz，`record` 会失败。它只读取
虚拟麦克风的 UID，绝不会读取物理麦克风。

然后打开任意音视频应用，把输入设备选为 **CamMicLink Microphone**，再由你自己的
供音端写入音频。完整的多进程测试（IOProc、AUHAL、AudioQueue，8–48 kHz）可以这样跑：

```bash
python3 tools/run_live_tests.py --seconds 8
```

## 从自己的应用供音

按 UID 找到隐藏写入端，并在它上面启动一个输出 IOProc：

```c
CFStringRef uid = CFSTR("com.oscarzhou.cammiclink.writer.v3");
AudioObjectPropertyAddress a = {
    kAudioHardwarePropertyTranslateUIDToDevice,
    kAudioObjectPropertyScopeGlobal,
    kAudioObjectPropertyElementMain
};
AudioDeviceID writer = kAudioObjectUnknown;
UInt32 size = sizeof(writer);
AudioObjectGetPropertyData(kAudioObjectSystemObject, &a,
                           sizeof(uid), &uid, &size, &writer);
```

用 `AudioDeviceCreateIOProcID(writer, callback, context, &proc)` 创建 IOProc，
`AudioDeviceStart` 启动，然后在回调里把交织双声道 Float32 写进
`output->mBuffers[0]`。采样位置使用 `outputTime->mSampleTime`：时间戳决定你的
音频落在共享时钟的哪个位置，不要改用私有帧计数器。

只有第一个写入者会被接受。在它停止之前，对写入端再次调用 `AudioDeviceStart` 会
返回 `kAudioHardwareIllegalOperationError`。

完整且无第三方依赖的实现见 [`tools/hal_probe.c`](tools/hal_probe.c) 中的
`callback()` 和 `run()`。

录音端不需要特殊代码：按 UID 选中公开设备，或直接在应用的输入设置里选
“CamMicLink Microphone”即可。

## 格式与时间约定

| 属性 | 值 |
| --- | --- |
| 采样率 | 48,000 Hz（固定；驱动会拒绝 44.1 kHz 等其他速率） |
| 声道 | 2，交织 |
| 样本格式 | Float32，packed，本机字节序 |
| 上报输入延迟 | 2,048 帧 ≈ 42.67 ms |
| 安全偏移 | 0 |
| 零时间戳周期 | 512 帧 |
| 环形缓冲 | 65,536 帧 ≈ 1.37 秒 |
| 单次 IO 回调上限 | 8,192 帧 |

驱动不做采样率转换，也不做任何 DSP。需要其他速率的客户端请自行转换（AudioQueue
会转；AUHAL 要求客户端速率与设备一致，见
[Apple TN2091](https://developer.apple.com/library/archive/technotes/tn2091/_index.html)）。
42.67 ms 是设备上报给
HAL 的输入延迟，不是通话端到端延迟——供音端、网络和客户端缓冲都会叠加在上面。

两个设备使用同一条时钟，在插件初始化时锚定一次。写入按
`mOutputTime.mSampleTime` 寻址，读取按 `mInputTime.mSampleTime` 寻址。HAL 会对
输入时间戳做延迟补偿，驱动不会重复减去延迟。

## 测试

离线测试会用 AddressSanitizer 和 UndefinedBehaviorSanitizer 编译真实驱动源码，
检查：

- 每个受支持 selector 的属性大小和返回数据边界；
- 输入/输出方向，以及对不支持的格式和采样率的拒绝；
- 11 组写入/读取块大小组合下的逐样本一致性，包括两个不同块大小、不同时刻加入的
  读者；
- 数字静音、拒绝第二个写入者、读者启停互不影响、停止写入后的静音、环形覆盖和
  非法时间戳；
- 时钟跨越十分钟空档后仍然连续。

[`docs/TEST-RESULTS.md`](docs/TEST-RESULTS.md) 记录了真机测量结果：8 个并发
客户端，在 8–48 kHz 各速率下 440 Hz 音调恢复为 439.87–440.05 Hz，时间断裂、
无效时间戳、坏缓冲和溢出均为 0。

自动化测试**没有**覆盖：真实手机到 Mac 的微信/Telegram/腾讯会议通话、这些应用
采集后施加的 DSP、超过 30 分钟的长稳、Intel 硬件，以及每一个 macOS 版本。
合成音正确不等于第三方应用最终听起来一定正确。

## 限制

- 仅支持 macOS。这是 CoreAudio HAL 插件，不是跨平台音频 API。
- 只允许一个写入者，不做混音。多个来源请先在应用内混好再写入。
- 固定 48 kHz 双声道 Float32，没有按设备协商格式。
- 写入端是隐藏设备，无法在“音频 MIDI 设置”里选中，只能由供音端按 UID 访问。
- 不是通用音频路由：不做按应用采集、不做多设备路由、不做音效。
- 本机构建使用 ad-hoc 签名；对外分发请用 Developer ID 签名并公证安装包。
- 版本 0.3.0。v3 系列的 UID 和 bundle 标识符视为稳定接口。

## 项目结构

```text
src/
  CamMicLinkDriver.c     AudioServerPlugIn 接口、属性、IO 入口
  CMLAudioCore.c/.h      共享时钟、按时间戳寻址的环形缓冲、原子操作
tests/
  test_driver.c          离线契约与时序回归
tools/
  hal_probe.c            真机拓扑、供音、录音和重采样探针
  run_live_tests.py      多进程真机测试脚本
reference/
  NullAudio.c            Apple 未修改的示例
  LICENSE.txt            Apple 的 MIT 许可证
  UPSTREAM.md            下载地址与压缩包 SHA-256
scripts/
  install.sh             安装已构建的驱动
  uninstall.sh           卸载驱动
docs/
  TEST-RESULTS.md        真机测试记录与注意事项
build.sh                 通用构建、ad-hoc/Developer ID 签名、打包、测试
```

## 许可证

MIT，见 [LICENSE](LICENSE) 与 [NOTICE.md](NOTICE.md)。

`reference/NullAudio.c` 版权归 Apple Inc.（2024），同样以 MIT 许可发布。驱动沿用
该示例的 CFPlugIn 接口与属性约定，实现为独立编写。本项目不包含任何 GPL 代码，
包括 BlackHole。

Apple、macOS、CoreAudio 和 AudioServerPlugIn 是 Apple Inc. 的商标。本项目与
Apple 无隶属关系，也未获其背书。

## 参与贡献

欢迎提 issue 和 pull request。请保持 IO 路径不分配内存、不加锁、不做文件 IO、
不写日志；设备格式保持 48 kHz 双声道 Float32；提交 PR 前先运行 `./build.sh`。
如果修改环形缓冲或时钟逻辑，请在 `tests/test_driver.c` 中补充对应的回归用例。
