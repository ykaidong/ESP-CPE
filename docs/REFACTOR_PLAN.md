# 重构整改计划（2026-10-22）

配套审计：`docs/CODE_AUDIT_2026-10-22.md`（问题编号 A1–A7、B1–B8、C1–C10、D1–D10）。

## 原则

1. **先验证基线**：DHCP 重复分配修复（`79931a2`）尚未上机验证，重构必须以"当前行为正确"为起点。
2. **行为保持优先**：除明确标注"行为变更"的项，不得改变时序、阈值与日志语义。
3. **不动 `iot_usbh_cdc` / `iot_usbh_ecm` fork**（若 D5 必须动，同步更新版本与 CHANGELOG）。
4. **一批一主题、一小项一提交**：便于 `git revert` 二分回退；编译与真机验证由用户完成。

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
| 批次 3（3.1–3.8） | 未开始 | — |
| 批次 4（清理 10 项） | 未开始 | — |

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

## 排期估算（不含编译与验证等待）

| 批次 | 工作量 | 依赖 |
|---|---|---|
| 0 | 用户侧验证 | — |
| 1 | 0.5–1 天 | 0 |
| 2 | 1–1.5 天 | 1（2.5 依赖 1.2 的 helper task 模式） |
| 3 | 0.5–1 天 | 0（建议排在 2 之后） |
| 4 | 0.5 天 | 2、3（避免冲突） |

## 提交信息风格

沿用现有约定，例如：
- `重构：将 OTA 上传移出 httpd 任务，避免控制台被独占`
- `重构：新增 system_restart() 统一重启前的清理与宽限`
- `清理：合并三份重复的 AP 配置校验`
