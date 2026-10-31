/*
 * SMS page. Device data is rendered with textContent only, never innerHTML.
 * Polling is two-tier while the page is visible: a cheap notification endpoint
 * (no AT session) carries a generation counter, and only a changed counter
 * triggers the full listing, which runs CMGL over the modem. A full listing
 * also runs on a slow timer, because a +CMTI indication can be lost (URC queue
 * overflow, modem reset) and the counter would then never move again. Rows are
 * reused between listings so the table no longer flickers, and an in-flight
 * guard stops a slow CMGL from overlapping with the next poll (the AT port
 * rejects concurrent calls).
 */
'use strict';

(function () {
    var NOTIFY_URL = '/api/sms/notify';
    var LIST_URL = '/api/sms/list';
    var SEND_URL = '/api/sms/send';
    var DELETE_URL = '/api/sms/delete';
    var POLL_MS = 10000;
    var FALLBACK_LIST_MS = 300000;

    var timer = null;
    var inFlight = false;
    var sending = false;
    var rows = {};
    var bannerKind = '';
    /* Generation and readiness of the last listing that succeeded; null until
     * then, so the first poll always lists. */
    var lastGeneration = null;
    var lastReady = null;
    var lastListMs = 0;
    /* Set while the "module not ready" banner is up, so recovery clears that
     * message without touching a send result. */
    var notReadyShown = false;
    /* Set by refreshNow(): list right away instead of waiting for the next
     * poll to notice the generation this tab's own action produced. */
    var forceList = false;

    function byId(id) {
        return CPE.byId(id);
    }

    function setBanner(text, cls) {
        var b = byId('sms-banner');
        if (!b) return;
        b.textContent = text || '';
        b.className = cls ? ('status-banner ' + cls) : 'status-banner';
        bannerKind = cls || '';
    }

    function setFormEnabled(enabled) {
        /* A poll that was already in flight when the send started must not
         * re-enable the button underneath it. */
        if (sending) enabled = false;
        var num = byId('sms-number');
        var text = byId('sms-text');
        var btn = byId('sms-send-btn');
        if (num) num.disabled = !enabled;
        if (text) text.disabled = !enabled;
        if (btn) btn.disabled = !enabled;
    }

    var GSM_EXTENDED = '^{}[]~|\\\f';

    /* The server counts septets for pure ASCII (a GSM 03.38 extension
     * character takes two) and UTF-8 code points otherwise; JS strings are
     * UTF-16, so a non-ASCII string is measured by its code units. */
    function textUnits(value) {
        var ascii = true;
        var units = 0;
        for (var i = 0; i < value.length; i++) {
            var c = value.charCodeAt(i);
            if (c >= 128) {
                ascii = false;
                break;
            }
            units += GSM_EXTENDED.indexOf(value.charAt(i)) >= 0 ? 2 : 1;
        }
        if (!ascii) {
            units = value.length;
        }
        return { units: units, limit: ascii ? 160 : 70 };
    }

    function updateCount() {
        var text = byId('sms-text');
        var count = byId('sms-count');
        if (!text || !count) return;

        var measure = textUnits(text.value);
        /* Follow the active limit so the field stops accepting text instead of
         * letting the server reject it; a paste can still overshoot. */
        if (text.maxLength !== measure.limit) {
            text.maxLength = measure.limit;
        }
        count.textContent = measure.units + ' / ' + measure.limit;
        count.className = measure.units > measure.limit ? 'sms-count over' : 'sms-count';
    }

    function makeCell(text, cls) {
        var td = document.createElement('td');
        if (cls) td.className = cls;
        td.textContent = text;
        return td;
    }

    function renderStorage(data) {
        var el = byId('sms-storage');
        if (!el) return;
        var s = data.storage || {};
        if (!(s.capacity > 0) || !(s.used >= 0)) {
            el.textContent = '';
            el.className = 'sms-storage';
            return;
        }
        el.textContent = '存储用量 ' + s.used + ' / ' + s.capacity;
        if (s.used >= s.capacity) {
            el.textContent += '（已满，将自动清理最旧的已读短信）';
            el.className = 'sms-storage over';
        } else if (s.near_full) {
            el.textContent += '（接近满，请及时清理）';
            el.className = 'sms-storage warn';
        } else {
            el.className = 'sms-storage';
        }
    }

    function displayBody(m) {
        return (m.body || '') + (m.truncated ? '…' : '');
    }

    function renderList(data) {
        var body = byId('sms-tbody');
        if (!body) return;

        var list = Array.isArray(data.messages) ? data.messages : [];
        var seen = {};
        var order = [];

        list.forEach(function (m) {
            var key = String(m.index);
            seen[key] = true;

            /* 2 and 3 are submits stored by this device: the address is the
             * recipient and there is no delivery timestamp. */
            var outgoing = m.stat === 'sent' || m.stat === 'unsent';
            var from = (outgoing ? '→ ' : '') + (m.from || '');
            var when = outgoing ? (m.stat === 'sent' ? '已发送' : '草稿') : (m.time || '');

            var row = rows[key];
            if (!row) {
                row = document.createElement('tr');
                row.appendChild(makeCell(from, 'sms-from'));
                row.appendChild(makeCell(displayBody(m), 'sms-body'));
                row.appendChild(makeCell(when, 'sms-time'));

                var opCell = document.createElement('td');
                opCell.className = 'sms-op';
                var del = document.createElement('button');
                del.type = 'button';
                del.className = 'sms-del';
                del.textContent = '删除';
                /* Read the indexes off the row, not the poll's message object:
                 * a reused row must delete every part of the current message. */
                del.addEventListener('click', function () { remove(row.smsIndexes); });
                opCell.appendChild(del);
                row.appendChild(opCell);

                rows[key] = row;
            } else {
                row.children[0].textContent = from;
                row.children[1].textContent = displayBody(m);
                row.children[2].textContent = when;
            }
            row.smsIndexes = m.indexes || [m.index];
            row.className = m.stat === 'unread' ? 'sms-unread' : '';
            order.push(row);
        });

        Object.keys(rows).forEach(function (k) {
            if (!seen[k]) {
                var r = rows[k];
                if (r.parentNode) r.parentNode.removeChild(r);
                delete rows[k];
            }
        });

        CPE.syncRows(body, order);

        var empty = byId('sms-empty');
        if (empty) {
            if (list.length === 0) {
                empty.textContent = '暂无短信';
            } else if (data.truncated) {
                empty.textContent = '短信过多，仅显示前 ' + list.length + ' 条';
            } else {
                empty.textContent = '';
            }
        }
    }

    function scheduleNext() {
        if (timer) clearTimeout(timer);
        timer = setTimeout(poll, POLL_MS);
    }

    function pollNow() {
        if (timer) clearTimeout(timer);
        timer = null;
        poll();
    }

    function refreshNow() {
        forceList = true;
        pollNow();
    }

    /* Full listing. Resolves only on success, so the caller keeps the old
     * generation and tries again on the next poll if this one fails. */
    function loadList(generation, ready) {
        return CPE.getJson(LIST_URL).then(function (data) {
            lastGeneration = generation;
            lastReady = ready;
            lastListMs = performance.now();
            setFormEnabled(true);
            /* A successful read only retires a connection error; an explicit
             * result message ("发送成功") must stay until the next action. */
            if (bannerKind === 'status-banner-warn') {
                setBanner('');
            }
            renderStorage(data);
            renderList(data);
        });
    }

    function poll() {
        /* Sending owns the modem for a few seconds; a request started
         * meanwhile would only fight it for the single AT session. */
        if (sending || inFlight) {
            scheduleNext();
            return;
        }
        inFlight = true;
        CPE.getJson(NOTIFY_URL).then(function (data) {
            var ready = data.ready === true;
            var generation = data.generation;
            if (!ready) {
                lastGeneration = generation;
                lastReady = false;
                setFormEnabled(false);
                if (data.supported === false) {
                    /* The modem answered and rejected the SMS commands; the
                     * menu entry is hidden for it, so a bookmark or a stale
                     * tab should not land on a dead page. */
                    window.location.replace('/index.html');
                    return;
                }
                setBanner('模组尚未就绪，短信功能暂不可用。', 'status-banner-info');
                notReadyShown = true;
                return;
            }
            if (notReadyShown) {
                notReadyShown = false;
                setBanner('');
            }
            var stale = performance.now() - lastListMs >= FALLBACK_LIST_MS;
            if (!forceList && !stale && lastReady === true &&
                lastGeneration === generation) {
                setFormEnabled(true);
                return;
            }
            forceList = false;
            return loadList(generation, ready);
        }).catch(function () {
            setBanner('读取短信失败，正在重试。', 'status-banner-warn');
        }).then(function () {
            inFlight = false;
            scheduleNext();
        });
    }

    function remove(indexes) {
        if (!window.confirm('确定删除这条短信吗？')) return;
        CPE.postJson(DELETE_URL, { indexes: indexes || [] }).then(function () {
            refreshNow();
        }).catch(function (error) {
            window.alert(CPE.errorText(error));
        });
    }

    function send() {
        var num = byId('sms-number');
        var text = byId('sms-text');
        if (!num || !text) return;
        if (!num.value || !text.value) {
            setBanner('号码和内容都不能为空。', 'status-banner-warn');
            return;
        }
        var measure = textUnits(text.value);
        if (measure.units > measure.limit) {
            setBanner('短信内容过长（ASCII 最多 160，扩展符号按 2 个计；中文最多 70 字）。',
                      'status-banner-warn');
            return;
        }
        sending = true;
        setFormEnabled(false);
        if (timer) {
            clearTimeout(timer);
            timer = null;
        }
        CPE.postJson(SEND_URL, { number: num.value, text: text.value }, 70000).then(function () {
            setBanner('发送成功。', 'status-banner-info');
            text.value = '';
            updateCount();
            sending = false;
            setFormEnabled(true);
            refreshNow();
        }).catch(function (error) {
            var code = error && (error.code || error.message);
            if (code === 'network_error' || code === 'sms_send_timeout') {
                /* The modem may already have sent it; retrying blindly could
                 * bill the user twice. */
                setBanner('发送结果未知，请稍后在短信列表中确认是否已发送。',
                          'status-banner-warn');
            } else {
                setBanner(CPE.errorText(error), 'status-banner-warn');
            }
            sending = false;
            setFormEnabled(true);
            scheduleNext();
        });
    }

    function init() {
        CPE.bindMenu();

        var btn = byId('sms-send-btn');
        if (btn) btn.addEventListener('click', send);
        var text = byId('sms-text');
        if (text) text.addEventListener('input', updateCount);
        setFormEnabled(false);

        /* Background tabs throttle timers, so poll once more on becoming
         * visible; the generation check decides whether a full listing is
         * actually needed. */
        document.addEventListener('visibilitychange', function () {
            if (!document.hidden) pollNow();
        });

        CPE.guardSession().then(function () {
            poll();
        }).catch(function () {
            /* Forced password change or missing session: already redirected. */
        });
    }

    document.addEventListener('DOMContentLoaded', init);
}());
