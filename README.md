# ESP-CPE — 4G USB ECM CPE 产品基座（ESP32-S3）

基于 Espressif 官方示例 `usb_ecm_4g_module` 重构的 4G CPE 产品固件：4G 模组通过 USB ECM 提供 WAN，
ESP32-S3 同时提供 SoftAP + NAPT + Web 管理面。本仓库是**重新开始的产品化基线**，按审计报告分阶段实施，
设计依据与进度见 [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md)。

| 目标芯片 | Flash | 4G 模组 | ESP-IDF |
| --- | --- | --- | --- |
| ESP32-S3 | 16MB（双 OTA + 1MB SPIFFS） | 通用 USB ECM 模组（无 AT 依赖） | v6.1 |

## 1. 与官方示例的关键差异

| 方面 | 官方示例 | 本产品基线 |
| --- | --- | --- |
| 启动顺序 | 等到 4G 拿到 IP 后才启动 AP/Web；无网络即永久卡死 | **AP/Web 先行**，4G 链路由后台任务管理；无 SIM/无 IP 仍可进后台排障 |
| 默认安全 | 开放 AP，管理员固定 `esp32/12345678`，HTTP Basic | **WPA2 + 每台唯一随机口令**；管理员口令首启随机、首次登录强制修改；会话 Cookie + CSRF |
| USB 恢复 | 无；偶发枚举失败只能重插 | 枚举事件补发 + ECM 重建 + 可选端口掉电 + **有界受控重启**；"link up 无 IP"永不重启 |
| 配置校验 | 任意参数直写 NVS，`channel:"999"` 会变砖 | 服务端强校验、失败回滚、启动时非法值回退默认 |
| 分区 | 单 factory，无 OTA | 16MB + `ota_0`/`ota_1` 双分区（OTA 接口在阶段三接入） |
| 驱动 | registry 版本存在 UAF/double free/TX 令牌丢失 | 本地 fork（`components/iot_usbh_*`）逐条修补，见各组件 CHANGELOG |

## 2. 首次启动与登录

首次上电会自动生成每台设备唯一的随机凭据，并**通过串口打印一次**（恢复出厂保留现有 AP 凭据，
只重置管理员口令）：

```
W (xxxx) cpe_config: ==================================================
W (xxxx) cpe_config:   Credentials (first boot)
W (xxxx) cpe_config:   SoftAP SSID     : ESP-CPE-XXXX
W (xxxx) cpe_config:   SoftAP password : ************
W (xxxx) cpe_config:   Admin user      : admin
W (xxxx) cpe_config:   Admin password  : ************
W (xxxx) cpe_config:   Web console     : http://192.168.4.1
W (xxxx) cpe_config: ==================================================
```

步骤：

1. 连接 `ESP-CPE-XXXX`（WPA2，口令见串口输出）；
2. 浏览器打开 `http://192.168.4.1`，用户名 `admin`，初始口令**与 SoftAP 口令相同**；
3. 首次登录会被强制要求设置新的管理员口令；
4. 之后可在 Web 后台修改 AP SSID/口令/信道、查看 4G 状态与客户端列表、恢复出厂。

> 恢复出厂会保留本机唯一的 SoftAP SSID/口令（避免复位后连不上 AP），其余配置回默认，
> 管理员口令重置为 AP 口令并再次要求首次登录修改。
>
> 忘记管理员口令时：**设备启动后 10 秒内双击复位键（默认 GPIO0，低电平有效）**，串口会打印
> 当前 AP 口令；管理员口令若从未修改过（仍等于 AP 口令）会一并打印，否则明确提示"已修改、
> 明文不可恢复"。开机后**连续按住同一按键 10 秒**会恢复出厂设置并重启。

> 安全说明：固定凭据已彻底移除；管理面默认仅 HTTP，但只开放给 SoftAP 网段并要求会话 + CSRF。
> HTTPS/证书与 Secure Boot/Flash 加密属于阶段三内容。

## 3. 架构

```text
app_main
 ├─ nvs / netif / event loop / cpe_core（配置、认证、状态、启动计数）
 ├─ app_wifi_start()        SoftAP（WPA2，配置来自 cpe_core）
 ├─ modem_http_start()      Web 管理面（登录/状态/配置/恢复出厂）
 └─ cpe_link task
      ├─ usbh_cdc_driver_install()（USB Host 由 CDC 组件安装一次，永不在运行时拆栈）
      ├─ iot_eth_new_usb_ecm()/install/start
      ├─ 事件：link up/down、WAN IP
      ├─ ONLINE：默认路由 + NAPT + 上游 DNS 下发给 AP 客户端
      └─ 失败：ECM 重建 → 根端口掉电（可选）→ NVS 计数受控重启 → 保底 FAULT（AP/Web 保持）
```

恢复策略细节与阈值见 [docs/IMPLEMENTATION_PLAN.md](docs/IMPLEMENTATION_PLAN.md) 3.3 节，可通过
`menuconfig → CPE Application Configuration` 调整。完整的启动流程、链路状态机、每个等待点/超时
以及常见日志含义见 [docs/RUNTIME_FLOW.md](docs/RUNTIME_FLOW.md)。

