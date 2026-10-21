# 运行时工作流程与等待点

> 适用固件：ESP32-S3 / 16MB Flash / 双 OTA / `iot_usbh_cdc` 3.1.3 + `iot_usbh_ecm` 0.4.2
>
> 代码位置：`main/main.c`（启动编排、OTA 校验）、`components/cpe_wan/`（USB/ECM 生命周期、
> NAPT/DNS、链路管理器与恢复策略）、`main/Kconfig.projbuild`（全部超时/次数阈值）、
> `components/cpe_core/`（配置、认证、状态、启动计数）

## 1. 设计原则

1. **AP/Web 先行**：SoftAP 和 Web 管理面不依赖 4G 链路，先启动；4G 由后台任务管理。
   没插模组、没 SIM、拿不到 IP 时，用户仍然可以连 AP 进入后台查看状态、改配置、恢复出厂。
2. **USB Host 只安装一次**：运行期不再反复拆装 USB Host 栈（审计 P0-2 的整栈重试链已移除）。
3. **分层恢复、有界重试**：从轻到重逐级升级，每一级都有次数/时间上限，失败后停在 FAULT 而不是无限循环。
4. **两类失败不重启整机**：
   - "总线上没有已枚举设备"（模组没插/枚举失败）：只做 USB 层恢复，最后 FAULT；
   - "link up 但拿不到 IP"（无 SIM / PDP 未激活 / DHCP 未就绪）：只等 DHCP，最后 FAULT。
   只有"模组已枚举、ECM 链路始终起不来"才允许受控重启，且重启次数受 NVS 启动计数限制。

## 2. 启动阶段（`app_main`）

```
上电
 │
 ├─ 0. reset_button_init()
 │       配置复位按键 GPIO（默认 GPIO0，低电平有效），并开始"双击打印"窗口计时
 │
 ├─ 1. nvs_flash_init()
 │       损坏（NO_FREE_PAGES / NEW_VERSION）时擦除重试一次
 │
 ├─ 2. esp_netif_init() + esp_event_loop_create_default()
 │
 ├─ 3. cpe_core_init()
 │       ├─ cpe_config_init()   读取/生成配置：AP SSID、AP 口令、管理员口令（每台唯一随机）
 │       ├─ cpe_auth_init()     PBKDF2 口令哈希、会话表
 │       ├─ cpe_status_init()   状态快照
 │       └─ 启动计数 NVS +1    （cpe_sys/boot_cnt，显示在 Web“启动计数”）
 │       任一步失败：打印错误后进入空闲降级（10s 循环），不 abort、不重启
 │
 ├─ 4. cpe_wan_events_register()
 │       IOT_ETH_EVENT（link up/down）、IP_EVENT_ETH_GOT_IP / LOST_IP
 │       必须在 ECM 驱动创建前注册，避免丢失早期 LINK_UP
 │
 ├─ 5. app_wifi_start()        SoftAP 先起（WPA2，配置来自 cpe_core）
 ├─ 6. cpe_wan_ap_hook_register()  AP 重启后重新应用 NAPT/上游 DNS
 ├─ 7. modem_http_start()      Web 管理面（会话 + CSRF 认证）
 │
 ├─ 8. cpe_wan_start()         （可选）模组 RESET 脉冲（仅当配置了 CPE_MODULE_RESET_GPIO），
 │                            保证外部供电的模组从干净 USB 状态开始枚举；
 │                            随后创建 cpe_link 后台链路管理器（见第 3、4 节）
 └─ 9. reset_button_start()    按键监控：启动后双击打印凭据 / 长按 10s 恢复出厂（见第 8 节）
```

启动失败的降级策略：

