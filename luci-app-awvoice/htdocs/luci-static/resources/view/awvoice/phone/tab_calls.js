// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_calls.js - Calls tab: call history with type filter, search, date filter, CSV export, clear per type and a
 * detail view (side panel on wide containers, inline under the tapped row on narrow ones; the CSS decides which one
 * is visible, so both are rendered). Under the call list sits the Recordings card: what the daemon kept, played and
 * deleted here.
 *
 * Also exports row() so the Overview "Last calls" list looks the same. The call log is a later milestone: while
 * api.available.listCalls is false the tab shows a note and every control is disabled.
 */

var TYPES = [ 'ALL', 'OUT', 'IN', 'MISS', 'BLOCKED' ];
var ICON = { IN: 'phone-in', OUT: 'phone-out', MISS: 'phone-miss' };
var CLS = { IN: 'av-in', OUT: 'av-out', MISS: 'av-miss' };

function typeLabel(t) {
	return ({ ALL: _('All'), OUT: _('Outgoing'), IN: _('Incoming'), MISS: _('Missed'), BLOCKED: _('Blocked') })[t];
}

/* a blocked call is the phone with the ban sign, in the missed call's red, whatever its direction */
function callIcon(u, c) {
	return c.result === 'blocked' ? u.icon('phone-block', 'av-miss') : u.icon(ICON[c.type], CLS[c.type]);
}

/* BLOCKED is a view on the records' result, not a call type of its own */
function ofCat(c, t) {
	return t === 'ALL' ? true : t === 'BLOCKED' ? c.result === 'blocked' : c.type === t;
}

function resultLabel(r) {
	return ({ answered: _('Answered'), missed: _('Missed'), rejected: _('Rejected'), blocked: _('Blocked'),
		busy: _('Busy'), no_answer: _('No answer'), failed: _('Failed') })[r] || r;
}

/* the network's release cause (3GPP TS 24.008, cause value); 0 = unknown */
function causeLabel(n) {
	var t = ({
		1: _('Number does not exist'), 3: _('No route to the number'), 16: _('Normal call end'), 17: _('Number busy'),
		18: _('No response'), 19: _('No answer'), 21: _('Call rejected'), 27: _('Number out of service'),
		28: _('Number not valid'), 31: _('Normal call end'), 34: _('No line available'), 38: _('Network out of order'),
		41: _('Temporary network failure'), 42: _('Network congestion')
	})[n];
	return t ? t + ' (' + n + ')' : n ? _('Network cause %d').format(n) : '';
}

