# Phase 2 验收与测试计划（驱动 + Web）

> 目标：验证 Phase 2 的驱动/枚举加固与 Web 安全改动在实机上成立。
> 前置：已完成 Phase 1 上板验收；模组 RESET 线暂不接线（软件兜底路径按"无 RESET 线"验收）。
>
> 辅助脚本：
> - `scripts/serial_log_check.ps1`：串口日志统计与异常检测
> - `scripts/web_security_test.py`：管理面安全回归（HTTP，Python 标准库）

## 0. 构建与烧录

```powershell
& 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd C:\Users\fytech\Desktop\usb_ecm_4g_module
idf.py build
idf.py -p COM6 flash monitor
```

期望：app 镜像仍能放入 3MB OTA 分区；自有代码 0 warning；启动日志无 panic。

## 1. Web 安全（M2.1）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 1.1 | 启动日志 | 出现 `Web server will listen on SoftAP interface "ap0" only`；httpd 绑定该接口 | ☐ |
| 1.2 | 连 AP 访问 `http://192.168.4.1` | 正常打开登录页 | ☐ |
| 1.3 | 从 4G 网段（模组分配的 192.168.225.x）访问设备 80 端口 | 无法连接/无响应（管理面只在 AP 侧） | ☐ |
| 1.4 | 未登录 GET `/api/status` | 401 + `Connection: close` | ☐ |
| 1.5 | 未登录 POST 带 `Content-Length: 2000000000` | 立即 413/401，httpd 不卡死，其它请求仍可响应 | ☐ |
| 1.6 | 登录后 POST 不带 CSRF | 403 | ☐ |
| 1.7 | 登录后 POST 畸形 JSON / 超长字段 | 400，NVS 不变 | ☐ |
| 1.8 | 非法 Wi-Fi 参数（channel=999、口令 >63、`if_hide_ssid:2`、`max_sta:1.5`） | 400，AP 不重启，重启后配置仍合法 | ☐ |
| 1.9 | 连续 5 次错误登录 | 第 6 次返回 429，锁定 60s；再触发一次锁定变 120s | ☐ |
| 1.10 | 登录后空闲 30 分钟再操作 | 会话失效，跳回登录页 | ☐ |
| 1.11 | 客户端名称 XSS 载荷 `"><img src=x onerror=alert(1)>` | 页面纯文本显示，无脚本执行 | ☐ |
| 1.12 | 恢复出厂后 | AP 凭据保留；管理员口令 = AP 口令；启动计数清零 | ☐ |

自动化：`scripts/web_security_test.py` 覆盖 1.4~1.9（锁定测试会真的锁 60s，默认需要 `--include-lockout` 才执行）。

### 1.x 自动化执行记录（2026-10-18）

- 设备地址：用户提供 192.168.1.4 不可达；实际设备 SoftAP 为 **192.168.4.1**（PC 在 192.168.4.2），测试按 192.168.4.1 执行。
- 默认凭据 `admin` / `FooTdEh2F4jv` 登录成功（首次为 `must_change=true`）。
- 首轮结果：未登录 401 + `Connection: close`、超长 Content-Length 被快速断开且服务器仍存活、登录/会话/CSRF、无 CSRF 403、畸形 JSON 400、弱口令 400 —— **全部 PASS**。
- 补测（测试代理把管理员口令临时改为 `CpeTest2026!` 完成首次改密后执行）：
  - 状态变更接口无 CSRF → 403；
  - 畸形 JSON → 400；`channel=999` → 400；
  - 客户端名称 XSS 载荷被服务端白名单拒绝（400），合法特殊字符名称可正常 JSON round-trip；
  - 连续错误登录触发 429，等待 60s 后正确口令恢复 200。
  - **全部 PASS**。
- 1.3（从 4G WAN 侧访问管理面）无法从 AP 侧验证，待有 4G 侧主机时补测。
- ⚠️ 设备当前管理员口令为测试用的临时口令 `CpeTest2026!`，交付/继续使用前请通过 Web 改成正式口令。

