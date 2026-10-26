// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require view';
'require poll';
'require ui';
'require view.awvoice.phone.api as api';
'require view.awvoice.phone.style as style';
'require view.awvoice.phone.ui as pui';
'require view.awvoice.phone.tab_overview as tabOverview';
'require view.awvoice.phone.tab_calls as tabCalls';
'require view.awvoice.phone.tab_rules as tabRules';
'require view.awvoice.phone.tab_settings as tabSettings';
'require view.awvoice.phone.tab_health as tabHealth';
'require view.awvoice.phone.tab_tools as tabTools';

/*
 * phone.js - Services > Phone
 *
 * One page, six tabs (Overview, Calls, Rules, Phone settings, Health, Test tools). The page renders only the content
 * area; the active LuCI theme provides the chrome and style.js maps the colours to whichever theme is active.
 *
 * All data comes through phone/api.js. Deep links: #tab=calls&rules=bl&sel=2
 */

var TOAST_MS = 2600;
var TOAST_ID = 'av-toast';
var SEEN_KEY = 'awvoice.calls-seen';

/* the live view refresh the page offers and its bounds; UCI awvoice.main.page_refresh_s, read by the page only */
var REFRESH_STEPS = [ 1, 2, 3, 5, 10, 15, 30 ];
var REFRESH_MIN = 1, REFRESH_MAX = 30, REFRESH_DEF = 1;

function tabList() {
	return [
		{ id: 'ov', title: _('Overview'), icon: 'grid', mod: tabOverview },
		{ id: 'calls', title: _('Calls'), icon: 'phone', mod: tabCalls },
		{ id: 'rules', title: _('Rules'), icon: 'filter', mod: tabRules },
		{ id: 'set', title: _('Phone settings'), icon: 'sliders', mod: tabSettings },
		{ id: 'health', title: _('Health'), icon: 'activity', mod: tabHealth },
		{ id: 'tools', title: _('Test tools'), icon: 'tool', mod: tabTools }
	];
}

function parseHash() {
	var h = {};
	String(location.hash || '').replace(/^#/, '').split('&').forEach(function(p) {
		var kv = p.split('=');
		if (kv[0]) h[kv[0]] = decodeURIComponent(kv[1] || '');
	});
	return h;
}

function makeToast(msg) {
	var t = document.getElementById(TOAST_ID);
	if (!t) {
		t = E('div', { 'id': TOAST_ID, 'class': 'av-toast', 'role': 'status', 'hidden': true });
		document.body.appendChild(t);
	}
	t.textContent = msg;
	t.hidden = false;
	window.clearTimeout(t._h);
	t._h = window.setTimeout(function() { t.hidden = true; }, TOAST_MS);
}

/* the missed-call marker is kept in this browser only (the newest call time already seen on the Calls tab) */
function readSeen() {
	try { return +window.localStorage.getItem(SEEN_KEY) || 0; } catch (e) { return 0; }
}

function writeSeen(ts) {
	try { window.localStorage.setItem(SEEN_KEY, String(ts)); } catch (e) { /* the badge then shows all missed calls */ }
}

/* the stored refresh falls back to the default on any value the page cannot use */
function clampRefresh(sec) {
	sec = +sec;
	return (sec >= REFRESH_MIN && sec <= REFRESH_MAX && sec === Math.floor(sec)) ? sec : REFRESH_DEF;
}

function readRefresh(ps) {
	var v = ps && ps.page && ps.page.refreshSec;
	return clampRefresh(v != null ? v : REFRESH_DEF);
}

