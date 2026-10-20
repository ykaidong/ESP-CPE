/*
 * Firmware upgrade page for the ESP-CPE management console.
 *
 * The upload is a raw binary body (not JSON), so it bypasses CPE.postJson():
 * a plain XMLHttpRequest is used to get upload progress events. Session and
 * CSRF handling stays the same as the rest of the console (api.js).
 */
'use strict';

(function () {
    /* The limit comes from /api/ota/status (max_image_size), which is derived
     * from the partition table; 0 means "not known yet" and lets the server
     * decide. */
    var maxOtaSize = 0;
    var uploading = false;

    function setStatus(message, isError) {
        var node = CPE.byId('upgrade-status');
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

    function refreshInfo() {
        CPE.getJson('/api/ota/status').then(function (info) {
            CPE.byId('ota-version').textContent = info.version || '--';
            CPE.byId('ota-partition').textContent = info.partition || '--';
            CPE.byId('ota-next').textContent = info.next_partition ? info.next_partition : '（保持当前分区）';
            CPE.byId('ota-pending').textContent = info.pending_verify ? '待验证（约 1 分钟内自动完成）' : '已确认';
            maxOtaSize = Number(info.max_image_size) || 0;
            CPE.byId('ota-max').textContent = formatBytes(maxOtaSize);
        }).catch(function (error) {
            if (error && error.message === 'unauthorized') {
                return;
            }
            setStatus('读取固件信息失败：' + CPE.errorText(error), true);
        });
    }

    function setBusy(busy) {
        uploading = busy;
        CPE.byId('btn-upload').disabled = busy;
        CPE.byId('ota-file').disabled = busy;
        CPE.byId('btn-reboot').disabled = busy;
    }

    function failUpload(code) {
        setBusy(false);
        CPE.byId('progress-wrap').hidden = true;
        var error = new Error(code || 'upload_failed');
        error.code = code || null;
        setStatus('升级失败：' + CPE.errorText(error), true);
    }

    /* The firmware image must start with the ESP32 image magic byte (0xE9).
     * Checking it in the browser gives an immediate, precise error and avoids
     * uploading megabytes of a wrong file (the server checks it as well). */
    function fileMagicOk(file) {
        return new Promise(function (resolve) {
            try {
                if (!file.slice || !window.FileReader) {
                    resolve(true); /* let the server decide */
                    return;
                }
                var reader = new FileReader();
                reader.onload = function () {
                    var bytes = new Uint8Array(reader.result || []);
                    resolve(bytes.length > 0 && bytes[0] === 0xE9);
                };
                reader.onerror = function () {
                    resolve(true);
                };
                reader.readAsArrayBuffer(file.slice(0, 1));
            } catch (e) {
                resolve(true);
            }
        });
    }

    function upload() {
        if (uploading) {
            return;
        }
        var input = CPE.byId('ota-file');
        if (!input || !input.files || input.files.length === 0) {
            setStatus('请先选择固件文件（.bin）', true);
            return;
        }
        var file = input.files[0];
        if (file.size <= 0) {
            setStatus('所选文件为空', true);
            return;
        }
        if (maxOtaSize > 0 && file.size > maxOtaSize) {
            setStatus('所选文件超过 OTA 分区容量（' + formatBytes(maxOtaSize) + '）', true);
            return;
        }
        fileMagicOk(file).then(function (ok) {
            if (!ok) {
                setStatus('所选文件不是有效的 ESP32 固件（镜像头校验失败），请选择 usb_ecm_4g_module.bin', true);
                return;
            }
            if (!window.confirm('确定要升级到所选固件吗？\n\n升级过程中请勿断电，设备会在完成后自动重启。')) {
                return;
            }
            startUpload(file);
        });
    }

    function startUpload(file) {
        var token = CPE.getCsrf();
        if (!token) {
            window.location.href = '/login.html';
            return;
        }

        setBusy(true);
        setStatus('正在上传…', false);
        CPE.byId('progress-wrap').hidden = false;
        CPE.byId('progress-bar').style.width = '0%';
        CPE.byId('progress-text').textContent = '0%';

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
            CPE.byId('progress-bar').style.width = percent + '%';
            CPE.byId('progress-text').textContent = percent + '%（' +
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
                CPE.byId('progress-bar').style.width = '100%';
                CPE.byId('progress-text').textContent = '100%';
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
            /* A reset can also happen when the device reboots right after a
             * successful upload; the onload path usually wins, but if the reset
             * arrives first the upload was likely accepted. */
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
        CPE.bindMenu();
        var uploadButton = CPE.byId('btn-upload');
        if (uploadButton) {
            uploadButton.addEventListener('click', upload);
        }
        var rebootButton = CPE.byId('btn-reboot');
        if (rebootButton) {
            rebootButton.addEventListener('click', reboot);
        }
        var backButton = CPE.byId('btn-back');
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