function csvCell(v) {
	var s = String(v == null ? '' : v);
	if (/^[=+\-@\t\r]/.test(s))
		s = "'" + s;
	return '"' + s.replace(/"/g, '""') + '"';
}

/* a recording's name leads with its epoch (digits and dashes, ending .wav) */
function recWhen(name) {
	var ts = parseInt(String(name).replace(/\.wav$/, '').split('-')[0], 10);
	return isNaN(ts) ? null : ts;
}

function fmtSize(bytes) {
	return bytes >= 1048576 ? (bytes / 1048576).toFixed(1) + ' ' + _('MB') : Math.max(1, Math.round(bytes / 1024)) + ' ' + _('KB');
}

/* the canonical 44-byte WAV header of the daemon's recording core: 16-bit mono PCM, little-endian fields */
/* the recording travels as the exact file on the storage: the chunks are its bytes,
 * header included - the page assembles and never builds a header itself. */

function b64Bytes(b64) {
	var bin = atob(b64), out = new Uint8Array(bin.length), i;
	for (i = 0; i < bin.length; i++)
		out[i] = bin.charCodeAt(i);
	return out;
}

/* the raw length of one base64 chunk: the contract's way to name the next offset */
function b64RawLen(b64) {
	return b64.length / 4 * 3 - (b64.length - b64.replace(/=+$/, '').length);
}

/* ---- call recordings: the files live on the router's storage; the page fetches, plays, saves and deletes ---- */

var recList = null, recBusy = null, recPlaying = null, recAudio = null, recUrl = null, recNotify = null;

/* a recording belongs to the call whose id and start time its name carries */
function recFor(c) {
	if (!recList)
		return null;
	for (var k = 0; k < recList.length; k++) {
		var m = /^(\d+)-\d+\.wav$/.exec(recList[k].name);
		if (m && Math.abs(+m[1] - (c.ts || 0)) < 120)
			return recList[k];
	}
	return null;
}

function recRefresh(ctx) {
	return ctx.api.listRecordings().then(function(list) { recList = list; },
		function(e) { recList = []; ctx.toast(e.message); });
}

/* one headless audio element for the whole tab: redraws move the buttons, never the sound */
function recAudioFor(url) {
	if (!recAudio) {
		recAudio = new window.Audio();
		recAudio.addEventListener('ended', function() { recPlaying = null; if (recNotify) recNotify(); });
	}
	if (recAudio.src !== url)
		recAudio.src = url;
	return recAudio;
}

/* the chunks are fetched one after another until a reply says no more follow */
function recFetchAll(ctx, rec) {
	var parts = [];
	function next(offset) {
		return ctx.api.getRecording(rec.name, offset).then(function(c) {
			parts.push(b64Bytes(c.b64));
			return c.more ? next(c.offset + b64RawLen(c.b64)) : { total: c.total || rec.bytes, parts: parts };
		});
	}
	return next(0);
}

/* the recording's own button: Play or Pause while it sounds, Loading while the chunks travel */
function recButton(ctx, rec) {
	var u = ctx.ui;
	var busy = recBusy === rec.name,
		playing = recPlaying === rec.name && recAudio && !recAudio.paused;

	return u.button(busy ? _('Loading…') : playing ? _('Pause') : _('Play'),
		playing && !busy ? 'pause' : 'play', busy ? 'av-busy cbi-button-action' : 'cbi-button-action',
		function() { recPlayPause(ctx, rec); });
}

function recPlayPause(ctx, rec) {
	if (recBusy === rec.name)
		return;
	if (recPlaying === rec.name && recAudio) {
		if (recAudio.paused)
			recAudio.play();
		else
			recAudio.pause();
		if (recNotify) recNotify();
		return;
	}
	recBusy = rec.name;
	if (recNotify) recNotify();
	recFetchAll(ctx, rec).then(function(wav) {
		recUrl = URL.createObjectURL(new Blob(wav.parts, { type: 'audio/wav' }));
		recAudioFor(recUrl);
		recAudio.play().catch(function() {});
		recPlaying = rec.name;
		recBusy = null;
		if (recNotify) recNotify();
	}, function(e) {
		recBusy = null;
		if (recNotify) recNotify();
		ctx.toast(e.message);
	});
}

function recSave(ctx, rec, btn) {
	if (recBusy === rec.name)
		return;
	recBusy = rec.name;
	if (recNotify) recNotify();
	btn.disabled = true;
	recFetchAll(ctx, rec).then(function(wav) {
		recUrl = URL.createObjectURL(new Blob(wav.parts, { type: 'audio/wav' }));
		var a = E('a', { 'href': recUrl, 'download': rec.name });
		document.body.appendChild(a);
		a.click();
		a.remove();
		setTimeout(function() { URL.revokeObjectURL(recUrl); }, 1000);
		recBusy = null;
		btn.disabled = false;
		if (recNotify) recNotify();
	}, function(e) {
		recBusy = null;
		btn.disabled = false;
		if (recNotify) recNotify();
		ctx.toast(e.message);
	});
}

function recDelete(ctx, rec) {
	if (recPlaying === rec.name || recBusy === rec.name)
		recStop();
	editors.confirm(ctx, _('Delete recording'), _('Delete this recording? This cannot be undone.'), _('Delete'),
		function() {
			ctx.api.deleteRecording(rec.name).then(function() {
				recList = null;
				recRefresh(ctx).then(function() { if (recNotify) recNotify(); });
				if (recNotify) recNotify();
				ctx.toast(_('Recording deleted'));
			}, function(e) { ctx.toast(e.message); });
		});
}

function recStop() {
	if (recAudio) {
		recAudio.pause();
		recAudio.src = '';
	}
	if (recUrl)
		URL.revokeObjectURL(recUrl);
	recUrl = null;
	recPlaying = null;
	recBusy = null;
}

return baseclass.extend({
	row: function(ctx, c, selected, handler) {
		var u = ctx.ui, name = (c.name || ctx.nameFor(c.number)), shown = c.number || _('Withheld');
		return E('button', {
			'class': 'av-row', 'type': 'button', 'data-id': c.id,
			'aria-current': selected ? 'true' : 'false', 'click': handler
		}, [
			callIcon(u, c),
			E('span', { 'class': 'n' }, name || shown),
			E('span', { 'class': 'd' }, u.fmtWhen(c.ts)),
			E('span', { 'class': 'm av-mono' }, name ? c.number : typeLabel(c.type)),
			E('span', { 'class': 't av-mono' }, u.fmtDuration(c.durationSec))
		]);
	},

	render: function(ctx) {
		var self = this, u = ctx.ui;
		var cat = 'ALL', sel = null;

		var chips = E('div', { 'class': 'av-chips', 'role': 'group', 'aria-label': _('Filter by call type') });
		var search = u.text('', _('Search number or name'), 'q');
		search.setAttribute('aria-label', _('Search calls'));
		var from = u.text('', '', '', 'date');
		from.setAttribute('aria-label', _('From date'));
		var exportBtn = u.button(_('Export CSV'), 'download', '', function() { doExport(); });
		var clearBtn = u.button(_('Clear all'), 'trash', 'cbi-button-remove', function() { doClear(); });
		var clearLbl = clearBtn.lastChild;
		var rows = E('div', { 'class': 'av-list-rows' });
		var none = E('div', { 'class': 'av-empty', 'hidden': true }, _('Nothing to show yet.'));
		var detail = E('aside', { 'class': 'cbi-section av-detail', 'aria-live': 'polite' });

		function filtered() {
			var q = search.value.trim().toLowerCase(), f = from.value ? new Date(from.value + 'T00:00:00').getTime() / 1000 : 0;
			return ctx.calls.filter(function(c) {
				if (!ofCat(c, cat)) return false;
				if (f && c.ts < f) return false;
				if (q && c.number.indexOf(q) < 0 && (c.name || ctx.nameFor(c.number)).toLowerCase().indexOf(q) < 0) return false;
				return true;
			});
		}

		function count(t) {
			return ctx.calls.filter(function(c) { return ofCat(c, t); }).length;
		}

		function detailNode(c, closer) {
			if (!c)
				return E('div', { 'class': 'av-empty' }, _('Select a call to see its details.'));
			var name = (c.name || ctx.nameFor(c.number)), acts = [], cause = causeLabel(c.cause);

			/* a withheld number cannot be saved or blocked; "Anonymous calls" in Rules blocks all withheld calls */
			if (c.number) {
				if (!name)
					acts.push(u.button(_('Add to phone book'), 'book', 'cbi-button-action important', function() {
						editors.contact(ctx, null, c.number, function() { ctx.toast(_('Added to phone book')); redraw(); });
					}));
				acts.push(blockBtn(c));
			}

			var rec = recFor(c);

			return E('div', [
				E('h3', [
					callIcon(u, c),
					E('span', c.result === 'blocked' ? _('Blocked call')
						: ({ IN: _('Incoming call'), OUT: _('Outgoing call'), MISS: _('Missed call') })[c.type]),
					E('span', { 'class': 'av-r' }, u.iconButton(_('Close'), 'x', closer))
				]),
				u.kv(_('Number'), c.number || _('Withheld'), 'av-mono'),
				u.kv(_('Name'), name || _('Unknown')),
				u.kv(_('Date'), u.fmtWhen(c.ts)),
				u.kv(_('Duration'), u.fmtDuration(c.durationSec), 'av-mono'),
				u.kv(_('Result'), resultLabel(c.result) + (cause ? ' - ' + cause : '')),
				u.kv(_('Recording'), rec ? u.fmtDuration(rec.seconds) + ' - ' + fmtSize(rec.bytes)
					+ (rec.channels == 2 ? ' - ' + _('stereo') : '') : _('None')),
				E('div', { 'class': 'av-acts' }, acts.concat(rec ? [
					recButton(ctx, rec),
					u.button(_('Save'), 'download', 'cbi-button-action', function() { recSave(ctx, rec); }),
					u.button(_('Delete'), 'trash', 'cbi-button-remove important', function() { recDelete(ctx, rec); })
				] : []))
			]);
		}

		function closeDetail() {
			sel = null;
			redraw();
		}

		function select(id) {
			sel = (sel === id) ? null : id;
			redraw();
		}

		/* the record's direction decides the list: an incoming or missed call toggles the
		 * incoming list, an outgoing call the outgoing list  */
		function blockListFor(c) {
			return c.type === 'OUT' ? 'outgoing' : 'incoming';
		}

		function blockBtnLabel(list, on) {
			return ({ incoming: on ? _('Unblock incoming') : _('Block incoming'),
				outgoing: on ? _('Unblock outgoing') : _('Block outgoing') })[list];
		}

		function toggleBlock(c, btn) {
			var list = blockListFor(c);
			ctx.api.getBlocking().then(function(b) {
				var on = b[list].indexOf(c.number) >= 0;
				b[list] = b[list].filter(function(n) { return n !== c.number; });
				if (!on)
					b[list].push(c.number);
				return ctx.api.saveBlocking(b).then(function() {
					btn.textContent = blockBtnLabel(list, !on);
					ctx.emit('blocking');
					ctx.toast((({ incoming: _('Incoming calls from this number are %s now'),
						outgoing: _('Outgoing calls to this number are %s now') })[list])
						.format(on ? _('allowed again') : _('blocked')));
				});
			}, function(e) { ctx.toast(e.message); });
		}

		/* the label shows the current state of that direction (a number may sit in both lists;
		 * the record toggles only its own) */
		function blockBtn(c) {
			var btn = u.button(_('Block number'), 'ban', 'cbi-button-remove', function() { toggleBlock(c, btn); });
			ctx.api.getBlocking().then(function(b) {
				btn.lastChild.textContent = blockBtnLabel(blockListFor(c), b[blockListFor(c)].indexOf(c.number) >= 0);
			}, function() { /* the label keeps the plain form until the lists can be read */ });
			return btn;
		}

		function doExport() {
			var list = filtered();
			if (!list.length)
				return ctx.toast(_('Nothing to export'));
			var lines = [ [ _('Type'), _('Number'), _('Name'), _('Date'), _('Duration (s)'), _('Result'), _('Network cause') ].map(csvCell).join(',') ];
			list.forEach(function(c) {
				lines.push([ typeLabel(c.type), c.number, (c.name || ctx.nameFor(c.number)),
					new Date(c.ts * 1000).toISOString(), c.durationSec, resultLabel(c.result), c.cause || '' ].map(csvCell).join(','));
			});
			var blob = new Blob([ lines.join('\r\n') + '\r\n' ], { type: 'text/csv' });
			var a = E('a', { 'href': URL.createObjectURL(blob), 'download': 'phone-calls.csv' });
			document.body.appendChild(a);
			a.click();
			document.body.removeChild(a);
			window.setTimeout(function() { URL.revokeObjectURL(a.href); }, 1000);
			ctx.toast(_('Exported %d calls').format(list.length));
		}

		function doClear() {
			if (cat === 'BLOCKED')
				return;		/* the daemon deletes by call type; the blocked view has no delete of its own */
			var what = cat === 'ALL' ? _('Clear all calls from the log? This cannot be undone.')
				: _('Clear all %s calls from the log? This cannot be undone.').format(typeLabel(cat).toLowerCase());
			editors.confirm(ctx, _('Clear call log'), what, _('Clear'), function() {
				return ctx.api.deleteCalls(cat === 'ALL' ? {} : { type: cat }).then(function() {
					sel = null;
					ctx.toast(_('Call log cleared'));
					return ctx.reloadCalls();
				});
			});
		}

		function redraw() {
			chips.textContent = '';
			TYPES.forEach(function(t) {
				chips.appendChild(E('button', { 'class': 'av-chip', 'type': 'button', 'aria-pressed': t === cat ? 'true' : 'false',
					'click': function() { cat = t; redraw(); } }, [ typeLabel(t), E('span', { 'class': 'av-mut' }, String(count(t))) ]));
			});
			clearLbl.textContent = cat === 'ALL' ? _('Clear all') : cat === 'BLOCKED' ? _('Clear all') : _('Clear %s').format(typeLabel(cat).toLowerCase());
			clearBtn.disabled = cat === 'BLOCKED';

			var list = filtered();
			rows.textContent = '';
			list.forEach(function(c) {
				rows.appendChild(self.row(ctx, c, sel === c.id, function() { select(c.id); }));
				if (sel === c.id)
					rows.appendChild(E('div', { 'class': 'av-inl' }, detailNode(c, closeDetail)));
			});
			none.hidden = list.length > 0;
			none.textContent = ctx.calls.length ? _('No calls match. Calls made from the phone show up here.') : _('Nothing to show yet.');

			var chosen = ctx.calls.filter(function(c) { return c.id === sel; })[0] || null;
			detail.textContent = '';
			detail.appendChild(detailNode(chosen, closeDetail));
		}

		search.addEventListener('input', redraw);
		from.addEventListener('change', redraw);
		ctx.on('contacts', redraw);
		ctx.on('select', function(id) { sel = id; redraw(); });
		ctx.on('calls', redraw);
		ctx.on('recordings', redraw);
		recNotify = function() { redraw(); };
		recRefresh(ctx);

		return E('div', [ u.off([
			chips,
			E('div', { 'class': 'av-fl' }, [ search, from, exportBtn, clearBtn ]),
			E('div', { 'class': 'av-split' }, [
				E('div', { 'class': 'cbi-section av-list' }, [ rows, none ]),
				detail
			])
		]) ]);
	}
});