| 失败点 | 行为 |
| --- | --- |
| NVS 初始化失败 | 打印 `FATAL`，10s 循环空转（可通过串口诊断） |
| 网络栈 / 事件循环失败 | 同上 |
| `cpe_core_init` 失败 | 擦除 NVS 后重试一次；仍失败则空闲降级 |
| SoftAP 启动失败 | 记录错误，继续启动 Web 和链路管理器 |
| Web 启动失败 | 记录错误，继续启动链路管理器（AP 仍可连） |
| `cpe_wan_start` 失败（任务创建失败） | 记录错误，保持 AP-only 模式，不重启 |
| 复位按键任务创建失败 | 记录错误，长按恢复出厂不可用（其余功能正常） |

## 3. 链路状态机

状态定义在 `components/cpe_core/include/cpe_status.h`，Web“运行状态”显示的就是它：

| 状态 | 含义 | 进入条件 |
| --- | --- | --- |
| `BOOT` | 启动/恢复中 | 初始状态；重枚举、重建 ECM 时回到此状态 |
| `USB_WAIT` | 等待 USB 模组 | 没有 link、也没有已枚举设备 |
| `LINK_WAIT` | 已枚举，等待 link / IP | 模组已出现，但 link 未 up，或 link up 但没拿到 IP |
| `ONLINE` | 在线 | `link_up && got_ip` |
| `FAULT` | 可恢复故障，AP/Web 仍可用 | 某一级恢复次数耗尽，后台继续重试 |

```
            ┌──────────────┐
            │     BOOT     │
            └──────┬───────┘
                   │ 安装 USB Host/CDC、创建 ECM/netif
        ┌──────────┴──────────┐
        │                     │
   无 USB 设备            模组已枚举
        │                     │
   ┌────▼─────┐          ┌────▼─────┐
   │ USB_WAIT │          │LINK_WAIT │
   └────┬─────┘          └────┬─────┘
        │ 恢复成功             │ link up + got IP
        └──────────┬───────────┘
                   │
             ┌─────▼─────┐
             │  ONLINE   │ ── 连续 120s → 清零启动计数
             └─────┬─────┘
                   │ link down / 重建 ECM
             ┌─────▼─────┐
             │   FAULT   │  AP/Web 保持可用，后台继续重试
             └───────────┘
```

## 4. 链路管理器循环（`components/cpe_wan/link_manager.c`）

任务优先级 6、栈 4KB。实现为显式状态机（`lm_phase_t` + `lm_ctx_t`），`lm_tick()` 每轮执行一次；
默认每 **500ms** 一拍，若本轮已执行阻塞式恢复动作（对应旧实现的 `continue`）则不再追加等待。

```
lm_tick()
  ├─ lm_ensure_ready()      CDC/USB Host 未安装 → wan_usb_cdc_install()（只装一次）
  │                         ECM 未就绪 → wan_ecm_bring_up()
  │                         （link 可能在 glue 挂接前就 up，这里会补一次 connected 同步）
  ├─ lm_service()           读取状态快照，处理 link/IP 边沿、NAPT/DNS、周期 heap 日志
  └─ 按状态分派：
       link up && got_ip   → lm_online()    ONLINE
       link up && !got_ip  → lm_wait_ip()   等待 DHCP（见 5.2）
       !link_up            → lm_wait_usb()  等待枚举（见 5.1）
```

恢复动作各自独立：`lm_recover_dhcp()`、`lm_rebuild_ecm()`、`lm_try_reenumerate()`、
`lm_wait_for_modem()`、`lm_escalate_usb_fault()`、`lm_controlled_restart()`。
所有阈值、等待时长、日志文本与副作用顺序与重构前一致。

### 4.1 ONLINE 时做什么

- 把 ECM netif 设为默认网卡（`esp_netif_set_default_netif`）；
- 开启 NAPT（`ip_napt_enable`），AP 客户端才能通过 4G 上网；
- 把模组上报的 DNS 下发给 SoftAP 的 DHCP 服务，客户端无需手动配置 DNS；
- 记录 `online_start_us`；连续在线满 `CPE_HEALTHY_CLEAR_SECONDS`（120s）后：
  - NVS `cpe_sys/boot_cnt` 清零；
  - 状态快照同步为 0（Web“启动计数”立即变 0）；
  - 打印 `Healthy for 120 s, boot counter cleared`。

