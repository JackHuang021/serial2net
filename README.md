# Serial2Net — UART-to-WiFi 透传桥

基于 ESP32 的串口转 WiFi 透明传输设备，用于嵌入式开发板无线调试。ESP32 连接目标开发板的 UART，通过 WiFi 将串口数据透传到 TCP 端口，支持 **Raw TCP** 和 **Telnet (RFC 854/856)** 两种协议。MacBook/PC 无需 USB 转串口线即可远程调试。

## 支持的芯片

| 芯片 | 架构 | 核心数 | 状态 |
|------|------|--------|------|
| ESP32-S3 | Xtensa LX7 | 双核 | ✅ 主要目标 |
| ESP32-C3 | RISC-V | 单核 | ✅ 完全支持 |
| ESP32 | Xtensa LX6 | 双核 | ✅ 完全支持 |

> **注意**：ESP32-C3 只有 UART0 和 UART1，默认的 UART 端口 2 需要手动改为 1（见下方引脚表）。

## 硬件连接

```
ESP32 端                目标设备端
───────                 ──────────
  GND    ──────────────  GND
  TX     ──────────────  RX
  RX     ──────────────  TX
  LED    ──────────────  WS2812B Data In (可选)
  BOOT   ──────────────  按钮到 GND (可选，长按进入 WiFi 配置)
```

默认引脚（可通过 `menuconfig` 修改）：

| 芯片 | UART TX | UART RX | WS2812B LED | BOOT 按钮 |
|------|---------|---------|-------------|-----------|
| ESP32-S3 | GPIO17 | GPIO16 | GPIO48 | GPIO0 |
| ESP32-C3 | GPIO6 | GPIO7 | GPIO8 | GPIO0 |
| ESP32 | GPIO17 | GPIO16 | GPIO18 | GPIO0 |

默认 UART 参数：**115200-8-N-1**，无硬件流控。

## 快速上手

### 1. 配置 & 编译

```bash
# 安装 ESP-IDF 工具链（首次）
~/.espressif/v6.0.2/esp-idf/install.sh

# 加载环境
source ~/.espressif/v6.0.2/esp-idf/export.sh

# 选择芯片（默认 ESP32-S3）
idf.py set-target esp32c3    # 如用 C3，需此步骤

# 配置 WiFi 和引脚（可选，默认值也能用）
idf.py menuconfig
# Serial2Net Configuration →
#   - WiFi Settings:         SSID / 密码 / 工作模式
#   - UART Settings:         波特率 / 引脚 / 端口号
#   - TCP Settings:          Raw TCP 监听端口（默认 8880）
#   - Telnet Settings:       Telnet 开关 / 端口（默认 23）
#   - LED Settings:          WS2812B 开关 / 引脚
#   - HTTP Configuration:    WiFi 配置门户开关 / BOOT 按钮引脚

# 编译
idf.py build
```

### 2. 烧录

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

### 3. 连接

ESP32 启动后会通过 mDNS 广播 `serial2net.local`，支持两种连接方式：

**Raw TCP（推荐，低延迟）：**

```bash
socat -,raw,echo=0,escape=0x1d TCP:serial2net.local:8880,nodelay
# 退出: Ctrl + ]

# 或使用 netcat
nc serial2net.local 8880
```

**Telnet（标准客户端，无需 raw 模式）：**

```bash
telnet serial2net.local
# 或指定端口
telnet serial2net.local 8023
```

如果 mDNS 解析失败，用 IP 直连：

```bash
socat -,raw,echo=0,escape=0x1d TCP:192.168.x.x:8880,nodelay
```

## WiFi 工作模式

| 模式 | 行为 |
|------|------|
| Station | 连接到指定的 WiFi 路由器 |
| AP | ESP32 创建热点 `serial2net`（密码 `12345678`），电脑直连 |
| STA + AP 回退（默认） | 先尝试 Station，15 秒超时后自动启动 AP |

WiFi 凭据优先级：**NVS 已保存的 > Kconfig 出厂默认值**。通过 Web 界面保存的凭据会持久化到 NVS，重启后仍然有效。

## Web 管理界面

设备启动后 HTTP 服务常驻运行（端口 80），可通过 STA IP 或 AP IP 访问。

**STA 正常连接时**：浏览器打开 `http://<设备IP>/` 即可访问管理界面。设备 IP 可通过串口日志或路由器 DHCP 列表查看。

**STA 未连接时**：设备自动启动 AP 热点，长按 BOOT 按钮（≥400ms）也可手动开启/关闭 AP：

