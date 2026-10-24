# 重构整改计划（2026-10-22）

配套审计：`docs/CODE_AUDIT_2026-10-22.md`（问题编号 A1–A7、B1–B8、C1–C10、D1–D10）。

## 原则

1. **先验证基线**：DHCP 重复分配修复（`79931a2`）尚未上机验证，重构必须以"当前行为正确"为起点。
2. **行为保持优先**：除明确标注"行为变更"的项，不得改变时序、阈值与日志语义。
3. **不动 `iot_usbh_cdc` / `iot_usbh_ecm` fork**（若 D5 必须动，同步更新版本与 CHANGELOG）。
4. **一批一主题、一小项一提交**：便于 `git revert` 二分回退；编译与真机验证由用户完成。
5. **注释自解释**：代码注释里不写审计编号（如"audit D8/C7"）或外部文档条目号。理由（为什么
   这样做）直接写清楚；编号与提交的对应关系只留在本文件，文档一旦丢失编号就成了无意义噪音。
6. **日志级别**：`ESP_LOGW`/`ESP_LOGE` 只留给异常（失败、降级、被拒绝的请求），正常流程
   （定时维护、控制台重启/恢复出厂、按键按下、升级成功落盘）一律 `ESP_LOGI`——否则警告刷屏会
   淹没真正的异常。每个源文件一个与模块同名的 TAG，现场可按 `cpe_wan`/`cpe_net`/`cpe_link`
   分别过滤驱动层、网络服务层与链路状态机。

## 节奏

每批：改代码 → 用户 `idf.py build` + 真机验证 → 通过后再进下一批。

---

## 进度（截至 2026-10-23）

| 项 | 状态 | 提交 |
|---|---|---|
| 1.1 OTA 上传移出 httpd 单任务 | 完成 | `d33fc5b` |
| 1.2 重启前宽限延时移出 handler | 完成 | `3fd33a9` |
| 1.3 `/wlan_advance` 复用 apply 任务 | 完成 | `f8556c0` |
| 1.4 AP 重启后的 NAPT/DNS 重应用移出事件循环 | 完成 | `8e474fe` |
| 1.5 netif action 走 TCP/IP 线程 | 完成 | `28c65d0`（`c372f83` 修返回值） |
| 1.6 ECM 句柄并发保护 | **部分完成，锁已回退** | 见下节 |
| 2.1 延迟重启统一入口 | 完成 | `14af5e6` |
| 2.2 恢复出厂统一入口 | 完成 | `5d5e5f7` |
| 2.3 `app_wifi_sta_disconnect()` | 完成 | `6e143a7` |
| 2.4 Kconfig 归位 | 完成 | `703ade8` |
| 2.5 会话/路由表收敛 | 完成 | `bc87830` |
| 2.6 统一 NVS helper | 完成 | `e3a8bd4` |
| 3.1 锁内不再跑 PBKDF2 | 完成 | `574af83` |
| 3.2 IP/枚举恢复路径上界 | 完成 | `7181a36` + `1d84032` |
| 3.3 枚举过滤器正名 | 完成 | `4f9c103` |
| 3.4 teardown 回退默认 netif | 完成（该条为误报，仅补注释） | `0650dc7` |
| 3.5 AT 句柄与复位信号量 | 完成 | `759af59` |
| 3.6 缓冲区余量与 CME 解析 | 完成 | `7e10c92` + `6f289bf` |
| 3.7 按键等待松手有上限 | 完成 | `e6983c0` |
| 3.8 OTA busy 改 test-and-set | 完成 | `62f6f2b` |
| 批次 4（清理 10 项） | 完成（每项一提交） | `ca5eeae`…`c6ab803`，见 `git log` |
| 批次 5（无 IP 时重启模块） | 完成（**待真机验证**） | `a54b7e5` |
| C10（审计里未列入批次） | 完成 | `ddb7d74` |

### 1.6 遗留说明（需重新设计）

`s_ecm` 的递归互斥量曾完整实现（`28c65d0`），但真机验证时造成死锁并被撤销（`bcca642`）：

- 事件回调（默认事件循环任务）通过 `wan_ecm_eth_handle()` / `wan_ecm_netif()` 取锁；
- 链路管理任务在 teardown 期间持锁，内部阻塞在 `esp_netif_tcpip_exec()` / `esp_netif_destroy()`
  （要等 TCP/IP 线程，而该线程可能正往事件循环投递事件）；
