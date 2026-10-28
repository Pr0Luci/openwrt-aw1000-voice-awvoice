// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_health.js - Health tab: the modem's AT port, the line, the modem with SIM and VoLTE, the system (kernel module
 * and audio bridge), the daemon's own counters, service control and the recent log. Redrawn on every 'health' event
 * (the page polls while this tab is visible).
 */

function yn(v, yes, no) {
	if (v == null) return [ _('Unknown'), 'av-mut' ];
	return v ? [ yes, 'av-ok' ] : [ no, 'av-err' ];
}

function count(v) {
	return v == null ? '—' : String(v);
}

/* a folder the daemon checked: ready with its free space, refused with the reason, or not checked by an older daemon */
function folder(s) {
	if (s && s.ready === true) {
		if (s.freeMb != null && s.freeMb < 1)
			return [ _('Nearly full: %s MB free').format(s.freeMb), 'av-warn' ];	/* the daemon starts no file under 1 MB */
		return [ s.freeMb == null ? _('Ready') : _('Ready, %s MB free').format(s.freeMb), 'av-ok' ];
	}
	if (s && s.ready === false)
		return [ s.error ? _('Cannot be used: %s').format(s.error) : _('Cannot be used'), 'av-err' ];
	return [ _('Not checked yet'), 'av-mut' ];
}

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, busyCall = false, down = false;

		var banIcon = u.icon('radio'), banTitle = E('b', _('Modem AT port')), banSub = E('div', { 'class': 'av-sub' });
		var ban = E('div', { 'class': 'av-ban', 'role': 'status' }, [ banIcon, E('div', [ banTitle, banSub ]) ]);

		var at = E('div'), line = E('div'), modem = E('div'), sys = E('div'), store = E('div');
		var counters = E('div', { 'class': 'av-tiles' }), logTail = E('div', { 'class': 'av-log av-mono' });
		var gone = E('div', { 'class': 'av-ban e', 'role': 'status' }, [ u.icon('alert'), E('div', _('The phone service does not answer, so there is no health data. Start it below.')) ]);
		gone.hidden = true;

		var bStart = u.button(_('Start'), 'play', '', function() { act('start'); });
		var bStop = u.button(_('Stop'), 'stop', '', function() { confirmAct('stop', _('Stop the phone service? Calls cannot be made or received until it is started again.')); });
		var bRestart = u.button(_('Restart'), 'refresh', '', function() { confirmAct('restart', _('Restart the phone service? This takes a few seconds.')); });
		var svcNote = E('div', { 'class': 'av-note' }, _('Stop and restart are refused while a call is active.'));
		var storeNote = E('div', { 'class': 'av-note' });

		function fill(node, rows) {
			node.textContent = '';
			rows.forEach(function(r) { node.appendChild(u.kv(r[0], r[1], r[2])); });
		}

		function act(action) {
			ctx.api.serviceAction(action).then(function(r) {
				if (!r.ok && r.refused === 'ACTIVE_CALL')
					ctx.toast(_('Refused: a call is active. Hang up first.'));
				else if (!r.ok)
					ctx.toast(_('The phone service could not be controlled from here.'));
				else
					ctx.toast(_('Done'));
				window.setTimeout(function() { ctx.refresh(); }, 1500);
			}, function(e) { ctx.toast(e.message); });
		}

		function confirmAct(action, msg) {
			editors.confirm(ctx, action === 'stop' ? _('Stop phone service') : _('Restart phone service'), msg,
				action === 'stop' ? _('Stop') : _('Restart'), function() { act(action); });
		}

		function syncButtons() {
			bStart.disabled = !down;
			bStop.disabled = busyCall;
			bRestart.disabled = busyCall;
		}

		ctx.on('status', function(st) {
			busyCall = !!st.call || st.state === 'call' || st.state === 'ring';
			down = st.state === 'down';
			syncButtons();
		});

		ctx.on('health', function(h) {
			gone.hidden = !!h;
			if (!h) {
				ban.className = 'av-ban e';
				banSub.textContent = _('No answer from the phone service.');
				[ at, line, modem, sys, store, counters ].forEach(function(n) { n.textContent = ''; });
				storeNote.textContent = '';
				logTail.textContent = '';
				return;
			}

			var okAt = h.atPort.responding;
			ban.className = 'av-ban ' + (okAt ? '' : 'w');
			banSub.textContent = okAt
				? (h.atPort.lastReplyAgoSec == null ? _('Modem AT port responding.') : _('Modem AT port responding. Last reply %d s ago.').format(h.atPort.lastReplyAgoSec))
				: _('Modem AT port is not responding.');

			var resp = yn(h.atPort.responding, _('Yes'), _('No'));
			fill(at, [
				[ _('Port'), h.atPort.port || '—', 'av-mono' ],
				[ _('USB interface'), count(h.atPort.usbInterface) ],
				[ _('Responding'), resp[0], resp[1] ],
				[ _('Last reply'), h.atPort.lastReplyAgoSec == null ? '—' : _('%d s ago').format(h.atPort.lastReplyAgoSec) ]
			]);

			var hook = h.line.hook == null ? [ _('Unknown'), 'av-mut' ] : [ h.line.hook === 'off' ? _('Off hook') : _('On hook'), '' ];
			var cal = yn(h.line.calibrated, _('Done'), _('Not done'));
			var chip = yn(h.line.calibrated, _('Answering'), _('Not answering'));
			fill(line, [
				[ _('Line chip'), chip[0], chip[1] ],
				[ _('Hook (ch%s)').format(count(h.line.channel)), hook[0], hook[1] ],
				[ _('State'), h.line.state || '—' ],
				[ _('Calibration'), cal[0], cal[1] ],
				[ _('Battery'), h.line.batteryMv == null ? '—' : (h.line.batteryMv / 1000).toFixed(1) + ' V', 'av-mono' ],
				[ _('Loop current'), h.line.loopUa == null ? '—' : (h.line.loopUa / 1000).toFixed(1) + ' mA', 'av-mono' ]
			]);

			var sim = yn(h.modem.simReady, _('Ready'), _('Not ready'));
			var reg = yn(h.modem.registered, _('Registered'), _('Not registered'));
			var ims = yn(h.modem.imsRegistered, _('Registered'), _('Not registered'));
			var audio = yn(h.modem.audioSlave, _('Ready'), _('Not set'));
			fill(modem, [
				[ _('SIM'), sim[0], sim[1] ], [ _('Network registration'), reg[0], reg[1] ], [ _('VoLTE registration'), ims[0], ims[1] ],
				[ _('Signal (RSRP)'), h.modem.rsrpDbm == null ? '—' : h.modem.rsrpDbm + ' dBm', 'av-mono' ],
				[ _('Network'), h.modem.network || '—' ],
				[ _('Audio mode'), audio[0], audio[1] ]
			]);

			var mod = yn(h.system.moduleLoaded, _('Loaded'), _('Not loaded')), br = h.system.bridge;
			var run = yn(br.running, _('Running'), _('Stopped'));
			fill(sys, [
				[ _('Kernel module'), mod[0], mod[1] ],
				[ _('Audio bridge'), run[0], run[1] ],
				[ _('Frames carried'), count(br.frames), 'av-mono' ],
				[ _('Resyncs'), count(br.resyncs), 'av-mono' ],
				[ _('Frames skipped'), count(br.txSkipped), 'av-mono' ],
				[ _('Longest gap'), br.maxGapUs == null ? '—' : br.maxGapUs + ' µs', 'av-mono' ]
			]);

			var st = h.storage || {}, rec = st.recordings, msg = st.messages;
			var recOk = folder(rec), msgOk = folder(msg);
			fill(store, [
				[ _('Recordings folder'), rec && rec.path ? rec.path : '—', 'av-mono' ],
				[ _('Recordings'), recOk[0], recOk[1] ],
				[ _('Messages folder'), msg && msg.path ? msg.path : '—', 'av-mono' ],
				[ _('Messages'), msgOk[0], msgOk[1] ]
			]);
			storeNote.textContent = _('Each folder is made and checked when the service starts or reloads, and again before a call writes a file.');

			counters.textContent = '';
			[ [ _('Modem timeouts'), h.counters.modemTimeouts ], [ _('Hook gaps over 20 ms'), h.counters.hookGapsOver20ms ],
				[ _('Longest hook gap (ms)'), h.counters.hookGapMaxMs ], [ _('Faults'), h.counters.faults ] ]
				.forEach(function(s) { counters.appendChild(E('div', { 'class': 'av-tile' }, [ E('small', s[0]), E('b', count(s[1])) ])); });

			logTail.textContent = h.log.tail.length ? h.log.tail.join('\n') : _('No log lines.');
		});

		function card(title, body) {
			return E('div', { 'class': 'cbi-section' }, [ E('h3', title), body ]);
		}

		return E('div', [
			ban, gone,
			E('div', { 'class': 'av-g2' }, [
				card(_('Modem AT port'), at),
				card(_('Line'), line),
				card(_('Modem, SIM and VoLTE'), modem),
				card(_('System'), sys),
				card(_('Storage'), E('div', [ store, storeNote ])),
				card(_('Service'), E('div', [ E('div', { 'class': 'av-acts' }, [ bStart, bStop, bRestart ]), svcNote ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Counters')),
				counters,
				E('details', [ E('summary', _('Recent log (redacted)')), logTail ])
			])
		]);
	}
});