## 2. 驱动与枚举（M2.2/M2.3）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 2.1 | 正常启动接模组 | 枚举成功、`ECM driver ready`、`4G WAN IP`、`CPE online` | ✅ 通过 |
| 2.2 | 反复热插拔模组 20 次 | 每次恢复 link/IP；无 panic；无重复端口打开错误 | ✅ 初步通过（7 次，恢复 18~20s；建议补到 20 次） |
| 2.3 | 无 RESET 线，运行中按 ESP32 复位 10 次 | 每次要么恢复枚举，要么稳定停在 FAULT；**不重启整机、不 panic** | ⛔ 受阻：软件侧修复已上板验证（不再 panic、AP/Web 可用、周期重试），但模组 USB 控制器复位后挂死属硬件限制，只有插拔恢复，见 2.w |
| 2.4 | 无 SIM 持续 10 分钟 | **预期已随 ONLINE 判据改写**（2026-10-24）：模组的 ECM DHCP 无卡也会分配地址，所以 `got_ip=true`（私网地址），**不会**出现 `link up but no IP`。期望：状态 `no mobile data (no SIM card)`；`CPE_PDP_WAIT_MS`（默认 60 s）后 `Restarting the 4G module (restart 1/2)`，最多 `CPE_IP_MODEM_RESETS` 次，之后停在 `no mobile data (no SIM card), AP/Web still available`；信号图标不点亮、`接收信号强度` 显示"未插入 SIM 卡"；AP/Web 全程可用，**不重启整机** | ☐ 待补 |
| 2.5 | 不接模组上电 10 分钟 | no-device FAULT，AP/Web 可用，无重启 | ☐ 待补 |
| 2.6 | 正常运行时观察 10 分钟日志 | **不出现** `Replaying the CONNECTED event for N existing device(s)` 周期刷屏；无 `CHECK_SHORT_DEV_DESC` 后卡死 | ✅ 通过（0 次刷屏） |
| 2.7 | 在线稳定 2 分钟 | `Healthy for 120 s, boot counter cleared`（仅首次有计数时打印） | ✅ 通过 |
| 2.8 | 连续 20 次插拔后的堆水位 | 每次插拔后 `free` 回到同一水平、`min` 不持续下降、`largest_internal` 不持续变小 | ✅ 初步通过（6 轮稳定，建议补到 20 次） |

堆水位日志：

- 固件在 `link up/down`、`got/lost IP` 时**总是**打印：
  `heap (link down): free=... min=... largest_internal=...`
- 周期性堆水位日志已移除，只在链路转换时打印，避免长期运行刷屏。
- 判读方法：
  - `free`：当前空闲堆；每轮插拔后应回到相近值；
  - `min`：开机以来历史最低值；反复插拔后若持续创新低，说明有内存未释放或碎片化；
  - `largest_internal`：最大连续内部块；持续下降说明碎片化。
- 需要更细的堆信息时，可在临时诊断代码里调用 `heap_caps_print_heap_info(MALLOC_CAP_INTERNAL)`。

串口日志检查：

```powershell
# 先保存 monitor 输出到 build_log.txt，再运行
.\scripts\serial_log_check.ps1 -LogFile .\build_log.txt
```

### 2.y 热插拔与堆水位记录（2026-10-18，`adf7f61` + 堆日志）

- 日志统计：`New device connected` 7 次、`CPE online` 6 次；每次拔插约 **18~20s** 恢复 link+IP；
- 无 panic/assert/watchdog；`CHECK_SHORT_DEV_DESC` 0 次；`Replaying ... existing device(s)` 刷屏 0 次；
- 堆水位（`free` / `min` / `largest_internal`）：