- 结果：事件循环任务永久阻塞，teardown 也回不来 → `Tearing down ECM driver` 之后没有
  `ECM driver ready`，`httpd` 持续报 `Failed to post esp_http_server event: ESP_ERR_TIMEOUT`。

**结论**：事件回调绝不能阻塞等待。彻底解决需要 ECM 侧"引用计数或延迟释放"
（回调只取快照且不等待，teardown 延后真正释放），方案确认后单独立项；
当前仍保持"句柄由链路管理任务独占、回调只比较指针"的既有约束。
`esp_netif_action_stop/connected` 走 TCP/IP 线程的部分保留。

### 期间的实测修复（非审计项）

| 提交 | 内容 |
|---|---|
| `79931a2` | 更新上游 DNS 时不再重启 DHCP 服务器（避免 IP 重复分配） |
| `ab17e41` | DHCP 的 DNS 选项改为开机时启用：`OP_SET` 在服务器运行期间必返 `ALREADY_STARTED`，运行时只校验 |
| `bcca642` | CDC 驱动安装提前到 `cpe_wan_start()`，消除 AT 事件注册的开机竞态 |
| `0fb6a11` | AT 复位超时由头文件硬编码改为 Kconfig（`CPE_AT_MODEM_RESET_TIMEOUT_MS`，默认 3000，范围 1000–10000） |

### 与计划的偏差

- **2.1**：实现为 `cpe_wan_reboot_deferred(reset_modem_first, reason)`，放在 `cpe_wan` 而非
  `cpe_core/cpe_system.c`。原因：该动作需要 `cpe_at`（AT 复位模组）与 `app_wifi`（deauth 站点），
  而这两个组件都依赖 `cpe_core`，放进 `cpe_core` 会形成循环依赖。
  5 处调用点中 4 处已改用它；`link_manager` 的受控重启保持内联（需停在自身循环里）。
- **2.2**：`cpe_system_factory_reset()` 落在 `cpe_core`，跨组件清理（站点昵称、维护策略）
  通过注册钩子完成，同时修掉审计 A7 的反向依赖（`cpe_config` 不再直接清启动计数）。
- **2.4**：按"谁读谁拥有"归位，选项名与默认值/范围一律不变（现有 `sdkconfig` 取值不受影响）：
  `CPE_USB_TASK_*`/`CPE_LINK_*`/`CPE_IP_*` → `components/cpe_wan/Kconfig.projbuild`；
  `CPE_MAX_REBOOTS`/`CPE_HEALTHY_CLEAR_SECONDS`/`CPE_MAINT_*` → `components/cpe_maintenance/`
  （`CPE_MAX_REBOOTS` 仍由 `link_manager` 读取，仅菜单归属移动，与健康计数归为一组）；
  `CPE_AT_*` → `components/cpe_at/`。板级 GPIO、USB 枚举、OTA 与恢复重试等应用级选项
  暂留 `main/Kconfig.projbuild`。
- **2.5**：路由表落在 `web_console/router_web/web_routes.c`（17 条 API + `"/*"` 静态），
  表项数对 `CPE_WEB_HTTPD_MAX_URI_HANDLERS`（新具名常量，替代 `modem_http_config.c` 的
  `20`）与 `CPE_WEB_ROUTE_LOG_MAX` 做 `_Static_assert`。鉴权分工：GET 全部由分发器完成
  （handler 内的 `reject_body + require_session` 样板已删除），POST 在分发器先过一次会话闸门，
  但 CSRF 与请求体仍留在 handler 的 `cpe_web_post_begin()`（它要把会话令牌交给 handler）。
  由此带来一处**请求拒绝顺序**变化：未鉴权的 POST 现在先返回 401，而不是先返回
  415/跨域 403。静态白名单实测需覆盖登录页图标目录：`/`、根目录 `*.html`、`/css/`、
  `/js/`、`/img/`（计划的 `/css|js|png` 会漏掉 `img/*.svg` 与 `img/favicon.ico`）。
- **C9**：审计列的 4 项只改了 2 项。`reboot_runs` 确为只写不读，已删；OTA 镜像状态查询
  已转内部。另两项与现状不符，未动：`ecm_runs` 实际被打印（`Scheduled ECM rebuild #N`），
  `cpe_web_station_registry_deinit` 有调用方（`modem_http_config.c` 启动失败的回滚路径）。
