# Smart Light — 基于 ESP32-S3 的智能灯控系统

**环境感知 · 自适应调光 · 温度映射色彩 · 闹钟定时 · MQTT 云端接入 · BLE 配网 · OTA 升级**

一个 ESP32-S3 物联网示例工程：固件 + 本地 MQTT Broker + 网页大屏，
覆盖传感器采集、本地控制、联网上云到远程固件升级的完整链路，不需要注册任何云平台账号。

工程按职责拆成独立组件，注释写得比较细。配套手册
[使用与验证手册](docs/使用与验证手册.md) 按阶段组织上手过程，
每一步都给出可观察的现象与通过标准，可以按顺序一步步做，也可以只挑其中某一部分参考。

```text
DS18B20 / 光敏 ──> ESP32-S3 ──┬──> RGB 灯 / OLED / 蜂鸣器   （本地闭环）
   （采集）          （控制）  │
                              └──> MQTT ──> Node.js Broker ──> 网页大屏（实时曲线 / 远程控制）
                                             │
                                             └──> HTTP /ota/*.bin ──> 双分区固件升级
```

---

## 一、功能一览

| 模块 | 能力 |
|------|------|
| **环境感知** | DS18B20 单总线测温（1-Wire / RMT 后端）+ 光敏电阻 ADC 采样，独立任务周期采集 |
| **自适应调光** | 光照百分比 → PWM 占空比线性映射，自动模式下随环境明暗平滑调节亮度 |
| **温度映射色彩** | 温度 → RGB 色相映射（冷色→暖色），灯光颜色直观反映环境温度 |
| **闹钟与定时器** | 3 组闹钟（支持按星期重复）+ 跨天定时开关灯，SNTP 校时保证本地时间准确 |
| **本地交互** | SSD1306 OLED 三页面轮转显示、蜂鸣器提示音、串口命令控制台 |
| **联网** | WiFi STA 自动连接 + 断线重连（10 次退避）；凭据优先从 NVS 读取 |
| **配网** | BLE 配网（`wifi_prov_scheme_ble` + POP 校验），无需把 WiFi 密码写进固件 |
| **云端** | MQTT 上报/下发（兼容 ThingsBoard 主题），遗嘱消息（LWT）上报在线状态，闹钟与定时器支持远程配置 |
| **远程升级** | HTTP OTA + 双分区 + 失败不切换分区，升级进度可查 |
| **上位机** | Node.js MQTT Broker（TCP 1883 + WebSocket 8083）、网页大屏、固件分发 |

---

## 二、环境要求

| 项 | 版本 |
|----|------|
| 芯片 | ESP32-S3-N16R8（16 MB Flash + 8 MB Octal PSRAM） |
| 框架 | ESP-IDF v5.2.x |
| 上位机 | Node.js 18+（依赖 `aedes` / `ws` / `mqtt`） |

> 本工程默认按带 PSRAM 的模组配置：WiFi + Bluedroid + BT Controller + TLS 同时运行时，
> 内部 SRAM 会比较紧张。若使用不带 PSRAM 的模组，可以不启用 `sdkconfig.defaults` 中的
> PSRAM 相关配置，并适当调低任务栈与缓冲区大小。

---

## 三、硬件接线

| 外设 | 引脚 | 说明 |
|------|------|------|
| RGB - R | GPIO18 | PWM（共阳极，低电平点亮） |
| RGB - G | GPIO16 | PWM |
| RGB - B | GPIO17 | PWM |
| 光敏 AO | GPIO4 | ADC1_CH3 |
| DS18B20 DQ | GPIO9 | 1-Wire，需要 4.7 kΩ 上拉到 3.3 V |
| OLED SCL | GPIO7 | I2C |
| OLED SDA | GPIO8 | I2C |
| 蜂鸣器 I/O | GPIO6 | 2 kHz PWM |

所有模块 VCC 接 3.3 V，GND 共地。改动接线只需修改 `components/board_config/include/board_config.h`。

> 光敏模块方向：默认 **ADC 值越大 = 环境越暗**。若手上的模块相反，可以把 `BOARD_LIGHT_SENSOR_INVERT` 改成 `1`。
>
> 启用 PSRAM 后 GPIO33~37 被占用，本工程使用的 4/6/7/8/9/16/17/18 不冲突。

---

## 四、学习路线（建议按阶段推进）