| 阶段 | free | min | largest |
| --- | --- | --- | --- |
| 开机 | 210556 | 210556 | 167936 |
| 首次 got IP | 178456 | 178440 | 135168 |
| 第 2 次插拔后 got IP | 175364 | 102596 | 126976 |
| 第 3 次插拔后 got IP | 175360 | 102596 | 126976 |
| 第 5 次插拔后 got IP | 175332 | 102596 | 126976 |
| 第 6 次插拔后 got IP | 175332 | 102596 | 126976 |
| 重新上电后首次 got IP | 178456 | 178440 | 135168 |

- 结论：**没有逐次下降**，free 稳定在 ~175.3KB、largest 稳定在 126976，未发现每轮泄漏；`min` 一次性降到 102596 是 Web 客户端/HTTP 会话造成的，之后保持稳定；重新上电后基线完全复现。
- 一次异常：第 4 次枚举后 0.78s 设备即断开（随后第 5 次正常），无错误日志，疑似插拔接触抖动；若复现需查连接器/线缆。
- 观察项（不影响功能）：
  - 每次重新枚举都会配置 ECM 两次（`Parsed MAC`/`packet filter`/`alt setting` 各出现两次）；
  - 拔模组时 `USB HOST: EP command error: ESP_ERR_INVALID_STATE`（清理在途 URB，预期）。
- 仍待补：2.3（ESP32 复位、模组不断电 10 次）、2.4（无 SIM 10 分钟）、2.5（不接模组 10 分钟）、2.2 达到 20 次循环。

### 2.z 多次复位日志：CHECK_SHORT_DEV_DESC、幽灵设备、USB 栈卸载 panic（已修复，构建通过，待上板验证）

现象（`C:\Users\fytech\Desktop\log.txt`，模组外供电、无 RESET 线、复位 ESP32）：

```
E (1088) USBH: Dev 0 EP 0 Error
E (1088) ENUM: Bad transfer status 1: CHECK_SHORT_DEV_DESC
E (1088) ENUM: CHECK_SHORT_DEV_DESC FAILED
W (31293) cpe: No USB link after 30000 ms (attempt 1)
W (31313) cpe: Restarting the USB host stack to force a clean re-enumeration
I (31333) USBH_CDC: No more clients and devices, uninstall USB Host library
Guru Meditation Error: Core 1 panic'ed (LoadProhibited).
--- 0x42022ea9: proc_req_callback at .../espressif__usb/src/usb_host.c:347
--- 0x42029bb1: root_port_callback at .../espressif__usb/src/hub.c:371
--- 0x42027c5a: intr_hdlr_main at .../espressif__usb/src/hcd_dwc.c:1087
```

三个层次的问题：

1. **枚举失败（硬件/板级）**：`CHECK_SHORT_DEV_DESC FAILED` = 主机读不到设备描述符前 8 字节，
   `Bad transfer status 1` 即 `USB_TRANSFER_STATUS_ERROR`（无响应/CRC 错误）。
   模组外供电、ESP32 复位时它不断电，USB 设备控制器保留旧状态，不响应枚举；
   ESP-IDF 枚举 FSM 不会自行重试。
2. **幽灵设备（固件逻辑，已修）**：失败枚举后 `usb_host_lib_info().num_devices` 仍计入设备对象，
   `usb_device_present()` 误判"模组已在总线上"，于是跳过恢复动作，最后停在 FAULT。
   修复：改用 `usb_host_device_addr_list_fill()`（只含完成枚举、已分配地址的设备）。
3. **卸载 USB Host 栈 panic（上游竞态，已绕过）**：`usb_host_uninstall()` 先 `p_host_lib_obj = NULL`
   （`usb_host.c:868`），之后才 `hub_uninstall()` → `hcd_port_deinit()`（`usb_host.c:879`）。
   这之间根端口中断仍使能，一旦有端口事件进来，`root_port_callback()` →
   `proc_req_callback()` 在 `usb_host.c:347` 解引用 NULL（`EXCVADDR 0x0`）→ `LoadProhibited`。
   `espressif/usb` 最新版即 1.6.0，暂无修复版本。