### 4.2 启动计数（boot counter）的作用

- 每次开机先 +1；
- 它同时是"允许自动重启的次数上限"：只有"已枚举但 ECM 卡死"才会触发受控重启，
  且要求 `boot_cnt < CPE_MAX_REBOOTS`（默认 3）；
- 在线满 120s 清零，保证：
  - 正常工作不会因为历史计数而失去重启额度；
  - 真正崩溃/重启循环在 3 次后停下，停在 FAULT，避免无限重启（AP/Web 仍可进）。

## 5. 恢复策略与等待点

### 5.1 没有 USB link（`!link_up`）

等待 `CPE_LINK_TIMEOUT_MS`（默认 **30s**）后判定失败。若此时**没有任何已枚举设备**：

> 判定"没有设备"使用 `usb_host_device_addr_list_fill()`（只包含完成枚举、已分配地址的设备），
> 不用 `usb_host_lib_info().num_devices`：失败枚举（如 `CHECK_SHORT_DEV_DESC FAILED`）后
> 设备对象仍可能被计数，会误判为"设备在总线上"而跳过根端口掉电恢复。

| 顺序 | 动作 | 条件 |
| --- | --- | --- |
| ① | 脉冲模组 `RESET_N`（`CPE_MODULE_RESET_GPIO`） | 硬件已接线（推荐） |
| ② | USB 根端口掉电再上电（`CPE_USB_PORT_POWER_OFF_MS`，默认 1000ms） | 未接 `RESET_N` 时的默认兜底 |

最多尝试 `CPE_USB_ENUM_RESET_ATTEMPTS`（默认 **3**）次；仍失败则：

- 状态 → `FAULT`（"no USB modem, AP/Web still available"）；
- 退避 `CPE_RECOVERY_RETRY_MS`（默认 **60s**）后重置尝试次数、重新开始；
- **绝不重启整机**（可能是用户根本没插模组）。

> **不再重启 USB Host 栈**。原"卸载 host/CDC → 500ms → 重新安装"的兜底在
> `espressif/usb` 1.6.0 上有竞态：`usb_host_uninstall()` 先把 `p_host_lib_obj` 置 NULL，
> 之后才卸载 hub/HCD；若在这段窗口内有根端口中断进来，`proc_req_callback()` 会解引用
> NULL，panic（`LoadProhibited`，`EXCVADDR 0x0`）。实测：复位后 1.08s 出现
> `CHECK_SHORT_DEV_DESC FAILED`，30s 后进入该兜底并在 `usb_host_uninstall()` 处 panic。
> 现在改用根端口掉电/上电：HCD 端口被 deinit、VBUS 掉电，模组看到的总线断开，
> 通常会自动复位其 USB 控制器；Host 库、CDC 驱动与 client 全程保持安装，
> 从根本上绕开该竞态。

> 已知硬件限制：外部独立供电的模组在 ESP32 复位后可能保留旧 USB 状态，导致
> `CHECK_SHORT_DEV_DESC FAILED`。纯软件兜底是根端口掉电（是否能让模组复位取决于
> 模组是否监听 VBUS）；产品硬件应提供 `RESET_N`→GPIO 或受控 5V 供电。

### 5.2 link up 但没有 IP

**不要立即拆 ECM**：拆 ECM 会把模组 USB 接口复位，而模组重新附着网络、重建 PDP、
启动 DHCP 服务器往往需要几十秒，短窗口会导致"重建—超时—再重建"的循环。

等待 `CPE_IP_TIMEOUT_MS`（默认 **90s**）后：

| 步骤 | 动作 | 次数 |
| --- | --- | --- |
| ① | 保持 ECM/USB 会话，只重启 DHCP 客户端（stop → 300ms → start），再给一个 90s 窗口 | `CPE_IP_DHCP_RESTARTS`，默认 **2** 次 |
| ② | DHCP 重试耗尽 → 拆 ECM 重建，退避 60s 后重新建链 | 循环 |
| ③ | 仍失败 | 保持 `FAULT`，后台继续重试，**绝不重启整机** |

