// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.tab_calls as calls';
'require view.awvoice.phone.webphone as webphone';

/*
 * tab_overview.js - Overview tab: the phone itself. The state banner, the four status tiles and the last calls sit
 * beside the page's own call card and its browser audio (webphone.js), so a call can be taken and spoken here without
 * opening any other tab. Redraws on every 'status' / 'calls' event.
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
		var u = ctx.ui;

		var banIcon = u.icon('check'), banTitle = E('b'), banSub = E('div', { 'class': 'av-sub' });
		/* the line's own number rides at the end of the state card - two rows in one column, held to the card's right
		   edge and read from the left - because a number under the card is a line the eye has to leave the state
		   for, and the two belong together: the state and the number that state answers to */
		var numTitle = E('div'), numValue = E('div', { 'class': 'av-mono' });
		var numCol = E('div', { 'class': 'av-ban-num', 'hidden': true }, [ numTitle, numValue ]);
		var ban = E('div', { 'class': 'av-ban alert-message', 'role': 'status' }, [ banIcon, E('div', [ banTitle, banSub ]), numCol ]);

		var phone = webphone.render(ctx);

		var tLine = E('b'), tNet = E('b'), tSig = E('b'), tSvc = E('b');
		/* the tile keeps the design it has always had on this page - a quiet label over the state - and takes its
	   box from the same framework card the rest of the page is built out of, so the theme paints the background,
	   the border and the radius instead of the page painting one that only matches the theme it was read under */
		function tile(label, b) { return E('div', { 'class': 'av-tile cbi-section' }, [ E('small', label), b ]); }
		var tiles = E('div', { 'class': 'av-tiles' }, [
			tile(_('Line'), tLine), tile(_('Voice network'), tNet), tile(_('Signal'), tSig), tile(_('Service'), tSvc)
		]);

		var goAll = E('a', { 'href': '#', 'click': function(ev) { ev.preventDefault(); ctx.goto('calls'); } }, _('View all'));
		var last = E('div');

		var lastCard = E('div', { 'class': 'cbi-section' }, [ E('h3', [ _('Last calls'), E('span', { 'class': 'av-r' }, goAll) ]), last ]);

		/* the phone and the browser audio first; the recent calls sit under the audio, so the line's work is one column */
		var root = E('div', [
			ban, tiles,
			E('div', { 'class': 'av-callgrid' }, [ phone.call, E('div', {}, [ phone.audio, lastCard ]) ])
		]);

		function dot(cls, text) {
			return [ E('span', { 'class': 'av-dot ' + cls }), E('span', text) ];
		}
		function fill(node, kids) {
			node.textContent = '';
			kids.forEach(function(k) { node.appendChild(k); });
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
				/* quiet hours hold the bell: the network offers the call and a lift answers it, but nothing here makes
				 * a sound, so the card cannot say the phone is ringing */
				if (st.quiet === true)
					return [ '', 'bell', _('Call held quiet'), num ? _('Quiet hours: a call from %s is waiting. Answer on this page, or lift the handset.').format(num)
						: _('Quiet hours: a call from a withheld number is waiting. Answer on this page, or lift the handset.') ];
				return [ 'warning', 'bell', _('Ringing'), num ? _('Incoming call from %s. Answer on this page, or lift the handset.').format(num)
					: _('Incoming call from a withheld number. Answer on this page, or lift the handset.') ];
			case 'call':
				if (st.machine === 'answering' || st.machine === 'answered')
					return [ '', 'message', _('Answering machine'), st.machine === 'answering'
						? _('The call is being answered; the phone has stopped ringing.')
						: _('The greeting is played to the caller, then the message is recorded.') ];
				return [ '', 'phone', _('In call'), c && c.phase !== 'active' ? phaseWord(c)
					: st.hook === 'off' ? _('Handset is off hook.') : _('Handset is on hook: the call is on this page.') ];
			case 'noservice':
				return [ 'warning', 'alert', _('No service'), _('The modem has no mobile service. Calls cannot be made until it returns.') ];
			}
			return [ 'error', 'alert', _('Phone service is down'), _('The phone service is not running. Open the Health tab to start it.') ];
		}

		function phaseWord(c) {
			return ({ ringing: _('Ringing'), dialing: _('Dialling'), alerting: _('Calling'), active: _('Connected') })[c.phase];
		}

		function onStatus(st) {
			var b = banner(st), c = st.call;
			ban.className = 'av-ban alert-message' + (b[0] ? ' ' + b[0] : '');
			banIcon.replaceWith(banIcon = u.icon(b[1]));
			banTitle.textContent = b[2];
			banSub.textContent = b[3];

			numCol.hidden = !st.myNumber;
			numTitle.textContent = _('My number');
			numValue.textContent = st.myNumber || '';

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