- **B1**：没有把四种原语强行统一成互斥量，而是**各自写明选型理由**：`cpe_config` 递归互斥
  （public setter 互相调用，普通互斥量会自锁）、`cpe_auth`/`cpe_status` 普通互斥量（临界区内
  不跨模块调用）、`cpe_ota` 临界区（只发布几个标量、无生命周期、不会与任何锁死锁）。锁序
  `cpe_auth → cpe_config` 写进两个公开头。
- **B4**：审计列的 4 处 `while (1)` 只有 3 处统一进 `fatal_idle()`；第 4 处（`cpe_wan_start()`
  失败）**改为降级继续**（AP-only），因为它与紧邻注释矛盾，且会把 AT 遥测/按键/维护任务一起
  拖死。`wan_ecm_teardown()` 改为返回 `esp_err_t`，但 ECM 重建预算是否递增**未改**（牵动
  3.2 的有界退化语义），只让失败在日志里显形。
- **B7**：helper task 的栈/优先级统一到 `http_internal.h` 的 `CPE_WEB_HELPER_TASK_STACK/PRIO`
  （三个 helper 任务原本一处裸写、两处各自定义）；各组件的延时与任务参数就地具名，**数值
  一个没改**——重启宽限 800/300/200 ms 三处保留各自取值，只在注释里说明用途。
- **3.1**：除了把 PBKDF2 移出锁外，登录路径在重新持锁后**再复查一次锁定期**（校验期间
  可能被另一次尝试锁住）；失败计数与退避的写入仍全部在锁内，退避策略不变。
- **3.2**：计划的"加界"与"只改文档"二选一，实际取折中——**只封顶最贵的一步**：新增
  `CPE_IP_MAX_ECM_REBUILDS`（默认 3，`0` = 旧行为）限制"链路起来了但没 IP"路径的 ECM
  拆建次数，用尽后降级为"只重启 DHCP 客户端"（AP/Web 全程可用、仍会自动恢复、绝不因
  此重启整机）。枚举路径（无模组）判定为有意设计，**不改行为**，只改 `cpe_wan.h` 与
  `link_manager.c` 的表述。顺带修掉 B8 的另两处文档不符：`cpe_wan_events_register()`
  返回的是**首个**错误（实现里即 `first_err`）、ECM 重建请求是 **sticky** 而非被丢弃。
- **3.3**：选择"正名 + 修正 help"而非"不匹配时 `return false`"。它装在 CDC 驱动里，
  拒绝枚举会让换 PID / 处于下载模式的模组被**静默**挡掉，日志上只剩"no USB modem"，
  比现状更难查；而它的真实用途只是给指定模组强制一个配置值。Kconfig 符号名一律不变，
  避免现有 `sdkconfig` 取值失效（同 2.4 的口径）。
- **3.4**：**审计 D7 的后半条（"teardown 从不回退默认 netif"）经查不成立，未改行为**。
  `esp_netif_destroy()` 会先摘除 netif（`esp_netif_lwip.c:1068`），lwIP 的 `netif_remove()`
  清空默认路由（`netif.c:820`），随后 `esp_netif_update_default_netif(ESP_NETIF_STOPPED)`
  （`:1081`）因 `esp_netif_is_active()` 走列表检查（`:301-309`）判定已销毁，清掉手动
  override 并遍历剩余网卡、把仍 up 的 SoftAP 设为默认（`:376-398`）。即默认网卡**已经
  自动回退**，显式回退只会与 IDF 重复。仅在 `usb_wan.c` 与 `RUNTIME_FLOW.md` 记录依据，
  防止有人照审计条目去"修"。
- **3.2 补漏（`1d84032`）**：`ecm_rebuilds` 最初也跟着 link 上升沿一起清零，而**拆 ECM
  本身就会把 link 打掉再拉起**，于是每次拆建后预算都被重置、上限永远到不了（行为与加界前
  完全相同）。改为只在"拿到 IP""进入 ONLINE""计划内重建"时归零，并同步 Kconfig help 与
  `RUNTIME_FLOW.md` 5.2 的口径。教训：给"拆建类"动作设上限时，不能复用被该动作自身触发的
  边沿做复位。