## 4. 构建与烧录

本机 ESP-IDF v6.1 环境：

```powershell
# 1) 激活 ESP-IDF（EIM 安装方式）
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd C:\Users\fytech\Desktop\usb_ecm_4g_module

# 2) 配置目标（首次）并构建
idf.py set-target esp32s3
idf.py build

# 3) 烧录 + 串口监视
idf.py -p COMx flash monitor
```

要点：

- Flash 必须 ≥16MB（分区表 `partitions_16m_ota.csv`：nvs/otadata/phy_init/ota_0 3M/ota_1 3M/storage 1M）；
- `sdkconfig` 不入库，所有非默认项在 `sdkconfig.defaults` / `sdkconfig.defaults.esp32s3` 中维护；
- 本地 fork 的 `iot_usbh_cdc` / `iot_usbh_ecm` 通过 `main/idf_component.yml` 的 `override_path` 覆盖 registry 版本。

## 5. 硬件接线

```
┌─────────────┐          ┌─────────────────┐
│             ┼──────────┼5V               │
│  4G Module  ┼──────────┼GND              │
│             │          │    ESP32-S3     │
│             │          │                 │
│             ┼──────────┼USB D+           │
│             ┼──────────┼USB D-           │
│             │          │                 │
└─────────────┘          │                 │
                         └─────────────────┘
```

USB Host 使用 ESP32-S3 内置 PHY；若模组需要外部供电，请确保地与 ESP32-S3 共地。

**重要（4G 模组复位线）**：如果模组独立供电（ESP32 复位时模组不断电），ESP32 重启后模组 USB
状态可能残留，枚举会报 `CHECK_SHORT_DEV_DESC FAILED`（实机已复现）。产品上请把模组的
`RESET_N` 接到一个空闲 GPIO（例如 GPIO5，按你的板子实际接线），并在
`CPE Application Configuration → 4G module RESET GPIO number` 中填写该 GPIO（默认 `-1` 未接）。
配置后固件会在启动时以及 USB 枚举恢复时自动脉冲模组复位，使枚举从干净状态开始。
若无法改硬件，可尝试开启 `CPE_USB_PORT_POWER_CYCLE`（根端口 VBUS 掉电，仅对能感知 VBUS 的模组有效），
或让模组供电由 ESP32 控制。两者都不可用时，固件仍会在"总线上没有已枚举设备"时自动**重启 USB Host 栈**
（卸载 host 撤掉总线端接，让模组感知断开，再重新安装触发一次全新枚举，次数由
`CPE_USB_ENUM_RESET_ATTEMPTS` 限制），这是纯软件的最后兜底。

特定模组若 ECM 不在配置 1，可开启 `CPE Application Configuration → Enable USB host enumeration filter`
并填写 VID/PID/配置号（默认关闭，避免官方示例"强制所有设备用配置 2"的缺陷）。

**复位/恢复出厂按键**：按键一端接 `GPIO0`、另一端接 `GND`（默认低电平有效），可通过
`CPE Application Configuration → Reset/factory-reset button GPIO` 修改，`-1` 表示不使用。

- **启动后 10 秒内双击**（`CPE_CREDENTIAL_PRINT_WINDOW_MS`）：打印当前 AP 口令；管理员口令
  未修改过时一并打印（用于忘记口令时取回），已修改则提示不可恢复；
- **开机后连续按住 10 秒**（`CPE_FACTORY_RESET_HOLD_MS`）：恢复出厂设置；**松开按键后**
  自动重启（GPIO0 在复位期间被拉低会进入下载模式）。

> 注意：GPIO0 是 ESP32-S3 的启动 strapping 引脚，**在芯片复位期间**按住会被 ROM 当作
> UART 下载模式，导致应用不启动。所以打印凭据改用"启动后双击"而不是"启动时按住"；
> 如果板子方便改线，建议换到其它空闲 GPIO。

## 6. 阶段状态

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 1 | 阻断 P0/P1 + 产品骨架（安全默认值、AP/Web 先行、USB 恢复根修、配置校验、16MB OTA 分区） | 构建通过，上板验收中 |
| Phase 2 | 驱动与 Web 全面加固；公网 IP/测速/AT 等诊断功能按审计修复后重新接入 | 待开始 |
| Phase 3 | OTA 升级/回滚、Secure Boot/加密、长稳测试、出厂流程 | 待开始 |

## 7. 目录

```
main/                 应用入口、CPE 恢复策略、Kconfig
components/cpe_core/  配置存储/校验、随机凭据、会话认证、状态、启动计数
components/web_wifi/  SoftAP + Web 管理面（SPIFFS 前端）
components/iot_usbh_cdc/  USB Host CDC 驱动本地 fork（已打补丁）
components/iot_usbh_ecm/  USB Host ECM 驱动本地 fork（已打补丁）
docs/                 实施计划、运行时流程、上板验收清单
partitions_16m_ota.csv
```

## 8. 许可

保留上游 Espressif 示例与组件的 Apache-2.0 许可；本地修改记录在各组件的 CHANGELOG.md。