日志会明确打印 DHCP 客户端状态（`ESP_NETIF_DHCP_STARTED` 等），便于区分
"DHCP 客户端没跑起来"和"客户端在跑但模组不回包"。

### 5.3 已枚举但 ECM 链路始终起不来

这是唯一会主动重启整机的场景：

- 每 `CPE_MAX_RECOVERY_ATTEMPTS`（默认 **3**）轮失败后，读取 NVS 启动计数；
- 若 `boot_cnt < CPE_MAX_REBOOTS`（默认 **3**）→ 受控 `esp_restart()`：
  `ECM link stuck, controlled reboot N/3`；
- 否则 → `FAULT`（"ECM link stuck, AP/Web still available"），后台继续重试。

### 5.4 全部等待点一览

| 等待点 | Kconfig | 默认值 | 为什么 |
| --- | --- | --- | --- |
| 等 USB 枚举/端口就绪 | 事件驱动 | — | USB Host 事件到达才继续；由 30s link 超时兜底 |
| 等 link up | `CPE_LINK_TIMEOUT_MS` | 30s | 正常枚举+ECM 配置几秒内完成，30s 覆盖模组上电慢的情况 |
| 等 IP（DHCP） | `CPE_IP_TIMEOUT_MS` | 90s | 模组 USB 复位后重新附着网络、重建 PDP、启动 DHCP 需要时间 |
| 无 IP 时重启 DHCP 客户端 | `CPE_IP_DHCP_RESTARTS` | 2 次 | 只重启 DHCP，不破坏 USB/ECM 会话 |
| 恢复退避（ECM 重建/无设备） | `CPE_RECOVERY_RETRY_MS` | 60s | 避免高频拆装 USB 驱动，给模组重新初始化时间 |
| 无设备时的恢复次数 | `CPE_USB_ENUM_RESET_ATTEMPTS` | 3 | 覆盖模组 `RESET_N` 脉冲与根端口掉电两种手段 |
| ECM 卡死判定轮数 | `CPE_MAX_RECOVERY_ATTEMPTS` | 3 | 多次 ECM 重建无效后才考虑重启整机 |
| 受控重启上限 | `CPE_MAX_REBOOTS` | 3 | 用 NVS 启动计数防止无限重启 |
| 健康清零时间 | `CPE_HEALTHY_CLEAR_SECONDS` | 120s | 排除"刚拿到 IP 就掉线"，并重置重启额度 |
| 主循环节拍 | 固定 500ms | — | 及时响应 link/IP 事件，又不空耗 CPU |

> 全部参数可在 `idf.py menuconfig → CPE Application Configuration` 中调整；
> 修改 Kconfig 后如果 `sdkconfig` 未自动重生成，先执行 `idf.py reconfigure` 再构建。

## 6. 关键日志解读