上手过程建议跟着 [docs/使用与验证手册.md](docs/使用与验证手册.md) 走，
手册把整个工程拆成 5 个阶段，每个阶段都写明了**通过标准**。
确认当前阶段通过之后再进入下一阶段，遇到问题时排查范围会小很多，也更不容易被多个环节同时卡住。

| 阶段 | 需要准备 | 通过标准 |
|---|---|---|
| ① 本地功能 | 板子 + 串口线 | OLED 有画面、灯随环境光变化、`COLOR` 能变色、`ALARM TEST` 有响铃 |
| ② 联网 | 2.4 GHz 路由器 | `STATUS` 里显示 WiFi 已连接、SNTP 已校时，北京时间正确 |
| ③ 云端 | 上位机 Broker（`host/`） | 大屏出现温度 / 光照曲线，可远程改灯、设定时与闹钟 |
| ④ 手机配网 | Android / iOS + ESP BLE Prov | 不把 WiFi 写进固件也能联网（可选） |
| ⑤ OTA 升级 | 上位机 HTTP 服务 | 发布新固件后设备下载、重启、版本号发生变化（可选） |

其中阶段 ① 不需要 WiFi、不需要云、也不需要手机，建议从这里开始，先把本地闭环跑顺。

联网部分建议先用**方式 A（把 WiFi 凭据写进固件）**跑通，再回来尝试 BLE 配网
（完整步骤见手册[第五章](docs/使用与验证手册.md)），这样排查会简单很多。

---

## 五、快速开始

### 5.1 编译与烧录

```bash
# 设置目标芯片（只需一次）
idf.py set-target esp32s3

# 配置 WiFi / MQTT Broker 地址
idf.py menuconfig        #  -> Smart Light 配置

# 编译 + 烧录 + 串口监视
idf.py build
idf.py -p COM3 flash monitor
```

