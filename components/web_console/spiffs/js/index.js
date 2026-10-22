/*
 * System status / online client list page for the ESP-CPE management console.
 *
 * All values coming from the device (station host names, IP addresses, error
 * strings, ...) are rendered with textContent or value assignment only - never
 * with innerHTML - so a stored host name cannot inject markup (audit P1-16).
 */
'use strict';

(function () {
    var STATION_URL = '/system/station_state';
    var CHANGE_NAME_URL = '/system/station_state/change_name';
    var DELETE_DEVICE_URL = '/system/station_state/delete_device';
    /* Keep in sync with cpe_status.h (CPE_STATE_FAULT). */
    var STATE_FAULT = 4;

    var stationList = [];
    /* mac -> { row, nameButton, macCell, ipCell, timeCell }. Rows are reused
     * between polls so the list no longer flickers (and loses the reading
     * position) every refresh; a modal dialog means the table can keep
     * updating while a name is being renamed. */
    var stationRows = {};
    var stationTimer = null;
    var statusTimer = null;
    /* Uptime is reported once per status poll; count locally from the last
     * synced value instead of advancing a counter on every timer tick. */
    var uptimeBaseS = 0;
    var uptimeBaseMs = 0;
    var uptimeSynced = false;
    var uptimeTimer = null;
    /* Set when a status poll fails: the page can no longer reach the device, so
     * the local estimate would keep ticking and every other value on screen
     * would silently stay at its last reading. */
    var statusStale = false;
    var lastStatusAt = null;
    /* The API value is read before the response travels back, so a poll usually
     * lags the local estimate slightly. Only correct clearly conflicting
     * values; re-basing every poll makes the seconds repeat. */
    var UPTIME_RESYNC_THRESHOLD_S = 3;

    function setText(id, text) {
        var node = CPE.byId(id);
        if (node) {
            node.textContent = text;
        }
    }

    function pad2(value) {
        return value < 10 ? '0' + value : String(value);
    }

    function formatDuration(totalSeconds) {
        var seconds = Math.max(0, Math.floor(totalSeconds));
        var hours = Math.floor(seconds / 3600);
        var minutes = Math.floor((seconds % 3600) / 60);
        var rest = seconds % 60;
        if (hours > 0) {
            return hours + '小时' + minutes + '分钟' + rest + '秒';
        }
        if (minutes > 0) {
            return minutes + '分钟' + rest + '秒';
        }
        return rest + '秒';
    }

    function formatClock(date) {
        return pad2(date.getHours()) + ':' + pad2(date.getMinutes()) + ':' +
            pad2(date.getSeconds());
    }

    function stationField(station, key) {
        return typeof station[key] === 'string' ? station[key] : '';
    }

    function findStation(mac) {
        for (var i = 0; i < stationList.length; i++) {
            if (stationField(stationList[i], 'mac_str') === mac) {
                return stationList[i];
            }
        }
        return null;
    }

    function createCell(text, label, extraClass) {
        var cell = document.createElement('td');
        cell.className = extraClass ? 'client-cell ' + extraClass : 'client-cell';
        /* The label is read by CSS (td::before) when the table collapses into
         * cards on a phone; setting it here keeps the markup declarative. */
        cell.setAttribute('data-label', label);
        cell.textContent = text;
        return cell;
    }

    /* Update an existing row in place: recreating the table on every poll made
     * the list visibly shake while scrolling on a phone. */
    function updateStationRow(entry, station) {
        var name = stationField(station, 'name_str') || '未知设备';
        if (entry.nameButton.textContent !== name) {
            entry.nameButton.textContent = name;
        }
        entry.nameButton.title = name;

        var mac = stationField(station, 'mac_str');
        if (entry.macCell.textContent !== mac) {
            entry.macCell.textContent = mac;
        }
        var ip = stationField(station, 'ip_str');
        if (entry.ipCell.textContent !== ip) {
            entry.ipCell.textContent = ip;
        }
        var online = formatDuration(Number(station.online_time_s) || 0);
        if (entry.timeCell.textContent !== online) {
            entry.timeCell.textContent = online;
        }
    }

    function createStationRow(station) {
        var mac = stationField(station, 'mac_str');
        var row = document.createElement('tr');

        var nameCell = createCell('', '主机名', 'client-cell-name');
        var nameButton = document.createElement('button');
        nameButton.type = 'button';
        nameButton.className = 'client-name-btn';
        nameButton.addEventListener('click', function () {
            openRenameDialog(mac);
        });
        nameCell.appendChild(nameButton);

        var macCell = createCell(mac, 'MAC 地址');
        var ipCell = createCell('', 'IP 地址');
        var timeCell = createCell('', '连接时间');

        var actionCell = createCell('', '操作', 'client-cell-actions');
        var actionWrap = document.createElement('div');
        actionWrap.className = 'client-actions';

        var editButton = document.createElement('button');
        editButton.type = 'button';
        editButton.className = 'edit-btn';
        editButton.textContent = '重命名';
        editButton.addEventListener('click', function () {
            openRenameDialog(mac);
        });

        var deleteButton = document.createElement('button');
        deleteButton.type = 'button';
        deleteButton.className = 'del-btn';
        deleteButton.textContent = '断开';
        deleteButton.addEventListener('click', function () {
            if (window.confirm('确定要断开并删除该客户端吗？')) {
                deleteStation(mac);
            }
        });

        actionWrap.appendChild(editButton);
        var divider = document.createElement('div');
        divider.className = 'edit-line';
        actionWrap.appendChild(divider);
        actionWrap.appendChild(deleteButton);
        actionCell.appendChild(actionWrap);

        row.appendChild(nameCell);
        row.appendChild(macCell);
        row.appendChild(ipCell);
        row.appendChild(timeCell);
        row.appendChild(actionCell);

        var entry = {
            row: row,
            nameButton: nameButton,
            macCell: macCell,
            ipCell: ipCell,
            timeCell: timeCell
        };
        updateStationRow(entry, station);
        return entry;
    }

    function updateStationCounts(data) {
        var list = Array.isArray(data.station_list) ? data.station_list : [];
        var maxSta = Number(data.max_sta) || 10;
        setText('connect-count', list.length + '台');
        setText('remain-count', Math.max(0, maxSta - list.length) + '台');
        setText('client-count-top', list.length + '台');
        return list;
    }

    function findRow(body, className) {
        for (var i = 0; i < body.children.length; i++) {
            if (body.children[i].className === className) {
                return body.children[i];
            }
        }
        return null;
    }

    /* Rows are only re-appended when the order really changed: moving nodes on
     * every poll drops the focus and the scroll anchor on a phone. */
    function syncRows(body, order) {
        var current = [];
        for (var i = 0; i < body.children.length; i++) {
            var child = body.children[i];
            if (child.className !== 'client-empty-row') {
                current.push(child);
            }
        }
        var changed = current.length !== order.length;
        for (var j = 0; !changed && j < order.length; j++) {
            if (current[j] !== order[j]) {
                changed = true;
            }
        }
        if (!changed) {
            return;
        }
        order.forEach(function (row) {
            body.appendChild(row);
        });
    }

    function updateEmptyState(body, isEmpty) {
        var existing = findRow(body, 'client-empty-row');
        if (!isEmpty) {
            if (existing) {
                body.removeChild(existing);
            }
            return;
        }
        if (existing) {
            return;
        }
        var row = document.createElement('tr');
        row.className = 'client-empty-row';
        var cell = document.createElement('td');
        cell.colSpan = 5;
        cell.textContent = '当前没有在线客户端';
        row.appendChild(cell);
        body.appendChild(row);
    }

    function renderStationList(data) {
        var body = CPE.byId('tbody');
        if (!body) {
            return;
        }
        stationList = updateStationCounts(data);

        var seen = {};
        var order = [];

        stationList.forEach(function (station, index) {
            var mac = stationField(station, 'mac_str');
            /* Index is only a fallback: a station without a MAC must not make
             * two rows share one cache entry. */
            var key = mac || ('idx' + index);
            var entry = stationRows[key];
            if (entry) {
                updateStationRow(entry, station);
            } else {
                entry = createStationRow(station);
                stationRows[key] = entry;
            }
            seen[key] = true;
            order.push(entry.row);
        });

        Object.keys(stationRows).forEach(function (key) {
            if (!seen[key]) {
                var stale = stationRows[key];
                if (stale.row.parentNode) {
                    stale.row.parentNode.removeChild(stale.row);
                }
                delete stationRows[key];
            }
        });

        syncRows(body, order);
        updateEmptyState(body, order.length === 0);
    }

    /* Fetch the counts once at page load so the network tab shows meaningful
     * client numbers without starting the (hidden-tab) polling. */
    function loadStationCounts() {
        CPE.getJson(STATION_URL).then(function (data) {
            updateStationCounts(data);
        }).catch(function () {
            /* 401 already redirected to the login page. */
        });
    }

    function refreshStations() {
        if (stationTimer) {
            clearTimeout(stationTimer);
            stationTimer = null;
        }
        CPE.getJson(STATION_URL).then(function (data) {
            renderStationList(data);
        }).catch(function () {
            /* 401 already redirected to the login page. */
        }).then(function () {
            stationTimer = setTimeout(refreshStations, 4000);
        });
    }

    /* Rename dialog. A modal works on every viewport, keeps both buttons within
     * reach of the thumb on a phone (the inline editor lived in the last table
     * column, which is off-screen there) and leaves the polling untouched. */
    function openRenameDialog(mac) {
        var station = findStation(mac);
        if (!station) {
            window.alert('该客户端已离线，请稍后重试');
            refreshStations();
            return;
        }

        var overlay = CPE.createElement('div', { className: 'modal-overlay' });
        var panel = CPE.createElement('div', { className: 'modal-panel' });
        var title = CPE.createElement('div', { className: 'modal-title', text: '重命名客户端' });

        var hintText = 'MAC ' + mac;
        if (stationField(station, 'ip_str')) {
            hintText += ' · IP ' + stationField(station, 'ip_str');
        }
        var hint = CPE.createElement('div', {
            className: 'modal-hint',
            text: hintText + '（主机名 1~32 个字符）'
        });

        var input = CPE.createElement('input', {
            type: 'text',
            className: 'modal-input',
            maxlength: '32',
            autocomplete: 'off',
            enterkeyhint: 'done'
        });
        /* value assignment (not innerHTML) keeps host names inert. */
        input.value = stationField(station, 'name_str');

        var errorBox = CPE.createElement('div', { className: 'modal-error' });
        var actions = CPE.createElement('div', { className: 'modal-actions' });
        var submit = CPE.createElement('button', { className: 'save-btn', type: 'button', text: '保存' });
        var cancel = CPE.createElement('button', { className: 'cancel-btn', type: 'button', text: '取消' });

        function close() {
            if (overlay.parentNode) {
                overlay.parentNode.removeChild(overlay);
            }
        }

        function save() {
            var name = input.value;
            if (name.length === 0 || name.length > 32) {
                errorBox.textContent = '主机名长度必须为 1~32 个字符';
                input.focus();
                return;
            }
            errorBox.textContent = '';
            submit.disabled = true;
            cancel.disabled = true;
            CPE.postJson(CHANGE_NAME_URL, {
                name_str: name,
                mac_str: mac
            }).then(function () {
                close();
                refreshStations();
            }).catch(function (error) {
                submit.disabled = false;
                cancel.disabled = false;
                errorBox.textContent = CPE.errorText(error);
            });
        }

        submit.addEventListener('click', save);
        cancel.addEventListener('click', close);
        input.addEventListener('keydown', function (event) {
            if (event.key === 'Enter') {
                save();
            }
        });
        overlay.addEventListener('click', function (event) {
            if (event.target === overlay) {
                close();
            }
        });

        actions.appendChild(cancel);
        actions.appendChild(submit);
        panel.appendChild(title);
        panel.appendChild(hint);
        panel.appendChild(input);
        panel.appendChild(errorBox);
        panel.appendChild(actions);
        overlay.appendChild(panel);
        document.body.appendChild(overlay);
        input.focus();
        input.select();
    }

    function deleteStation(mac) {
        CPE.postJson(DELETE_DEVICE_URL, {
            mac_str: mac
        }).then(function () {
            refreshStations();
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
            refreshStations();
        });
    }

    /* Map the modem's signal reading to the five-bar icon. RSRP is the LTE
     * quality metric, so prefer it; RSSI is only a fallback for modems that
     * report CSQ without QCSQ. */
    function signalLevel(status) {
        if (status.qcsq_valid && typeof status.rsrp === 'number') {
            if (status.rsrp >= -85) {
                return 5;
            }
            if (status.rsrp >= -95) {
                return 4;
            }
            if (status.rsrp >= -105) {
                return 3;
            }
            if (status.rsrp >= -115) {
                return 2;
            }
            return 1;
        }
        if (status.rssi_valid && typeof status.rssi === 'number') {
            if (status.rssi >= -65) {
                return 5;
            }
            if (status.rssi >= -75) {
                return 4;
            }
            if (status.rssi >= -85) {
                return 3;
            }
            if (status.rssi >= -95) {
                return 2;
            }
            return 1;
        }
        return 0;
    }

    var SIGNAL_LABELS = ['信号强度未知', '信号很弱', '信号较弱', '信号一般', '信号良好', '信号很强'];

    function renderSignalIcon(status) {
        var icon = CPE.byId('signal-icon');
        if (!icon) {
            return;
        }
        var level = signalLevel(status);
        /* SVG elements expose className as a read-only SVGAnimatedString. */
        icon.setAttribute('class', level === 0 ? 'signal-icon' : 'signal-icon level-' + level);
        icon.setAttribute('aria-label', SIGNAL_LABELS[level]);
    }

    function renderUptime() {
        if (!uptimeSynced || statusStale) {
            return;
        }
        var elapsedMs = performance.now() - uptimeBaseMs;
        setText('connect-time', formatDuration(uptimeBaseS + elapsedMs / 1000));
        /* Re-arm against the next whole second instead of using a fixed 1 s
         * interval: interval drift can skip or repeat a second, which reads as
         * the clock stuttering. */
        if (uptimeTimer) {
            clearTimeout(uptimeTimer);
        }
        uptimeTimer = setTimeout(renderUptime, 1000 - elapsedMs % 1000 + 15);
    }

    function syncUptime(status) {
        var serverS = Number(status.uptime_s) || 0;
        var now = performance.now();
        var localS = uptimeSynced
            ? uptimeBaseS + (now - uptimeBaseMs) / 1000
            : serverS;
        if (!uptimeSynced || Math.abs(serverS - localS) >= UPTIME_RESYNC_THRESHOLD_S) {
            uptimeBaseS = serverS;
            uptimeBaseMs = now;
            uptimeSynced = true;
        }
        renderUptime();
    }

    function renderStatus(status) {
        renderSignalIcon(status);
        syncUptime(status);
        setText('status-state', status.state_text || '未知');
        setText('status-usb', status.usb_connected ? '已连接' : '未连接');
        setText('status-link', status.link_up ? '已连接' : '未连接');
        setText('status-ip', (status.got_ip && status.ip) ? status.ip : '未获取');
        setText('status-rssi', status.rssi_valid ? (status.rssi + ' dBm') : '未知');
        setText('status-rsrp', status.qcsq_valid ? (status.rsrp + ' dBm') : '未知');
        setText('status-rsrq', status.qcsq_valid ? (status.rsrq + ' dB') : '未知');
        setText('status-sinr', status.qcsq_valid ? (status.sinr + ' dB') : '未知');
        setText('status-carrier', status.carrier || '未知');
        setText('status-registered', status.sim_valid
            ? (status.modem_registered ? '已注册' : '未注册')
            : '未知');
        setText('status-ps', status.ps_valid
            ? (status.ps_attached ? '已附着' : '未附着')
            : '未知');
        setText('status-pdp', status.pdp_valid
            ? (status.pdp_active ? '已激活' : '未激活')
            : '未知');
        setText('status-temp', status.modem_temp_valid
            ? (status.modem_temp + ' °C')
            : '未知');
        setText('status-modem-model', status.modem_model || '未知');
        setText('status-modem-fw', status.modem_fw || '未知');
        setText('status-imei', status.imei || '未知');
        setText('status-sim', status.sim_valid
            ? (status.sim_inserted ? '已安装' : '未安装')
            : '未知');
        /* Only meaningful when the modem reported a card as present. */
        setText('status-phone', (status.sim_valid && !status.sim_inserted)
            ? '--'
            : (status.phone_number || '未知'));
        setText('status-recovery', String(status.recovery_attempts || 0));
        setText('status-boot', String(status.boot_count || 0));
        setText('status-device-name', status.device_name || '-');
        setText('status-last-error', status.last_error || '无');

        /* AT telemetry tells whether the WAN can actually work: got_ip alone
         * is misleading, the modem's DHCP server may answer without a SIM. */
        var simKnown = status.sim_valid === true;
        var internetState;
        if (simKnown && !status.sim_inserted) {
            internetState = '未安装 SIM 卡';
        } else if (simKnown && !status.modem_registered) {
            internetState = '未注册移动网络';
        } else if (status.got_ip) {
            internetState = '移动数据已连接';
        } else if (status.link_up || status.usb_connected) {
            internetState = '正在获取网络…';
        } else {
            internetState = '未检测到 4G 模块';
        }
        setText('internet-state', internetState);

        var apSsid = status.ap && typeof status.ap.ssid === 'string' ? status.ap.ssid : '';
        setText('wifi-state', apSsid ? ('2.4 GHz Wi-Fi 正常（' + apSsid + '）') : '2.4 GHz Wi-Fi 正常');

        var banner = CPE.byId('status-banner');
        if (banner) {
            if (status.state === STATE_FAULT) {
                banner.textContent = '当前处于故障状态：4G 链路未就绪，系统会自动重试。' +
                    '您仍可以通过本页面修改 WLAN 配置或恢复出厂设置。';
                banner.className = 'status-banner status-banner-warn';
            } else if (simKnown && !status.sim_inserted) {
                banner.textContent = '未检测到 SIM 卡，4G 网络不可用。插入 SIM 卡后系统会自动恢复。';
                banner.className = 'status-banner status-banner-warn';
            } else if (simKnown && !status.modem_registered) {
                banner.textContent = 'SIM 卡已安装，但尚未注册到移动网络，正在等待网络注册。';
                banner.className = 'status-banner status-banner-info';
            } else if (!status.link_up) {
                banner.textContent = '正在等待 4G 网络就绪，本页面在链路恢复前仍可正常使用。';
                banner.className = 'status-banner status-banner-info';
            } else {
                banner.textContent = '';
                banner.className = 'status-banner';
            }
        }
    }

    /* The last successful reading is kept on screen, but it must be labelled:
     * otherwise a frozen page looks live (and the local uptime keeps counting
     * even though the device was never contacted again). */
    function showStaleStatus() {
        var banner = CPE.byId('status-banner');
        if (!banner) {
            return;
        }
        var lastSeen = lastStatusAt
            ? '，当前显示的是 ' + formatClock(lastStatusAt) + ' 的数据'
            : '';
        banner.textContent = '与设备失去连接，正在重试' + lastSeen + '；恢复后会自动更新。';
        banner.className = 'status-banner status-banner-warn';
    }

    function refreshStatus() {
        if (statusTimer) {
            clearTimeout(statusTimer);
            statusTimer = null;
        }
        CPE.getStatus().then(function (status) {
            statusStale = false;
            lastStatusAt = new Date();
            renderStatus(status);
        }).catch(function () {
            /* 401 already redirected to the login page. */
            if (statusStale) {
                return;
            }
            statusStale = true;
            if (uptimeTimer) {
                clearTimeout(uptimeTimer);
                uptimeTimer = null;
            }
            showStaleStatus();
        }).then(function () {
            statusTimer = setTimeout(refreshStatus, 5000);
        });
    }

    function switchTab(showNetwork) {
        if (!CPE.showTab(showNetwork, 'networkShow', 'customerShow',
                         'tab-network', 'tab-clients')) {
            return;
        }
        if (showNetwork) {
            /* Leaving the client tab stops polling. */
            if (stationTimer) {
                clearTimeout(stationTimer);
                stationTimer = null;
            }
        } else {
            refreshStations();
        }
    }

    function bindActions() {
        var changePassword = CPE.byId('btn-change-password');
        if (changePassword) {
            changePassword.addEventListener('click', function () {
                CPE.openPasswordModal(false).catch(function () {});
            });
        }
        var factoryReset = CPE.byId('btn-factory-reset');
        if (factoryReset) {
            factoryReset.addEventListener('click', function () {
                CPE.factoryReset().catch(function () {});
            });
        }
    }

    function init() {
        /* Without a CSRF token this browser has no session to probe; go to the
         * login page directly instead of triggering a 401 on /api/status. */
        if (!CPE.getCsrf()) {
            window.location.href = '/login.html';
            return;
        }
        CPE.bindMenu();
        bindActions();
        /* Background tabs throttle timers; catch up as soon as the page is
         * visible again instead of leaving a stale time on screen. */
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) {
                renderUptime();
            }
        });
        var networkTab = CPE.byId('tab-network');
        var clientTab = CPE.byId('tab-clients');
        if (networkTab) {
            networkTab.addEventListener('click', function () {
                switchTab(true);
            });
        }
        if (clientTab) {
            clientTab.addEventListener('click', function () {
                switchTab(false);
            });
        }

        CPE.bootstrap().then(function (status) {
            renderStatus(status);
            switchTab(true);
            loadStationCounts();
            refreshStatus();
        }).catch(function () {
            /* Forced password change already redirected to the login page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
