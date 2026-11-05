// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.audio as audio';

/*
 * webphone.js - the page's own phone: the call card (state chips, number field and keypad, the two controls, the
 * technical details) and the browser-audio card. It runs on the daemon's own call control and its encrypted audio
 * stream, so it needs no SIP service; the installed SIP service is a separate feature on its own tab.
 *
 * Answering or dialling from this page takes the browser's microphone and speaker by itself. One owner at a time: the
 * handset, the answering machine and this page never share a call's audio.
 */

var STATE_WORDS = {
	disabled: _('Off'), idle: _('Idle'), outgoing_setup: _('Setting up a call'), incoming_ringing: _('Ringing'),
	early_media: _('Early media'), active: _('In call'), terminating: _('Ending'), recovering: _('Recovering'),
	fault: _('Fault')
};
var OWNER_WORDS = { none: _('None'), handset: _('Handset'), machine: _('Answering machine'), web: _('This page'), sip: _('SIP front-end') };
var ENDPOINT_WORDS = { lan_sip: _('SIP over the LAN'), cellular: _('Cellular'), browser: _('This page') };
var MEDIA_WORDS = { ready: _('Ready'), idle: _('Idle') };

/* the line's refusal vocabulary: the word travels with the sentence, so a report from the field reads the same */
var REFUSAL_TEXT = {
	'refused-busy': _('The line already has this call, or someone else answered it.'),
	'refused-nocall': _('There is no call for that.'),
	'refused-revision': _('The call moved on. Look at the state and try again.'),
	'refused-mode': _('The line is not free for this page: another program has it.'),
	'refused-rate': _('The audio rate is not one the line carries.'),
	'refused-port': _('The voice port has another opener.'),
	'refused-object': _('The qmodem_voip name belongs to another program.'),
	'NOT_AVAILABLE': _('The line did not take the request (the number may be blocked, or the service is not ready).')
};

var PAD = [ [ '1', '' ], [ '2', 'ABC' ], [ '3', 'DEF' ], [ '4', 'GHI' ], [ '5', 'JKL' ], [ '6', 'MNO' ],
	[ '7', 'PQRS' ], [ '8', 'TUV' ], [ '9', 'WXYZ' ], [ '*', '' ], [ '0', '+' ], [ '#', '' ] ];

function count(v) {
	return typeof v === 'number' && isFinite(v) ? String(v) : '—';
}

function word(map, v) {
	if (v == null || v === '')
		return '—';
	return map[v] || String(v);
}

