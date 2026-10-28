// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.tab_calls as calls';

/*
 * tab_overview.js - Overview tab: the state banner, the live call card, four status tiles, "My number" and the last
 * calls. Redraws on every 'status' / 'calls' event.
 *
 * The call timer counts on from the backend's elapsedSec (seconds since the answer), so a wrong clock in the browser
 * changes nothing. Before the answer there is no timer: the phase is shown instead.
 *
 * Display choice (the backend supplies the raw value): signal bars from the LTE RSRP in dBm.
 */

var LAST_CALLS = 3;
var SIGNAL_STEPS = [ -90, -100, -110 ];     /* dBm: >= first = 4 bars, ..., below the last = 1 bar */

function signalBars(dbm) {
	if (dbm == null) return 0;
	for (var i = 0; i < SIGNAL_STEPS.length; i++)
		if (dbm >= SIGNAL_STEPS[i]) return 4 - i;
	return 1;
}

function bars(n) {
	var s = E('span', { 'class': 'av-bars', 'role': 'img', 'aria-label': _('%d of 4 bars').format(n) });
	for (var i = 0; i < 4; i++)
		s.appendChild(E('i', { 'class': i < n ? 'on' : '' }));
	return s;
}

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, timer = 0, base = 0, since = 0;

		var banIcon = u.icon('check'), banTitle = E('b'), banSub = E('div', { 'class': 'av-sub' });
		var ban = E('div', { 'class': 'av-ban', 'role': 'status' }, [ banIcon, E('div', [ banTitle, banSub ]) ]);
		var mine = E('div', { 'class': 'av-note', 'hidden': true });

		var clock = E('div', { 'class': 'av-big av-mono' });
		var dirLbl = E('div', { 'class': 'av-mut' });
		var remote = E('span', { 'class': 'av-r av-mono' });
		var hang = u.button(_('Hang up'), 'phone-miss', 'cbi-button-remove', function() { hangUp(); });
		var live = E('div', { 'class': 'cbi-section', 'hidden': true }, [
			E('h3', [ _('Live call'), remote ]),
			E('div', { 'class': 'av-g2' }, [ E('div', [ clock, dirLbl ]), E('div', { 'class': 'av-acts' }, hang) ])
		]);

		var tLine = E('b'), tNet = E('b'), tSig = E('b'), tSvc = E('b');
		function tile(label, b) { return E('div', { 'class': 'av-tile' }, [ E('small', label), b ]); }
		var tiles = E('div', { 'class': 'av-tiles' }, [
			tile(_('Line'), tLine), tile(_('Voice network'), tNet), tile(_('Signal'), tSig), tile(_('Service'), tSvc)
		]);

		var goAll = E('a', { 'href': '#', 'click': function(ev) { ev.preventDefault(); ctx.goto('calls'); } }, _('View all'));
		var last = E('div');
		var root = E('div', [
			ban, mine, live, tiles,
			E('div', { 'class': 'cbi-section' }, [ E('h3', [ _('Last calls'), E('span', { 'class': 'av-r' }, goAll) ]), last ])
		]);

		function dot(cls, text) {
			return [ E('span', { 'class': 'av-dot ' + cls }), E('span', text) ];
		}
		function fill(node, kids) {
			node.textContent = '';
			kids.forEach(function(k) { node.appendChild(k); });
		}

		function hangUp() {
			ctx.api.hangup().then(function(r) {
				ctx.toast(r.ok ? _('Hang-up sent') : _('There is no call to hang up'));
				return ctx.refresh();
			}, function(e) { ctx.toast(e.message); });
		}

		function tick() {
			if (!root.isConnected && timer) { window.clearInterval(timer); timer = 0; return; }
			clock.textContent = u.fmtDuration(base + Math.floor((Date.now() - since) / 1000));
		}

		function lineWord(s) {
			return ({ idle: _('Idle'), offhook: _('Handset lifted'), ring: _('Ringing'), call: _('In call'),
				noservice: _('No service'), down: _('Unavailable') })[s];
		}

		function banner(st) {
			var c = st.call, num = c ? c.number : '';
			switch (st.state) {
			case 'idle':
				return [ '', 'check', _('Idle (on hook)'), _('Ready for calls. Handset is on hook.') ];
			case 'offhook':
				return [ '', 'phone', _('Handset lifted'), _('The handset is off hook. Dial a number.') ];
			case 'ring':
				return [ 'w', 'bell', _('Ringing'), num ? _('Incoming call from %s. Lift the handset to answer.').format(num)
					: _('Incoming call from a withheld number. Lift the handset to answer.') ];
			case 'call':
				if (st.machine === 'answering' || st.machine === 'answered')
					return [ '', 'message', _('Answering machine'), st.machine === 'answering'
						? _('The call is being answered; the phone has stopped ringing.')
						: _('The greeting is played to the caller, then the message is recorded.') ];
				return [ '', 'phone', _('In call'), c && c.phase !== 'active' ? phaseWord(c) : _('Handset is off hook.') ];
			case 'noservice':
				return [ 'w', 'alert', _('No service'), _('The modem has no mobile service. Calls cannot be made until it returns.') ];
			}
			return [ 'e', 'alert', _('Phone service is down'), _('The phone service is not running. Open the Health tab to start it.') ];
		}

		function phaseWord(c) {
			return ({ ringing: _('Ringing'), dialing: _('Dialling'), alerting: _('Calling'), active: _('Connected') })[c.phase];
		}

		function onStatus(st) {
			var b = banner(st), c = st.call;
			ban.className = 'av-ban ' + b[0];
			banIcon.replaceWith(banIcon = u.icon(b[1]));
			banTitle.textContent = b[2];
			banSub.textContent = b[3];

			mine.hidden = !st.myNumber;
			mine.textContent = st.myNumber ? _('My number: %s').format(st.myNumber) : '';

			live.hidden = !c;
			if (timer) { window.clearInterval(timer); timer = 0; }
			if (c) {
				var name = (c.name || ctx.nameFor(c.number));
				remote.textContent = name ? name + ' ' + c.number : c.number || _('Withheld');
				dirLbl.textContent = st.machine === 'answering' || st.machine === 'answered' ? _('Answering machine')
					: c.direction === 'OUT' ? _('Outgoing') : _('Incoming');
				if (c.phase === 'active') {
					base = c.elapsedSec;
					since = Date.now();
					tick();
					timer = window.setInterval(tick, 1000);
				} else {
					clock.textContent = phaseWord(c);
				}
			}

			var down = st.state === 'down', ring = st.state === 'ring', warn = ring || st.state === 'noservice';
			fill(tLine, dot(down ? 'off' : warn ? 'warn' : '', lineWord(st.state)));
			var reg = st.voiceNetwork.ims;
			fill(tNet, dot(reg === true ? '' : reg === false ? 'warn' : 'off',
				reg === true ? _('VoLTE registered') : reg === false ? _('Not registered') : _('Unknown')));
			fill(tSig, st.signal.rsrpDbm == null ? [ E('span', '—') ] : [ bars(signalBars(st.signal.rsrpDbm)),
				E('span', { 'class': 'av-mono' }, st.signal.rsrpDbm + ' dBm' + (st.signal.network ? ' ' + st.signal.network : '')) ]);
			fill(tSvc, dot(st.service.running && !down ? '' : 'err', st.service.running && !down ? _('Running') : _('Stopped')));
		}

		function onCalls() {
			last.textContent = '';
			ctx.calls.slice(0, LAST_CALLS).forEach(function(c) {
				last.appendChild(calls.row(ctx, c, false, function() {
					ctx.emit('select', c.id);
					ctx.goto('calls');
				}));
			});
			if (!ctx.calls.length)
				last.appendChild(E('div', { 'class': 'av-empty' }, _('No calls yet.')));
		}

		ctx.on('status', onStatus);
		ctx.on('calls', onCalls);
		ctx.on('contacts', onCalls);
		return root;
	}
});