| 日志 | 含义 |
| --- | --- |
| `USB CDC driver installed` | USB Host + CDC 驱动安装成功（每次启动只装一次） |
| `New device connected, address: N, speed: high/full/low` | 设备枚举完成，并打印协商出的 USB 速度（用于确认 HS/FS） |
| `Replaying the CONNECTED event for 1 existing device(s)` | 驱动重建/重装后，把已枚举设备的连接事件补发给上层 |
| `ECM interface found: VID: ..., PID: ..., IFNUM: 4` | ECM 描述符解析成功（VID/PID/控制接口号） |
| `USB ECM interface configured` | 数据接口 alternate setting、端点配置完成 |
| `4G link up` | ECM 上报网络连接（USB 链路层就绪，**不代表有 IP**） |
| `4G WAN IP: 192.168.x.x` | DHCP 拿到 IP |
| `CPE online (WAN IP ...)` | 进入 ONLINE，此时 NAPT/DNS 已配置 |
| `Link up but no IP after 90000 ms (attempt N)` | 90s 内没拿到 IP，准备重启 DHCP 客户端 |
| `Restarting the DHCP client (status N, ...)` | 只重启 DHCP；`status 2` = DHCP 客户端正在运行 |
| `DHCP client retries exhausted, rebuilding the ECM driver` | DHCP 重试耗尽，开始拆 ECM 重建 |
| `No USB link after 30000 ms (attempt N)` | 30s 内没有 USB link |
| `Firmware version ... on ota_0, next boot ota_1` | 启动时打印运行版本/分区，以及下次启动分区 |
| `Image is pending verification, validating in 60000 ms` | 该次启动来自 OTA，等待健康判定后标记有效 |
| `OTA image validated and marked valid (rollback cancelled)` | 新固件已验证，取消回滚 |
| `OTA image validated: version ... on ota_x (...), boot partition switched` | 上传完成、镜像校验通过、已切换启动分区 |
| `Firmware update stored, restarting into the new image` | Web 升级成功，即将重启进入新固件 |
| `Power-cycling the USB root port to force re-enumeration` | 无设备时触发软件兜底：根端口掉电/上电（不再卸载 USB 栈） |
| `USB root port powered off for 1000 ms` | 根端口已掉电并等待模组感知总线断开 |
| `Healthy for 120 s, boot counter cleared` | 已连续在线 120s，启动计数清零（正常健康标志） |
| `ECM link stuck, controlled reboot N/3` | 已枚举但 ECM 卡死，触发有界受控重启 |
| `no USB modem, AP/Web still available` | 无设备，进入 FAULT，AP/Web 保持可用 |
| `Factory reset performed (AP credentials kept, ...)` | 恢复出厂完成：AP SSID/口令保留，管理员口令重置为 AP 口令 |
| `Reset button double-clicked: printing credentials` | 启动后双击，开始打印当前凭据 |
| `Admin password  : (changed, plaintext not stored; hold the button 10 s to factory reset)` | 管理员口令已修改，明文无法恢复（AP 口令仍会打印） |
| `Reset button pressed (hold 10000 ms for factory reset)` | 运行中按下按键，开始计时 |
| `Reset button held for 10000 ms, restoring factory settings` | 长按达到阈值，恢复出厂并重启 |

## 7. 与 AP/Web 管理面的关系

- SoftAP 与 Web 在 4G 之前启动，任何链路状态下都可访问 `http://192.168.4.1`；
- Web 服务器的监听 socket **只绑定 SoftAP 网口**（`httpd_config_t.if_name`），4G WAN 侧无法访问管理面；
- Web 状态页每 5s 轮询一次 `/status`；客户端列表在页签激活时每 4s 轮询；
- 只有 `ONLINE` 状态下 NAPT 才开启，此时 AP 客户端才能访问外网；
- 认证：会话 Cookie（`CPE_SID`）+ CSRF 双提交；首次登录强制修改管理员口令；
  连续 5 次失败锁定 60s，之后按 120s/240s/600s 指数退避；会话空闲 30 分钟失效；
  登出/改密会使会话失效；恢复出厂保留本机 AP 凭据，管理员口令重置为 AP 口令。

## 8. 复位/恢复出厂按键（默认 GPIO0）

同一个按键承担两个功能，均由 `main/reset_button.c` 实现：

| 操作 | 条件 | 行为 |
| --- | --- | --- |
| 启动后 `CPE_CREDENTIAL_PRINT_WINDOW_MS`（默认 10s）内**双击**按键 | 任意状态 | 打印当前 AP SSID/口令；管理员口令若仍等于 AP 口令则一并打印，否则明确标注"已修改、明文不可恢复" |
| 开机后连续按住 `CPE_FACTORY_RESET_HOLD_MS`（默认 10s） | 任意状态 | 恢复出厂设置（保留 AP 凭据、管理员口令重置为 AP 口令）、清启动计数、重启 |

- 按键为**低电平有效**、内部上拉，接在 GPIO 与 GND 之间；GPIO 和有效电平可通过
  `CPE_RESET_BUTTON_GPIO` / `CPE_RESET_BUTTON_ACTIVE_LEVEL` 配置，`-1` 表示不使用；
