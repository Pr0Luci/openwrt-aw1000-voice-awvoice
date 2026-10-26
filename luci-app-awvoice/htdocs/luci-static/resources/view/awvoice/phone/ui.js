// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';

/*
 * ui.js - small presentation helpers shared by the tabs of the phone page.
 * No data access here; phone/api.js is the only door to data.
 */

var PHONE = '<path d="M22 16.92v3a2 2 0 0 1-2.18 2 19.79 19.79 0 0 1-8.63-3.07 19.5 19.5 0 0 1-6-6 19.79 19.79 0 0 1-3.07-8.67A2 2 0 0 1 4.11 2h3a2 2 0 0 1 2 1.72 12.84 12.84 0 0 0 .7 2.81 2 2 0 0 1-.45 2.11L8.09 9.91a16 16 0 0 0 6 6l1.27-1.27a2 2 0 0 1 2.11-.45 12.84 12.84 0 0 0 2.81.7A2 2 0 0 1 22 16.92z"/>';

var ICONS = {
	phone: PHONE,
	'phone-in': PHONE + '<polyline points="16 2 16 8 22 8"/><line x1="23" y1="1" x2="16" y2="8"/>',
	'phone-out': PHONE + '<polyline points="23 7 23 1 17 1"/><line x1="16" y1="8" x2="23" y2="1"/>',
	'phone-miss': PHONE + '<line x1="23" y1="1" x2="17" y2="7"/><line x1="17" y1="1" x2="23" y2="7"/>',
	'phone-block': PHONE + '<circle cx="19.5" cy="4.5" r="3.5"/><line x1="17" y1="7" x2="22" y2="2"/>',
	grid: '<rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/>',
	filter: '<polygon points="22 3 2 3 10 12.46 10 19 14 21 14 12.46 22 3"/>',
	sliders: '<line x1="4" y1="21" x2="4" y2="14"/><line x1="4" y1="10" x2="4" y2="3"/><line x1="12" y1="21" x2="12" y2="12"/><line x1="12" y1="8" x2="12" y2="3"/><line x1="20" y1="21" x2="20" y2="16"/><line x1="20" y1="12" x2="20" y2="3"/><line x1="1" y1="14" x2="7" y2="14"/><line x1="9" y1="8" x2="15" y2="8"/><line x1="17" y1="16" x2="23" y2="16"/>',
	activity: '<polyline points="22 12 18 12 15 21 9 3 6 12 2 12"/>',
	tool: '<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/>',
	plus: '<line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/>',
	edit: '<path d="M17 3a2.83 2.83 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/>',
	x: '<line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/>',
	trash: '<polyline points="3 6 5 6 21 6"/><path d="M19 6l-2 14a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2L5 6"/><path d="M10 11v6M14 11v6"/><path d="M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2"/>',
	download: '<path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/>',
	user: '<path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>',
	ban: '<circle cx="12" cy="12" r="10"/><line x1="4.93" y1="4.93" x2="19.07" y2="19.07"/>',
	book: '<path d="M4 19.5A2.5 2.5 0 0 1 6.5 17H20"/><path d="M6.5 2H20v20H6.5A2.5 2.5 0 0 1 4 19.5v-15A2.5 2.5 0 0 1 6.5 2z"/>',
	play: '<polygon points="5 3 19 12 5 21 5 3"/>',
	stop: '<rect x="6" y="6" width="12" height="12"/>',
	refresh: '<polyline points="23 4 23 10 17 10"/><path d="M20.49 15a9 9 0 1 1-2.12-9.36L23 10"/>',
	clock: '<circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/>',
	bell: '<path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.73 21a2 2 0 0 1-3.46 0"/>',
	repeat: '<polyline points="17 1 21 5 17 9"/><path d="M3 11V9a4 4 0 0 1 4-4h14"/><polyline points="7 23 3 19 7 15"/><path d="M21 13v2a4 4 0 0 1-4 4H3"/>',
	radio: '<circle cx="12" cy="12" r="2"/><path d="M16.24 7.76a6 6 0 0 1 0 8.49m-8.48-.01a6 6 0 0 1 0-8.49m11.31-2.82a10 10 0 0 1 0 14.14m-14.14 0a10 10 0 0 1 0-14.14"/>',
	info: '<circle cx="12" cy="12" r="10"/><line x1="12" y1="16" x2="12" y2="12"/><line x1="12" y1="8" x2="12.01" y2="8"/>',
	check: '<path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><polyline points="22 4 12 14.01 9 11.01"/>',
	alert: '<path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"/><line x1="12" y1="9" x2="12" y2="13"/><line x1="12" y1="17" x2="12.01" y2="17"/>'
};

