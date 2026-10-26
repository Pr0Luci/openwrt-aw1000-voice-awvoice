// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';

/*
 * loader.js - a small stand-in for the LuCI runtime, enough to load the phone page's view files in a plain browser
 * for a render check: class loading by 'require' lines, E(), _(), poll, ui.tabs and ui.showModal. The provider
 * module (view.awvoice.phone.live) is replaced by fixture.js. Run: serve the package directory and open
 * tests/harness/index.html (see README.txt there).
 */

(function() {
	var ROOT = '../../htdocs/luci-static/resources/';
	var modules = {};
	var stubs = {};

	String.prototype.format = function() {
		var a = arguments, i = 0;
		return this.replace(/%[ds]/g, function() { return a[i++]; });
	};

	window._ = function(s) { return s; };

	window.E = function(tag, attrs, kids) {
		if (attrs != null && (Array.isArray(attrs) || typeof attrs === 'string' || attrs instanceof Node)) {
			kids = attrs;
			attrs = null;
		}
		var n = document.createElement(tag);
		Object.keys(attrs || {}).forEach(function(k) {
			var v = attrs[k];
			if (typeof v === 'function') n.addEventListener(k, v);
			else if (v === true) n.setAttribute(k, '');
			else if (v !== false && v != null) n.setAttribute(k, v);
		});
		(function add(k) {
			if (k == null || k === false) return;
			if (Array.isArray(k)) k.forEach(add);
			else if (k instanceof Node) n.appendChild(k);
			else n.appendChild(document.createTextNode(String(k)));
		})(kids);
		return n;
	};

	var base = {
		extend: function(proto) {
			function C() {}
			Object.assign(C.prototype, proto);
			C.__proto__ = base;
			C.prototype.__isClass = true;
			return C;
		}
	};
	stubs['baseclass'] = { extend: function(p) { return base.extend(p); } };
	stubs['view'] = { extend: function(p) { return base.extend(p); } };
	stubs['poll'] = { add: function(fn, sec) { window.setInterval(fn, sec * 1000); } };
	stubs['ui'] = {
		showModal: function(title, kids) {
			var m = document.getElementById('modal') || document.body.appendChild(E('div', { 'id': 'modal', 'class': 'modal' }));
			m.textContent = '';
			m.appendChild(E('h4', title));
			(Array.isArray(kids) ? kids : [ kids ]).forEach(function(k) { m.appendChild(typeof k === 'string' ? document.createTextNode(k) : k); });
			m.hidden = false;
		},
		hideModal: function() { var m = document.getElementById('modal'); if (m) m.hidden = true; },
		tabs: {
			initTabGroup: function(panes) {
				var menu = E('ul', { 'class': 'cbi-tabmenu' });
				var host = panes[0].parentNode;
				host.parentNode.insertBefore(menu, host);
				function pick(p) {
					panes.forEach(function(q) { q.style.display = q === p ? '' : 'none'; });
					Array.prototype.forEach.call(menu.children, function(li) { li.className = li.getAttribute('data-tab') === p.getAttribute('data-tab') ? 'cbi-tab' : 'cbi-tab-disabled'; });
					p.dispatchEvent(new CustomEvent('cbi-tab-active', { detail: { tab: p.getAttribute('data-tab') } }));
				}
				panes.forEach(function(p) {
					menu.appendChild(E('li', { 'data-tab': p.getAttribute('data-tab') }, E('a', { 'href': '#', 'click': function(ev) { ev.preventDefault(); pick(p); } }, p.getAttribute('data-tab-title'))));
				});
				pick(panes[0]);
			}
		}
	};
	window.L = { env: { media: '/luci-static/bootstrap' } };

	function pathOf(name) {
		if (name === 'view.awvoice.phone.live')
			return 'tests/harness/fixture.js';
		return ROOT + name.replace(/\./g, '/') + '.js';
	}

	function load(name) {
		if (stubs[name]) return Promise.resolve(stubs[name]);
		if (modules[name]) return modules[name];
		modules[name] = fetch(pathOf(name).replace(/^tests\/harness\//, '')).then(function(r) { return r.text(); }).then(function(src) {
			var deps = [], args = '', re = /^'require\s+(\S+)(?:\s+as\s+(\S+))?';$/gm, m;
			while ((m = re.exec(src))) {
				deps.push(load(m[1]));
				args += ', ' + (m[2] || m[1].replace(/[^a-zA-Z0-9_]/g, '_'));
			}
			return Promise.all(deps).then(function(inst) {
				var factory = eval('(function(window, document, L' + args + ') {' + src + '\n})');
				var cls = factory.apply(null, [ window, document, window.L ].concat(inst));
				return new cls();
			});
		});
		return modules[name];
	}

	window.loadView = function(name) { return load(name); };
})();
