# Phase 1 验收与测试计划（上板前/上板后）

> 目的：验证审计报告中 Phase 1 要消除的 P0/P1 问题已在代码层面失效，并给出可复现的验证步骤。
> 编译由本地完成（AI 运行环境无法执行 ESP-IDF 子进程管道），日志/结果可回贴给助手修正。

## 0. 构建

```powershell
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd C:\Users\fytech\Desktop\usb_ecm_4g_module
.\scripts\build.ps1 -FullClean
```

期望：

- `BUILD OK`，生成 `build/esp_cpe.bin`（工程名仍为 `usb_ecm_4g_module` 时是 `build/usb_ecm_4g_module.bin`）；
- app 镜像 < `ota_0` 分区 3MB；`build/storage.bin` ≈ 1MB 分区内；
- 无 `warning: ... [-Wimplicit-function-declaration]`（若有，回贴）。

## 1. 启动顺序与默认安全（P0-2/P0-3/P1-13/P1-19）

| # | 操作 | 期望 |
| --- | --- | --- |
| 1.1 | 不接 4G 模组，仅 USB 供电上电 | 串口出现 `SoftAP started`、`CPE started, management console on http://192.168.4.1`；设备**不重启**（观察 2 分钟） |
| 1.2 | 手机搜索 Wi-Fi | 广播 `ESP-CPE-XXXX`，加密方式 WPA2，**不是开放网络** |
| 1.3 | 用串口打印的随机口令连接 AP | 连接成功 |
| 1.4 | 串口日志检查 | 无 `esp32`/`12345678` 固定凭据；AP 口令只在首启打印一次；正常重启不再打印 |
| 1.5 | 浏览器打开 `http://192.168.4.1` | 出现登录页（无浏览器 Basic 弹窗） |
| 1.6 | 错误口令登录 5 次 | 返回锁定（429/401），60s 内正确口令也拒绝 |
| 1.7 | 正确登录（admin + AP 口令） | 强制跳转改密；改密后需重新登录 |
| 1.8 | 未登录直接 GET `/api/status` | 401，响应头 `Connection: close` |
| 1.9 | 未登录 POST `/wlan_general`，带 `Content-Length: 2000000000` | 立即 413（或 401 + close），**httpd 不被卡死**，其它请求仍可响应 |
| 1.10 | 登录后 POST `/wlan_advance`，`{"channel":"999"}` | 400，NVS 不变，AP 不重启；重启设备后 AP 正常（无配置砖） |
| 1.10b | POST `/wlan_general` 传 >63 字符口令 / >32 字符设备名 / `if_hide_ssid:"2"` / `max_sta:1.5` | 400（非法值不再被静默忽略） |
| 1.11 | POST `/wlan_advance` `{"channel":"13","bandwidth":"20"}`，再 POST `/wlan_general` `{"ssid":"...","if_hide_ssid":true}` | 200；AP 参数生效；重启后保持 |
| 1.11b | 客户端列表点“编辑”后等待 ≥10 秒再提交新名称 | 输入框不被 4s 轮询擦除，改名成功 |
| 1.11c | 修改 AP SSID 或口令 | 浏览器连接断开后提示“已提交/网络中断”（不是“操作失败”）；用新参数可重新连接 |
| 1.12 | 客户端列表修改名称为 `"><img src=x onerror=alert(1)>` | 页面以纯文本显示，无脚本执行（XSS 修复） |
| 1.13 | 重启设备 | 不出现启动 abort/重启循环；仅 AP 口令不再重复打印 |
| 1.14 | 登录后 Web“恢复出厂” | AP 名称/口令保持不变；管理员口令重置为 AP 口令并强制改密；启动计数清零；站点昵称被清除 |
| 1.14b | 启动后 10 秒内双击复位键（默认 GPIO0） | 打印当前 AP 口令；管理员口令未修改过 → 一并打印（= AP 口令）；已修改过 → 该行显示"已修改，明文不可恢复" |
| 1.14c | 正常运行时连续按住复位键 ≥10s（`CPE_FACTORY_RESET_HOLD_MS`） | 恢复出厂设置并自动重启；重启后 AP 口令不变，管理员口令 = AP 口令 |

