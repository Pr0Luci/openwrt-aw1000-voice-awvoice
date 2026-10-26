// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';

/*
 * tab_tools.js - Test tools tab: ring test, line test and the loopback test call. Each drives the handset, so the
 * buttons are disabled unless the line is idle with the handset on hook; the backend enforces the same rule. A tool
 * whose backend is not built yet is shown disabled with a note.
 */

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, idle = false, buttons = [];

		var TESTS = [
			{ kind: 'ring', icon: 'bell', title: _('Ring test'), help: _('Rings the handset for 5 seconds. Keep the handset on hook.'),
				ok: _('Ringing for 5 seconds.'), confirm: _('Ring the handset for 5 seconds? Keep the handset on hook.'), ready: ctx.api.available.ringTest },
			{ kind: 'line', icon: 'activity', title: _('Line test'), help: _('Checks the hook state, the battery and the loop current against the values of a healthy line.'),
				ok: _('Line test finished.'), ready: ctx.api.available.lineTest },
			{ kind: 'loopback', icon: 'repeat', title: _('Test call'), help: _('Loopback: you hear your own voice after a short delay.'),
				ok: _('Loopback ready. Lift the handset and speak.'), ready: ctx.api.available.loopbackTest }
		];

		var ban = E('div', { 'class': 'av-ban w' }, [ u.icon('info'), E('div', _('Tests need an idle line with the handset on hook, and a running phone service.')) ]);

		function sync(st) {
			idle = st.state === 'idle';
			buttons.forEach(function(b) { b.btn.disabled = !(idle && b.ready); });
		}
		ctx.on('status', sync);

		function refusal(r) {
			return ({ ACTIVE_CALL: _('A call is active. Hang up first.'), OFF_HOOK: _('The handset is lifted. Put it on hook first.'),
				SERVICE_DOWN: _('The phone service is not running.'), NO_CALL: _('There is no call.') })[r] || _('This test is not available.');
		}

		function card(t) {
			var out = E('div', { 'class': 'av-note', 'role': 'status' });
			function run() {
				out.className = 'av-note';
				out.textContent = _('Running…');
				ctx.api.runTest(t.kind).then(function(r) {
					if (r.ok) {
						out.className = 'av-note av-ok';
						out.textContent = t.ok;
					} else {
						out.className = 'av-note av-warn';
						out.textContent = refusal(r.refused);
					}
				}, function(e) { out.className = 'av-note av-err'; out.textContent = e.message; });
			}
			var btn = u.button(_('Run test'), '', 'cbi-button-action important', function() {
				if (t.confirm)
					editors.confirm(ctx, t.title, t.confirm, _('Run test'), run);
				else
					run();
			}, { 'disabled': true });
			buttons.push({ btn: btn, ready: !!t.ready });
			return E('div', { 'class': 'cbi-section' }, [
				E('h3', [ u.icon(t.icon), E('span', t.title), t.ready ? '' : E('span', { 'class': 'av-r' }, u.tag(_('Not available yet'))) ]),
				E('p', { 'class': 'av-mut' }, t.help),
				btn, out
			]);
		}

		var cards = TESTS.map(card);
		sync(ctx.status);
		return E('div', [ ban, E('div', { 'class': 'av-g3' }, cards) ]);
	}
});
