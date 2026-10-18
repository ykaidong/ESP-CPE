# Agent 工作规则（ESP-CPE 仓库）

本文件记录用户明确要求 AI 代理遵守的持久化规则。每次会话开始或恢复后，先读本文件。

## 1. ESP-IDF 编译验证规则（用户 2026-10-17 要求）

1. **每次需要做编译验证之前，必须先询问用户**：由用户手动构建验证，还是由 AI 代理构建验证。
   不得未经询问直接开始构建。
2. 若由 **AI 代理构建**：不能直接执行 `idf.py build` —— 必须先启动 ESP-IDF 的 PowerShell 环境。
   用户提供的启动命令为：

   ```powershell
   powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' }"
   ```

   该命令来自 Windows Terminal 中 ESP-IDF 的终端配置文件（`C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` 是 EIM 安装的激活脚本）。
   在此环境就绪后再执行 `idf.py build`（或等价 cmake/ninja 流程）。
3. 若由 **用户手动构建**：AI 代理只提供命令与预期结果，等待用户回传 `build_log.txt` 错误段后再修改代码。
4. 构建产物：`build/`、`sdkconfig`、`build_log.txt` 均为本地生成物，已被 `.gitignore` 忽略（`build_log.txt` 视需要清理）。
5. 本环境实测（2026-10-17）：DSH 沙箱内 `idf.py` 因 asyncio 命名管道被拒（WinError 5），代理构建需要对该条构建命令一次性授予 `danger-full-access`；首次构建通过，产物 `build/usb_ecm_4g_module.bin`。

## 2. 其他既有约定

- 设备/模组：ESP32-S3 + 16MB Flash + 通用 USB ECM 模组（无 AT 依赖）。
- 阶段推进：每阶段 `idf.py build` 通过（用户手动或代理执行，取决于每次询问结果）后再进入下一阶段；当前进度见 `docs/IMPLEMENTATION_PLAN.md`。
- 不提交 `sdkconfig`/`sdkconfig.old`；关键配置写入 `sdkconfig.defaults*`。
- 本地 fork：`components/iot_usbh_cdc`（3.1.1）、`components/iot_usbh_ecm`（0.4.1），修改需更新 CHANGELOG 与版本。
