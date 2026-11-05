// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require uqr';
'require ui';

/*
 * tab_sip.js - phone tab: the SIP feature's own settings. The mode card asks one question - whether a phone on this
 * network joins the page and the handset - and never names what serves that side; the Service section holds the
 * installed service's own settings (its live config is /etc/config/qmodem_sip, written at the admin's hand, with no
 * defaults shipped and read-only when the config is absent), and the Diagnostics section reads the phone service's
 * health and the installed service's presence facts. The page's own phone does not live here: it is on the Overview
 * tab (webphone.js) and needs no SIP service.
 *
 * The tab is split into two sections so that it never grows into one long column, and every grid folds to one column
 * when the page's container is narrow. Deep links: #tab=sip&sip=service.
 */

var SECTIONS = [ [ 'service', _('Service') ], [ 'diag', _('Diagnostics') ] ];
/* the switch's two sides, in the order the card lists them: the words behind them belong to the page's own door (api.js) */
var SWITCH = [ 'web', 'sip' ];
var SWITCH_TEXT = {
	web: _('SIP off - the page and the handset'),
	sip: _('SIP on - a network phone joins them')
};

/* the URL's own state, so a QA run and a bookmark can land on one section */
function hashGet(key) {
	var m = new RegExp('(?:^|&)' + key + '=([^&]*)').exec(String(location.hash || '').replace(/^#/, ''));

	return m ? decodeURIComponent(m[1]) : '';
}

function hashSet(key, value) {
	var parts = String(location.hash || '').replace(/^#/, '').split('&').filter(function(s) { return s && s.indexOf(key + '=') !== 0; });

	parts.push(key + '=' + encodeURIComponent(value));
	try {
		history.replaceState(null, '', '#' + parts.join('&'));
	} catch (e) { /* the hash is a convenience only */ }
}

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, mode = null, busy = false, health = null, waiting = false, said = null;
		var svc = null, svcBusy = false, svcFailed = false;
		var sub = SECTIONS.some(function(s) { return s[0] === hashGet('sip'); }) ? hashGet('sip') : 'service';

		/* ---- the section switcher ---- */

		var chips = E('div', { 'class': 'av-chips cbi-tabmenu', 'role': 'group', 'aria-label': _('SIP sections') });
		var panes = {};

		function setSub(next) {
			sub = next;
			hashSet('sip', next);
			drawSub();
		}

		function drawSub() {
			chips.textContent = '';
			SECTIONS.forEach(function(s) {
				chips.appendChild(E('button', { 'class': 'av-chip', 'type': 'button', 'aria-pressed': s[0] === sub ? 'true' : 'false',
					'click': function() { setSub(s[0]); } }, s[1]));
			});
			Object.keys(panes).forEach(function(k) { panes[k].hidden = k !== sub; });
		}

		/* ---- the mode card (Service) ---- */

		var sel = u.select([ [ 'web', SWITCH_TEXT.web ], [ 'sip', SWITCH_TEXT.sip ] ], 'web');
		sel.id = 'av-mode-sel';
		var modeErr = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		var saveBtn = u.button(_('Save and apply'), '', 'cbi-button-apply important', saveMode);
		/* one line for the chosen side: the two are read one at a time, not all at once */
		var MODE_DESC = {
			web: _('SIP off: the page and the handset use the line. The page is a phone of its own, so this works with the handset down, and with no handset at all.'),
			sip: _('SIP on: a phone on your own network rings and dials beside the page and the handset. Set it up below.')
		};
		var modeDesc = E('div', { 'class': 'av-note' });
		/* the switch is this package's setting and the service is another program: the admin is told both, rather
		 * than one word that covers the other */
		var modeSvcNote = E('div', { 'class': 'av-note', 'hidden': true });
		var SWITCH_SAID = {
			started: _('The network phone service was started.'),
			stopped: _('The network phone service was stopped.'),
			absent: _('The side is set. No SIP service is installed here, so there is no network phone to start until one is.'),
			denied: _('The side is set, but this account may not start or stop the SIP service.'),
			failed: _('The side is set, but the SIP service did not answer: start or stop it yourself.'),
			waiting: _('The side is set. The line is in a call, so the SIP service moves as soon as the call ends.')
		};
		var modeChip = E('span', { 'class': 'av-statuschip static ok' }, [ E('span', { 'class': 'dot' }), E('span', _('in use')) ]);
		sel.addEventListener('change', drawMode);

		/* what became of the installed service is said right after the switch moved; on any other read the line stays
		 * quiet unless the service and the side disagree - asked for and not running, or not asked for and still
		 * running - which is a start or a stop that did not take, and one press of this card's own button settles */
		/* is the installed service up? the bus answer first, and its pidfile beside it: a stopped service leaves no
		   object behind to ask, while a running one always has a live direction process */
		function svcRun() {
			var st = svc && svc.service;

			if (st)
				return !!(st.inbound.running || st.outbound.running);
			return !!(svc && svc.diag && (svc.diag.inboundPid > 0 || svc.diag.outboundPid > 0));
		}

		/* the start script is the installed program: a service can answer on the bus and still have no config file of
		   its own, and a config file with no program is the other half of the same truth */
		function svcThere() {
			return !!((svc && svc.diag && svc.diag.script) || (svc && svc.available && !svcFailed));
		}

		/* the switch and the installed service disagree when one is asked for and the other is not running, or the
		   other way round: the router then needs the press even though the side reads as chosen, so neither the
		   chip, the note nor the button is decided on the stored word alone */
		function switchOut() {
			if (mode == null || !svc || svcFailed)
				return false;
			return (mode === 'sip') !== (svcThere() && svcRun());
		}

		function drawModeSvcNote(what) {
			var asked = mode === 'sip';

			if (what != null && SWITCH_SAID[what]) {
				modeSvcNote.hidden = false;
				modeSvcNote.textContent = SWITCH_SAID[what];
				return;
			}
			/* the note is a disagreement only: the service running with the side on, and quiet with the side off,
			   are both what the switch asked for, and a card that explains a settled state is noise */
			modeSvcNote.hidden = !switchOut();
			modeSvcNote.textContent = modeSvcNote.hidden ? ''
				: (!asked
					? _('No network phone is asked for, but the installed service is still running. Save and apply stops it.')
					: (!svcThere()
						/* nothing is installed to answer one: the side is stored and the page says what is missing */
						? _('A network phone is asked for, but no SIP service is installed on this router. Save and apply starts one as soon as it is installed.')
						: _('A network phone is asked for, but the installed service is not running. Save and apply starts it.')));
		}

		/* the switch and the installed service disagree when one is asked for and the other is not running, or the
		 * other way round: the router then needs the press even though the side reads as chosen, so the button is
		 * not disabled on the word alone */
		function switchDisagrees() {
			var st = svc && svc.available && svc.service;
			var run = !!(st && (st.inbound.running || st.outbound.running));

			if (!svc || svcFailed || mode == null)
				return false;
			return (mode === 'sip') !== run;
		}

		function drawMode(what) {
			modeDesc.textContent = MODE_DESC[sel.value] || '';
			drawModeSvcNote(what);
			if (mode == null) {
				modeChip.hidden = true;
				saveBtn.disabled = true;
				return;
			}
			var pending = mode !== sel.value, out = switchOut();

			modeChip.hidden = false;
			modeChip.className = 'av-statuschip static ' + ((pending || out) ? 'unk' : 'ok');
			modeChip.lastChild.textContent = (pending || out) ? _('not applied yet') : _('in use');
			/* the button waits for a change or for a router that is out of order: the side in use is already the
			   chosen one, and a switch that could not finish while a call was up is finished by the next press */
			saveBtn.disabled = busy ? true : (!(pending || out));
		}

		/* ---- the installed SIP service ---- */

		var svcHost = E('div');

		/* ---- the diagnostics ---- */

		var healthRows = E('div');
		var diagHost = E('div');

		/* ---- state and actions ---- */

		function setBusy(on) {
			busy = on;
			sel.disabled = on;
			if (on)
				saveBtn.classList.add('av-busy');
			else
				saveBtn.classList.remove('av-busy');
			drawMode();
		}

		/* ---- the installed SIP service's card ---- */

		/* the card reads the service's own config, offers the fields an admin decides, and writes them only when
		 * the admin saves. The order on the page: the state at a glance, then the two directions, then the
		 * actions in a bar that stays in reach while the fields are read. */
		function buildServiceCard() {
			svcHost.textContent = '';
			/* the word of a switch that just moved stays until the box agrees with it - the settle reads below clear
			   it - because a card that answers "started" and then, a second later, "not running" says both */
			drawModeSvcNote(said);
			if (!svc) {
				svcHost.appendChild(u.notYet(_('Reading the installed SIP service.')));
				return;
			}
			if (!svc.available) {
				svcHost.appendChild(E('div', { 'class': 'av-note' }, _('There is no installed SIP service config on this router: its settings are shown read-only when it is present, and this card never creates one.')));
				return;
			}

			var c = svc.config, ints = (svc.interfaces || []).slice();

			/* the known interfaces are offered, and a stored name this page cannot see stays selectable */
			[ c.inbound.iface, c.outbound.iface ].forEach(function(n) {
				if (n && ints.indexOf(n) < 0)
					ints.push(n);
			});

			var fields = {
				inEnabled: u.toggle(c.inbound.enabled, _('Allow the calls coming in')),
				inIface: u.select(ints.map(function(n) { return [ n, n ]; }), c.inbound.iface),
				inPort: u.text(String(c.inbound.listenPort), '5060', 'av-mono', 'number'),
				inUser: u.text(c.inbound.username, '', 'av-mono'),
				inPass: u.text('', c.inbound.passwordSet ? _('stored - type to replace') : _('not set'), '', 'password'),
				outEnabled: u.toggle(c.outbound.enabled, _('Allow the calls going out')),
				outIface: u.select(ints.map(function(n) { return [ n, n ]; }), c.outbound.iface),
				outPort: u.text(String(c.outbound.listenPort), '5061', 'av-mono', 'number'),
				outServer: u.text(c.outbound.server, _('sip.example.net'), 'av-mono'),
				outServerPort: u.text(String(c.outbound.port), '5061', 'av-mono av-port', 'number'),
				outUser: u.text(c.outbound.username, '', 'av-mono'),
				outPass: u.text('', c.outbound.passwordSet ? _('stored - type to replace') : _('not set'), '', 'password'),
				outRealm: u.text(c.outbound.realm, '', 'av-mono'),
				outInterval: u.text(String(c.outbound.interval), '300', 'av-mono', 'number')
			};

			var svcErr = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
			var genBtn = u.button(_('Create a new password'), 'repeat', 'cbi-button-action', function() {
				genCredentials(fields.inUser.value);
			});
			genBtn.setAttribute('title', _('The installed service makes a random password and stores it; it is shown once'));
			/* one verb writes these settings, and it says what it does to the service: a second button that only
			   wrote the file would leave the running service on the old values, and the admin could not tell from
			   the label which of the two he needed */
			var saveApply = u.button(_('Save and restart'), '', 'cbi-button-apply important', function() { confirmRestart(); });
			saveApply.setAttribute('title', _('The service restarts, and a call in progress drops'));
			var refresh = u.iconButton(_('Read again'), 'refresh', function() { reloadService(); });

			/* ---- the changes that are not saved yet ---- */

			var dirty = false;
			var dirtyChip = E('span', { 'class': 'av-statuschip unk' }, [ E('span', { 'class': 'dot' }), E('span', _('not saved')) ]);
			var resetBtn = u.button(_('Reset'), '', 'cbi-button-reset', function() { resetFields(); });
			resetBtn.setAttribute('title', _('The fields go back to the values the config file holds'));

			/* the unsaved state stands beside the service's own chips; the reset is a control of the form, so it
			   stays on the page whether or not there is something to undo, the way every other OpenWrt page shows it */
			function drawDirty() {
				dirtyChip.hidden = !dirty && !svcBusy;
				dirtyChip.lastChild.textContent = svcBusy ? _('saving') : _('not saved');
			}

			function markDirty() {
				if (dirty)
					return;
				dirty = true;
				drawDirty();
			}

			function setBusy(on) {
				svcBusy = on;
				saveApply.disabled = on;
				genBtn.disabled = on;
				resetBtn.disabled = on;
				drawDirty();
			}

			/* every control of the form is watched: a change on the page is a change that is not saved yet */
			[ fields.inEnabled.input, fields.inIface, fields.inPort,
				fields.inUser, fields.inPass,
				fields.outEnabled.input, fields.outIface, fields.outPort, fields.outServer, fields.outServerPort,
				fields.outUser, fields.outPass, fields.outRealm, fields.outInterval ].forEach(function(el) {
					el.addEventListener('input', markDirty);
					el.addEventListener('change', markDirty);
				});

			function values() {
				return {
					inbound: {
						enabled: fields.inEnabled.get(), iface: fields.inIface.value,
						listenPort: parseInt(fields.inPort.value, 10), transport: 'udp_tcp',
						username: fields.inUser.value.trim(), password: fields.inPass.value,
						passwordSet: c.inbound.passwordSet
					},
					outbound: {
						enabled: fields.outEnabled.get(), iface: fields.outIface.value,
						listenPort: parseInt(fields.outPort.value, 10), transport: 'tls',
						server: fields.outServer.value.trim(), port: parseInt(fields.outServerPort.value, 10),
						username: fields.outUser.value.trim(), password: fields.outPass.value,
						passwordSet: c.outbound.passwordSet, realm: fields.outRealm.value.trim(),
						interval: parseInt(fields.outInterval.value, 10)
					}
				};
			}

			function saveService() {
				if (svcBusy)
					return;
				svcErr.textContent = '';
				setBusy(true);
				ctx.api.saveSipService(values()).then(function() {
					dirty = false;
					return ctx.api.applySipService().then(function() {
						ctx.toast(_('Saved; the installed SIP service restarted'));
					}, function(e) {
						throw new Error(_('Saved, but the installed SIP service did not restart: %s').format(e.message));
					});
				}).then(function() {
					return loadService();
				}, function(e) {
					svcErr.textContent = e.message;
				}).then(function() {
					setBusy(false);
				});
			}

			/* the restart drops the calls in progress, so the admin is asked before it happens */
			function confirmRestart() {
				if (svcBusy)
					return;
				ui.showModal(_('Restart the installed SIP service?'), [
					E('div', { 'class': 'avp' }, [
						E('div', { 'class': 'av-note' }, _('The values are written first, then the SIP service restarts so they take effect now. A call in progress drops while it restarts.'))
					]),
					E('div', { 'class': 'right' }, [
						E('button', { 'class': 'cbi-button', 'type': 'button', 'click': function() { ui.hideModal(); } }, _('Cancel')),
						E('button', { 'class': 'cbi-button cbi-button-positive important', 'type': 'button',
							'click': function() { ui.hideModal(); saveService(); } }, _('Save and restart'))
					])
				]);
			}

			/* reading the service again drops the changes that are not saved, so the admin is asked */
			function reloadService() {
				if (!dirty) {
					loadService();
					return;
				}
				ui.showModal(_('Read the service again?'), [
					E('div', { 'class': 'avp' }, [
						E('div', { 'class': 'av-note' }, _('The changes that are not saved yet are lost.'))
					]),
					E('div', { 'class': 'right' }, [
						E('button', { 'class': 'cbi-button', 'type': 'button', 'click': function() { ui.hideModal(); } }, _('Cancel')),
						E('button', { 'class': 'cbi-button cbi-button-positive important', 'type': 'button',
							'click': function() { ui.hideModal(); loadService(); } }, _('Read again'))
					])
				]);
			}

			/* the reset is the form's own, so it is always on the page. Nothing was changed: it reads the service
			   again, exactly as the framework's Reset hands the stored values back to the fields. Something was:
			   those edits are lost, so the admin is asked first */
			function resetFields() {
				if (svcBusy)
					return;
				if (!dirty) {
					loadService();
					return;
				}
				ui.showModal(_('Reset the fields?'), [
					E('div', { 'class': 'avp' }, [
						E('div', { 'class': 'av-note' }, _('The fields go back to the values in the installed service\'s config file. Nothing is written to it.'))
					]),
					E('div', { 'class': 'right' }, [
						E('button', { 'class': 'cbi-button', 'type': 'button', 'click': function() { ui.hideModal(); } }, _('Cancel')),
						E('button', { 'class': 'cbi-button cbi-button-remove important', 'type': 'button',
							'click': function() { ui.hideModal(); loadService(); } }, _('Reset'))
					])
				]);
			}

			function genCredentials(username) {
				if (!username) {
					svcErr.textContent = _('Enter the inbound account name first');
					return;
				}
				svcErr.textContent = '';
				setBusy(true);
				ctx.api.generateSipCredentials(username).then(function(r) {
					var code = E('code', { 'class': 'av-mono', 'style': 'display:block;padding:8px;border:1px solid var(--av-bd);border-radius:4px;overflow-wrap:anywhere;user-select:all' }, r.password);
					ui.showModal(_('New SIP credentials'), [
						E('div', { 'class': 'avp' }, [
							u.kv(_('Account'), r.username, 'av-mono'),
							E('div', { 'class': 'av-note' }, _('The password is shown once, and it is stored in the installed SIP service\'s config. Save it now; the service picks it up by itself.')),
							code
						]),
						E('div', { 'class': 'right' }, [
							E('button', { 'class': 'cbi-button cbi-button-positive important', 'type': 'button', 'click': ui.hideModal }, _('Done'))
						])
					]);
					return loadService();
				}, function(e) {
					svcErr.textContent = e.message;
				}).then(function() {
					setBusy(false);
				});
			}

			/* ---- the status chips: what the service reports, one glance ---- */

			var chipsRow = E('div', { 'class': 'av-statuschips' });

			function chip(label, state, text, onclick) {
				return E('button', { 'class': 'av-statuschip ' + (state === true ? 'ok' : state === false ? 'off' : 'unk'),
					'type': 'button', 'click': onclick || null }, [
					E('span', { 'class': 'dot' }),
					E('span', label + ': ' + text)
				]);
			}

			function buildChips() {
				var s = svc.service, d = svc.diag;

				chipsRow.textContent = '';
				chipsRow.appendChild(chip(_('Calls in'), s ? s.inbound.enabled : null,
					s ? (s.inbound.enabled ? (s.inbound.registered ? _('on, registered') : _('on')) : _('off')) : _('unknown'),
					function() { dirIn.scrollIntoView({ block: 'nearest' }); }));
				chipsRow.appendChild(chip(_('Calls out'), s ? s.outbound.enabled : null,
					s ? (s.outbound.enabled ? (s.outbound.registered ? _('on, registered') : _('on')) : _('off')) : _('unknown'),
					function() { dirOut.scrollIntoView({ block: 'nearest' }); }));
				chipsRow.appendChild(chip(_('Password'), s ? s.configured : null,
					s ? (s.configured ? _('set') : _('not set')) : _('unknown'),
					function() { dirIn.scrollIntoView({ block: 'nearest' }); }));
				chipsRow.appendChild(chip(_('Audio link'), d ? d.mediaSocket : null,
					d ? (d.mediaSocket ? _('present') : _('absent')) : _('unknown'),
					function() { setSub('diag'); }));
				chipsRow.appendChild(chip(_('Process'), d ? !!(d.inboundPid || d.outboundPid) : null,
					d ? ((d.inboundPid || d.outboundPid) ? _('running') : _('stopped')) : _('unknown'),
					function() { setSub('diag'); }));
				chipsRow.appendChild(dirtyChip);
			}

			/* ---- one card per direction, always open like every other card on the page: the title row carries
			   the state and the switch, the body the fields ---- */

			function dirCard(which) {
				var inb = which === 'inbound';
				var box = inb ? fields.inEnabled : fields.outEnabled;
				var state = E('span', { 'class': 'av-dir-s' });
				var d;

				function refreshState() {
					if (!box.get()) {
						state.textContent = _('Off - open to set up');
						return;
					}
					state.textContent = inb
						? _('On - port %s, account %s').format(fields.inPort.value, fields.inUser.value.trim() || _('none'))
						: _('On - provider %s').format(fields.outServer.value.trim() || _('none yet'));
				}

				/* the switch is one of the card's controls: moving it redraws the state line beside its own title */
				box.input.addEventListener('change', refreshState);
				[ fields.inPort, fields.inUser, fields.outPort, fields.outServer ].forEach(function(el) {
					el.addEventListener('input', refreshState);
				});

				d = E('div', { 'class': 'cbi-section' }, [
					E('h3', [
						E('span', { 'class': 'av-dir-t' }, inb ? _('Phones on this network call this router') : _('Calls through a SIP provider')),
						E('span', { 'class': 'av-r' }, [ state, box.node ])
					]),
					E('div', { 'class': 'av-dir-b' }, [
						E('div', { 'class': 'av-note' }, inb
							? _('A telephone app on this network calls this router, and the router carries the call out over the mobile line.')
							: _('The router registers with a SIP provider, so calls can come and go over the internet, with the mobile line carrying the voice.')),
						E('div', { 'class': 'av-fgrid' }, inb ? [
							u.gridField(_('Network'), fields.inIface, _('The network those phones call from.')),
							u.gridField(_('SIP port'), fields.inPort, _('The port the phones connect to (5060 by default).')),
							u.gridField(_('Connection type'), E('span', { 'class': 'av-mut' }, _('UDP and TCP')), _('The service fixes this for calls to this router.')),
							u.gridField(_('Account name'), fields.inUser, _('The name the phones sign in with.')),
							u.gridField(_('Password'), fields.inPass,
								_('The password the phones sign in with. Only a new one is written, and the stored one is never shown here. "Save and restart" writes it and starts the service on it.')),
							u.gridField(_('A password made for you'), genBtn,
								_('The service makes a random password, stores it, and shows it once - copy it into the phone then.'))
						] : [
							u.gridField(_('Network'), fields.outIface, _('The network the router uses to reach the provider.')),
							u.gridField(_('SIP port'), fields.outPort, _('The local port for this connection.')),
							u.gridField(_('Provider address'), E('div', { 'class': 'av-inline' }, [ fields.outServer, fields.outServerPort ]),
								_('The server and port your provider gave you.')),
							u.gridField(_('Encryption'), E('span', { 'class': 'av-mut' }, 'TLS'), _('The service fixes TLS for calls through a provider.')),
							u.gridField(_('Account name'), fields.outUser, _('The sign-in name the provider gave you.')),
							u.gridField(_('Password'), fields.outPass, _('Only a new password is written; the stored one is never shown.')),
							u.gridField(_('Realm'), fields.outRealm, _('The provider\'s domain name, when its setup asks for one; often empty.')),
							u.gridField(_('Re-register every (seconds)'), fields.outInterval, _('How often the router tells the provider it is still online (60 to 3600).'))
						])
					])
				]);
				refreshState();
				return d;
			}

			var dirIn = dirCard('inbound'), dirOut = dirCard('outbound');

			/* ---- the page: the state first, then the two directions, then the actions ----
			   The audio path and the RTP range are not offered: the service binds one port fixed at its own
			   build, and the socket is the file this package's daemon serves - an edit to either can only
			   take the call's audio away, silently. */

			svcHost.appendChild(E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('The installed SIP service'), E('span', { 'class': 'av-r' }, refresh) ]),
				E('div', { 'class': 'av-note' }, _('The installed SIP service\'s own settings. AWvoice never installs or configures that package.')),
				chipsRow
			]));
			svcHost.appendChild(dirIn);
			svcHost.appendChild(dirOut);
			svcHost.appendChild(svcErr);
			/* the form's actions are the framework's own action bar: the one verb that writes these settings, and
			   the reset every OpenWrt page shows */
			svcHost.appendChild(E('div', { 'class': 'cbi-page-actions' }, [ saveApply, resetBtn ]));
			buildChips();
			drawDirty();
			drawDiag();
		}

		function onOff(v, on, unk) {
			return v === true ? E('span', { 'class': 'av-ok' }, on) : v === false ? E('span', { 'class': 'av-mut' }, unk || _('No')) : '—';
		}

		function drawDiag() {
			diagHost.textContent = '';
			if (!svc) {
				diagHost.appendChild(E('div', { 'class': 'av-note' }, svcFailed
					? _('The diagnostics could not be read (the rpcd plugin is not answering).')
					: _('Reading the installed SIP service.')));
				return;
			}
			if (!svc.available) {
				diagHost.appendChild(E('div', { 'class': 'av-note' }, _('There is no installed SIP service config on this router.')));
				return;
			}
			if (!svc.diag) {
				diagHost.appendChild(E('div', { 'class': 'av-note' }, _('The diagnostics could not be read (the rpcd plugin is not answering).')));
				return;
			}
			var d = svc.diag, s = svc.service;

			diagHost.appendChild(u.kv(_('Settings file'), E('span', {}, [ E('span', { 'class': 'av-mono' }, '/etc/config/qmodem_sip'), d.config ? '' : ' - ' + _('absent') ])));
			diagHost.appendChild(u.kv(_('Service start script'), E('span', {}, [ E('span', { 'class': 'av-mono' },
				ctx.api.getSipServiceScript()), d.script ? '' : ' - ' + _('absent') ])));
			diagHost.appendChild(u.kv(_('Password file'), E('span', {}, [ E('span', { 'class': 'av-mono' }, '/var/run/qmodem_voip/sip.conf'), d.credentials ? '' : ' - ' + _('absent') ])));
			diagHost.appendChild(u.kv(_('Process, calls in'), E('span', { 'class': 'av-mono' }, d.inboundPid ? 'pid ' + d.inboundPid : _('not running'))));
			diagHost.appendChild(u.kv(_('Process, calls out'), E('span', { 'class': 'av-mono' }, d.outboundPid ? 'pid ' + d.outboundPid : _('not running'))));
			if (s) {
				diagHost.appendChild(u.kv(_('Calls in'), E('span', {}, [
					onOff(s.inbound.enabled, _('enabled'), _('off')), ' - ',
					onOff(s.inbound.running, _('running'), _('stopped')), ' - ',
					onOff(s.inbound.registered, _('registered'), _('not registered'))
				])));
				diagHost.appendChild(u.kv(_('Calls out'), E('span', {}, [
					onOff(s.outbound.enabled, _('enabled'), _('off')), ' - ',
					onOff(s.outbound.running, _('running'), _('stopped')), ' - ',
					onOff(s.outbound.registered, _('registered'), _('not registered'))
				])));
				diagHost.appendChild(u.kv(_('Passwords'), s.configured ? _('set for %s').format(s.username) : _('not set')));
			} else {
				diagHost.appendChild(E('div', { 'class': 'av-note' }, _('The installed service did not answer its own status.')));
			}
		}

		function loadService() {
			return ctx.api.getSipService().then(function(r) {
				svc = r;
				svcFailed = false;
				/* the switch's own state is read from the service too, so the chip and the note move with this
				   reply: a save paints them before it is in, and left alone they keep saying what the box was a
				   moment ago rather than what it is */
				buildServiceCard();
				drawMode();
			}, function(e) {
				svc = null;
				svcFailed = true;
				svcHost.textContent = '';
				svcHost.appendChild(E('div', { 'class': 'av-note' }, e.message));
				drawDiag();
			});
		}

		/* ---- the health words ---- */

		function drawHealth() {
			healthRows.textContent = '';
			/* what is on the line, said in the customer's own words: which side is in use, never the name of whatever
			 * serves the network phone, and read through the same door the switch uses so the two can never disagree */
			healthRows.appendChild(u.kv(_('Line sharing'), health && health.mode != null
				? (ctx.api.sipSide(health.mode) === 'sip' ? _('the page, the handset and a network phone') : _('the page and the handset'))
				: '—'));
			healthRows.appendChild(u.kv(_('Who holds the name (qmodem_voip)'), health && health.objectOwner != null ? health.objectOwner : '—'));
			healthRows.appendChild(u.kv(_('Who holds the voice port'), health && health.portOwner != null ? health.portOwner : '—'));
			healthRows.appendChild(u.kv(_('Last refusal'), health && health.lastRefusal != null ? health.lastRefusal : '—'));
			healthRows.appendChild(u.kv(_('Browser audio'), health && health.audio != null ? health.audio : '—'));
		}

		function loadHealth() {
			return ctx.api.getPhoneHealth().then(function(h) {
				health = h;
				drawHealth();
				drawMode();
				finishWaitingSwitch();
			}, function() {
				health = null;
				drawHealth();
			});
		}

		/* The handover the admin asked for, taken when the line lets it. Two timings belong to it. A service that was
		 * just started or stopped needs a moment before it can report itself at all, so the card reads the box again
		 * shortly after a move - left alone it would keep saying what was true a second ago. And a call can outlast
		 * the click that asked for the switch, so while this page is open the unfinished intent is asked again on a
		 * slow timer rather than left for the admin to press a second time. Both stop as soon as the router is in
		 * order, and neither of them moves the switch: they only read and say. */
		var SETTLE_MS = 1500;
		var SETTLE_MAX = 6;
		var WAIT_AGAIN_MS = 2000;
		var WAIT_AGAIN_MAX = 60;
		var waitAgain = 0;
		var settleLeft = 0;

		function settleSwitch() {
			if (!settleLeft)
				return;
			settleLeft--;
			setTimeout(function() {
				Promise.all([ loadHealth(), loadService() ]).then(function() {
					if (switchOut() || waiting) {
						settleSwitch();
						return;
					}
					settleLeft = 0;
					said = null;
					drawMode();
				});
			}, SETTLE_MS);
		}

		function finishWaitingSwitch() {
			if (!waiting || busy)
				return Promise.resolve();
			if (waitAgain++ >= WAIT_AGAIN_MAX) {
				waiting = false;
				said = 'waiting';
				drawModeSvcNote(said);
				return Promise.resolve();
			}
			return ctx.api.reconcileSipSwitch().then(function(state) {
				if (state === 'waiting') {
					setTimeout(finishWaitingSwitch, WAIT_AGAIN_MS);
					return;
				}
				waiting = false;
				waitAgain = 0;
				if (state === 'in-order')
					return;
				settleLeft = SETTLE_MAX;
				settleSwitch();
				said = state;
				drawMode(state);
				loadService();
			}, function() {
				waiting = false;
				waitAgain = 0;
				said = 'failed';
				drawMode('failed');
			});
		}

		/* ---- the mode ---- */

		function saveMode() {
			var v = sel.value;

			if (busy)
				return;
			modeErr.textContent = '';
			if (SWITCH.indexOf(v) < 0) {
				modeErr.textContent = _('Unknown SIP mode');
				return;
			}
			setBusy(true);
			ctx.api.saveSipSwitch(v).then(function(r) {
				r = r || {};
				mode = v;
				/* a switch the line could not hand over yet is finished on the next read, not left to the admin */
				waiting = r.service === 'waiting';
				waitAgain = 0;
				said = r.service;
				settleLeft = SETTLE_MAX;
				settleSwitch();
				ctx.toast(_('Saved and applied'));
				drawMode(r.service);
				loadHealth();
				loadService();
			}, function(e) {
				modeErr.textContent = e.message;
				ctx.toast(e.message);
			}).then(function() {
				setBusy(false);
			});
		}

		/* the code Linphone scans. That app reads only an address from a code and fetches the account itself,
		   so what this shows is the address of a file this router writes for a few minutes - and the admin is told
		   that plainly, because that file carries the password and is served without a session. */
		var qrBtn = u.button(_('Show a Linphone QR code'), 'grid', 'cbi-button-action', function() { showCode(); });
		qrBtn.setAttribute('title', _('Linphone reads this router\'s address from the code and fetches the account itself'));

		function codeModal(body, buttons) {
			ui.showModal(_('Scan this with Linphone'), [
				E('div', { 'class': 'avp' }, body)
			].concat(buttons ? [ E('div', { 'class': 'right' }, buttons) ] : []));
		}

		function showCode() {
			if (busy || qrBtn.disabled)
				return;
			qrBtn.disabled = true;
			ctx.api.sipProvision().then(function(c) {
				var svg = E('div', { 'class': 'av-qr' });
				var when = new Date(c.expires * 1000);
				svg.innerHTML = uqr.renderSVG(c.url, { pixelSize: 4, whiteColor: 'white', blackColor: 'black', ecc: 'M' });
				codeModal([
					svg,
					E('div', { 'class': 'av-qr-url av-mono' }, c.url),
					u.kv(_('Account'), c.username || '-', 'av-mono'),
					u.kv(_('Realm'), c.realm || '-', 'av-mono'),
					E('div', { 'class': 'av-note' }, _('Linphone reads one address from this code and fetches its account from this router. That address is served without a session for %d minutes, and the account\'s password is in the file it reaches - a device on this network that knew the address could read it while it lasts. It stops being served at %s.').format(
						Math.max(1, Math.round(c.ttl / 60)),
						u.pad2(when.getHours()) + ':' + u.pad2(when.getMinutes()))),
					E('div', { 'class': 'av-note' }, _('If Linphone already has an account for this router, remove it there first - the app offers its scanner only while it holds no account at all.'))
				], [ E('button', { 'class': 'cbi-button', 'type': 'button', 'click': function() { ui.hideModal(); } }, _('Close')) ]);
			}, function(e) {
				codeModal([ E('div', { 'class': 'av-note' }, e.message) ],
					[ E('button', { 'class': 'cbi-button', 'type': 'button', 'click': function() { ui.hideModal(); } }, _('Close')) ]);
			}).then(function() {
				qrBtn.disabled = false;
			});
		}

		var modeCard = E('div', { 'class': 'cbi-section' }, [
			E('h3', [ _('Mode'), E('span', { 'class': 'av-r' }, modeChip) ]),
			E('div', { 'class': 'av-moderow cbi-page-actions' }, [
				E('div', { 'class': 'av-modesel' }, [
					E('label', { 'for': 'av-mode-sel' }, _('Who may use the phone line')),
					sel
				]),
				saveBtn,
				qrBtn
			]),
			modeDesc,
			modeSvcNote,
			modeErr
		]);

		var servicePane = E('div', {}, [ modeCard, svcHost ]);

		var diagPane = E('div', { 'class': 'av-g2' }, [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('Health'), E('span', { 'class': 'av-r' },
					u.iconButton(_('Read again'), 'refresh', function() { loadHealth(); loadService(); })) ]),
				E('div', { 'class': 'av-note' }, _('From the phone service\'s health and the installed service\'s own report.')),
				healthRows,
				E('div', { 'class': 'av-note' }, _('If another program holds the voice port or the qmodem_voip name, the page\'s own phone and the line sharing wait; the handset is unaffected.'))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('The installed SIP service')),
				E('div', { 'class': 'av-note' }, _('Read-only, and never a credential\'s content: what the installed service and its files report right now.')),
				diagHost
			])
		]);

		panes = { service: servicePane, diag: diagPane };

		ctx.api.getSipSettings().then(function(m) {
			mode = m;
			sel.value = (m === 'sip' ? 'sip' : 'web');
			drawMode();
		}, function(e) {
			modeErr.textContent = e.message;
		});
		loadHealth();
		loadService();
		drawSub();
		drawMode();
		buildServiceCard();
		drawDiag();

		return E('div', [ chips, servicePane, diagPane ]);
	}
});