1. 电脑连接热点 `serial2net`（密码 `12345678`）
2. 浏览器打开 `http://192.168.4.1/`
3. 扫描 WiFi → 选择网络 → 输入密码 → 连接
4. 连接成功后 AP 自动关闭（省电），HTTP 服务仍可通过 STA IP 访问

无需重新编译即可更换 WiFi 网络或调整 UART 参数。

### UART 配置

在 Web 界面中可直接修改 UART 波特率，即时生效，重启后仍保留：

1. 打开管理界面 → **UART Settings** 卡片
2. 选择目标波特率（支持 1200 – 4,000,000）
3. 点击 **Apply** 即可生效

波特率保存在 NVS 中，重启后自动加载，优先级高于 Kconfig 编译默认值。其他 UART 参数（端口号、TX/RX 引脚、数据位等）需通过 `menuconfig` 修改并重新编译。

## LED 状态指示（WS2812B）

| 颜色 | 状态 |
|------|------|
| 暗白 | 启动中 |
| 🔵 蓝（1 Hz 闪烁） | WiFi 连接中 |
| 青 | WiFi STA 已连接，等待 TCP 客户端 |
| 品红 | AP 模式运行中，等待 TCP 客户端 |
| 红 | WiFi 连接失败（达到最大重试次数） |
| 🟢 绿 | TCP 客户端已连接，桥接正常 |
| ⚪ 白（瞬闪 50ms） | 有数据正在透传 |

## 协议说明

### Raw TCP（端口 8880）

透明 TCP 管道，**已启用 TCP_NODELAY**（禁用 Nagle 算法），适合低延迟按键转发。无任何协议开销——收到什么发什么。

### Telnet RFC 854/856（端口 23）

标准 Telnet 协议实现。连接后立即协商 Binary Transmission 模式（RFC 856），协商完成后等同于 Raw TCP。特点：

- 支持标准 `telnet` 客户端，无需配置 raw 模式
- 协商期间（通常 < 100ms）UART 数据静默，防止 IAC 字节冲突
- 协商超时可配置（默认 3 秒），超时自动断开
- 可通过 Kconfig 禁用，节省 ~2KB 固件空间

## 架构

基于 FreeRTOS 的嵌入式应用。7 个并发任务：

| 任务 | 优先级 | 职责 |
|------|--------|------|
| `led` | 5 | 20 Hz LED 状态机（颜色 + 数据闪烁） |
| `boot_btn` | 3 | 监测 BOOT 按钮，长按切换 AP 开关 |
| `tcp_accept` | 9 | 阻塞 `accept()`，新连接时踢掉旧客户端 |
| `telnet_accept` | 9 | 同上，Telnet 端口（需启用 Telnet） |
| `uart2tcp` | 10 | `uart_read_bytes()` → `send()` 到客户端 |
| `tcp2uart` | 10 | `recv()` 从客户端 → `uart_write_bytes()` |
| HTTP (内置) | — | esp_http_server 内部任务，处理 Web 请求 |

**数据流**：UART ↔ 两个独立 FreeRTOS 任务 ↔ 单个 TCP 客户端 socket。全双工透明桥接，无协议帧封装。

## 项目结构

```
serial2net/
├── CMakeLists.txt              # 项目配置
├── main/
│   ├── CMakeLists.txt          # 组件依赖
│   ├── Kconfig.projbuild       # menuconfig 配置菜单
│   ├── idf_component.yml       # 托管组件 (mDNS, LED strip)
│   ├── serial2net.c            # 主程序：WiFi / UART / TCP / 任务调度
│   ├── telnet.c                # Telnet 协议（RFC 854 + RFC 856）
│   ├── telnet.h                # Telnet 协议头文件
│   ├── wifi_config.c           # HTTP 服务 + WiFi/AP 管理 + Web UI
│   ├── wifi_config.h           # HTTP/AP/UART 配置 API
│   └── sdkconfig.ci            # CI 参考配置
├── .gitignore
└── CLAUDE.md                   # AI 辅助开发指南
```

## 切换芯片

```bash
idf.py set-target esp32c3    # 或 esp32
idf.py menuconfig            # 检查 UART 端口和引脚
idf.py build flash
```

> **ESP32-C3 特别注意**：该芯片只有 UART0/UART1，需在 menuconfig 中将 UART Port Number 从 2 改为 1（UART0 通常被 console 占用）。

## 许可证

MIT
