/*
 * WLAN settings page for the ESP-CPE management console.
 *
 * The SoftAP password is never sent back to the browser: an empty password
 * field means "keep the current password". Values coming from the device are
 * written with .value / .checked / textContent only (no innerHTML).
 */
'use strict';

(function () {
    var GENERAL_URL = '/wlan_general';
    var ADVANCE_URL = '/wlan_advance';

    /* Saving may restart the SoftAP and drop the connection before the reply is
     * read; waiting less keeps the dead socket from occupying the browser
     * connection pool. */
    var SAVE_TIMEOUT_MS = 4000;

    var currentMaxSta = 8;

    function showMessage(text, isError) {
        var node = CPE.byId('wlan-msg');
        if (!node) {
            return;
        }
        node.textContent = text;
        node.className = isError ? 'wlan-msg wlan-msg-error' : 'wlan-msg wlan-msg-ok';
    }

    function clearMessage() {
        var node = CPE.byId('wlan-msg');
        if (node) {
            node.textContent = '';
            node.className = 'wlan-msg';
        }
    }

    function loadGeneral() {
        return CPE.getJson(GENERAL_URL).then(function (data) {
            var ssid = CPE.byId('wlanName');
            var password = CPE.byId('password');
            var hidden = CPE.byId('select');
            var deviceName = CPE.byId('deviceName');

            if (ssid) {
                ssid.value = typeof data.ssid === 'string' ? data.ssid : '';
            }
            if (password) {
                /* Never echo the stored password. */
                password.value = '';
                password.placeholder = data.has_password
                    ? '已设置，留空表示不修改'
                    : '请输入 8~63 位密码';
            }
            if (hidden) {
                hidden.checked = data.if_hide_ssid === 'true' || data.if_hide_ssid === true;
            }
            if (deviceName) {
                deviceName.value = typeof data.device_name === 'string' ? data.device_name : '';
            }
            currentMaxSta = Number(data.max_sta) || 8;
        });
    }

    function loadAdvance() {
        return CPE.getJson(ADVANCE_URL).then(function (data) {
            var bandwidth = CPE.byId('broadband');
            var channel = CPE.byId('channel');
            if (bandwidth) {
                bandwidth.value = String(data.bandwidth);
            }
            if (channel) {
                channel.value = String(data.channel);
            }
        });
    }

    function saveBasic() {
        clearMessage();
        var ssidInput = CPE.byId('wlanName');
        var passwordInput = CPE.byId('password');
        var hiddenInput = CPE.byId('select');
        var deviceNameInput = CPE.byId('deviceName');
        if (!ssidInput || !passwordInput) {
            return;
        }

        var ssid = ssidInput.value.trim();
        if (ssid.length < 1 || ssid.length > 32) {
            showMessage('WLAN 名称长度必须为 1~32 个字符', true);
            return;
        }
        var password = passwordInput.value;
        if (password.length > 0 && (password.length < 8 || password.length > 63)) {
            showMessage('密码长度必须为 8~63 个字符，留空表示不修改', true);
            return;
        }
        var deviceName = deviceNameInput ? deviceNameInput.value.trim() : '';
        if (deviceName.length > 32) {
            showMessage('设备名称长度不能超过 32 个字符', true);
            return;
        }

        var body = {
            ssid: ssid,
            password: password,
            if_hide_ssid: !!(hiddenInput && hiddenInput.checked),
            max_sta: currentMaxSta
        };
        if (deviceName.length > 0) {
            body.device_name = deviceName;
        }

        CPE.postJson(GENERAL_URL, body, SAVE_TIMEOUT_MS).then(function () {
            passwordInput.value = '';
            showMessage('基本设置已保存。若修改了 WLAN 名称或密码，请使用新参数重新连接 Wi-Fi。', false);
        }).catch(function (error) {
            if (error.message === 'network_error') {
                /* Changing SSID/password restarts the SoftAP and drops this
                 * HTTP connection before the response can be read. */
                passwordInput.value = '';
                showMessage('设置已提交，设备无线可能正在重启；请使用新的 WLAN 参数重新连接后刷新页面确认。', false);
            } else {
                showMessage(CPE.errorText(error), true);
            }
        });
    }

    function saveAdvance() {
        clearMessage();
        var bandwidthInput = CPE.byId('broadband');
        var channelInput = CPE.byId('channel');
        if (!bandwidthInput || !channelInput) {
            return;
        }
        var bandwidth = Number(bandwidthInput.value);
        var channel = Number(channelInput.value);
        if (bandwidth !== 20 && bandwidth !== 40) {
            showMessage('带宽必须为 20 或 40 MHz', true);
            return;
        }
        if (!(channel >= 1 && channel <= 13)) {
            showMessage('信道必须为 1~13', true);
            return;
        }

        CPE.postJson(ADVANCE_URL, {
            bandwidth: bandwidth,
            channel: channel
        }, SAVE_TIMEOUT_MS).then(function () {
            showMessage('高级设置已保存。信道/带宽变更可能需要重新连接 Wi-Fi。', false);
        }).catch(function (error) {
            if (error.message === 'network_error') {
                showMessage('设置已提交，设备无线可能正在切换信道；请重新连接 Wi-Fi 后刷新页面确认。', false);
            } else {
                showMessage(CPE.errorText(error), true);
            }
        });
    }

    function switchTab(showBasic) {
        CPE.showTab(showBasic, 'baseShow', 'highShow', 'tab-basic', 'tab-advance');
    }

    function init() {
        CPE.bindMenu();
        var basicTab = CPE.byId('tab-basic');
        var advanceTab = CPE.byId('tab-advance');
        var saveBasicButton = CPE.byId('btn-save-basic');
        var saveAdvanceButton = CPE.byId('btn-save-advance');
        if (basicTab) {
            basicTab.addEventListener('click', function () {
                switchTab(true);
            });
        }
        if (advanceTab) {
            advanceTab.addEventListener('click', function () {
                switchTab(false);
            });
        }
        if (saveBasicButton) {
            saveBasicButton.addEventListener('click', saveBasic);
        }
        if (saveAdvanceButton) {
            saveAdvanceButton.addEventListener('click', saveAdvance);
        }

        CPE.bootstrap().then(function () {
            switchTab(true);
            return loadGeneral().then(loadAdvance);
        }).catch(function () {
            /* Forced password change already redirected to the login page. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