## 2. 4G 链路与恢复（P0-1/P0-2/P1-11/P1-19）

| # | 操作 | 期望 |
| --- | --- | --- |
| 2.1 | 接入已激活的 4G 模组 | 串口出现 USB 枚举、ECM 配置、`4G WAN IP: x.x.x.x`、`NAPT enabled`、`CPE online` |
| 2.2 | AP 客户端上网 | 可打开网页（NAPT + 上游 DNS） |
| 2.3 | 无 SIM 卡（或未激活）启动 | `link up but no IP` → FAULT；**不重启**；AP/Web 保持可用；后台每 60s 重试 |
| 2.4 | 在线时拔掉 4G 模组 | 状态变为 link down/FAULT，AP/Web 不中断；30s 后 L1 重建，重新插回后自动恢复（NEW_DEV 补发） |
| 2.5 | 在线时插拔 20 次 | 无 panic/堆损坏；每次均能恢复 link 与 IP |
| 2.6 | 上电时不接模组 / 只供电不接数据线 | 90s 内部署 L1/L2 后停在 FAULT，**整机不重启**，AP/Web 可用，后台每 60s 重试 |
| 2.6b | 模组在总线上但 ECM 接口异常（如强制错误配置号） | 允许有界重启（≤ `CPE_MAX_REBOOTS`，默认 3），之后停在 FAULT；日志 `ECM link stuck` |
| 2.7 | 重启后检查 NVS 启动计数 | 在线稳定 120s 后被清零（日志 `boot counter cleared`） |
| 2.8 | 模组外部供电，运行中按 ESP32 复位键（模组不断电） | 配置 `CPE_MODULE_RESET_GPIO` 后：日志出现 `Pulsing the 4G module reset`，模组重新枚举并恢复；不会出现 `CHECK_SHORT_DEV_DESC FAILED` 后卡死 |
| 2.9 | 同上但未接模组 RESET 线 | 若模组不感知 VBUS，会停在 `no USB modem` FAULT（AP/Web 可用）；这是硬件限制，需按 README 接 RST 或供电控制 |

## 3. 静态安全回归（可用 grep 自查）

```powershell
# 不应再有编译期固定凭据/默认口令
Select-String -Path components\web_wifi\**\*,components\web_wifi\*.c -Pattern '12345678|esp32:|ZXNwMzI6'
# 不应有未鉴权状态变更路由（除 login/static）
Select-String -Path components\web_wifi\router_web\modem_http_config.c -Pattern 'httpd_uri_t|basic_auth|strncmp\('
# 不应有网络 IO 的 ESP_ERROR_CHECK
Select-String -Path components\web_wifi\router_web\modem_http_config.c -Pattern 'ESP_ERROR_CHECK'
# main 不应再整栈拆装 USB Host
Select-String -Path main\main.c -Pattern 'usb_host_install|usb_stack_tear_down|usb_host_uninstall'
# 驱动与组件版本
Get-Content components\iot_usbh_cdc\idf_component.yml, components\iot_usbh_ecm\idf_component.yml
```

期望：固定凭据 0 命中；`ESP_ERROR_CHECK` 在网络 IO 处 0 命中；`main.c` 不出现上述 USB 重装符号；fork 版本为 3.1.1 / 0.4.2。

## 4. 已知未覆盖（Phase 2/3）

- OTA 升级接口与回滚（分区已就绪，接口未接）；
- Secure Boot / Flash 加密 / NVS 加密；
- HTTPS；
- iperf/AT/network_test 诊断功能（示例代码已删除，阶段二按需重新接入）；
- 长时间稳定性与并发 Web 压测。