修复（本轮）：**运行期不再卸载/重装 USB Host 栈**。没有 `RESET_N` 时改为根端口掉电/上电
（`usb_host_lib_set_root_port_power(false/true)`，掉电时长 `CPE_USB_PORT_POWER_OFF_MS`，默认 1000ms）：
掉电时 HCD 端口 deinit、VBUS 撤除，模组看到总线断开后通常复位其 USB 控制器；再上电触发全新枚举。
Host 库、CDC 驱动与 client 全程保持安装，既不丢事件也彻底避开上面的竞态。

修复后复位场景预期日志：

```
E (...) ENUM: CHECK_SHORT_DEV_DESC FAILED            ← 复位瞬间仍可能出现，允许
W (31xxx) cpe: No USB link after 30000 ms (attempt 1)
W (31xxx) cpe: Power-cycling the USB root port to force re-enumeration
I (31xxx) cpe: USB root port powered off for 1000 ms
I (...) USBH_CDC: New device connected, address: 1
I (...) cpe: 4G link up / 4G WAN IP: ...
```

判定：只要掉电/上电后能重新枚举并恢复 link/IP，且**不再出现 panic**，该兜底即有效；
若 3 次尝试后仍枚举失败，则确认该模组不响应主机侧断开，只能加 `RESET_N` 或控制供电。

### 2.w 模组 USB 控制器挂死：主机侧已验证，模组侧无解（2026-10-18）

硬件事实：4G 模组只有 USB 口，无 `RESET_N`/其它引线；VBUS 由模组外部电源提供，ESP32 无法控制。

现象（复位 ESP32、模组不断电）：

```
E (1083) HUB: Root port reset failed
（或）E (1082) ENUM: CHECK_SHORT_DEV_DESC FAILED
W (31301) cpe: No USB link after 30000 ms (attempt 1)
W (31301) cpe: Power-cycling the USB root port to force re-enumeration
I (31301) cpe: USB root port powered off for 1000 ms
（之后没有任何 New device connected，直到物理插拔模组才恢复）
```

判定依据：

1. **主机侧正常**：同一固件、同一端口，接 SanDisk U 盘（VID 0781）反复复位 ESP32 均稳定枚举，
   仅打印 `No ECM interface found for device VID: 0781, ... ignore it`（ECM 驱动正常跳过非 ECM 设备）。
2. **模组侧挂死**：`Root port reset failed` 表示主机在 `RESET_HOLD+HOLD_RECOVERY`
   （默认 30+30ms）后端口没有进入 enabled，即模组在总线复位期间自己掉线且不再回来；
   `CHECK_SHORT_DEV_DESC FAILED` 则是复位完成但读不到描述符。
3. **主机侧断开起不到作用**：`usb_host_lib_set_root_port_power(false)` 只 deinit ESP32 的
   HCD 端口并清 `HPRT.PP`；VBUS 外部供电时它撤不掉模组电源，对模组最多算"总线挂起"，
   不会让模组重初始化 USB 控制器。
4. 物理插拔会切断 D+/D-/VBUS，模组 USB 栈才会重启 → 唯一有效的恢复手段。

结论：**纯软件无法复位挂死的模组 USB 控制器**。固件只能保证不 panic、AP/Web 可用、周期性重试；
根治需要模组固件/换模组，或下一版硬件增加模组 `RESET_N` 或受控供电。

