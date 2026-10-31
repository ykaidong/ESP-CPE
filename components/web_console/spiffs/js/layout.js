/*
 * Shared page chrome for the management console.
 *
 * The header (logo + menu button) and the left navigation used to be copied
 * into every HTML page. They are generated here instead, so adding or renaming
 * a page only touches NAV_ITEMS. Every page provides:
 *
 *   <div class="header-base" id="app-header"></div>
 *   <div class="left-base" id="leftMenu"></div>   (not on the login page)
 *
 * This script runs synchronously at the end of <body>, before the page script,
 * so menuBtn/leftMenu exist by the time the page binds its handlers.
 */
'use strict';

(function () {
    var NAV_ITEMS = [
        { href: 'index.html', text: '系统状态', pages: ['/', '/index.html'] },
        { href: 'wlan.html', text: 'WLAN设置', pages: ['/wlan.html'] },
        { href: 'system.html', text: '系统设置', pages: ['/system.html'] },
        { href: 'upgrade.html', text: '系统升级', pages: ['/upgrade.html'] },
        /* Some modems reject the SMS commands; the entry is hidden once the
         * device reports that, and stays visible while the answer is unknown. */
        { href: 'sms.html', text: '短信', pages: ['/sms.html'], optional: true }
    ];

    /* Single administrator account: the header exposes logout directly instead
     * of a one-item dropdown. */
    function renderLogout(header) {
        var button = document.createElement('button');
        button.type = 'button';
        button.className = 'header-logout';
        button.id = 'btn-logout';
        button.textContent = '退出登录';
        header.appendChild(button);
    }

    function renderHeader(withMenu) {
        var header = document.getElementById('app-header');
        if (!header) {
            return;
        }
        header.className = withMenu ? 'header-base flex flex-jcb' : 'header-base';

        var brand = document.createElement('div');
        brand.className = 'header-brand';

        var logo = document.createElement('img');
        logo.className = 'header-brand-logo';
        logo.src = './img/celllink.svg';
        logo.alt = 'CellLink';

        var brandText = document.createElement('div');
        brandText.className = 'header-brand-text';

        var brandName = document.createElement('span');
        brandName.className = 'header-brand-name';
        brandName.textContent = 'ESP-CPE';

        var brandSub = document.createElement('span');
        brandSub.className = 'header-brand-sub';
        brandSub.textContent = '管理后台';

        brandText.appendChild(brandName);
        brandText.appendChild(brandSub);

        brand.appendChild(logo);
        brand.appendChild(brandText);
        header.appendChild(brand);

        if (withMenu) {
            var menuButton = document.createElement('div');
            menuButton.className = 'header-menu';
            menuButton.id = 'menuBtn';
            header.appendChild(menuButton);
            renderLogout(header);
        }
    }

    function activeItem() {
        var path = window.location.pathname;
        for (var i = 0; i < NAV_ITEMS.length; i++) {
            if (NAV_ITEMS[i].pages.indexOf(path) !== -1) {
                return NAV_ITEMS[i];
            }
        }
        return null;
    }

    /* The SMS entry starts hidden and is revealed by a cheap capability poll
     * (the endpoint does not touch the AT session). Any failure reveals it:
     * hiding a working page because the device did not answer would be worse
     * than showing it for a module without SMS. */
    function probeOptionalNav(element) {
        var controller = null;
        var timer = null;
        function reveal() {
            element.hidden = false;
        }
        try {
            controller = new AbortController();
            timer = setTimeout(function () { controller.abort(); }, 4000);
        } catch (e) {
            controller = null;
        }
        var options = { credentials: 'same-origin' };
        if (controller) {
            options.signal = controller.signal;
        }
        fetch('/api/sms/notify', options).then(function (resp) {
            if (!resp.ok) {
                return null;
            }
            return resp.json();
        }).then(function (data) {
            if (!data || data.supported !== false) {
                reveal();
            }
        }).catch(reveal).then(function () {
            if (timer) clearTimeout(timer);
        });
    }

    function renderNav(item) {
        var nav = document.getElementById('leftMenu');
        if (!nav) {
            return;
        }
        var list = document.createElement('ul');
        var optional = [];
        NAV_ITEMS.forEach(function (entry) {
            var element = document.createElement('li');
            var link = document.createElement('a');
            link.href = entry.href;
            link.textContent = entry.text;
            if (entry === item) {
                link.className = 'content';
            }
            element.appendChild(link);
            if (entry.optional) {
                element.hidden = true;
                optional.push(element);
            }
            list.appendChild(element);
        });
        nav.appendChild(list);
        optional.forEach(probeOptionalNav);
    }

    /* The page title above the content is generated from the same NAV_ITEMS
     * entry, so renaming a page only touches this file. */
    function renderPageTitle(item) {
        var header = document.querySelector('.page-header');
        if (!header || !item || header.children.length > 0) {
            return;
        }
        var title = document.createElement('span');
        title.className = 'header-title-one';
        title.textContent = item.text;
        header.appendChild(title);
    }

    var current = activeItem();
    renderHeader(document.getElementById('leftMenu') !== null);
    renderNav(current);
    renderPageTitle(current);
}());