- **3.5**：按计划的三条都做了。其中"探针失败即走 `at_release_port()`"带来一处**行为变化**：
  候选接口探测失败时会一并 `at_clear_status()`，遥测比原来更早清零（原本要等设备消失或
  连续 `AT_MAX_FAILURES` 次失败才清）；数值本就过期，如需保留旧值可给 `at_release_port()`
  加"不清理遥测"的变体。事件注册**仍永不放弃**（CDC 驱动由链路管理任务安装，可能确实晚），
  只是 10 次后上报表态并降为 5s 一次。
- **3.6**：除了加余量，还把 `cpe_status_t` 的 `ip`/`public_ip`/`imei`（16→20）与
  `modem_model`/`modem_fw`（32→40）一起放大——只放大 AT 侧缓冲的话，截断会原样挪进
  `cpe_status_set_modem_info()`。已核对所有使用点都是 `snprintf(dst, sizeof dst, …)`，
  JSON 走 sbuf（动态增长），无越界风险。顺带清掉 D9 的第 4 项：`random_password()` 改为
  **收容量**（与 `random_token()` 同口径），但仍封顶 `CPE_PASSWORD_LEN`——**不**把 AP 口令
  变成 63 字符，那会让手工输入无法使用。
- **3.7**：审计写的是"等待松手加超时上限"，实际实现为"**松手即重启 + 未松手每 5s 报错**"，
  **没有**加"超时后强制重启"。因为 GPIO0 是 ESP32-S3 的 strapping 脚，带着按键重启会让 ROM
  进入 UART 下载模式，对无人值守设备比"重启被推迟"更糟。现在的状态是：出厂设置**已落盘**，
  任务不阻塞（Web 全程可用），一旦松手立刻完成重启。
- **3.8**：改成 test-and-set 后必须配套一件事——**所有失败路径归还预约**（`goto fail`）。
  旧代码在失败时本就不会置位，所以没有这个问题；一旦提前预约，漏掉归还就会让 OTA 永久忙。

---

## 批次 0：基线验证（阻塞后批，用户侧）

| 项 | 内容 | 通过标准 |
|---|---|---|
| 0.1 | `idf.py build` + 烧录 | 无新 warning |
| 0.2 | 两台客户端先连 AP → 等 4G 上线 → 再连第三台 | 第三台拿到 `.4`，无 `Duplicate IP`；DNS 未变时无客户端掉线 |
| 0.3 | 4G 断开再恢复 | 客户端保留原 IP；DNS 变化时仅一次 deauth 重连 |
| 0.4 | OTA / 重启 / 恢复出厂各跑一遍 | 与修复前表现一致 |

---

## 批次 1：并发与阻塞（风险最高）

| # | 对应问题 | 改动 | 文件 |
|---|---|---|---|
| 1.1 | D1 | OTA 上传移出 httpd 任务（专用任务 + 队列/双缓冲），空闲超时 180 s → 15 s | `api_ota.c`、`cpe_ota.c` |
| 1.2 | D1 | 重启前宽限延时搬进短命 helper task（复用 `api_wlan.c:300` 模式） | `api_ota.c:290`、`api_system.c:93,117` |
| 1.3 | D1 | `/wlan_advance` 复用 `/wlan_general` 的 `wlan_apply_job_t` helper task | `api_wlan.c:406-423` |
| 1.4 | D3 | `on_ap_start_reapply_napt` 只投递事件，NAPT/DNS/deauth 移回业务任务 | `wan_net.c:174-189,223` |
| 1.5 | D4 | `esp_netif_action_stop/connected` 改走 tcpip 线程 | `usb_wan.c:152,280` |
| 1.6 | D5 | `s_ecm` 加互斥量；`iot_eth_install()` 失败可回收（或句柄只建一次） | `usb_wan.c:46,138-189,217-224` |

**验收**：批次 0 全过；上传/重启期间 `/api/status` 仍可响应；反复断 4G 无断言；堆无单调下降。

---

## 批次 2：服务层收敛（架构主线）