Windows 上也可以使用 `build.ps1`（会按本机 ESP-IDF 安装路径准备工具链环境变量）：

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
powershell -ExecutionPolicy Bypass -File build.ps1 -ExtraArgs "-p COM3 flash monitor"
```

> 脚本顶部的 `IDF_PATH` / `IDF_TOOLS_PATH` 等变量默认取环境变量，未设置时才会用到脚本内的
> 缺省值，如果两者与本机不一致，改一处即可。

依赖组件（首次编译自动下载到 `managed_components/`）：

| 组件 | 版本 |
|------|------|
| espressif/onewire_bus | 1.1.1 |
| espressif/ds18b20 | 0.4.0 |
| espressif/ssd1306 | ^1.0.0 |

### 5.2 启动上位机

```cmd
cd host
npm install
node broker.js
```

Windows 下双击 `host\start-broker.cmd` 亦可（若 Broker 已在运行则直接打开大屏）。
浏览器打开 **http://localhost:8083** 即可看到大屏页面。

### 5.3 配网

这一步骤可以在跑通联网之后再来看，不影响阶段 ① 的验证。

设备首次上电且 NVS 中没有凭据时，会自动进入 BLE 配网广播：

1. 串口打印 `设备名: PROV_LIGHT_xxxx` 与 `校验码 POP: abcd1234`
2. 手机安装 **ESP BLE Prov**（ESP-IDF Provisioning）→ Provision New Device → 扫描
3. 选中 `PROV_LIGHT_xxxx` → 输入 POP → 选择 2.4 GHz WiFi 并发送密码
4. 设备自动重启并用新凭据联网，**不会重复要求配网**

> 配网推荐使用 **ESP BLE Prov**。EspBlufi 使用的是另一套 BluFi 协议，与本工程的
> `wifi_prov_scheme_ble` 不同，用它可能会扫不到设备。
>
> POP 校验码默认为 `abcd1234`，实际使用时建议在 menuconfig
> （`Smart Light 配置 → BLE 配网`）中改成自己的。
>
> 随时可以用串口命令 `PROV` 手动开启配网，`WIFI RESET` 清除已保存凭据。
> 完整的配网步骤与常见问题见手册[第五章](docs/使用与验证手册.md)。

---

## 六、工程结构

按职责拆分为**组件化 + FreeRTOS 多任务**架构，共 15 个组件：

```text
smart-light/
├── main/
│   ├── main.c                     # 只做：初始化 -> 启动任务 -> 联网 -> 接云 -> OTA
│   ├── Kconfig.projbuild          # menuconfig 配置菜单
│   └── CMakeLists.txt
├── components/
│   ├── board_config/              # 引脚与外设参数（改接线只改这一个头文件）
│   ├── sys_state/                 # 系统状态中心：唯一全局状态持有者 + 事件组
│   ├── rgb_led/                   # RGB PWM 驱动（LEDC，共阳极）
│   ├── buzzer/                    # 蜂鸣器 PWM 驱动
│   ├── light_sensor/              # 光敏 ADC 采集
│   ├── temp_sensor/               # DS18B20 1-Wire 驱动（支持热重扫恢复）
│   ├── sensor_service/            # 采集任务（光照 500ms / 温度 3s）
│   ├── light_ctrl/                # 灯光状态机任务 + 渐变过渡 + 闹钟响铃效果
│   ├── alarm_mgr/                 # 闹钟(3 组) + 定时开关灯，1 秒节拍任务
│   ├── oled_ui/                   # SSD1306 三页面显示任务
│   ├── net_prov/                  # WiFi 连接/重连 + BLE 配网
│   ├── sntp_time/                 # SNTP 校时
│   ├── cloud_mqtt/                # 通用 MQTT 接入：上报 / 下发 / 遗嘱
│   ├── console_cmd/               # 串口命令控制台（命令表驱动）
│   └── cloud_ota/                 # HTTP OTA：进度、版本校验、失败保护
├── host/                          # 上位机（Node.js）
│   ├── broker.js                  # MQTT Broker + HTTP 服务 + /ota 固件分发
│   ├── dashboard.html             # 大屏页面（曲线 / 在线检测 / 远程控制 / 定时闹钟 / OTA 推送）
│   ├── package.json               # 依赖 aedes + ws + mqtt
│   ├── start-broker.cmd           # 一键启动（已在运行则直接打开大屏）
│   ├── push-firmware.cmd          # 把编译产物发布到 ota/ 目录
│   ├── ota/                       # OTA 固件分发目录（放 .bin 即可被设备拉取）
│   └── 使用说明.md
├── docs/
│   └── 使用与验证手册.md           # 逐项验证清单，每步都有通过标准
├── partitions.csv                 # 16MB Flash 双 OTA 分区表
├── sdkconfig.defaults
├── build.ps1                      # 构建脚本（自动准备工具链环境变量）
├── flash_ble_prov.cmd             # 擦全片后烧录，用于 BLE 配网演示
└── README.md
```

### FreeRTOS 任务划分

| 任务 | 优先级 | 栈 | 职责 |
|------|--------|-----|------|
| `cloud_task` | 5 | 5 KB | MQTT 连接、周期上报、下发处理、OTA |
| `light_ctrl` | 5 | 3 KB | 灯光状态机、颜色渐变、闹钟红闪 + 响铃 |
| `sensor_task` | 4 | 3 KB | 光照 / 温度周期性采集 |
| `alarm_task` | 4 | 2.5 KB | 闹钟与定时器秒级检查 |
| `oled_task` | 3 | 4 KB | OLED 三页面刷新 |
| `console_cmd` | 3 | 4 KB | 串口命令解析执行 |

### 数据流与解耦设计

```text
传感器 ──> sensor_task ──> sys_state ──(事件组)──> light_ctrl ──> RGB PWM
                              │
                              ├──(事件组)──> oled_task ──> OLED
                              └──(周期读取)──> cloud_task ──> MQTT Broker
                                                 ↑
串口命令 ──> console_cmd ──> sys_state ───────────┘
网页大屏 ──> MQTT 下发 ──> sys_state
                └─ 定时 / 闹钟指令 ──> alarm_mgr ──> sys_state ──(立即回报)──> 大屏