function refusalText(w) {
	return (w ? w + ' - ' : '') + (REFUSAL_TEXT[w] || _('The line did not take the request.'));
}

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, call = null, missing = false, busy = false, health = null;
		var audioEng = null, audioBusy = false, audioReason = '', audioLevel = 0;
		/* a call this page answers or dials is meant to be spoken on here: the audio follows the call by itself, and the
		 * deadline keeps the offer from firing for a call that never connects */
		var autoAudioUntil = 0;
		/* whether this browser may reach the audio port is the browser's own answer: a request to that address
		 * completes only when the browser trusts its certificate, so the page asks and shows the state it is given.
		 * nothing is stored, so nothing can go stale and nothing claims a state the page has not seen */
		var trustState = null, trustProbing = false;

		var callRows = E('div');
		var callOffMsg = E('div', { 'class': 'av-note' }, _('Reading the phone side\'s state.'));
		var callOffCard = E('div', { 'class': 'cbi-section' }, [
			E('h3', _('Call')),
			callOffMsg,
			E('div', { 'class': 'av-acts' }, [
				u.button(_('Open the Health tab'), 'activity', '', function() { ctx.goto('health'); })
			])
		]);
		/* the call's own state at a glance: the chips say where the line is, who is on it and which leg carries the
		 * call; the counters and the engine's words wait in the folded card under the keypad */
		var callChips = E('div', { 'class': 'av-statuschips' });
		var durNode = E('span', { 'class': 'av-mono av-mut' }, '00:00');
		var actErr = E('div', { 'class': 'av-err-msg', 'role': 'alert' });
		var dial = u.text('', _('Number to call'), 'av-mono');
		dial.setAttribute('inputmode', 'tel');
		dial.setAttribute('aria-label', _('Number to call'));
		dial.maxLength = 32;
		var backBtn = E('button', { 'class': 'cbi-button', 'type': 'button', 'aria-label': _('Delete the last digit'), 'title': _('Delete the last digit') }, '⌫');
		/* Two controls, one place each, their label and their action following the state: the primary is Call on an
		 * idle line and Answer while a call rings; the secondary is Reject while it rings, Cancel while an outgoing
		 * attempt sets up, Hang up in a call. A disabled control keeps its slot (the layout never jumps), and each
		 * click reads the state at that moment rather than a handler captured for an older one. */
		var primaryBtn = u.button(_('Call'), 'phone-out', 'cbi-button-action important av-railbtn', onPrimary);
		var secondaryBtn = u.button(_('Hang up'), 'x', 'cbi-button-remove important av-railbtn', onSecondary);
		/* the handset's flash key, in the same column as the call controls, the same width and height as the hang-up:
		 * hold, swap, take the waiting call, take a held call back; a key within the window that follows is a code
		 * (TS 22.030 in-call codes) */
		var flashBtn = u.button(_('Flash'), 'phone', 'cbi-button av-railbtn', onFlash);
		var callRail = E('div', { 'class': 'av-rail', 'role': 'group', 'aria-label': _('Call controls') },
			[ primaryBtn, flashBtn, secondaryBtn ]);
		var pad = E('div', { 'class': 'av-pad', 'role': 'group', 'aria-label': _('Keypad') });
		var padBtns = PAD.map(function(k) {
			var b = E('button', { 'class': 'cbi-button av-key', 'type': 'button', 'aria-label': k[0] + (k[1] ? ' ' + k[1] : ''), 'click': function() { press(k[0]); } }, [
				E('span', { 'class': 'd' }, k[0]),
				k[1] ? E('span', { 'class': 'l' }, k[1]) : ''
			]);
			pad.appendChild(b);
			return b;
		});
		var padHint = E('div', { 'class': 'av-note' });
		/* the number field and its backspace end exactly where the keypad ends; the three buttons share the rail's
		 * full height beside it, one third each */
		var dialCol = E('div', { 'class': 'av-dialcol' }, [
			E('div', { 'class': 'av-dial' }, [ dial, backBtn ]),
			pad
		]);
		var padWrap = E('div', { 'class': 'av-padwrap' }, [ dialCol, callRail ]);
		/* the counters and the engine's words, folded away: a caller needs the state, not the frame counts */
		var callDetailsState = E('span', { 'class': 'av-dir-s' });
		var callDetails = E('details', { 'class': 'av-dir' }, [
			E('summary', {}, [ E('span', { 'class': 'av-dir-t' }, _('Technical details')), callDetailsState ]),
			E('div', { 'class': 'av-dir-b' }, [ callRows ])
		]);
		var callCard = E('div', { 'class': 'cbi-section' }, [
			E('h3', [ _('Call'), E('span', { 'class': 'av-r' }, durNode) ]),
			callChips,
			padWrap,
			padHint,
			actErr,
			callDetails
		]);
		callCard.hidden = true;

		/* ---- the page's browser audio ---- */

		var audioStatus = E('div', { 'class': 'av-note' });
		var audioBar = E('div', { 'class': 'av-meter', 'aria-hidden': 'true' }, [ E('span') ]);
		var audioChip = E('span', { 'class': 'av-statuschip static off' }, [ E('span', { 'class': 'dot' }), E('span', _('idle')) ]);
		var audioBtn = u.button(_('Use this computer\'s microphone and speaker'), 'radio', 'cbi-button-action important', startAudio);
		var muteBtn = u.button(_('Mute'), 'pause', '', toggleMute);
		var stopBtn = u.button(_('Stop audio'), 'x', '', stopAudio);
		/* the one manual step the browser forces: its own certificate prompt for the audio port. it stands above the
		   action, because the browser takes no audio from an address it does not trust yet */
		var trustBtn = u.button(_('Open the audio address (one time)'), 'info', '', openAudioAddr);
		var trustNote = E('div', { 'class': 'av-note' }, _('This browser has not taken the router\'s certificate for the audio port yet. Open the address once and accept it; the browser asks once, and then this step stays out of the way.'));
		var audioCard = E('div', { 'class': 'cbi-section' }, [
			E('h3', [ _('Browser audio'), E('span', { 'class': 'av-r' }, audioChip) ]),
			E('div', { 'class': 'av-note' }, _('The call\'s voice in this computer\'s speaker, and this computer\'s microphone on the line. One owner at a time: while this page has the audio, the handset hears nothing. The page must be open over HTTPS.')),
			audioStatus,
			trustBtn,
			trustNote,
			E('div', { 'class': 'av-acts' }, [ audioBtn, muteBtn, stopBtn ]),
			audioBar
		]);
		audioCard.hidden = true;

		/* the call's own busy: the rail and the keypad wait while an action is in flight */
		function setCallBusy(on) {
			busy = on;
			drawControls();
		}

		function drawNote(text, cls) {
			callOffCard.hidden = false;
			callOffMsg.className = 'av-note' + (cls ? ' ' + cls : '');
			callOffMsg.textContent = text;
			callRows.textContent = '';
			callCard.hidden = true;
			drawRing(false);
			drawAudio();
		}

		/* the label, icon and enabled state of one rail button; rebuilt only when the action changes */
		function labelBtn(btn, icon, text) {
			var key = icon + '|' + text;

			if (btn._label === key)
				return;
			btn._label = key;
			btn.textContent = '';
			btn.appendChild(u.icon(icon));
			btn.appendChild(E('span', text));
			btn.setAttribute('aria-label', text);
		}

		/* the two controls follow the state: the primary is Call or Answer, the secondary Reject, Cancel or Hang up */
		function drawControls() {
			var active = call && call.available && !missing;
			var st = active ? call.state : 'idle';
			var ringing = st === 'incoming_ringing';
			var held = !!(call && call.held), waiting = !!(call && call.waiting);
			var callish = (st !== 'idle' && st !== 'disabled') || held || waiting;
			var live = st === 'active';
			var primOn, secOn, secIcon, secText;

			dial.disabled = !active || busy || callish;
			backBtn.disabled = dial.disabled || !dial.value;
			if (ringing) {
				labelBtn(primaryBtn, 'phone-in', _('Answer'));
				primOn = !busy && call.answerOwner === 'none';
				secIcon = 'phone-miss';
				secText = _('Reject');
				secOn = !busy;
			} else {
				labelBtn(primaryBtn, 'phone-out', _('Call'));
				primOn = !busy && active && st === 'idle' && !held && !waiting && !!dial.value.trim();
				secIcon = 'x';
				secText = st === 'outgoing_setup' ? _('Cancel') : _('Hang up');
				secOn = !busy && active && (st === 'active' || st === 'outgoing_setup' || held || waiting);
			}
			labelBtn(secondaryBtn, secIcon, secText);
			primaryBtn.disabled = !primOn;
			secondaryBtn.disabled = !secOn;
			padBtns.forEach(function(b) { b.disabled = !active || busy || st === 'incoming_ringing' || st === 'outgoing_setup' || st === 'terminating'; });
			/* the flash: a call it can act on, and the keys after it are codes while its window lasts */
			flashBtn.disabled = !active || busy || !(st === 'active' || held || waiting);
			if (active && live && Date.now() < flashUntil)
				padHint.textContent = _('Flash window: 1 ends the call and takes the other · 2 holds, swaps or takes back · 3 joins · 0 refuses the waiting call.');
			else if (active && held)
				padHint.textContent = _('The call is held: Flash takes it back.');
			else if (active && waiting)
				padHint.textContent = _('A call is waiting: Flash takes it.');
			else
				padHint.textContent = !active ? '' : live ? _('In a call: a key sends a tone on the cellular leg.')
					: st === 'idle' ? _('Type or press the keys, then Call.') : _('The keypad waits for the call to connect.');
		}

		function onPrimary() {
			if (busy || !call || !call.available)
				return;
			if (call.state === 'incoming_ringing') {
				autoAudioUntil = Date.now() + 15000;	/* answering here means speaking here */
				act(function() { return ctx.api.webAnswer(); }, _('Answered'));
			} else if (call.state === 'idle')
				doDial();
		}

		function onFlash() {
			if (busy || !call || !call.available)
				return;
			if (!(call.state === 'active' || call.held || call.waiting))
				return;
			flashUntil = Date.now() + 3000;	/* the daemon's window: a key now is a code */
			act(function() { return ctx.api.webFlash(); }, null);
		}

		function onSecondary() {
			if (busy || !call || !call.available)
				return;
			if (call.state === 'incoming_ringing')
				act(function() { return ctx.api.webReject(); }, _('Rejected'));
			else if (call.state === 'active' || call.state === 'outgoing_setup')
				act(function() { return ctx.api.webHangup(); }, _('Call ended'));
		}

		/* a live timer between polls: the call's own duration, counted on from the moment it was read */
		var callAt = 0, callDuration = 0;
		var flashUntil = 0, wasWaiting = false, wasHeld = false;

		function durationNow() {
			if (!call || call.state !== 'active')
				return call ? call.durationSec : 0;
			return callDuration + Math.max(0, Math.floor((Date.now() - callAt) / 1000));
		}

		window.setInterval(function() {
			if (callCard.offsetParent !== null)
				durNode.textContent = u.fmtDuration(durationNow());
		}, 1000);

		/* the chips carry the call's state: the line, the number and the leg that carries the call */
		function infoChip(label, state, text) {
			return E('span', { 'class': 'av-statuschip static ' + (state === true ? 'ok' : state === false ? 'off' : 'unk') },
				[ E('span', { 'class': 'dot' }), E('span', label + ': ' + text) ]);
		}

		function drawCallChips() {
			callChips.textContent = '';
			if (!call || !call.available)
				return;
			var st = call.state;

			callChips.appendChild(infoChip(_('State'),
				st === 'active' ? true : (st === 'idle' || st === 'disabled') ? false : null,
				call.held && st === 'idle' ? _('Held')
					: st === 'incoming_ringing' && bellHeld() ? _('Call held quiet') : word(STATE_WORDS, st)));
			if (call.withheld && call.numberPresent)
				callChips.appendChild(infoChip(_('Number'), null, _('Withheld')));
			else if (call.number)
				callChips.appendChild(infoChip(_('Number'), true, call.number));
			if (st !== 'idle' && st !== 'disabled')
				callChips.appendChild(infoChip(_('Path'), null, word(ENDPOINT_WORDS, call.endpoint)));
			if (call.held)
				callChips.appendChild(infoChip(_('Held call'), null, _('yes')));
			if (call.waiting)
				callChips.appendChild(infoChip(_('Waiting call'), null, _('yes')));
		}

		/* quiet hours hold the bell: the daemon has said the handset is deliberately not driven for this caller, and
		 * this page is another side of the same phone, so its own ring stays off too. Only the daemon's own word
		 * counts - a reply that says nothing about quiet (an older service, a line out of service) leaves the page
		 * ringing exactly as it did before quiet hours existed. */
		function bellHeld() {
			return !!ctx.status && ctx.status.quiet === true;
		}

		function drawStatus() {
			callCard.hidden = false;
			callOffCard.hidden = true;
			callRows.textContent = '';
			drawRing(call.state === 'incoming_ringing' && !bellHeld());
			/* a second caller is waiting: the page says so by ear too, once, gesture-gated like the ring */
			if (call.waiting && !wasWaiting)
				ringNow();
			wasWaiting = call.waiting;
			/* the call comes back from hold: this page speaks it again without a second press */
			if (wasHeld && !call.held && call.state === 'active' && call.answerOwner === 'web' && !audioEng)
				autoAudioUntil = Date.now() + 5000;
			wasHeld = call.held;
			callAt = Date.now();
			callDuration = call.durationSec;
			durNode.textContent = u.fmtDuration(durationNow());
			drawCallChips();
			callDetailsState.textContent = _('media') + ': ' + word(MEDIA_WORDS, call.mediaEngine);
			callRows.appendChild(u.kv(_('Answer owner'), word(OWNER_WORDS, call.answerOwner)));
			callRows.appendChild(u.kv(_('Revision'), count(call.revision), 'av-mono'));
			callRows.appendChild(u.kv(_('Frames to the audio side'), count(call.captureFrames), 'av-mono'));
			callRows.appendChild(u.kv(_('Frames from the audio side'), count(call.playbackFrames), 'av-mono'));
			callRows.appendChild(u.kv(_('Dropped frames'), count(call.dropCount), 'av-mono'));
			/* a call that stops being this page's takes the microphone with it, at once */
			if (audioEng && (call.state !== 'active' || call.answerOwner !== 'web')) {
				var e = audioEng;

				audioEng = null;
				audioReason = _('The call is no longer this page\'s: the audio stopped.');
				e.stop();
			}
			drawControls();
			drawAudio();
		}

		function refreshStatus() {
			return ctx.api.getCallStatus().then(function(st) {
				if (!st.available) {
					missing = true;
					call = null;
					drawNote(_('The phone side is not running.'), 'av-err');
					drawControls();
					return;
				}
				missing = false;
				call = st;
				drawStatus();
			}, function(e) {
				call = null;
				drawNote(e.message, 'av-err');
				drawControls();
			});
		}

		/* one action at a time; the refusal word is shown with its sentence, and the panel is re-read either way */
		function act(fn, okText) {
			if (busy)
				return;
			actErr.textContent = '';
			setCallBusy(true);
			fn().then(function(r) {
				if (!r.ok)
					actErr.textContent = refusalText(r.refused);
				else if (okText)
					ctx.toast(okText);
				return refreshStatus();
			}, function(e) {
				actErr.textContent = e.message;
			}).then(function() {
				setCallBusy(false);
			});
		}

		function doDial() {
			if (busy || dial.disabled)
				return;
			var n = dial.value.trim();
			if (!n)
				return;
			autoAudioUntil = Date.now() + 60000;	/* a call placed here is spoken here */
			act(function() {
				return ctx.api.webOriginate(n).then(function(r) {
					if (r.ok)
						dial.value = '';
					return r;
				});
			}, _('Calling'));
		}

		/* idle: the keys build the number; in a call: a key is a tone */
		function press(key) {
			if (!call || !call.available || busy || missing)
				return;
			if (call.state === 'active')
				act(function() { return ctx.api.webDtmf(key); }, null);
			else if (call.state === 'idle') {
				if (dial.value.length < dial.maxLength)
					dial.value += key;
				drawControls();
			}
		}

		function backspace() {
			dial.value = dial.value.slice(0, -1);
			drawControls();
		}

		dial.addEventListener('input', function() { drawControls(); });
		dial.addEventListener('keydown', function(ev) {
			if (ev.key === 'Enter') {
				ev.preventDefault();
				doDial();
			}
		});
		backBtn.addEventListener('click', backspace);

		/* ---- browser audio ---- */

		function audioGate() {
			var w = health && health.audio;

			if (w == null)
				return _('The audio listener\'s state is not known yet.');
			if (w === 'listening' || w === 'connected')
				return null;
			return _('The audio listener is not up (%s).').format(w);
		}

		function drawAudio() {
			var gate = audioGate(), live = audioEng && audioEng.live;
			var active = call && call.available && call.state === 'active' && call.answerOwner === 'web';
			var port = health && health.port;

			/* the port's own page is the check: a browser that trusts the certificate completes the request, one
			   that does not fails it at the handshake without a prompt. the step waits for that answer */
			if (trustState == null && !gate && audio.supported() && port > 0)
				probeTrust();
			var trustable = audio.supported() && port > 0 && !live && !gate && trustState === false;

			audioCard.hidden = !(call && call.available) || missing;
			/* the step and its note stand and go together: the note explains the button beside it, never the page */
			trustBtn.hidden = !trustable;
			trustNote.hidden = !trustable;
			/* the start is offered only while the page holds nothing: once it holds the audio, Stop is the control.
			   a call taken or placed here starts the audio itself, so the button is shown only when it can do something */
			audioBtn.disabled = audioBusy || !!audioEng || !active || !audio.supported() || !!gate;
			audioBtn.hidden = audioBtn.disabled;
			/* the session controls belong to a session: they stand only while the page holds the audio */
			muteBtn.hidden = !audioEng;
			stopBtn.hidden = !audioEng;
			muteBtn.disabled = !live;
			muteBtn.lastChild.textContent = audioEng && audioEng.muted ? _('Unmute') : _('Mute');
			audioBar.hidden = !live;
			audioBar.firstChild.style.width = Math.round(Math.min(1, Math.max(0, audioLevel)) * 100) + '%';
			audioChip.className = 'av-statuschip static ' + (live ? 'ok' : audioEng ? 'unk' : 'off');
			audioChip.lastChild.textContent = live ? _('live') : audioEng ? _('starting') : _('idle');
			if (!audio.supported())
				audioStatus.textContent = _('This page cannot offer browser audio: it needs an HTTPS page, a microphone and the AudioWorklet API.');
			else if (live)
				audioStatus.textContent = _('Live: the call\'s voice is in the speaker, and the microphone is the line\'s.');
			else if (audioReason && (active || audioEng))
				audioStatus.textContent = audioReason;
			else if (gate)
				audioStatus.textContent = gate;
			else
				audioStatus.textContent = active ? _('Ready: this page may take the call\'s audio.') : '';

			/* a call answered or dialled from this page is spoken here: the audio follows it without a second press */
			if (autoAudioUntil) {
				if (Date.now() > autoAudioUntil)
					autoAudioUntil = 0;
				else if (active && !audioEng && !audioBusy && audio.supported() && !gate) {
					autoAudioUntil = 0;
					startAudio();
				}
			}
		}

		/* the page's own ring: a short double burst every four seconds while the line rings. a browser keeps sound silent
		 * until the page has had a gesture, so the context is made on the first one; before that the chip is the only
		 * sign, which is honest: no sound was allowed yet */
		var ringCtx = null, ringTimer = 0;

		function ringBurst(when) {
			var o = ringCtx.createOscillator(), g = ringCtx.createGain();

			o.type = 'sine';
			o.frequency.value = 1000;
			o.connect(g);
			g.connect(ringCtx.destination);
			g.gain.setValueAtTime(0, when);
			g.gain.linearRampToValueAtTime(0.12, when + 0.02);
			g.gain.setValueAtTime(0.12, when + 0.38);
			g.gain.linearRampToValueAtTime(0, when + 0.4);
			o.start(when);
			o.stop(when + 0.42);
		}

		function ringNow() {
			if (!ringCtx || ringCtx.state !== 'running')
				return;
			ringBurst(ringCtx.currentTime);
			ringBurst(ringCtx.currentTime + 0.6);
		}

		function armRing() {
			if (ringCtx) {
				if (ringCtx.state === 'suspended')
					ringCtx.resume();
				return;
			}
			ringCtx = new window.AudioContext();
		}
		document.addEventListener('pointerdown', armRing, { 'once': true });
		document.addEventListener('keydown', armRing, { 'once': true });

		function drawRing(ringing) {
			if (!ringing || !ringCtx) {
				if (ringTimer) {
					window.clearInterval(ringTimer);
					ringTimer = 0;
				}
				return;
			}
			if (ringTimer)
				return;
			ringNow();
			ringTimer = window.setInterval(ringNow, 4000);
		}

		function onAudioEvent(ev) {
			var held;

			if (ev.type === 'level') {
				/* the bar holds the loudest recent peak and falls off over about a second: the capture posts one
				   peak per 20 ms frame, so a single loud sample would otherwise flash past unseen. The same decay
				   step audio-level.js names for its own meter helper is pinned to it in the page tests. */
				held = ev.level > audioLevel ? Math.min(1, ev.level) : audioLevel * 0.985;
				audioLevel = held < 0.0001 ? 0 : held;
				audioBar.firstChild.style.width = Math.round(Math.min(1, Math.max(0, audioLevel)) * 100) + '%';
				return;
			}
			if (ev.type === 'refused' && ev.word)
				audioReason = refusalText(ev.word);
			else if (ev.type === 'closed' || ev.type === 'stopped') {
				audioEng = null;
				audioReason = ev.reason || '';
			}
			drawAudio();
		}

		/* one request to the audio port's own page. a browser that trusts the port's certificate completes it; one
		   that does not fails it at the handshake, without a prompt. the answer is the state, nothing is stored */
		function probeTrust() {
			var port = health && health.port;
			var host = (health && health.address) || window.location.hostname;

			if (trustProbing || !audio.supported() || !(port > 0))
				return;
			trustProbing = true;
			window.fetch('https://' + host + ':' + port + '/', { mode: 'no-cors', cache: 'no-store' })
				.then(function() { trustState = true; }, function() { trustState = false; })
				.then(function() { trustProbing = false; drawAudio(); });
		}

		/* back from the address tab: the certificate is the browser's decision, so ask the browser again */
		window.addEventListener('focus', function() {
			if (trustState === false && trustBtn.offsetParent !== null)
				probeTrust();
		});

		/* the note page on the audio port: the browser's certificate prompt needs a top-level visit to that address */
		function openAudioAddr() {
			var port = health && health.port;
			var host = (health && health.address) || window.location.hostname;

			if (port > 0) {
				trustState = null;
				window.open('https://' + host + ':' + port + '/', '_blank', 'noopener');
			}
		}

		function startAudio() {
			if (audioBusy || audioEng || !call || call.state !== 'active')
				return;
			audioBusy = true;
			audioReason = '';
			drawAudio();
			ctx.api.webAudioOpen(call.revision).then(function(r) {
				if (!r.ok) {
					audioReason = refusalText(r.refused);
					return null;
				}
				return audio.start({ token: r.token, port: r.port, host: (health && health.address) || null,
					onEvent: onAudioEvent }).then(function(res) {
					if (res.ok) {
						audioEng = res.engine;
						audioReason = '';
						trustState = true;
					} else if (res.refused === 'no-microphone') {
						audioReason = res.reason;
					} else if (!res.refused) {
						/* the browser got no stream at all: ask it about the port again, so the step stands only if
						   the browser itself says the port is out of reach */
						trustState = null;
						audioReason = res.reason || '';
					} else {
						audioReason = refusalText(res.refused);
					}
				});
			}, function(e) {
				audioReason = e.message;
			}).then(function() {
				audioBusy = false;
				drawAudio();
			});
		}

		function toggleMute() {
			if (!audioEng)
				return;
			audioEng.mute(!audioEng.muted);
			drawAudio();
		}

		function stopAudio() {
			var e = audioEng;

			audioEng = null;
			audioReason = '';
			if (e)
				e.stop();
			drawAudio();
		}

		/* the audio gate reads the daemon's own word: the listener's state and its port */
		var healthAt = 0;

		function loadHealth() {
			healthAt = Date.now();
			return ctx.api.getPhoneHealth().then(function(h) {
				health = h;
				drawAudio();
			}, function() {
				health = null;
				drawAudio();
			});
		}

		ctx.on('status', function() {
			refreshStatus();
			if (Date.now() - healthAt > 5000)
				loadHealth();
		});

		refreshStatus();
		loadHealth();

		return {
			call: E('div', [ callCard, callOffCard ]),
			audio: audioCard
		};
	}
});
