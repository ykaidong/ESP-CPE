/*
 * SMS forwarding panel of the SMS page. The form is filled once (and after a
 * save); the periodic refresh only updates the state line, so a poll never
 * overwrites text the user is editing. All device data is rendered with
 * textContent.
 */
'use strict';

(function () {
    var INFO_URL = '/api/sms/mqtt';
    var SAVE_URL = '/api/sms/mqtt';
    var CA_URL = '/api/sms/mqtt/ca';
    var CA_CLEAR_URL = '/api/sms/mqtt/ca/clear';
    var CA_MAX_BYTES = 3900;
    var STATUS_MS = 30000;

    var STATE_TEXT = {
        disabled: '未启用',
        idle: '等待 4G 网络',
        connecting: '正在连接…',
        connected: '已连接',
        error: '连接失败'
    };
    var MQTT_ERROR_TEXT = {
        tls_error: 'TLS 握手失败（检查服务器证书与系统时间）',
        socket_error: '无法创建网络连接（套接字资源不足，请稍后重试）',
        network_error: '无法连接服务器',
        connection_refused: '服务器拒绝连接（检查地址与端口）',
        subscribe_failed: '订阅下行主题失败',
        mqtt_init_failed: '无法创建 MQTT 客户端',
        mqtt_start_failed: '无法启动 MQTT 客户端',
        mqtt_error: 'MQTT 错误'
    };

    var timer = null;
    var supported = true;

    function byId(id) {
        return CPE.byId(id);
    }

    function fields() {
        return {
            enabled: byId('mqtt-enabled'),
            allowSend: byId('mqtt-allow-send'),
            host: byId('mqtt-host'),
            port: byId('mqtt-port'),
            user: byId('mqtt-user'),
            pass: byId('mqtt-pass'),
            topicUp: byId('mqtt-topic-up'),
            topicDown: byId('mqtt-topic-down'),
            caFile: byId('mqtt-ca-file'),
            caUpload: byId('mqtt-ca-upload'),
            caClear: byId('mqtt-ca-clear'),
            save: byId('mqtt-save')
        };
    }

    function setState(text, cls) {
        var el = byId('mqtt-state');
        if (!el) return;
        el.textContent = text;
        el.className = cls ? ('sms-mqtt-state ' + cls) : 'sms-mqtt-state';
    }

    function setNote(text) {
        var el = byId('mqtt-note');
        if (el) el.textContent = text || '';
    }

    function setEnabled(enabled) {
        var f = fields();
        Object.keys(f).forEach(function (key) {
            if (f[key]) f[key].disabled = !enabled;
        });
    }

    function renderCert(installed) {
        var el = byId('mqtt-ca-state');
        if (!el) return;
        el.textContent = installed
            ? '已安装自定义 CA 证书，仅用它校验服务器证书。'
            : '未安装自定义 CA，使用系统内置公共 CA。';
    }

    function renderRuntime(runtime) {
        if (!runtime) {
            return;
        }
        renderCert(runtime.cert_installed === true);
        var state = runtime.state || 'disabled';
        var text = '状态：' + (STATE_TEXT[state] || state);
        if (state === 'connected') {
            text += '（待确认 ' + (runtime.pending || 0) + ' 条）';
        }
        setState(text, state === 'error' ? 'error'
                        : (state === 'connected' ? 'ok' : ''));
        if (state === 'error' && runtime.last_error) {
            setNote('错误：' + (MQTT_ERROR_TEXT[runtime.last_error] || runtime.last_error));
        } else if (runtime.state !== 'disabled' && runtime.wan_online === false) {
            setNote('4G 网络未连接，恢复后会自动重连。');
        } else if (runtime.state !== 'disabled' && runtime.time_synced === false) {
            setNote('系统时间尚未校准，证书校验可能失败。');
        } else {
            setNote('');
        }
    }

    function fillForm(config) {
        var f = fields();
        if (!f.enabled || !config) return;
        f.enabled.checked = config.enabled === true;
        f.allowSend.checked = config.allow_remote_send === true;
        f.host.value = config.host || '';
        f.port.value = config.port || 8883;
        f.user.value = config.username || '';
        /* The stored password is never sent back; an empty field means
         * "keep the one on the device". */
        f.pass.value = '';
        f.topicUp.value = config.topic_up || '';
        f.topicDown.value = config.topic_down || '';
    }

    function applyData(data, fill) {
        if (!data) return;
        supported = data.supported !== false;
        if (!supported) {
            setEnabled(false);
            setState('当前固件未启用短信转发功能。');
            setNote('');
            return;
        }
        if (fill) {
            fillForm(data.config);
        }
        renderRuntime(data.runtime);
    }

    function load(fill) {
        return CPE.getJson(INFO_URL).then(function (data) {
            applyData(data, fill);
        });
    }

    function scheduleNext() {
        if (timer) clearTimeout(timer);
        timer = setTimeout(function () {
            if (!document.hidden) {
                load(false).catch(function () { /* keep the last view */ });
            }
            scheduleNext();
        }, document.hidden ? STATUS_MS * 4 : STATUS_MS);
    }

    function save() {
        var f = fields();
        if (!f.host) return;
        if (!f.host.value) {
            setNote('请填写服务器地址。');
            return;
        }
        if (Number(f.port.value) <= 0 || Number(f.port.value) > 65535) {
            setNote('端口必须为 1~65535。');
            return;
        }
        f.save.disabled = true;
        CPE.postJson(SAVE_URL, {
            enabled: f.enabled.checked,
            allow_remote_send: f.allowSend.checked,
            host: f.host.value,
            port: Number(f.port.value),
            username: f.user.value,
            password: f.pass.value,
            topic_up: f.topicUp.value,
            topic_down: f.topicDown.value
        }).then(function () {
            f.pass.value = '';
            setNote('转发设置已保存。');
            return load(true);
        }).catch(function (error) {
            setNote(CPE.errorText(error));
        }).then(function () {
            f.save.disabled = false;
        });
    }

    /* The control task picks the new certificate up on its next tick, so an
     * immediate refresh would still report the old state. */
    function reloadAfterSwitch() {
        return new Promise(function (resolve) {
            setTimeout(resolve, 2500);
        }).then(function () {
            return load(true);
        });
    }

    function sendCa(pem) {
        var f = fields();
        f.caUpload.disabled = true;
        CPE.postText(CA_URL, pem).then(function () {
            if (f.caFile) f.caFile.value = '';
            renderCert(true);
            setNote('CA 证书已上传，正在重新连接…');
            return reloadAfterSwitch();
        }).catch(function (error) {
            setNote(CPE.errorText(error));
        }).then(function () {
            f.caUpload.disabled = false;
        });
    }

    function uploadCa() {
        var f = fields();
        var file = f.caFile && f.caFile.files ? f.caFile.files[0] : null;
        if (!file) {
            setNote('请先选择 CA 证书文件。');
            return;
        }
        if (file.size > CA_MAX_BYTES) {
            setNote('CA 证书文件过大（最大约 4 KB）。');
            return;
        }
        var reader = new FileReader();
        reader.onload = function () {
            sendCa(String(reader.result));
        };
        reader.onerror = function () {
            setNote('无法读取所选文件。');
        };
        reader.readAsText(file);
    }

    function clearCa() {
        var f = fields();
        f.caClear.disabled = true;
        CPE.postJson(CA_CLEAR_URL, {}).then(function () {
            if (f.caFile) f.caFile.value = '';
            renderCert(false);
            setNote('已清除自定义 CA，正在重新连接…');
            return reloadAfterSwitch();
        }).catch(function (error) {
            setNote(CPE.errorText(error));
        }).then(function () {
            f.caClear.disabled = false;
        });
    }

    function init() {
        if (!byId('mqtt-save')) {
            return;
        }
        setEnabled(false);
        var saveBtn = byId('mqtt-save');
        if (saveBtn) saveBtn.addEventListener('click', save);
        var caUpload = byId('mqtt-ca-upload');
        if (caUpload) caUpload.addEventListener('click', uploadCa);
        var caClear = byId('mqtt-ca-clear');
        if (caClear) caClear.addEventListener('click', clearCa);

        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) {
                load(false).catch(function () { /* keep the last view */ });
            }
        });

        load(true).then(function () {
            if (supported) {
                setEnabled(true);
            }
            scheduleNext();
        }).catch(function (error) {
            setNote(CPE.errorText(error));
            scheduleNext();
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