| # | 对应问题 | 改动 | 文件 |
|---|---|---|---|
| 2.1 | A1/A3 | 新增 `system_restart(reason)`：AT 复位 → deauth → 统一宽限 → `esp_restart`；替换 5 处"延时+重启" | 新增 `cpe_core/cpe_system.c`；改 `api_system.c`、`api_ota.c`、`reset_button.c`、`cpe_maintenance.c`、`cpe_wan/link_manager.c` |
| 2.2 | A1 | 新增 `cpe_system_factory_reset()`，统一按键与 Web 两条路径 | `cpe_core` + `api_system.c:55-96` + `reset_button.c:122-141` |
| 2.3 | A1 | `app_wifi_sta_disconnect(mac)`，handler 不再直调 `esp_wifi_deauth_sta` | `app_wifi.c/h`、`api_station.c:90-98` |
| 2.4 | A2 | Kconfig 归位：`CPE_USB_TASK_*`/`CPE_LINK_*`/`CPE_IP_*`/`CPE_MAX_REBOOTS`/`CPE_MAINT_*`/`CPE_AT_*` 迁到各自组件 | `main/Kconfig.projbuild` → `cpe_wan`/`cpe_maintenance`/`cpe_at` |
| 2.5 | A6/D2/B6 | `cpe_web_get_begin()`（与 `cpe_web_post_begin` 对称）；路由表集中为静态表 `{uri, method, handler, auth}`；`/*` 只对白名单免鉴权 | `http_session.c`、`http_internal.h`、各 `api_*.c`、`static_files.c:84-120,162-167` |
| 2.6 | A5/C6 | 统一 NVS helper `cpe_kv_load_u32/save/erase`，收敛命名空间 | `cpe_maintenance.c`、`station_names.c`、`cpe_config_store.c` |

**行为变更（需确认）**：2.5 收紧静态资源访问——仅 `/`、根目录 `*.html`、`/css/`、`/js/`、`/img/`
匿名可访问，SPIFFS 内其它文件不再公开（`static_files.c` 的 `path_is_public()`）。

---

## 批次 3：正确性与边界

| # | 对应问题 | 改动 | 文件 |
|---|---|---|---|
| 3.1 | D6 | 锁内不再跑 PBKDF2（锁内取快照 → 锁外校验 → 锁内提交） | `cpe_config.c:393-441`、`cpe_auth.c:112-183` |
| 3.2 | D7/B8 | 恢复策略二选一：给 IP/枚举路径加上界（终态保 AP/Web），或修正 `cpe_wan.h:49-51` 与 Kconfig help | `link_manager.c:236-246,275`、`cpe_wan.h` |
| 3.3 | B8 | 枚举过滤器不匹配时 `return false`，或改名 `config_selector_cb` 并改 help | `usb_wan.c:53-63`、`main/Kconfig.projbuild:3-39` |
| 3.4 | D7 | teardown 回退默认 netif（或文档化为"无默认路由"） | `usb_wan.c:268` vs `:138-189` |
| 3.5 | D8 | AT：句柄单一释放路径；复位信号量改世代计数；重试设上限并 `cpe_status_set_error` | `cpe_at.c:972-1086,1019-1038,1141-1144` |
| 3.6 | D9 | 缓冲区留余量 + 检查 `snprintf` 返回；`+CME ERROR` 长度无关解析 | `wan_net.c:61-64`、`cpe_at.c:84-86,674-677` |
| 3.7 | D10 | 按键等待松手加超时上限 | `reset_button.c:135-141` |
| 3.8 | D10 | `cpe_ota_begin` busy 检查改 test-and-set | `cpe_ota.c:96-134` |

---

## 批次 4：清理（低风险，每小项单独提交）

1. C1 校验逻辑合并为一处
2. C2 `cpe_status` setter 表驱动 + 补 deinit
3. C3 OTA 状态复位抽 `ota_state_reset()`
4. C4 随机串生成器合并；C5 `cpe_recovery.c` 复用 `write_u32()`
5. B5 错误码与 `close_socket` 策略收进统一表
6. B2/B3 日志级别归位（正常流程降 INFO）+ TAG 拆为 `cpe_wan`/`cpe_net`/`cpe_link`
7. C8 `json_util` 改批量 `memcpy` + 查表转义
8. C9 删死字段/死接口（`ecm_runs`、`reboot_runs`、`cpe_ota_running_pending_verify` 转 static、`station_registry_deinit`）
9. B4 `main.c` 致命错误统一 `cpe_fatal_idle(reason)`；补 4 处返回值检查
10. B1/B7 并发原语统一为互斥量；魔法数具名化；`app_wifi` 三 bool 改枚举 + 补 `stop()`

---

## 批次 5：恢复能力增强（实测驱动，**最后做**）