return baseclass.extend({
	icon: function(name, cls) {
		var s = E('span', { 'class': 'av-i' + (cls ? ' ' + cls : ''), 'aria-hidden': 'true' });
		s.innerHTML = '<svg viewBox="0 0 24 24" focusable="false">' + (ICONS[name] || '') + '</svg>';
		return s;
	},

	pad2: function(n) {
		return (n < 10 ? '0' : '') + n;
	},

	/* seconds -> mm:ss (or h:mm:ss) */
	fmtDuration: function(sec) {
		sec = Math.max(0, Math.floor(+sec || 0));
		var h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
		return (h ? h + ':' + this.pad2(m) : this.pad2(m)) + ':' + this.pad2(s);
	},

	/* epoch seconds -> "Today 09:42", "Yesterday 19:03", "Mon 20:31", "12 Sep 07:45" */
	fmtWhen: function(ts) {
		var d = new Date(ts * 1000), now = new Date(), y = new Date(now);
		y.setDate(y.getDate() - 1);
		var hm = this.pad2(d.getHours()) + ':' + this.pad2(d.getMinutes());
		var day;
		if (d.toDateString() === now.toDateString())
			day = _('Today');
		else if (d.toDateString() === y.toDateString())
			day = _('Yesterday');
		else if (now - d < 6 * 864e5)
			day = d.toLocaleDateString(undefined, { weekday: 'short' });
		else
			day = d.toLocaleDateString(undefined, { day: 'numeric', month: 'short' });
		return day + ' ' + hm;
	},

	tag: function(text) {
		return E('span', { 'class': 'av-tag' }, text);
	},

	/* Icon + label button built on the native LuCI button classes. */
	button: function(label, icon, cls, handler, attrs) {
		var a = Object.assign({ 'class': 'cbi-button av-btn ' + (cls || ''), 'type': 'button', 'click': handler }, attrs || {});
		return E('button', a, [ icon ? this.icon(icon) : '', label ? E('span', label) : '' ]);
	},

	/* Icon-only button; the label is for screen readers and the tooltip. */
	iconButton: function(label, icon, handler, cls) {
		return E('button', {
			'class': 'cbi-button av-btn ' + (cls || ''), 'type': 'button',
			'aria-label': label, 'title': label, 'click': handler
		}, this.icon(icon));
	},

	kv: function(label, value, cls) {
		return E('div', { 'class': 'av-kv' }, [
			E('span', label),
			E('span', { 'class': cls || '' }, value)
		]);
	},

	/* Native CBI row: title + control (+ help). Styled by whichever theme is active. */
	field: function(title, help, control) {
		return E('div', { 'class': 'cbi-value' }, [
			E('label', { 'class': 'cbi-value-title' }, title),
			E('div', { 'class': 'cbi-value-field' }, [
				control,
				help ? E('div', { 'class': 'cbi-value-description' }, help) : ''
			])
		]);
	},

	/* The banner of a part whose backend is not built yet (data contract: "not available yet"). */
	notYet: function(what) {
		return E('div', { 'class': 'av-ban w', 'role': 'status' }, [
			this.icon('info'),
			E('div', [ E('b', _('Not available yet')), what ? E('div', { 'class': 'av-sub' }, what) : '' ])
		]);
	},

	/* Wrap controls so that all of them are disabled together (a fieldset passes `disabled` to its controls). */
	off: function(kids, off) {
		return E('fieldset', { 'class': 'av-fs', 'disabled': off ? true : null }, kids);
	},

	/* LuCI-style checkbox; returns { node, input, get, set }. */
	checkbox: function(value, label, onchange) {
		var input = E('input', { 'type': 'checkbox' });
		input.checked = !!value;
		if (onchange)
			input.addEventListener('change', function() { onchange(input.checked); });
		var node = E('label', { 'class': 'av-inline' }, [ input, label ? E('span', label) : '' ]);
		return {
			node: node, input: input,
			get: function() { return input.checked; },
			set: function(v) { input.checked = !!v; }
		};
	},

	select: function(options, value) {
		var sel = E('select', { 'class': 'cbi-input-select' }, options.map(function(o) {
			return E('option', { 'value': o[0] }, o[1]);
		}));
		sel.value = value;
		return sel;
	},

	text: function(value, placeholder, cls, type) {
		var i = E('input', { 'class': 'cbi-input-text ' + (cls || ''), 'type': type || 'text', 'placeholder': placeholder || '' });
		i.value = value == null ? '' : value;
		return i;
	},

	/* "1,2,3,4,5" (ISO weekdays, 1 = Monday) -> "Mon-Fri" / "Every day" / "Mon, Wed" */
	summarizeDays: function(days) {
		var names = [ _('Mon'), _('Tue'), _('Wed'), _('Thu'), _('Fri'), _('Sat'), _('Sun') ];
		var d = (days || []).slice().sort(function(a, b) { return a - b; });
		if (d.length === 7)
			return _('Every day');
		if (d.length === 0)
			return _('Never');
		var run = d.every(function(v, i) { return i === 0 || v === d[i - 1] + 1; });
		if (run && d.length > 2)
			return names[d[0] - 1] + '–' + names[d[d.length - 1] - 1];
		return d.map(function(v) { return names[v - 1]; }).join(', ');
	}
});