return view.extend({
	/* the page keeps its own Save and Reset buttons per tab, so the view's footer bar is left out */
	addFooter: function() {
		return E([]);
	},

	load: function() {
		/* a poll that fails is the "down" state, never a broken page */
		return Promise.all([
			api.getStatus().catch(function() { return api.downStatus(); }),
			api.available.listCalls ? api.listCalls() : [],
			api.available.listContacts ? api.listContacts() : [],
			api.available.getPhoneSettings ? api.getPhoneSettings().catch(function() { return null; }) : null
		]);
	},

	render: function(data) {
		style.inject();

		var hash = parseHash(), listeners = {}, activeTab = 'ov', root, TABS = tabList();
		var lastState = data[0].state, seenTs = readSeen(), pollSec = readRefresh(data[3]);

		var ctx = {
			api: api, ui: pui,
			status: data[0], calls: data[1], contacts: data[2],
			on: function(evt, fn) { (listeners[evt] = listeners[evt] || []).push(fn); },
			emit: function(evt, arg) { (listeners[evt] || []).forEach(function(fn) { fn(arg); }); },
			toast: makeToast,
			nameFor: function(number) {
				if (!number)
					return '';
				for (var i = 0; i < ctx.contacts.length; i++)
					for (var j = 0; j < ctx.contacts[i].numbers.length; j++)
						if (ctx.contacts[i].numbers[j].value === number)
							return ctx.contacts[i].name;
				return '';
			},
			refreshContacts: function() {
				return api.listContacts().then(function(list) { ctx.contacts = list; ctx.emit('contacts'); });
			},
			reloadCalls: function() {
				return api.listCalls().then(function(list) { ctx.calls = list; ctx.emit('calls'); });
			},
			refresh: function() {
				return poller();
			},
			/* the poll loop ticks every second, so the new interval holds from its next tick */
			setRefresh: function(sec) {
				pollSec = clampRefresh(sec);
				poll.remove(poller);
				poll.add(poller, pollSec);
			},
			goto: function(id) {
				var a = root.querySelector('.cbi-tabmenu [data-tab="' + id + '"] a');
				if (a) a.click();
			}
		};

		function stateWord(s) {
			return ({ idle: _('Idle'), offhook: _('Handset lifted'), ring: _('Ringing'), call: _('In call'),
				noservice: _('No service'), down: _('Phone service down') })[s] || s;
		}

		/* what of the status reply marks a move of the call: the line's state, the call's id and its phase
		 * (a remote end with the handset still up keeps the state "call" - the release tone - and only the
		 * call itself says it ended) */
		function callMoveKey(st) {
			return st.state + '|' + (st.call ? st.call.id + ':' + st.call.phase : '-');
		}
		var lastCallMove = callMoveKey(data[0]);

		function newStatus(st) {
			var key = callMoveKey(st);

			if (key !== lastCallMove) {
				lastCallMove = key;
				/* the call log and its badge follow every move of the call */
				if (api.available.listCalls)
					ctx.reloadCalls();
			}
			if (st.state !== lastState) {
				lastState = st.state;
				makeToast(_('Phone line: %s').format(stateWord(st.state)));
			}
			ctx.status = st;
			ctx.emit('status', st);
		}

		function poller() {
			var wantHealth = activeTab === 'health';
			var health = wantHealth ? api.getHealth().catch(function() { return null; }) : null;
			return Promise.all([ api.getStatus().catch(function() { return api.downStatus(); }), health ]).then(function(r) {
				newStatus(r[0]);
				if (wantHealth) ctx.emit('health', r[1]);
			});
		}

		var panes = TABS.map(function(t) {
			return E('div', { 'data-tab': t.id, 'data-tab-title': t.title }, t.mod.render(ctx));
		});

		var tabsHost = E('div', {}, [ E('div', {}, panes) ]);
		root = E('div', { 'class': 'cbi-map avp' }, [
			E('h2', { 'name': 'content' }, _('AWvoice(RJ11)')),
			E('div', { 'class': 'cbi-map-descr' }, _('Make and take the SIM\'s calls from an ordinary desk phone on the RJ11 socket - the router drives the line, the modem carries the call.')),
			tabsHost
		]);

		ui.tabs.initTabGroup(panes);

		/* tab icons + missed-calls badge */
		var badge = E('span', { 'class': 'av-badge', 'hidden': true });
		TABS.forEach(function(t) {
			var a = root.querySelector('.cbi-tabmenu [data-tab="' + t.id + '"] a');
			if (!a) return;
			a.insertBefore(pui.icon(t.icon), a.firstChild);
			if (t.id === 'calls') a.appendChild(badge);
		});

		function newestCall() {
			return ctx.calls.reduce(function(m, c) { return c.ts > m ? c.ts : m; }, 0);
		}

		/* missed calls since the last visit of the Calls tab */
		function updateBadge() {
			if (activeTab === 'calls' && newestCall() > seenTs) {
				seenTs = newestCall();
				writeSeen(seenTs);
			}
			var n = ctx.calls.filter(function(c) { return c.type === 'MISS' && c.ts > seenTs; }).length;
			badge.textContent = String(n);
			badge.hidden = n === 0;
			badge.setAttribute('aria-label', _('%d missed calls').format(n));
		}
		ctx.on('calls', updateBadge);
		/* the settings tab's save applies the new live view refresh at once */
		ctx.on('settings', function(ps) { if (ps && ps.page) ctx.setRefresh(ps.page.refreshSec); });

		panes.forEach(function(p) {
			p.addEventListener('cbi-tab-active', function(ev) {
				activeTab = ev.detail.tab;
				var h = parseHash();
				h.tab = activeTab;
				try {
					history.replaceState(null, '', '#' + Object.keys(h).map(function(k) { return k + '=' + encodeURIComponent(h[k]); }).join('&'));
				} catch (e) { /* the hash is a convenience only */ }
				updateBadge();
				poller();
			});
		});

		/* first paint of data-driven parts */
		ctx.emit('status', ctx.status);
		ctx.emit('calls');
		ctx.emit('contacts');

		/* deep links */
		window.setTimeout(function() {
			if (hash.tab) ctx.goto(hash.tab);
			if (hash.rules) ctx.emit('subrules', hash.rules);
			if (hash.sel) ctx.emit('select', +hash.sel);
		}, 0);

		poll.add(poller, pollSec);
		return root;
	}
});
