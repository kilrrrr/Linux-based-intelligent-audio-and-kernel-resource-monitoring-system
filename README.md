# kernel_detection

这是一个以 Linux 内核资源监测、驱动实验和嵌入式音乐播放器联调为主的项目仓库。

当前仓库主要保留两条主线：

1. `Linux-kernel/`：资源监测内核模块与配套用户态工具。
2. 音乐播放器联调样例：服务端和 RV1126 设备端。

## 目录说明

| 目录 | 作用 |
| --- | --- |
| `Linux-kernel/` | Linux 内核模块与用户态工具。包含 `res_guard` 资源监测模块、`music_gpio` GPIO 驱动，以及 `res_guardd` / `res_guardctl` / `res_guard_test` / `res_guard_alsa_demo` 等工具。 |
| `platform-code2/music_player/` | 设备端程序，运行在 Linux 平台上，负责音乐播放、按键/LED、摄像头、Socket 通信等功能。 |
| `server-code/music_server/` | 中间服务端，基于 `libevent` 和 `json-c`，负责设备绑定、状态转发和控制命令分发。 |

## 主要模块

### 1. `Linux-kernel/`

这一部分是仓库里相对完整的一套内核资源监测实现。

核心内容：

- `res_guard.c`：周期采样 CPU 和内存状态，并统计音频欠载、恢复失败、UART 丢包、等待超时等事件。
- `res_guard.h`：用户态和内核态共用的事件、快照、阈值、`ioctl` 接口定义。
- `res_guardd.c`：守护进程，持续轮询 `/dev/res_guard`，输出快照和告警信息。
- `res_guardctl.c`：命令行管理工具，可查看状态、修改阈值、等待事件、清空计数器、ACK 告警。
- `res_guard_test.c`：测试程序，用于触发和验证 `res_guard` 接口。
- `res_guard_alsa_demo.c`：ALSA 场景下的演示程序，用于把音频事件上报给 `res_guard`。
- `device.c` + `fops.c`：`music_gpio` 驱动，提供按键和 LED 的字符设备接口。

目标接口：

- `/dev/res_guard`
- `/proc/res_guard/status`
- `/dev/buttons`
- `/dev/leds`

注意：

- `music_gpio` 中的 GPIO 默认值只是占位配置，换板子后通常需要按实际硬件修改。
- 内核模块需要与你的目标内核版本匹配的 headers。

### 2. `server-code/music_server/`

这是音乐播放器联调方案里的中控服务。

职责包括：

- 维护设备和控制端的绑定关系。
- 接收设备端上报的在线状态、播放状态、歌曲列表、摄像头信息。
- 将控制命令转发给对应设备，并把设备回复返回给对端。

实现特点：

- 使用 `libevent` 处理 TCP 连接。
- 使用 `json-c` 处理 JSON 消息。
- 协议采用按行分隔的 JSON 文本。

### 3. `platform-code2/music_player/`

这是运行在 RV1126 设备侧的程序，负责真正执行播放和外设控制逻辑。

功能包括：

- 连接服务端并定时上报在线状态。
- 读取音乐目录并维护播放列表。
- 响应播放控制命令，如开始、暂停、继续、上一首、下一首、音量调整、播放模式切换。
- 调用摄像头模块，支持状态查询、启动、停止和抓拍。
- 接入按键、LED、共享内存等本地设备逻辑。

当前代码里可以看到一些和部署环境强相关的默认路径：

- 音乐目录：`/mount/usb/`
- 摄像头节点：`/dev/video0`
- 摄像头抓拍目录：`/tmp/music_player_camera`

## 构建方式

### Linux 内核模块与工具

```bash
cd Linux-kernel
make
make userspace
make userspace-alsa
```

说明：

- `make` 会编译内核模块。
- `make userspace` 会编译 `res_guardd`、`res_guardctl`、`res_guard_test`。
- `make userspace-alsa` 会额外编译 `res_guard_alsa_demo`，需要 ALSA 开发库。

常见运行示例：

```bash
cd Linux-kernel
sudo insmod res_guard.ko
sudo ./res_guardctl status
sudo ./res_guardd
```

如果需要测试 GPIO 驱动：

```bash
cd Linux-kernel
sudo insmod music_gpio.ko
```

### 服务端

```bash
cd server-code/music_server
make
./main
```

依赖：

- `g++`
- `libevent`
- `json-c`

### 设备端

```bash
cd platform-code2/music_player
make
./music_player
```

依赖：

- `gcc`
- `pthread`
- Linux 设备环境
- 音乐文件目录
- 摄像头设备

## 典型启动顺序

如果你要跑完整的“服务端 + 设备端”链路，建议按下面顺序：

1. 先统一服务端和设备端的 IP、端口配置。
2. 在电脑上启动 `server-code/music_server`。
3. 在 RV1126 上启动 `platform-code2/music_player/music_player`。
4. 观察设备端上报和服务端日志，确认连接成功后再做联调。

## 当前网络配置

当前仓库内默认使用的联调地址：

- 服务端（电脑）：`192.163.1.100:8000`
- RV1126 设备：`192.163.1.10`
- 设备端连接目标：`192.163.1.100:8000`

需要重点检查的位置：

- `server-code/music_server/server.h`
- `platform-code2/music_player/socket.h`

## 建议阅读顺序

如果你的目标是熟悉仓库结构，建议按这个顺序阅读：

1. `Linux-kernel/res_guard.h`
2. `Linux-kernel/res_guard.c`
3. `Linux-kernel/res_guardctl.c`
4. `server-code/music_server/server.cpp`
5. `platform-code2/music_player/main.c`

## 备注