> **2026-10-22 缓解（主动重启场景）**：AT 遥测可用的前提下，所有主动重启（Web 重启、
> Web/按键恢复出厂、OTA、L3 受控重启）会在 `esp_restart()` 前请求 AT 任务发送
> `AT+CFUN=1,1`，让模组在 ESP 复位前自行软复位，重启后干净地重新枚举；串口日志为
> `cpe_at: Restarting the modem with AT+CFUN=1,1`。AT 端口不可用时立即跳过，最多等待约 3 s，
> 不阻塞重启。该缓解**不覆盖**掉电、看门狗等非主动复位，2.w 的实验结论保持不变。
>
> 新增验收项：连续 5~10 次主动重启，模组每次都能重新枚举、4G 自动恢复且无需插拔；
> 拔掉模组时重启耗时不增加，仅出现 `Modem reset did not succeed (ESP_ERR_INVALID_STATE)`。

尝试过的实验（均未能解决）：

| 实验 | 改动 | 结果 |
| --- | --- | --- |
| A 加长主机复位 | `CONFIG_USB_HOST_RESET_HOLD_MS=100`、`CONFIG_USB_HOST_RESET_RECOVERY_MS=100`（仅本地 sdkconfig） | ❌ **无效**（2026-10-18，固件 `1c22d2f`）：复位后仍是 `CHECK_SHORT_DEV_DESC FAILED`，且未枚举所以也没有 `speed` 行 |
| B 强制全速（FS-only） | 安装 Host 时 `usb_host_config_t::fsls_only=1`，跳过 HS chirp | ❌ **不可用**（2026-10-18，固件 `e40f0ee`）：ESP-IDF v6.1 + `usb` 1.6.0 的 HAL 未定义 `USB_DWC_HAL_INIT_HAS_CONFIG`，`hcd_port_init()` 直接返回 `ESP_ERR_NOT_SUPPORTED`：`HCD DWC: FS/LS-only host requires ESP-IDF with USB DWC HAL configuration support`，USB Host 装不上。该实验的固件/配置已回撤 |
| 观察（保留） | 设备连接日志打印枚举速度 `speed: high/full/low` | 由于复位后枚举失败，需要**物理拔插模组**一次才能看到速度行，可用于日后确认模组 HS/FS |

结论：**纯软件无法复位挂死的模组 USB 控制器**。下一步只能：
1. 换模组（USB 设备控制器能正确处理主机总线复位），或下一版硬件给模组加 `RESET_N` 或受控供电；
2. 让模组厂确认"主机复位后 USB 设备控制器不响应"是否为已知固件缺陷、有无新固件；
3. 当前固件行为保持不变：不 panic、AP/Web 始终可用、每 ~90s 重试一次；现场遇到时插拔模组即可恢复。

### 2.x 串口日志首轮记录（2026-10-18，固件 `8a3a140` / `iot_usbh_cdc 3.1.2`）

`serial_log_check.ps1` 结果：**PASS**

| 统计项 | 次数 |
| --- | --- |
| New device connected / ECM interface found | 1 / 1 |
| 4G WAN IP / CPE online | 1 / 1 |
| Boot counter cleared | 1 |
| Link up but no IP / No USB link | 0 / 0 |
| USB root port power cycles / ECM link stuck | 0 / 0 |
| CHECK_SHORT_DEV_DESC | 0 |
| `Replaying ... existing device(s)` 刷屏 | 0 |

结论：

- 2.1（正常启动/上网）通过；2.6（无周期日志刷屏）通过；2.7（120s 清零启动计数）通过；
- 无 panic/assert/watchdog，无 USB 枚举失败。
- 日志中大量 `httpd_uri: uri handler execution failed` 与 `httpd_sock_err: error in recv : 11` 来自本次 Web 安全测试的**故意拒绝请求**（401/403/413/畸形 JSON，服务器主动关闭连接），不是故障；后续可考虑降低这类日志级别。
- 观察到启动时 ECM 配置/MAC 解析出现两次（先 `Disconnected` 后 `Connected`），最终 link up 正常；后续可评估去掉重复配置。
- 待补：2.2 热插拔 20 次、2.3 无 RESET 线复位 10 次、2.4/2.5 长时间无 SIM/无模组、2.8 堆水位。

## 3. 复位键（Phase 1.5 回归）