- 双击判定：第二次按下的起点距第一次按下起点 ≤ `CPE_DOUBLE_CLICK_MS`（默认 800ms），
  且整体落在启动后窗口内；
- 长按触发恢复出厂后，固件会**等待按键松开**再重启：GPIO0 在复位期间被拉低会让 ROM 进入
  UART 下载模式，所以不能"按着 10 秒直接重启"；
- AP 口令总是可打印（明文存于 NVS）；管理员口令只在仍等于 AP 口令时可打印，修改后只剩
  PBKDF2 哈希，因此打印行会明确标注"已修改"，避免误导；
- **GPIO0 是 ESP32-S3 的启动 strapping 引脚**：在芯片复位期间按住会进入 UART 下载模式，
  应用不会启动。这也是打印凭据改用"启动后双击"而不是"启动时按住"的原因；
  板子方便改线时建议换到其它空闲 GPIO。

## 9. 固件升级（OTA，Phase 3）

### 9.1 本地升级流程

管理面入口：左侧菜单"系统升级"→ `http://192.168.4.1/upgrade.html`（选择 `.bin`、进度条、确认框、
出错提示、"重启设备"按钮）。等价的命令行流程：

1. PC 连上 SoftAP，登录管理面（需先完成首次改密）；
2. `GET /api/ota/status` 读取当前版本/分区/上传上限；
3. `POST /api/ota/upload` 上传完整 app 镜像（`application/octet-stream`，body 即 `.bin`，
   受会话 + CSRF 保护）：
   - 固件按 `Content-Length` 流式写入**非活动** OTA 分区（`esp_ota_write`，4KB 分块）；
   - 写满且 `esp_ota_end()` 校验通过后才 `esp_ota_set_boot_partition()` 切换启动分区；
   - 响应 `{"status":"ok","message":"ota_ready","reboot":true}` 后自动重启；
4. 新固件启动后处于 `PENDING_VERIFY`，在 `CPE_OTA_VALIDATE_DELAY_MS`（默认 60s）后由
   `cpe_ota_mark_running_valid()` 标记为有效，取消回滚；
5. 若新固件在标记有效前崩溃/反复复位，bootloader 自动回滚到旧版本。

失败/异常处理：

- 请求无 body、超过分区大小 → 400/413；
- **错误响应前先排空未读 body**（`ota_drain_unread_body()`，5 秒上限）：否则服务器在客户端仍在上传时
  关闭 socket，内核回 RST，浏览器 XHR 触发 `onerror`，页面只能显示"上传中断"而看不到真正的 4xx 错误；
- 上传内容首字节不是 `0xE9`（ESP 镜像 magic）→ 在写 flash 之前直接 `400 invalid_firmware_image`，
  非活动分区不被擦除；**浏览器端也会先读首字节预校验**，选错文件时立即提示、不上传；
- 镜像校验（`esp_ota_end`）失败 → 400 `invalid_firmware_image`，旧版本不变；
- 上传中拔电/断网 → `otadata` 未切换，重启仍进旧版本；重新上传即可；
- 上传进行中再次上传 → 409 `ota_busy`；当前运行镜像处于 `PENDING_VERIFY`（尚未跑满健康判定）→ 409 `pending_verify`；
- `POST /api/system/reboot`：运维用重启接口（会话 + CSRF）。

> `idf.py flash` 直接烧录的镜像不走 OTA 状态机，不受回滚影响。
> 注意：当前模组在 ESP32 重启后可能 USB 挂死（见 `PHASE2_TEST_PLAN.md` 2.w），升级重启后
> 4G 若未恢复需人工插拔模组，这是硬件限制，与 OTA 无关。

## 10. 相关文档

- 阶段计划与审计条目映射：[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md)
- 上板验收清单：[PHASE1_TEST_PLAN.md](PHASE1_TEST_PLAN.md)
- 构建与登录流程：[../README.md](../README.md)