```

**关键设计**：模块之间不互相读写全局变量，统一通过 `sys_state`（带互斥锁）读写，
并用**事件组**通知状态变化。因此新增一个传感器或一路输出，不需要改动已有模块。

---

## 七、云端：MQTT 接入

### 7.1 接入层

设备端将云接入抽象为**通用 MQTT 组件**：Broker 地址、账号密码、主题格式均可配置，
不绑定厂商 SDK、不需要三元组签名。上位机可替换为
EMQX / Mosquitto / ThingsBoard 或本仓库自带的 Broker —— 切换平台只改配置、不改业务代码。

`host/broker.js` 基于 Node.js `aedes` 实现，无需 Docker 或云账号：

| 端口 | 用途 |
|------|------|
| 1883 | MQTT over TCP —— 给 ESP32 用 |
| 8083 | HTTP —— 大屏页面、MQTT over WebSocket、状态 API、`/ota/*.bin` 固件分发 |

### 7.2 MQTT 主题约定

`<id>` 为设备 ID（未配置时自动取 MAC 尾号，如 `light_7B7544`）：

| 主题 | 方向 | 载荷 |
|------|------|------|
| `light/<id>/telemetry` | 设备 → 云 | `{"temp":28.3,"light":45,"mode":1,"r":0,"g":196,"b":26,"ts":123456,"timer":{...},"alarms":[...]}` |
| `light/<id>/state` | 设备 → 云 | `{"mode":1,"r":0,"g":196,"b":26}` |
| `light/<id>/status` | 设备 → 云 | `"online"` / `"offline"`（LWT 遗嘱，retained） |
| `light/<id>/set` | 云 → 设备 | `{"mode":2}` / `{"r":255,"g":80,"b":0}` / `{"cmd":"ota","url":"..."}` / `{"cmd":"alarm",...}` / `{"cmd":"timer",...}` |

勾选 `CLOUD_MQTT_TB_MODE` 可切换到 ThingsBoard 的 `v1/devices/me/telemetry` 标准主题。

**定时与闹钟**：设备把当前配置随每条遥测一起下发（`timer` / `alarms` 字段），
大屏据此渲染控件；云端下发指令后设备会立刻补发一条遥测回执。

| 动作 | 下发报文（`light/<id>/set`） |
|------|------|
| 设置定时开关灯 | `{"cmd":"timer","action":"set","hour_on":22,"minute_on":0,"hour_off":6,"minute_off":0,"week":[1,2,3,4,5]}` |
| 关闭定时 | `{"cmd":"timer","action":"off"}` |
| 添加闹钟 | `{"cmd":"alarm","action":"add","hour":7,"minute":30,"repeat":"daily","week":[1,3,5]}` |
| 删除 / 清空闹钟 | `{"cmd":"alarm","action":"del","slot":1}` / `{"cmd":"alarm","action":"clear"}` |
| 响铃测试 | `{"cmd":"alarm","action":"test"}` |

`week` 为 **1~7 的数组（1 = 周一 … 7 = 周日）**，空数组表示每天；`repeat` 支持 `daily` / `once`。

### 7.3 网页大屏

`dashboard.html` 是一个纯前端页面（无框架、无构建），直连 Broker 的 WebSocket：

- **上下分层图表**：上层温度折线、下层光照渐变面积，两路信号互不干扰
- **鼠标悬停**：显示该时刻的温度 / 光照 / 时间戳，支持自动吸附最近数据点
- **在线判定**：以"是否持续上报遥测"为权威依据，避开 retained 遗嘱消息造成的在线误判
- **远程控制**：一键切换模式、调节 RGB 颜色
- **定时与闹钟**：可视化设置跨天定时开关灯、3 组闹钟（可按星期重复），
  配置由设备回读，串口与网页两端改都同步
- **OTA 推送**：列出服务器上的固件，点一下就给设备下发升级指令

### 7.4 配置设备端

```bash
idf.py menuconfig    # -> Smart Light 配置 -> MQTT 云端
```

| 配置项 | 说明 |
|--------|------|
| `CLOUD_MQTT_ENABLE` | 启用通用 MQTT 接入 |
| `CLOUD_MQTT_URI` | Broker 地址，如 `mqtt://192.168.1.100:1883` |
| `CLOUD_MQTT_USERNAME` / `PASSWORD` | 可选，多数 Broker 默认允许匿名 |
| `CLOUD_MQTT_DEVICE_ID` | 留空则用 MAC 尾号 |
| `CLOUD_MQTT_TB_MODE` | 按 ThingsBoard 主题格式 |
| `CLOUD_MQTT_POST_INTERVAL_SEC` | 上报周期，默认 5 秒 |

---

## 八、OTA 远程升级

采用**双分区 + 失败保护**策略，升级失败不会变砖：

```text
① 编译新固件            idf.py build
② 发布到服务器          host\push-firmware.cmd   （复制到 host\ota\smart_light.bin）
③ 触发升级              串口敲 OTA  /  大屏点"推送"  /  MQTT 下发 {"cmd":"ota"}
④ 设备下载并校验        写入备用分区（ota_0 ↔ ota_1 交替）
⑤ 校验通过才重启        新固件首启动执行 mark_app_valid_cancel_rollback()
```

| 失败环节 | 设备行为 |
|----------|----------|
| URL 不可达 / 下载中断 | 报错退出，**不切换分区**，继续跑旧固件 |
| 固件非本项目产物 | 项目名校验不通过，拒绝写入 |
| 写入后重启异常 | bootloader 自动回滚到原分区 |

串口命令 `OTA`（用默认地址）或 `OTA <网址>`（临时指定）。

---

## 九、串口命令

在串口监视器输入（不区分大小写，输入后按回车）：

```text
AUTO                     自动模式（温度定色 + 光照定亮度）
MANUAL                   手动模式
OFF                      关闭灯光
COLOR 255,100,50         设置颜色（自动切到手动模式）
PAGE 1|2|3               切换 OLED 页面
BUZZ ON|OFF              蜂鸣器开关
TEMP                     温度诊断（含原始 scratchpad 与热重扫）
ALARM ADD 07:30 daily    添加每日闹钟
ALARM ADD 07:30 once     添加单次闹钟
ALARM ADD 07:30 daily 1,3,5   周一/三/五闹钟（1=周一 … 7=周日）
ALARM DEL 1|2|3|ALL      删除闹钟
ALARM LIST               列出闹钟
ALARM TEST               立即测试一次响铃
TIMER ON 22:00-6:00      定时开灯（跨天自动识别）
TIMER OFF                关闭定时器
TIMER REPEAT 1,2,3,4,5   定时器按星期重复
TIMER SHOW               查看定时器
PROV                     启动 BLE 配网
WIFI STATUS|RESET        查看网络 / 清除配网信息并重启
CLOUD POST|STATUS|OTA    云端上报 / 状态 / 升级
OTA [网址]                OTA 远程升级固件
STATUS                   打印系统状态
REBOOT                   重启
HELP                     帮助
```

---

## 十、已知问题与注意事项

下面这些是开发过程中记录下来的现象与处理方式，改动代码或换硬件时可以作为参考：

- **PSRAM 配置**：内部 SRAM 需要同时承载 WiFi + Bluedroid + BT Controller，
  本工程默认启用 PSRAM；未启用时可能出现 `Guru Meditation`。相关配置见 `sdkconfig.defaults`。
- **蓝牙控制器只初始化一次**：`btdm_controller_init` 走 `init → deinit → init`
  会命中 IDF 的 `free() target pointer is outside heap areas`。
  配网结束后直接重启而不 `deinit`。
- **NVS 残留凭据影响连接流程**：`wifi_prov_mgr` 会在 NVS 留下 provisioned 记录，
  因此启动时先读 NVS 实际凭据再决定走连接还是配网。
- **空配置会覆盖 NVS 凭据**：用全零 `wifi_config_t` 调 `esp_wifi_set_config()` 会清掉已保存的
  SSID/密码。无凭据时不调用该接口，让驱动自行加载。
- **retained 遗嘱消息造成在线误判**：设备重启瞬间的 `offline` 遗嘱被 retain，
  因此大屏以"是否持续上报遥测"判定在线状态。
- **DS18B20 总线无响应**：`reset bus failed: no devices found` 属于电气层问题
  （无 presence 脉冲），与固件无关；`TEMP` 命令带**热重扫**，插好线即可恢复，无需重启。

---

## 十一、常见问题

**Q：编译报 ssd1306 / ds18b20 找不到？**
A：托管组件下载失败。可以先检查网络，或把组件手动放入 `components/` 再编译。

**Q：串口能看日志但敲命令没反应？**
A：通常是 console 端口没选对。本板子用 ESP32-S3 内置 USB Serial/JTAG，需要设置
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` 且 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`
（默认配置下 USB 口只能看日志、不能输入）。

**Q：OLED 不显示？**
A：可以先看串口有没有 `oled_ui` 的初始化日志，再确认 I2C 地址（默认 0x3C）与 SCL/SDA 是否接反。

**Q：温度一直读不到 / 显示无效？**
A：DS18B20 需要 4.7 kΩ 上拉电阻，VCC 接 3.3 V。接好后用串口命令 `TEMP` 做一次热重扫，不需要重启。
若日志出现 `rmt: user buffer too small`，说明总线电平不够干净，可以把
`board_config.h` 的 `BOARD_ONEWIRE_BACKEND` 改成 `2` 换用 UART 后端。

**Q：闹钟不响？**
A：闹钟依赖 SNTP 校时，可以先在串口确认时间同步日志，再用 `ALARM TEST` 验证响铃是否正常。

---

## 许可证

本项目基于 MIT License 开源，详见 [LICENSE](LICENSE)。