1. **link up 但一直拿不到 IP 时，增加「重启 4G 模块」这一级恢复手段**

   现场现象：链路已 up、DHCP 反复重启仍拿不到地址，此时**重启模块即可恢复**。现有
   `link_manager` 的 IP 恢复阶梯只到「重启 DHCP → 重建 ECM（有上界）」，之后退化为只重试
   DHCP，缺少这一级。

   设计要点（实现前逐条确认）：

   - 位置：作为**最后一级**——DHCP 重启次数用尽（`CONFIG_CPE_IP_DHCP_RESTARTS`）且 ECM
     重建预算用尽（`CONFIG_CPE_IP_MAX_ECM_REBUILDS`）之后才触发，不替代更轻的 ECM 重建。
   - 手段：复用已有的 `cpe_at_modem_reset()`（`AT+CFUN=1,1`，best effort）。AT 关闭时它返回
     `ESP_ERR_NOT_SUPPORTED`，此时跳过本手段、继续原有 DHCP 重试，不得视为失败。
   - 有界：新增 `CONFIG_CPE_IP_MODEM_RESETS`（每个 link-up 周期允许几次，默认建议 1~2，
     0 = 关闭），配套 `ctx->modem_resets` 计数；与 `dhcp_restarts`/`ecm_rebuilds` 同样在
     拿到 IP、进入 ONLINE、link 下降、计划内重建时归零。
   - 重启模块会掉 USB：必须走 `lm_teardown_ecm()` + 回 BOOT 相 + 冷却延时，等待窗口
     （`wait_start_us`）一并清零，不能在原地继续等 IP。
   - 冷却：两次模块重启之间要有最小间隔，避免比"拿不到 IP"更糟的反复重启。
   - 待实测确认：`AT+CFUN=1,1` 之后模块能否在同一 USB 枚举下回来；若必须重新枚举，应交给
     link manager 已有的"设备消失"路径处理，而不是在这里补特例。

   验证场景：拔天线/弱信号或人为让运营商侧不分配地址，观察是否走到模块重启并恢复上线；
   以及确认不会无限重启模块。

   **实现结果（已提交，待真机验证）**：`link_manager.c` 新增 `lm_reset_modem()`，挂在
   `lm_wait_ip()` 里「ECM 重建预算用尽」分支之前；`CONFIG_CPE_IP_MODEM_RESETS`（默认 2，
   范围 0–5，0 = 关闭）。与上面设计的差异：

   - **手段取两种**：先 `cpe_at_modem_reset()`（AT+CFUN=1,1）；若未确认成功（AT 关闭、
     端口忙、超时）则**回落到 `wan_usb_modem_reset()`**（模组复位 GPIO，没有则重启 USB
     根端口），与"无模组枚举"路径用同一个硬件手段。顺序上 AT 复位必须在 ECM 拆除**之前**
     （它走 USB），硬件复位必须在拆除**之后**（不让驱动在设备被拔走时还挂着）。
   - **计数归零点**只取三处：拿到 IP、进入 ONLINE、计划内重建。**不在 link 下降/上升沿
     归零**——与 `ecm_rebuilds` 同一理由：模块重启本身就会导致掉线，沿上归零会让预算永远
     用不完。
   - **每次模块重启后把 `ecm_rebuilds` 也清零**，让阶梯从头再走一遍（DHCP → ECM 重建 →
     下一次模块重启）。不清零的话第二次会跳过 ECM 重建直接再次重启模块，等于把两次预算
     连着烧掉。上界仍是 `CONFIG_CPE_IP_MODEM_RESETS`。
   - **冷却做成 `LM_MODEM_RESET_COOLDOWN_MS = 60000`**（`link_manager.c` 常量，未进 Kconfig）：
     阶梯本身每步都要等 `CONFIG_CPE_IP_TIMEOUT_MS`，只有把前面几项配成 0 时才会碰这道闸。

---

## 排期估算（不含编译与验证等待）

| 批次 | 工作量 | 依赖 |
|---|---|---|
| 0 | 用户侧验证 | — |
| 1 | 0.5–1 天 | 0 |
| 2 | 1–1.5 天 | 1（2.5 依赖 1.2 的 helper task 模式） |
| 3 | 0.5–1 天 | 0（建议排在 2 之后） |
| 4 | 0.5 天 | 2、3（避免冲突） |
| 5 | 0.5–1 天（含真机验证） | 4 全部完成，最后做 |

## 提交信息风格

沿用现有约定，例如：
- `重构：将 OTA 上传移出 httpd 任务，避免控制台被独占`
- `重构：新增 system_restart() 统一重启前的清理与宽限`
- `清理：合并三份重复的 AP 配置校验`
