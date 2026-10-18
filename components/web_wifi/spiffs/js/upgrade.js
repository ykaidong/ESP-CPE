/*
 * Firmware upgrade page for the ESP-CPE management console.
 *
 * The upload is a raw binary body (not JSON), so it bypasses CPE.postJson():
 * a plain XMLHttpRequest is used to get upload progress events. Session and
 * CSRF handling stays the same as the rest of the console (api.js).
 */
'use strict';

(function () {
    var MAX_OTA_SIZE = 3 * 1024 * 1024; /* keep in sync with the 3MB OTA partitions */
    var uploading = false;

    function byId(id) {
        return document.getElementById(id);
    }

    function setStatus(message, isError) {
        var node = byId('upgrade-status');
        if (!node) {
            return;
        }
        node.textContent = message || '';
        node.className = isError ? 'upgrade-status error' : 'upgrade-status';
    }

    function formatBytes(value) {
        if (!value && value !== 0) {
            return '--';
        }
        if (value >= 1024 * 1024) {
            return (value / (1024 * 1024)).toFixed(1) + ' MB（' + value + ' 字节）';
        }
        return value + ' 字节';
    }

    function bindMenu() {
        var menuButton = byId('menuBtn');
        var menu = byId('leftMenu');
        if (!menuButton || !menu) {
            return;
        }
        menuButton.addEventListener('click', function () {
            var hidden = menu.style.display === 'none';
            menu.style.display = hidden ? 'block' : 'none';
        });
    }

    function refreshInfo() {
        CPE.getJson('/api/ota/status').then(function (info) {
            byId('ota-version').textContent = info.version || '--';
            byId('ota-partition').textContent = info.partition || '--';
            byId('ota-next').textContent = info.next_partition ? info.next_partition : '（保持当前分区）';
            byId('ota-pending').textContent = info.pending_verify ? '待验证（约 1 分钟内自动完成）' : '已确认';
            byId('ota-max').textContent = formatBytes(info.max_image_size);
        }).catch(function (error) {
            if (error && error.message === 'unauthorized') {
                return;
            }
            setStatus('读取固件信息失败：' + CPE.errorText(error), true);
        });
    }

    function setBusy(busy) {
        uploading = busy;
        byId('btn-upload').disabled = busy;
        byId('ota-file').disabled = busy;
        byId('btn-reboot').disabled = busy;
    }

    function failUpload(code) {
        setBusy(false);
        byId('progress-wrap').hidden = true;
        var error = new Error(code || 'upload_failed');
        error.code = code || null;
        setStatus('升级失败：' + CPE.errorText(error), true);
    }

    function upload() {
        if (uploading) {
            return;
        }
        var input = byId('ota-file');
        if (!input || !input.files || input.files.length === 0) {
            setStatus('请先选择固件文件（.bin）', true);
            return;
        }
        var file = input.files[0];
        if (file.size <= 0) {
            setStatus('所选文件为空', true);
            return;
        }
        if (file.size > MAX_OTA_SIZE) {
            setStatus('所选文件超过 OTA 分区容量（' + formatBytes(MAX_OTA_SIZE) + '）', true);
            return;
        }
        if (!window.confirm('确定要升级到所选固件吗？\n\n升级过程中请勿断电，设备会在完成后自动重启。')) {
            return;
        }

        var token = CPE.getCsrf();
        if (!token) {
            window.location.href = '/login.html';
            return;
        }

        setBusy(true);
        setStatus('正在上传…', false);
        byId('progress-wrap').hidden = false;
        byId('progress-bar').style.width = '0%';
        byId('progress-text').textContent = '0%';

        var xhr = new XMLHttpRequest();
        xhr.open('POST', '/api/ota/upload', true);
        xhr.setRequestHeader('Accept', 'application/json');
        xhr.setRequestHeader('Content-Type', 'application/octet-stream');
        xhr.setRequestHeader('X-CSRF-Token', token);

        xhr.upload.onprogress = function (event) {
            if (!event.lengthComputable) {
                return;
            }
            var percent = Math.round((event.loaded * 100) / event.total);
            byId('progress-bar').style.width = percent + '%';
            byId('progress-text').textContent = percent + '%（' +
                formatBytes(event.loaded) + ' / ' + formatBytes(event.total) + '）';
        };

        xhr.onload = function () {
            var data = {};
            if (xhr.responseText) {
                try {
                    data = JSON.parse(xhr.responseText);
                } catch (e) {
                    data = {};
                }
            }
            if (xhr.status >= 200 && xhr.status < 300) {
                setBusy(false);
                byId('progress-bar').style.width = '100%';
                byId('progress-text').textContent = '100%';
                setStatus('升级包已写入并通过校验，设备正在重启… 请等待约 1 分钟，重新连接 Wi-Fi 后刷新页面。', false);
                return;
            }
            if (xhr.status === 401) {
                CPE.clearCsrf();
                window.location.href = '/login.html';
                return;
            }
            failUpload(data.error || ('http_' + xhr.status));
        };
        xhr.onerror = function () {
            /* A reset right after a successful upload is expected (the device
             * reboots). Report it as such instead of a hard failure. */
            failUpload('upload_interrupted');
        };
        xhr.send(file);
    }

    function reboot() {
        if (uploading) {
            return;
        }
        if (!window.confirm('确定要重启设备吗？重启期间 Wi-Fi 会断开约 20 秒。')) {
            return;
        }
        CPE.postJson('/api/system/reboot', {}).then(function () {
            setStatus('设备正在重启，请等待约 20 秒后刷新页面。', false);
        }).catch(function (error) {
            if (error && error.message === 'network_error') {
                setStatus('设备正在重启，请等待约 20 秒后刷新页面。', false);
                return;
            }
            setStatus('重启失败：' + CPE.errorText(error), true);
        });
    }

    function init() {
        bindMenu();
        var uploadButton = byId('btn-upload');
        if (uploadButton) {
            uploadButton.addEventListener('click', upload);
        }
        var rebootButton = byId('btn-reboot');
        if (rebootButton) {
            rebootButton.addEventListener('click', reboot);
        }
        var backButton = byId('btn-back');
        if (backButton) {
            backButton.addEventListener('click', function () {
                window.location.href = '/index.html';
            });
        }
        refreshInfo();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
}());
