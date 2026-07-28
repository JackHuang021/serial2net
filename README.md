# Serial2Net — UART-to-WiFi 透传桥

基于 ESP32 的串口转 WiFi 透明传输设备，用于嵌入式开发板无线调试。ESP32 连接目标开发板的 UART，通过 WiFi 将串口数据透传到 TCP 端口，MacBook/PC 无需 USB 转串口线即可远程调试。

## 硬件连接

```
ESP32 端                目标设备端
───────                 ──────────
  GND    ──────────────  GND
  TX     ──────────────  RX
  RX     ──────────────  TX
  LED    ──────────────  WS2812B Data In (可选)
```

默认引脚（可通过 `menuconfig` 修改）：

| 芯片 | UART TX | UART RX | WS2812B |
|------|---------|---------|---------|
| ESP32-S3 | GPIO17 | GPIO16 | GPIO48 |
| ESP32-C3 | GPIO6 | GPIO7 | GPIO8 |
| ESP32 | GPIO17 | GPIO16 | GPIO18 |

默认 UART 参数：**115200-8-N-1**，无硬件流控。

## 快速上手

### 1. 配置 & 编译

```bash
# 安装 ESP-IDF 工具链（首次）
~/.espressif/v6.0.2/esp-idf/install.sh

# 加载环境
source ~/.espressif/v6.0.2/esp-idf/export.sh

# 配置 WiFi 和引脚（可选，默认值也能用）
idf.py menuconfig
# 进入 Serial2Net Configuration →
#   - WiFi Settings: 设置 SSID / 密码
#   - UART Settings: 设置波特率 / 引脚
#   - TCP Settings: 设置监听端口（默认 8880）
#   - LED Settings: 配置 WS2812B 引脚

# 编译
idf.py build
```

### 2. 烧录

```bash
idf.py -p /dev/cu.usbmodem* flash monitor
```

### 3. 连接

ESP32 启动后会通过 mDNS 广播 `serial2net.local`，MacBook 上直接：

```bash
socat -,raw,echo=0,escape=0x1d TCP:serial2net.local:8880,nodelay
# 退出: Ctrl + ]
```

如果 mDNS 解析失败，用 IP 直连：

```bash
socat -,raw,echo=0,escape=0x1d TCP:192.168.x.x:8880,nodelay
```

### 4. 切换芯片

代码完全可移植，切换芯片只需：

```bash
idf.py set-target esp32c3    # 或 esp32
idf.py menuconfig            # 检查引脚默认值
idf.py build flash
```

## WiFi 工作模式

| 模式 | 行为 |
|------|------|
| Station | 连接到指定的 WiFi 路由器 |
| AP | ESP32 创建热点 `serial2net`，MacBook 直连 |
| STA + AP 回退（推荐） | 先尝试 Station，15 秒超时后自动启动 AP |

## LED 状态指示（WS2812B）

| 颜色 | 状态 |
|------|------|
| 暗白 | 启动中 |
| 蓝（闪烁） | WiFi 连接中 |
| 青 | WiFi 已连接，等待 TCP 客户端 |
| 品红 | AP 模式运行中 |
| 红 | WiFi 连接失败 |
| 绿 | TCP 客户端已连接，桥接正常 |
| 白（瞬闪） | 有数据正在透传 |

## 项目结构

```
serial2net/
├── CMakeLists.txt              # 项目配置
├── main/
│   ├── CMakeLists.txt          # 组件依赖
│   ├── Kconfig.projbuild       # menuconfig 配置菜单
│   ├── idf_component.yml       # 托管组件 (mDNS, LED)
│   └── serial2net.c            # 核心代码
└── .gitignore
```

## 许可证

MIT
