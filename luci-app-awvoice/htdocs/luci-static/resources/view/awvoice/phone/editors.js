// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require ui';

/*
 * editors.js - modal editors (contact, alarm, number list, speed dial, confirm) shared by the Rules, Calls and Phone
 * settings tabs. Each editor validates input, calls the matching api method and reports the saved record through
 * onSaved(). Rejections from the api are shown inside the modal.
 */

var MAX_NUMBERS = 3;
var CLOCK_WARN_MIN = 5;
var NUMBER_RE = /^\+?[0-9*#]{1,32}$/;
var PREFIX_RE = /^\+?[0-9]{1,32}$/;
var QUICK_RE = /^[0-9]{2}$/;
var TIME_RE = /^([01][0-9]|2[0-3]):([0-5][0-9])$/;

function modal(ctx, title, rows, errNode, onSave, saveLabel) {
	var save = E('button', { 'class': 'cbi-button cbi-button-save important', 'type': 'button' }, saveLabel || _('Save'));
	var cancel = E('button', { 'class': 'cbi-button', 'type': 'button', 'click': ui.hideModal }, _('Cancel'));

	save.addEventListener('click', function() {
		save.disabled = true;
		Promise.resolve(onSave()).then(function(done) {
			save.disabled = false;
			if (done !== false)
				ui.hideModal();
		}, function(err) {
			save.disabled = false;
			errNode.textContent = (err && err.message) || String(err);
		});
	});

	ui.showModal(title, [
		E('div', { 'class': 'avp' }, rows.concat([ errNode ])),
		E('div', { 'class': 'right' }, [ cancel, ' ', save ])
	]);
}

function row(label, control) {
	return E('div', { 'class': 'av-modalrow' }, [ E('label', label), control ]);
}

return baseclass.extend({
	NUMBER_RE: NUMBER_RE,
	QUICK_RE: QUICK_RE,

	/* c = existing contact or null; prefillNumber = number to start a new contact with */
	contact: function(ctx, c, prefillNumber, onSaved) {
		var u = ctx.ui, types = [ [ 'mobile', _('Mobile') ], [ 'home', _('Home') ], [ 'work', _('Work') ] ];
		var name = u.text(c ? c.name : '', _('Name'));
		var quick = u.text(c ? c.quick : '', _('2 digits, optional'), 'av-mono');
		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		var numRows = [];
		var seed = c ? c.numbers : [ { value: prefillNumber || '', type: 'mobile' } ];

		quick.maxLength = 2;

		var numbers = E('div');
		function addNumber(n) {
			if (numRows.length >= MAX_NUMBERS)
				return;
			var val = u.text(n.value, _('Number'), 'av-mono');
			var type = u.select(types, n.type);
			numRows.push({ val: val, type: type });
			numbers.appendChild(E('div', { 'class': 'av-inline' }, [ val, type ]));
		}
		seed.forEach(addNumber);

		var more = u.button(_('Add number'), 'plus', '', function() {
			addNumber({ value: '', type: 'mobile' });
			more.hidden = numRows.length >= MAX_NUMBERS;
		});

		modal(ctx, c ? _('Edit contact') : _('Add contact'), [
			row(_('Name'), name),
			row(_('Numbers (up to %d)').format(MAX_NUMBERS), numbers),
			E('div', { 'class': 'av-modalrow' }, more),
			row(_('Speed dial code'), quick),
			E('div', { 'class': 'av-note' }, _('Dial the two digits and then # to call this contact.'))
		], err, function() {
			err.textContent = '';
			var list = [];
			for (var i = 0; i < numRows.length; i++) {
				var v = numRows[i].val.value.trim();
				if (!v)
					continue;
				if (!NUMBER_RE.test(v)) {
					err.textContent = _('Numbers may contain digits, *, # and a leading +');
					return false;
				}
				list.push({ value: v, type: numRows[i].type.value });
			}
			var q = quick.value.trim();
			if (q && !QUICK_RE.test(q)) {
				err.textContent = _('Speed dial code must be exactly 2 digits');
				return false;
			}
			return ctx.api.saveContact({ id: c ? c.id : 0, name: name.value, numbers: list, quick: q }).then(function(saved) {
				ctx.refreshContacts().then(function() { if (onSaved) onSaved(saved); });
			});
		});
		window.setTimeout(function() { name.focus(); }, 0);
	},

	alarm: function(ctx, a, onSaved) {
		var u = ctx.ui;
		var label = u.text(a ? a.label : '', _('Label'));
		var time = u.text(a ? a.time : '07:00', 'HH:MM', 'av-mono');
		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		var dayNames = [ _('Mon'), _('Tue'), _('Wed'), _('Thu'), _('Fri'), _('Sat'), _('Sun') ];
		var selected = a ? a.days : [ 1, 2, 3, 4, 5 ];
		var boxes = dayNames.map(function(n, i) { return u.checkbox(selected.indexOf(i + 1) >= 0, n); });
		var enabled = u.checkbox(a ? a.enabled : true, _('Alarm is on'));

		/* the alarm rings by the router's clock: show it, and warn when it is far from this computer's clock */
		var clockNote = E('div', { 'class': 'av-note' }, _('Alarms follow the router\'s clock.'));
		ctx.api.getClock().then(function(c) {
			if (!c.routerTime)
				return;
			clockNote.textContent = _('The router\'s time is %s.').format(c.routerTime);
			if (c.diffMin != null && Math.abs(c.diffMin) > CLOCK_WARN_MIN) {
				clockNote.className = 'av-note av-warn';
				clockNote.textContent += ' ' + _('It differs from this computer\'s clock by %d minutes. Check the time synchronisation under System.').format(Math.abs(c.diffMin));
			}
		}, function() { /* the note above stays */ });

		modal(ctx, a ? _('Edit alarm') : _('Add alarm'), [
			row(_('Label'), label),
			row(_('Time (24-hour)'), time),
			row(_('Days'), E('div', { 'class': 'av-days' }, boxes.map(function(b) { return b.node; }))),
			E('div', { 'class': 'av-modalrow' }, enabled.node),
			clockNote
		], err, function() {
			err.textContent = '';
			if (!TIME_RE.test(time.value.trim())) {
				err.textContent = _('Enter the time as HH:MM, for example 06:30');
				return false;
			}
			var days = [];
			boxes.forEach(function(b, i) { if (b.get()) days.push(i + 1); });
			if (!days.length) {
				err.textContent = _('Pick at least one day');
				return false;
			}
			return ctx.api.saveAlarm({
				id: a ? a.id : 0, label: label.value.trim() || _('Alarm'),
				time: time.value.trim(), days: days, enabled: enabled.get()
			}).then(function(saved) { if (onSaved) onSaved(saved); });
		});
	},

	/*
	 * One entry per line: exact numbers (digits, *, # and a leading +) or, with prefix set, number prefixes (digits and
	 * a leading +). There is no pattern language.
	 */
	list: function(ctx, title, current, prefix, onSaved) {
		var area = E('textarea', { 'class': 'cbi-input-textarea av-mono av-ta', 'rows': 6 });
		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		var re = prefix ? PREFIX_RE : NUMBER_RE;
		area.value = current.join('\n');

		modal(ctx, title, [
			row(prefix ? _('One prefix per line') : _('One number per line'), area),
			E('div', { 'class': 'av-note' }, prefix
				? _('A prefix blocks every number that starts with it, for example +9477 or 077.')
				: _('Write numbers in any form: 077... and +9477... are the same number.'))
		], err, function() {
			err.textContent = '';
			var list = area.value.split('\n').map(function(s) { return s.trim(); }).filter(Boolean);
			for (var i = 0; i < list.length; i++) {
				if (!re.test(list[i])) {
					err.textContent = prefix ? _('Not a valid prefix: %s').format(list[i]) : _('Not a valid number: %s').format(list[i]);
					return false;
				}
			}
			return Promise.resolve(onSaved(list));
		});
	},

	/* Pick an existing contact and give it a speed dial code. */
	speedDial: function(ctx, onSaved) {
		var u = ctx.ui;
		var free = ctx.contacts.filter(function(c) { return !c.quick; });
		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		if (!free.length) {
			ctx.toast(_('Every contact already has a speed dial code'));
			return;
		}
		var who = u.select(free.map(function(c) { return [ String(c.id), c.name ]; }), String(free[0].id));
		var slot = u.text('', _('2 digits'), 'av-mono');
		slot.maxLength = 2;

		modal(ctx, _('Add speed dial'), [
			row(_('Contact'), who),
			row(_('Speed dial code'), slot),
			E('div', { 'class': 'av-note' }, _('Dial the two digits and then # to call this contact. Without # the digits are an ordinary number.'))
		], err, function() {
			err.textContent = '';
			var q = slot.value.trim();
			if (!QUICK_RE.test(q)) {
				err.textContent = _('Speed dial code must be exactly 2 digits');
				return false;
			}
			var c = free.filter(function(x) { return String(x.id) === who.value; })[0];
			return ctx.api.saveContact({ id: c.id, name: c.name, numbers: c.numbers, quick: q }).then(function(saved) {
				ctx.refreshContacts().then(function() { if (onSaved) onSaved(saved); });
			});
		});
	},

	confirm: function(ctx, title, message, label, onYes) {
		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		modal(ctx, title, [ E('p', message) ], err, function() { return Promise.resolve(onYes()); }, label);
	}
});
