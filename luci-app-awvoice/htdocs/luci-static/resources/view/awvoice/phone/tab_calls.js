// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_calls.js - Calls tab: call history with type filter, search, date filter, CSV export, clear per type and a
 * detail view (side panel on wide containers, inline under the tapped row on narrow ones; the CSS decides which one
 * is visible, so both are rendered).
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
				E('div', { 'class': 'av-acts' }, acts)
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
