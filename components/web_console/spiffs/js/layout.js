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
        { href: 'upgrade.html', text: '系统升级', pages: ['/upgrade.html'] }
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

    function renderNav() {
        var nav = document.getElementById('leftMenu');
        if (!nav) {
            return;
        }
        var path = window.location.pathname;
        var list = document.createElement('ul');
        NAV_ITEMS.forEach(function (item) {
            var entry = document.createElement('li');
            var link = document.createElement('a');
            link.href = item.href;
            link.textContent = item.text;
            if (item.pages.indexOf(path) !== -1) {
                link.className = 'content';
            }
            entry.appendChild(link);
            list.appendChild(entry);
        });
        nav.appendChild(list);
    }

    renderHeader(document.getElementById('leftMenu') !== null);
    renderNav();
}());