| # | 测试 | 期望 | 结果 |
| --- | --- | --- | --- |
| 3.1 | 启动后 10s 内双击 GPIO0 | 打印当前 AP 口令；管理员口令未改则一并打印 | ☐ |
| 3.2 | 改管理员口令后双击 | AP 口令打印；管理员行显示"changed, plaintext not stored" | ☐ |
| 3.3 | 长按 10s | 恢复出厂；松开后重启 | ☐ |
| 3.4 | GPIO0 在复位期间被按住 | 不用于打印（会进下载模式）；文档已说明 | ☐ |

## 4. 已知接受风险（不阻塞 Phase 2）

| 风险 | 说明 |
| --- | --- |
| 端口句柄 TOCTOU | 未引入引用计数，仅两阶段释放收窄窗口；需实机长稳观察 |
| HCD 控制传输无超时 | 上游 `usb` 组件限制；极端设备可能长时间占用，需上游升级 |
| `iot_eth_install()` 失败泄漏 | API 限制；失败概率低，已有 60s 退避 |
| HCD assert/abort | 上游 managed 组件未 fork |

## 5. 结果记录

| 日期 | 固件版本 | 执行人 | 结果 | 备注 |
| --- | --- | --- | --- | --- |
| 2026-10-18 | `8a3a140` | 用户/代理 | 2.1/2.6/2.7 PASS | 首轮串口日志 |
| 2026-10-18 | `c0ed06a` | 用户/代理 | 2.2 初步 PASS、2.8 初步 PASS | 热插拔 7 次、堆水位 6 轮稳定 |
| 2026-10-18 | `f66b533`/`4cd94f4` | 代理（脚本） | 1.4~1.9、1.11 PASS | Web 安全自动化回归 |
| 2026-10-18 | `b105d1c` | 用户 | panic 消失 | 根端口掉电替代 USB 栈卸载；2.3 受模组硬件限制 |
| 2026-10-18 | `1c22d2f`/`e40f0ee` | 用户 | 实验 A/B 均无效 | 加长复位无效；FS-only 在 IDF 6.1 不支持，已回撤 |

## 6. Phase 2 收口结论（2026-10-18）

**已完成并验收**：

- 驱动：CDC/ECM 本地 fork 的 P0/P1 生命周期缺陷修复、`NEW_DEV` 补发 + 5s 周期重扫、Union/EC 描述符解析、
  `enum_filter_cb` 透传、枚举速度日志（`iot_usbh_cdc` 3.1.3 / `iot_usbh_ecm` 0.4.2）；
- 恢复链路：移除运行期 USB 栈卸载（修复 `proc_req_callback` NULL 解引用 panic），改为根端口掉电/上电 +
  `usb_host_device_addr_list_fill()` 判定设备存在；无设备时按上限重试并保持 FAULT（AP/Web 常驻）；
- Web：管理面仅绑定 SoftAP 网口、会话 + CSRF、登录锁定与指数退避、请求体/JSON 校验、XSS 白名单；
  安全自动化回归全部 PASS；
- 诊断：链路转换时的堆水位日志、`scripts/serial_log_check.ps1`、`scripts/web_security_test.py`。

**未完成/受阻（不阻塞 Phase 3 代码工作）**：

- 2.3 复位 ×10、2.4 无 SIM 10 分钟、2.5 无模组 10 分钟、2.2 补到 20 次、2.8 补到 20 轮：需实机长时间占用；
- 2.3 的根因在模组侧（见 2.w）：外供电模组只有 USB 口、无 `RESET_N`、VBUS 不可控，
  ESP32 复位后其 USB 控制器挂死，纯软件无法复位；产品化需换模组或下一版硬件增加模组复位/供电控制。

**遗留风险**：见第 4 节与 `docs/PHASE2_PLAN.md` 第 5 节（TOCTOU、上游 HCD 无超时、`usb_host_uninstall()` 竞态已绕开）。
