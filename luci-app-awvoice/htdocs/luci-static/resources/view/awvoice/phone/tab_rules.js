// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_rules.js - Rules tab: phone book, call blocking and alarm (wake-up) calls, each as a
 * sub-section chosen with the chips. These features arrive with a later milestone: while their api methods are not
 * available the tab shows a note and every control is disabled; the code is complete so only the provider changes.
 */

var DELAYS = [ 5, 10, 15, 20, 25, 30 ];       /* seconds; the standard range is 1..30, these are the values networks commonly accept */
var NUMBER_RE = editors.NUMBER_RE;

function networkWord(n) {
	return ({ on: _('Forwarding is on'), off: _('Forwarding is off'), unknown: _('Not known yet') })[n] || _('Not known yet');
}

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, active = 'pb', a = ctx.api.available;
		var ready = !!(a.listContacts && a.getBlocking && a.listAlarms);
		/* no Forwarding pane: this network's registration carries no supplementary-service channel for it
		 * ); the daemon's forwarding code and the stored rule stay until the network offers it again */
		var subs = [ [ 'pb', _('Phone book') ], [ 'bl', _('Call blocking') ], [ 'al', _('Alarm call') ] ];
		var chips = E('div', { 'class': 'av-chips', 'role': 'group', 'aria-label': _('Rule type') });
		var panes = {};

		/* ---------- phone book ---------- */
		var pbList = E('div');
		function drawBook() {
			pbList.textContent = '';
			if (!ctx.contacts.length)
				pbList.appendChild(E('div', { 'class': 'av-empty' }, _('No contacts yet.')));
			ctx.contacts.forEach(function(c) {
				var nums = c.numbers.map(function(n) { return n.value; }).join(' · ');
				pbList.appendChild(E('div', { 'class': 'av-it' }, [
					u.icon('user'),
					E('div', { 'class': 't' }, [ c.name, E('small', { 'class': 'av-mono' }, nums) ]),
					c.quick ? u.tag(_('Speed %s').format(c.quick)) : '',
					u.iconButton(_('Edit'), 'edit', function() { editors.contact(ctx, c, '', drawBook); }),
					u.iconButton(_('Delete'), 'trash', function() {
						editors.confirm(ctx, _('Delete contact'), _('Delete %s from the phone book?').format(c.name), _('Delete'), function() {
							return ctx.api.removeContact(c.id).then(function() { return ctx.refreshContacts(); }).then(drawBook);
						});
					}, 'cbi-button-remove')
				]));
			});
		}
		panes.pb = E('div', { 'class': 'cbi-section' }, [
			E('h3', [ _('Phone book'), E('span', { 'class': 'av-r' }, u.button(_('Add contact'), 'plus', 'cbi-button-action important', function() {
				editors.contact(ctx, null, '', drawBook);
			})) ]),
			pbList,
			E('div', { 'class': 'av-note' }, _('Up to 3 numbers per contact. A speed dial code is 2 digits, dialled with # after them.'))
		]);

		/* ---------- blocking ---------- */
		var blAnon = u.checkbox(false, _('Block calls without caller ID'));
		var blocking = { anonymous: false, incoming: [], incomingPrefix: [], outgoing: [], outgoingPrefix: [] };
		var lists = [
			{ key: 'incoming', title: _('Incoming numbers'), prefix: false }, { key: 'incomingPrefix', title: _('Incoming prefixes'), prefix: true },
			{ key: 'outgoing', title: _('Outgoing numbers'), prefix: false }, { key: 'outgoingPrefix', title: _('Outgoing prefixes'), prefix: true }
		];
		lists.forEach(function(l) { l.out = E('small', { 'class': 'av-mono' }); });
		function drawBlocking() {
			blAnon.set(blocking.anonymous);
			lists.forEach(function(l) { l.out.textContent = blocking[l.key].length ? blocking[l.key].join('   ') : _('None set'); });
		}
		function saveBlocking(patch) {
			var next = Object.assign({}, blocking, patch);
			return ctx.api.saveBlocking(next).then(function(b) { blocking = b; drawBlocking(); ctx.toast(_('Saved')); },
				function(e) { ctx.toast(e.message); drawBlocking(); });
		}
		blAnon.input.addEventListener('change', function() { saveBlocking({ anonymous: blAnon.get() }); });
		panes.bl = E('div', { 'class': 'cbi-section' }, [
			E('h3', _('Call blocking')),
			u.field(_('Anonymous calls'), _('Incoming calls without a caller ID are rejected'), blAnon.node)
		].concat(lists.map(function(l) {
			return u.field(l.title, '', E('div', { 'class': 'av-inline' }, [ l.out, u.button(_('Edit'), 'edit', '', function() {
				editors.list(ctx, l.title, blocking[l.key], l.prefix, function(list) {
					var p = {}; p[l.key] = list;
					return saveBlocking(p);
				});
			}) ]));
		})).concat([
			E('div', { 'class': 'av-note' }, _('Numbers are compared in international form, so 077... and +9477... are the same number.')),
			E('div', { 'class': 'av-note' }, _('Emergency numbers (112, 911 and the national ones) are never blocked.'))
		]));

		/* ---------- alarms ---------- */
		var alList = E('div');
		function drawAlarms() {
			if (!ready) {
				alList.textContent = '';
				alList.appendChild(E('div', { 'class': 'av-empty' }, _('No alarms set.')));
				return;
			}
			ctx.api.listAlarms().then(function(list) {
				alList.textContent = '';
				if (!list.length)
					alList.appendChild(E('div', { 'class': 'av-empty' }, _('No alarms set.')));
				list.forEach(function(al) {
					var on = u.checkbox(al.enabled, '', function(v) {
						ctx.api.saveAlarm(Object.assign({}, al, { enabled: v })).then(drawAlarms, function(e) { ctx.toast(e.message); drawAlarms(); });
					});
					on.input.setAttribute('aria-label', _('%s alarm on').format(al.label));
					alList.appendChild(E('div', { 'class': 'av-it' }, [
						u.icon('clock'),
						E('div', { 'class': 't' }, [ al.label, E('small', al.time + ' · ' + u.summarizeDays(al.days)) ]),
						on.node,
						u.iconButton(_('Edit'), 'edit', function() { editors.alarm(ctx, al, drawAlarms); }),
						u.iconButton(_('Delete'), 'trash', function() {
							editors.confirm(ctx, _('Delete alarm'), _('Delete the %s alarm?').format(al.label), _('Delete'), function() {
								return ctx.api.removeAlarm(al.id).then(drawAlarms);
							});
						}, 'cbi-button-remove')
					]));
				});
			}, function(e) { ctx.toast(e.message); });
		}
		panes.al = E('div', { 'class': 'cbi-section' }, [
			E('h3', [ _('Alarm (wake-up) call'), E('span', { 'class': 'av-r' }, u.button(_('Add alarm'), 'plus', 'cbi-button-action important', function() {
				editors.alarm(ctx, null, drawAlarms);
			})) ]),
			alList
		]);

		/* ---------- chips ---------- */
		function show(id) {
			active = id;
			chips.textContent = '';
			subs.forEach(function(s) {
				chips.appendChild(E('button', { 'class': 'av-chip', 'type': 'button', 'aria-pressed': s[0] === active ? 'true' : 'false',
					'click': function() { show(s[0]); } }, s[1]));
				panes[s[0]].hidden = s[0] !== active;
			});
		}
		var root = E('div', [ u.off([ chips, panes.pb, panes.bl, panes.al ]) ]);
		show('pb');

		ctx.on('contacts', drawBook);
		ctx.on('subrules', show);
		/* a rule saved from a call record (the Calls tab) reaches this view at once */
		ctx.on('blocking', function() {
			ctx.api.getBlocking().then(function(b) { blocking = b; drawBlocking(); }, function() { });
		});

		drawBook();
		drawBlocking();
		drawAlarms();
		if (ready) {
			ctx.api.getBlocking().then(function(b) { blocking = b; drawBlocking(); }, function(e) { ctx.toast(e.message); });
		}
		return root;
	}
});
