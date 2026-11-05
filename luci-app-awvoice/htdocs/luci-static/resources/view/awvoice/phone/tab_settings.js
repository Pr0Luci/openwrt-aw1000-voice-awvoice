// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';
'require view.awvoice.phone.editors as editors';
'require view.awvoice.phone.greeting as greeting';

/*
 * tab_settings.js - Phone settings tab: line, tones, audio, features, recording, answering machine, speed dial,
 * indicator and advanced options. Edits are held in the form and written with "Save and apply"; "Reset" discards
 * them. The machine's greeting is managed here as a file, never as a path: the status line, Play, Upload and Record
 * talk to the daemon's own methods, and what they show is what the daemon answered.
 * Fields this daemon build does not serve yet are shown disabled with a "Not available yet" tag
 * (api.settingsLive says which are live).
 */

var NUMBER_RE = editors.NUMBER_RE;
var EARPIECE_STEPS = [ 0, 1, 2, 3, 4, 5 ];     /* the range this modem reports for AT+CLVL; the daemon ignores a level outside it */
var REFRESH_STEPS = [ 1, 2, 3, 5, 10, 15, 30 ]; /* live view refresh, seconds; UCI page_refresh_s, read by the page only */

return baseclass.extend({
	render: function(ctx) {
		var u = ctx.ui, live = ctx.api.settingsLive, loaded = null, h = null;

		function num(id, min, max, label) {
			var i = u.text('', '', '', 'number');
			i.min = min; i.max = max;
			i.setAttribute('aria-label', label);
			return i;
		}

		var cid = u.select([ [ 'dtmf', _('DTMF') ], [ 'bellcore', _('FSK (Bellcore)') ], [ 'etsi', _('FSK (ETSI)') ], [ 'off', _('Off') ] ], 'dtmf');
	var cidWhen = u.select([ [ 'ring1', _('After the first ring burst') ], [ 'prering', _('Before the first ring') ] ], 'ring1');
		var myNo = u.text('', _('Number'), 'av-mono wide');
		var pause = num('pause', 3, 15, _('Dial pause in seconds'));
		var flashMin = num('fmin', 20, 500, _('Hook flash minimum in milliseconds'));
		var flashMax = num('fmax', 300, 3000, _('Hook flash maximum in milliseconds'));

		var ringback = u.select([ [ 'auto', _('Automatic') ], [ 'on', _('Always play') ], [ 'off', _('Never play') ] ], 'auto');
		var volume = u.select([ [ '', _('Modem default') ] ].concat(EARPIECE_STEPS.map(function(n) { return [ String(n), String(n) ]; })), '');
		var micGain = num('mic', -99, 99, _('Microphone gain in dB'));
		var earGain = num('ear', -99, 99, _('Earpiece gain in dB'));

		/* the caller-id restriction has no field: this network's registration carries no supplementary-service
		 * channel for it; the stored value is kept untouched until the network offers it again */
		var conf = u.checkbox(false, _('Allow a second call with the flash key'));
	var hd = u.checkbox(false, _('HD voice (wideband 16 kHz)'));
		var wait = u.checkbox(true, _('Call waiting'));
		var hotNo = u.text('', _('Number'), 'av-mono wide');
		var hotDelay = num('hot', 3, 99, _('Hotline delay in seconds'));
		var led = u.select([ [ '', _('None') ] ], '');

var rxGain = num('rx', 0, 65535, _('Modem receive gain'));
var usb = num('usb', 0, 9, _('Modem USB interface'));
var ringRate = num('rr', 100, 3000, _('Call-blink rate in milliseconds'));
var offhookRate = num('or', 200, 10000, _('Off-hook blink rate in milliseconds'));
var refresh = u.select(REFRESH_STEPS.map(function(n) { return [ String(n), n === 1 ? _('1 second (live)') : _('%d seconds').format(n) ]; }), '1');

/* the recordings: kept on the router's storage, listed on the Calls tab */
var recEnabled = u.checkbox(false, _('Record phone calls'));
var recPath = u.text('', '/etc/awvoice/recordings', 'av-mono wide');
var recMax = num('recmax', 1, 4095, _('Recording budget in megabytes'));
var recUsage = E('span', { 'class': 'av-mono' }, '');

/* the answering machine: its greeting, the wait before it answers and its message storage */
var machOn = u.checkbox(false, _('Answer the phone for me'), function() { machineNote(); });
var machDelay = num('mdelay', 0, 60, _('Answering machine delay in seconds'));
var machPath = u.text('', '/etc/awvoice/messages', 'av-mono wide');
var machMax = num('mmax', 1, 4095, _('Message budget in megabytes'));
var machSecs = num('msecs', 10, 600, _('Longest message in seconds'));
var msgUsage = E('span', { 'class': 'av-mono' }, '');
var machNote = E('div', { 'class': 'av-note' });

/*
 * The greeting: one file at the daemon's own constant path (machine.h), with no path to type. The three things a user
 * can do with it are here - hear it, put a WAV of theirs in its place, record their own voice - and the state line is
 * the daemon's answer, not the page's guess.
 */
var GREET_RECORD_MAX_SEC = 30;		/* about 480 KB at 8 kHz: a greeting the caller will wait out, and far under the cap */
var greetState = E('div', { 'class': 'av-note' });
var greetPath = E('div', { 'class': 'av-mut av-mono' }, greeting.PATH);
var greetBusy = E('div', { 'class': 'av-mut' });
var greetFile = E('input', { type: 'file', accept: '.wav,audio/wav', 'hidden': true });
var greetAudio = null, greetUrl = null, greetKey = null;		/* one headless player: redraws move the buttons, never the sound */
var rec = null, recTick = null;					/* the microphone while it records, and its one-second line */
var greetSend = null;						/* the upload in flight, so it can be dropped, and only once */
var greetPlay, greetUpload, greetRecord;

	function greetButton(label, icon, cls, handler) {
		var b = u.button(label, icon, cls, handler);

		/* the button is [icon span, label span]: the label is the span that is not the icon, or relabelling writes the
		 * words over the icon and leaves the old ones beside them */
		b.__label = b.querySelector('span:not(.av-i)');
		b.__icon = b.querySelector('span.av-i');
		return b;
	}

	function greetLabel(b, text) {
		if (b.__label && b.__label.textContent !== text)
			b.__label.textContent = text;
	}

	/* the mark has to change with the word: a triangle beside "Stop" says two different things at once, and the eye
	   follows the shape before it reads the label */
	function greetIcon(b, name) {
		var next = u.icon(name);

		if (b.__icon && next)
			b.replaceChild(next, b.__icon);
		b.__icon = next;
	}

	/* what this browser can offer at all: a secure page, an audio engine and the capture processor */
	function greetMicReady() {
		return !!(window.isSecureContext && (window.AudioContext || window.webkitAudioContext) &&
			navigator.mediaDevices && navigator.mediaDevices.getUserMedia && window.AudioWorkletNode);
	}

	function greetAudioContext() {
		var C = window.AudioContext || window.webkitAudioContext;

		return C ? new C() : null;
	}

	/* the status line and the three buttons, from the health reply the daemon answered */
	function greetDraw() {
		var c = greeting.card(h), busy = !!greetSend || !!rec;

		if (!c.known)
			greetState.textContent = _('The greeting’s state is not known yet: the phone service did not answer.');
		else if (c.loaded)
			greetState.textContent = _('Ready — %s.').format(u.fmtDuration(Math.round(c.ms / 1000)));
		else
			greetState.textContent = greeting.sentence(c.word);
		greetPlay.disabled = !c.playable || busy;
		greetUpload.disabled = busy || !apiUploadReady();
		/* the button that started the recording is the one that ends it, so it must never be grey while it is running -
		   a disabled Stop is a recording nobody can get out of */
		greetRecord.disabled = rec ? false : (busy || !greetMicReady());
		greetLabel(greetPlay, greetKey === 'play' && greetAudio && !greetAudio.paused ? _('Stop') : _('Play'));
		greetIcon(greetPlay, greetKey === 'play' && greetAudio && !greetAudio.paused ? 'stop' : 'play');
		greetLabel(greetUpload, busy && greetSend ? _('Sending…') : _('Upload WAV…'));
		greetLabel(greetRecord, rec ? _('Stop') : _('Record'));
		greetIcon(greetRecord, rec ? 'stop' : 'mic');
		greetBusy.textContent = greetSend ? greetSend :
			(rec ? _('Recording… %s s. Speak, then press Stop.').format(Math.floor(rec.seconds)) : '');
	}

	function apiUploadReady() {
		return !!ctx.api.getGreeting;		/* the door is this build's; an older service answers with its own refusal */
	}

	/* the daemon's answer about the greeting, freshly read: every action that changes the file ends with this */
	function greetRefresh() {
		return ctx.api.getHealth().then(function(x) {
			h = x;
			ctx.emit('health', x);
			greetDraw();
			machineNote();
		}, function() {
			h = null;
			greetDraw();
			machineNote();
		});
	}

	/* stop whatever sound the card is making and give the browser's copy of the file back */
	function greetStopPlay() {
		if (greetAudio) {
			try { greetAudio.pause(); } catch (e) { /* already gone */ }
			greetAudio.src = '';
		}
		if (greetUrl)
			URL.revokeObjectURL(greetUrl);
		greetAudio = null; greetUrl = null; greetKey = null;
	}

	function greetFetchAll() {
		var parts = [], first = null;

		function next(offset) {
			return ctx.api.getGreeting(offset).then(function(c) {
				var bytes;

				if (first == null)
					first = c.total;
				else if (c.total !== first)
					throw new Error(_('The greeting changed while it was being read. Try again.'));
				bytes = greeting.unb64(c.b64);
				if (!bytes)
					throw new Error(_('The greeting did not travel intact.'));
				parts.push(bytes);
				return c.more ? next(c.offset + greeting.rawLen(c.b64)) : { parts: parts, total: first };
			});
		}
		return next(0);
	}

	function greetTogglePlay() {
		if (greetKey === 'play' && greetAudio) {
			if (greetAudio.paused) {
				greetAudio.play().catch(function() { greetStopPlay(); greetDraw(); });
			} else {
				/* stop, not pause: a greeting is heard from its first word or not at all, and the middle of one is
				   of no use to anyone - the next press starts it again from the start */
				greetAudio.pause();
				greetAudio.currentTime = 0;
			}
			greetDraw();
			return;
		}
		greetFetchAll().then(function(wav) {
			greetStopPlay();
			greetUrl = URL.createObjectURL(new Blob(wav.parts, { type: 'audio/wav' }));
			greetAudio = new window.Audio(greetUrl);
			/* the word on the button follows the sound, not the click: the browser starts when it has the bytes
			   decoded, which is a moment after this call, and it can end on its own - a button that kept saying
			   Play while the greeting ran gave no way to be seen to stop it */
			greetAudio.addEventListener('play', greetDraw);
			greetAudio.addEventListener('pause', greetDraw);
			greetAudio.addEventListener('ended', function() { greetKey = null; greetDraw(); });
			greetKey = 'play';
			greetDraw();
			greetAudio.play().catch(function() {
				greetStopPlay();
				ctx.toast(_('The browser would not play the greeting.'));
			});
		}, function(e) { ctx.toast(e.message); });
	}

	/*
	 * The upload, piece by piece, and the commit that alone puts it in the greeting's place. Anywhere it fails the
	 * staging is dropped, so the router is never left holding half a greeting for the next attempt to trip over, and
	 * the sentence named is the daemon's own word for what it refused.
	 */
	function greetSendFile(bytes, polish, label) {
		var pieces = greeting.plan(bytes.length), at = 0;

		if (greetSend)
			return Promise.reject(new Error(_('An upload is already running.')));
		greetSend = label;
		greetDraw();

		function step() {
			var p = pieces[at++];

			return ctx.api.greetingPut(p.offset, greeting.b64(bytes.subarray(p.offset, p.offset + p.size))).then(function(r) {
				if (!r.ok)
					throw new Error(greeting.sentence(r.refused) || _('The phone service would not take that piece.'));
				greetSend = _('%d%% sent.').format(Math.round(100 * (p.offset + p.size) / bytes.length));
				greetDraw();
				return at < pieces.length ? step() : null;
			});
		}

		return ctx.api.greetingBegin().then(function(r) {
			if (!r.ok)
				throw new Error(greeting.sentence(r.refused) || _('The upload could not be started.'));
			return pieces.length ? step() : null;
		}).then(function() {
			return ctx.api.greetingCommit(polish);
		}).then(function(r) {
			if (!r.ok)
				throw new Error(greeting.sentence(r.refused) || _('The greeting was not accepted.'));
			greetStopPlay();		/* the sound in the browser is of the file that just went */
			ctx.toast(_('The greeting is set.'));
			return r.ms;
		}).then(function(ms) {
			return greetRefresh().then(function() { return ms; });
		}, function(e) {
			return ctx.api.greetingAbort().then(function() { throw e; }, function() { throw e; });
		}).then(null, function(e) {
			greetSend = null;
			greetDraw();
			throw e;
		}).then(function(x) {
			greetSend = null;
			greetDraw();
			return x;
		});
	}

	/*
	 * A chosen file becomes the one shape the machine plays: the browser decodes it and resamples it to 8 kHz mono,
	 * the page writes the canonical header, and the daemon re-checks all of it before the old greeting is touched.
	 * Nothing about the author's rate or channel count reaches the user as an error they cannot fix.
	 */
	function greetDecodeTo8k(arrayBuffer) {
		var ctxA = greetAudioContext();

		if (!ctxA || !ctxA.decodeAudioData)
			return Promise.reject(new Error(_('This browser cannot read audio files.')));
		return new Promise(function(ok, no) {
			ctxA.decodeAudioData(arrayBuffer.slice(0), ok, no);
		}).then(function(buf) {
			var O = window.OfflineAudioContext || window.webkitOfflineAudioContext, frames, off, src;

			if (!O) {
				ctxA.close();
				throw new Error(_('This browser cannot resample audio.'));
			}
			if (!buf.duration || !(buf.duration < 400)) {
				ctxA.close();
				throw new Error(_('The file is too long to be a greeting.'));
			}
			frames = Math.max(1, Math.ceil(buf.duration * 8000));
			off = new O(1, frames, 8000);
			src = off.createBufferSource();
			src.buffer = buf;
			src.connect(off.destination);
			src.start(0);
			return off.startRendering();
		}).then(function(rendered) {
			ctxA.close();
			return greeting.to16(rendered.getChannelData(0));
		}, function(e) {
			ctxA.close();
			throw new Error(e && e.message ? e.message : _('That file could not be read as audio.'));
		});
	}

	function greetPickFile(file) {
		var reader;

		if (!file)
			return;
		if (!file.size) {
			ctx.toast(_('The file is empty.'));
			return;
		}
		reader = new FileReader();
		reader.onerror = function() { ctx.toast(_('The file could not be read.')); };
		reader.onload = function() {
			var bytes = new Uint8Array(reader.result), word = greeting.rejectWav(bytes, file.size);

			if (word) {
				ctx.toast(greeting.sentence(word));
				return;
			}
			greetDecodeTo8k(reader.result).then(function(samples) {
				var wav = greeting.wav(samples, 8000);

				if (!wav)
					throw new Error(_('The converted file is not something the machine can play.'));
				if (wav.length > greeting.MAX_BYTES)
					throw new Error(greeting.sentence('TOO_BIG'));
				return greetSendFile(wav, 0, _('Sending…'));
			}).then(null, function(e) { ctx.toast(e.message); });
		};
		reader.readAsArrayBuffer(file);
	}

	/* the microphone: the same capture processor the page's own phone uses, at the rate the greeting is kept in */
	function greetRecordStart() {
		var ctxA = greetAudioContext();

		if (!ctxA || !greetMicReady()) {
			ctx.toast(greetMicReady() ? _('The audio engine did not start.') : _('The microphone needs the page over https, and this browser does not offer it.'));
			return Promise.resolve(false);
		}
		return navigator.mediaDevices.getUserMedia({ audio: {
			echoCancellation: true, noiseSuppression: true, autoGainControl: true, channelCount: 1
		} }).then(function(stream) {
			rec = { ctx: ctxA, stream: stream, parts: [], bytes: 0, seconds: 0 };
			return ctxA.audioWorklet.addModule(L.resource('view/awvoice/phone/audio-worklet.js'));
		}).then(function() {
			rec.source = ctxA.createMediaStreamSource(rec.stream);
			rec.node = new window.AudioWorkletNode(ctxA, 'awvoice-capture');
			rec.sink = ctxA.createGain();
			rec.sink.gain.value = 0;
			rec.node.connect(rec.sink);
			rec.sink.connect(ctxA.destination);
			rec.source.connect(rec.node);
			rec.node.port.onmessage = function(e) {
				var d = e.data || {}, frame;

				if (!d.pcm || d.pcm.byteLength !== 320 || !rec)
					return;
				frame = greeting.swap16(new Uint8Array(d.pcm));	/* the processor's frames are big-endian; a WAV is not */
				rec.parts.push(frame);
				rec.bytes += frame.length;
				rec.seconds = rec.bytes / 2 / 8000;
				if (rec.seconds >= GREET_RECORD_MAX_SEC)
					greetRecordStop();
			};
			return ctxA.resume();
		}).then(function() {
			recTick = window.setInterval(function() { greetDraw(); }, 1000);	/* the one line that says it is still going */
			greetDraw();
			return true;
		}, function(e) {
			greetRecordGiveUp();
			ctx.toast(e && e.name === 'NotAllowedError' ? _('The microphone was not allowed for this page.') :
				(e && e.message ? e.message : _('The microphone could not be opened.')));
			return false;
		});
	}

	/* everything the microphone holds goes: the stream, the processors and the context */
	function greetRecordGiveUp() {
		if (recTick) {
			window.clearInterval(recTick);
			recTick = null;
		}
		if (rec) {
			if (rec.stream)
				rec.stream.getTracks().forEach(function(t) { t.stop(); });
			if (rec.ctx) {
				try { rec.ctx.close(); } catch (e) { /* already closed */ }
			}
		}
		rec = null;
		greetBusy.textContent = '';
		greetDraw();
	}

	/* the recording, trimmed and with the tone added by the daemon (it owns the audio it plays), then staged */
	function greetRecordStop() {
		var held = rec, samples, i, merged, view;

		if (!held)
			return;
		greetRecordGiveUp();
		if (!held.bytes) {
			ctx.toast(_('Nothing was recorded. Check the microphone and try again.'));
			return;
		}
		merged = new Uint8Array(held.bytes);
		for (i = 0, view = 0; i < held.parts.length; i++) {
			merged.set(held.parts[i], view);
			view += held.parts[i].length;
		}
		samples = new Int16Array(merged.buffer);
		greetSendFile(greeting.wav(samples, 8000), 1, _('Sending…')).then(null, function(e) { ctx.toast(e.message); });
	}

	greetFile.addEventListener('change', function() {
		var f = greetFile.files && greetFile.files[0];

		greetFile.value = '';			/* the same file twice is still a choice: the input is emptied first */
		greetPickFile(f);
	});
	greetPlay = greetButton(_('Play'), 'play', 'cbi-button-action', function() { greetTogglePlay(); });
	greetUpload = greetButton(_('Upload WAV…'), 'download', 'cbi-button-action', function() { greetFile.click(); });
	greetRecord = greetButton(_('Record'), 'mic', 'cbi-button-action', function() {
		if (rec)
			greetRecordStop();
		else
			greetRecordStart();
	});

	/* the tab is left: the microphone goes out of the room and an upload in flight is dropped, not abandoned */
	ctx.on('tableave', function(id) {
		if (id !== 'set')
			return;
		if (rec)
			greetRecordGiveUp();
		greetStopPlay();
		if (greetSend)
			ctx.api.greetingAbort().then(function() { greetSend = null; greetDraw(); }, function() { greetSend = null; greetDraw(); });
	});

	/* the FSK variants carry the timing choice; DTMF has its own placement (it goes out while the phone rings) */
	function fskPicked() {
		cidWhen.disabled = cid.value !== 'bellcore' && cid.value !== 'etsi';
	}
	function when() {
		return cid.value === 'bellcore' || cid.value === 'etsi' ? cidWhen.value : null;
	}
	cid.addEventListener('change', fskPicked);

		var err = E('div', { 'class': 'av-err-msg', 'role': 'alert' });

		function tag() { return u.tag(_('Not available yet')); }
		function offIf(key, kids) { return u.off(kids, !live[key]); }

		/* keeps a stored refresh that is not one of the offered steps as its own option */
		function setRefresh(sec) {
			if (REFRESH_STEPS.indexOf(sec) < 0)
				refresh.appendChild(E('option', { 'value': String(sec) }, _('%d seconds').format(sec)));
			refresh.value = String(sec);
		}

		/* whether the machine would answer as things stand: the daemon answers only when it holds a greeting, the switch
		 * asks for it and the message folder can take a file. The greeting card says what the file is; this line says
		 * what happens to a call. */
		function machineNote() {
			var c = greeting.card(h), st = storeState('messages'), note;

			if (!machOn.get()) {
				machNote.textContent = _('Off: an unanswered call keeps ringing until the caller gives up.');
				return;
			}
			if (!c.known)
				note = _('The greeting’s state is not known yet, so the machine may not answer.');
			else if (!c.loaded)
				note = _('The machine will not answer until a greeting is ready: record one or upload a WAV below.');
			else
				note = _('The machine plays the greeting and takes a message after the tone.');
			if (st && st.ready === false)
				note += ' ' + _('It will not answer until the message folder can be used: %s.').format(
					st.error || _('no reason given'));
			else if (st && st.ready === true && st.freeMb != null && st.freeMb < 1)
				note += ' ' + _('It will not answer until the message folder has room for a file.');
			else if (c.loaded)
				note += ' ' + _('It answers after %s seconds.').format(String(h.machine.delaySec));
			machNote.textContent = note;
		}

		function fill() {
			led.textContent = '';
			led.appendChild(E('option', { 'value': '' }, _('None')));
			var names = (h && h.indicator && h.indicator.leds) || [];
			names.forEach(function(n) {
				led.appendChild(E('option', { 'value': n }, n));
			});
			if (loaded.indicator.led && names.indexOf(loaded.indicator.led) < 0)
				led.appendChild(E('option', { 'value': loaded.indicator.led }, loaded.indicator.led));
			led.value = loaded.indicator.led;
		}

		function load() {
			return ctx.api.getPhoneSettings().then(function(p) {
				loaded = p;
				cid.value = p.line.cid; myNo.value = p.line.myNumber; pause.value = p.line.dialPauseSec;
				cidWhen.value = p.line.cidWhen; fskPicked();
				flashMin.value = p.hookFlash.minMs; flashMax.value = p.hookFlash.maxMs;
				ringback.value = p.tones.ringback;
				volume.value = p.audio.earpieceVolume == null ? '' : String(p.audio.earpieceVolume);
				micGain.value = p.audio.micGainDb == null ? '' : p.audio.micGainDb;
				earGain.value = p.audio.earpieceGainDb == null ? '' : p.audio.earpieceGainDb;
				rxGain.value = p.audio.modemRxGain == null ? '' : p.audio.modemRxGain;
				conf.set(p.features.conference); wait.set(p.features.callWaiting); hd.set(p.audio.hd);
				hotNo.value = p.features.hotline.number; hotDelay.value = p.features.hotline.delaySec;
				ringRate.value = p.indicator.ringRateMs; offhookRate.value = p.indicator.offhookRateMs;
				fill();
				usb.value = p.modem.usbInterface;
				machOn.set(p.machine.enabled);
				machDelay.value = p.machine.delaySec;
				machPath.value = p.machine.path;
				machMax.value = p.machine.maxMb;
				machSecs.value = p.machine.maxSec;
				setRefresh(p.page.refreshSec);
				err.textContent = '';
				return ctx.api.getHealth();
			}).then(function(health) {
				h = health;
				fill();
				machineNote();
				greetDraw();
				/* the usage lines carry the folders' state, so they are drawn again once it is known */
				recRefreshUsage();
				msgRefreshUsage();
			}, function(e) { err.textContent = e.message; });
		}

		function loadRec() {
			return ctx.api.getRecordingSettings().then(function(s) {
				recEnabled.set(s.enabled);
				recPath.value = s.path;
				recMax.value = String(s.maxMb);
			}, function(e) { err.textContent = err.textContent || e.message; });
		}

		/* one refusal must not hide the files that did go: every delete settles, then the count says how it went;
		 * the usage line is read again and the Calls tab is told, so no card keeps pointing at a gone file */
		function delAll(list, del, noun, done, usage, evt) {
			return Promise.all(list.map(function(f) {
				return del(f.name).then(function() { return null; }, function(e) { return e; });
			})).then(function(errs) {
				var bad = errs.filter(function(e) { return e; });

				ctx.toast(bad.length ? _('%d of %d %s could not be deleted: %s').format(bad.length, list.length, noun, bad[0].message) : done);
				ctx.emit(evt);
				return usage();
			});
		}

		/* the folder as the daemon reads it: no trailing slashes, and never the root (the daemon refuses both the
		 * root and a folder that is a file, and says why on the Health tab) */
		function dirPath(v) {
			v = String(v).trim().replace(/\/+$/, '');
			return /^\/./.test(v) ? v : null;
		}

		/* the folders' state from the health reply: a folder that cannot take a file is said here, before a call
		 * is lost to a path that only fails at write time */
		function storeState(which) {
			return h && h.storage ? h.storage[which] : null;
		}

		function storeTail(which) {
			var st = storeState(which);

			if (st && st.ready === false)
				return ' ' + _('The folder cannot be used: %s.').format(st.error || _('no reason given'));
			if (st && st.freeMb != null)
				return ' ' + (st.freeMb < 1 ? _('Nearly full: %s MB free.').format(st.freeMb) : _('%s MB free.').format(st.freeMb));
			return '';
		}

		function recRefreshUsage() {
			return ctx.api.listRecordings().then(function(list) {
				var bytes = list.reduce(function(a, r) { return a + r.bytes; }, 0);
				recUsage.textContent = (list.length ?
					_('%d recordings, %s MB on the storage').format(list.length, (bytes / 1048576).toFixed(1)) :
					_('No recordings on the storage.')) + storeTail('recordings');
				return list;
			}, function(e) { recUsage.textContent = e.message; return []; });
		}

		function recDeleteAll() {
			return recRefreshUsage().then(function(list) {
				if (!list.length)
					return;
				editors.confirm(ctx, _('Delete all recordings'),
					_('Delete all %d recordings? This cannot be undone.').format(list.length), _('Delete all'),
					function() { return delAll(list, ctx.api.deleteRecording, _('recordings'), _('Recordings deleted'), recRefreshUsage, 'recordings'); });
			});
		}

		function msgRefreshUsage() {
			return ctx.api.listMessages().then(function(list) {
				var bytes = list.reduce(function(a, m) { return a + m.bytes; }, 0);
				msgUsage.textContent = (list.length ?
					_('%d messages, %s MB on the storage').format(list.length, (bytes / 1048576).toFixed(1)) :
					_('No messages on the storage.')) + storeTail('messages');
				return list;
			}, function(e) { msgUsage.textContent = e.message; return []; });
		}

		function msgDeleteAll() {
			return msgRefreshUsage().then(function(list) {
				if (!list.length)
					return;
				editors.confirm(ctx, _('Delete all messages'),
					_('Delete all %d messages? This cannot be undone.').format(list.length), _('Delete all'),
					function() { return delAll(list, ctx.api.deleteMessage, _('messages'), _('Messages deleted'), msgRefreshUsage, 'messages'); });
			});
		}

		function optInt(el) {
			var v = String(el.value).trim();
			return v === '' ? null : parseInt(v, 10);
		}

		function save() {
			err.textContent = '';
			if (!loaded) { err.textContent = _('The settings could not be read yet.'); return; }

			var my = myNo.value.trim(), hn = hotNo.value.trim();
			var dp = parseInt(pause.value, 10), fmin = parseInt(flashMin.value, 10), fmax = parseInt(flashMax.value, 10);
			var rx = optInt(rxGain), ui = parseInt(usb.value, 10);

			if (my && !NUMBER_RE.test(my)) { err.textContent = _('Numbers may contain digits, *, # and a leading +'); return; }
			if (!(dp >= 3 && dp <= 15)) { err.textContent = _('Dial pause must be 3 to 15 seconds'); return; }
			if (!(fmin >= 20 && fmin <= 500)) { err.textContent = _('Hook-flash minimum must be 20 to 500 ms'); return; }
			if (!(fmax >= 300 && fmax <= 3000)) { err.textContent = _('Hook-flash maximum must be 300 to 3000 ms'); return; }
			if (fmax < fmin + 100) { err.textContent = _('Hook-flash maximum must be at least 100 ms above the minimum'); return; }
			if (rx != null && !(rx >= 0 && rx <= 65535)) { err.textContent = _('Modem receive gain must be 0 to 65535'); return; }
			if (!(ui >= 0 && ui <= 9)) { err.textContent = _('Modem USB interface must be 0 to 9'); return; }
			var rr = parseInt(ringRate.value, 10), orate = parseInt(offhookRate.value, 10);
			if (!(rr >= 100 && rr <= 3000)) { err.textContent = _('The indicator call-blink rate must be 100 to 3000 ms'); return; }
			if (!(orate >= 200 && orate <= 10000)) { err.textContent = _('The indicator off-hook blink rate must be 200 to 10000 ms'); return; }

			var rp = dirPath(recPath.value), rm = parseInt(recMax.value, 10);
			if (!rp) { err.textContent = _('The recording folder must be an absolute path that is not the root'); return; }
			if (!(rm >= 1 && rm <= 4095)) { err.textContent = _('The recording budget must be 1 to 4095 MB'); return; }

			var mp = dirPath(machPath.value);
			var md = parseInt(machDelay.value, 10), mmb = parseInt(machMax.value, 10), ms = parseInt(machSecs.value, 10);
			if (!mp || mp.length >= 128) { err.textContent = _('The message folder must be an absolute path shorter than 128 characters and not the root'); return; }
			if (mp === rp) { err.textContent = _('The messages cannot be kept in the recording folder. Choose another folder.'); return; }
			if (!(md >= 0 && md <= 60)) { err.textContent = _('The answering machine delay must be 0 to 60 seconds'); return; }
			if (!(mmb >= 1 && mmb <= 4095)) { err.textContent = _('The message budget must be 1 to 4095 MB'); return; }
			if (!(ms >= 10 && ms <= 600)) { err.textContent = _('The longest message must be 10 to 600 seconds'); return; }

			ctx.api.savePhoneSettings({
				line: { cid: cid.value, cidWhen: when(), myNumber: my, dialPauseSec: dp },
				hookFlash: { minMs: fmin, maxMs: fmax },
				tones: { ringback: ringback.value },
				audio: { earpieceVolume: volume.value === '' ? null : parseInt(volume.value, 10), modemRxGain: rx,
					micGainDb: optInt(micGain), earpieceGainDb: optInt(earGain), hd: hd.get() },
				features: { clir: loaded.features.clir, conference: conf.get(), callWaiting: wait.get(), hotline: { number: hn, delaySec: parseInt(hotDelay.value, 10) || loaded.features.hotline.delaySec } },
				indicator: { led: led.value, ringRateMs: rr, offhookRateMs: orate },
				modem: { usbInterface: ui },
				page: { refreshSec: parseInt(refresh.value, 10) || 1 },
				machine: { enabled: machOn.get(), delaySec: md, path: mp, maxMb: mmb, maxSec: ms }
			}).then(function() {
				return ctx.api.saveRecordingSettings({ enabled: recEnabled.get(), path: rp, maxMb: rm });
			}).then(function() {
				ctx.toast(_('Settings saved and applied'));
				return load();
			}).then(function() {
				ctx.emit('settings', loaded);
			}, function(e) { err.textContent = e.message; });
		}

		var sdList = E('div');
		function drawSpeed() {
			sdList.textContent = '';
			var items = ctx.contacts.filter(function(c) { return c.quick; });
			if (!items.length)
				sdList.appendChild(E('div', { 'class': 'av-empty' }, _('No speed dial numbers yet.')));
			items.forEach(function(c) {
				sdList.appendChild(E('div', { 'class': 'av-it' }, [
					u.tag(c.quick),
					E('div', { 'class': 't' }, [ c.name, E('small', { 'class': 'av-mono' }, c.numbers[0].value) ]),
					u.iconButton(_('Remove speed dial'), 'x', function() {
						ctx.api.saveContact({ id: c.id, name: c.name, numbers: c.numbers, quick: '' })
							.then(function() { return ctx.refreshContacts(); }, function(e) { ctx.toast(e.message); });
					})
				]));
			});
		}
		ctx.on('contacts', drawSpeed);

		var speedReady = !!ctx.api.available.saveContact;

		var root = E('div', [
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Line')),
				u.field(_('Caller ID'), _('How the caller\'s number reaches the handset\'s display'), cid),
				u.field(_('Caller-id timing'), _('Where the FSK variants put the message: between the first and the second ring burst, or before the first ring (the European placement). DTMF has its own placement.'), cidWhen),
				u.field(_('My number'), _('Shown on the Overview. It is never dialled.'), myNo),
				u.field(_('Dial pause'), _('The number is dialled this long after the last digit (3 to 15 seconds)'),
					E('div', { 'class': 'av-inline' }, [ pause, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Hook-flash timing'), _('How long a press of the flash key lasts, in milliseconds. The maximum must be at least 100 above the minimum.'),
					E('div', { 'class': 'av-inline' }, [ flashMin, E('span', { 'class': 'av-mut' }, _('to')), flashMax, E('span', { 'class': 'av-mut' }, 'ms') ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Tones')),
				u.field(_('Ringback tone'), _('Automatic plays the router\'s own ringing tone until the network\'s own sound is heard. Choose Never play if the network already sends one.'), ringback)
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Audio')),
				u.field(_('Earpiece volume'), _('The modem\'s own volume levels'), volume),
				offIf('micGainDb', [ u.field(_('Microphone gain'), _('In dB. Lower the microphone if the other side hears distortion.'),
					E('div', { 'class': 'av-inline' }, [ micGain, E('span', { 'class': 'av-mut' }, 'dB'), live.micGainDb ? '' : tag() ])) ]),
				offIf('earpieceGainDb', [ u.field(_('Earpiece gain'), _('In dB'),
					E('div', { 'class': 'av-inline' }, [ earGain, E('span', { 'class': 'av-mut' }, 'dB'), live.earpieceGainDb ? '' : tag() ])) ]),
				offIf('hd', [ u.field(_('HD voice'), _('Wideband 16 kHz audio, off by default. Saving the change asks the modem for that rate and restarts it (mobile data drops for about half a minute); when the modem answers with it, the phone line is brought up at that rate - a few seconds without dial tone, and never during a call. Until then the line stays as it was. Switching back does the same in reverse.'),
					E('div', { 'class': 'av-inline' }, [ hd.node ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Features')),
				offIf('callWaiting', [ u.field(_('Call waiting'), _('A short beep tells you that a second call is coming in. Flash takes it and holds the first call; flash and 1 ends the first call and takes the new one; flash and 0 refuses it.'),
					E('div', { 'class': 'av-inline' }, [ wait.node, live.callWaiting ? '' : tag() ])) ]),
				offIf('conference', [ u.field(_('3-way calling'), _('Flash holds the call. Dial the second number, then flash and 3 to join the two calls.'),
					E('div', { 'class': 'av-inline' }, [ conf.node, live.conference ? '' : tag() ])) ]),
				offIf('hotline', [ u.field(_('Hotline'), _('Dials this number if you lift the handset and dial nothing for the delay (3 to 99 seconds). Other calls can still be dialled in that time.'),
					E('div', { 'class': 'av-inline' }, [ hotNo, hotDelay, E('span', { 'class': 'av-mut' }, 's') ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Recording')),
				E('div', { 'class': 'av-note' }, _('Calls are recorded onto the router\'s own storage while they happen; the recordings are listed on the Calls tab. The oldest give way when the budget is full. A call the answering machine answered is kept as its message, not as a recording; the part of the conversation after you pick the handset up is not recorded.')),
				u.field(_('Record calls'), _('Off by default'), E('div', { 'class': 'av-inline' }, [ recEnabled.node ])),
				u.field(_('Recording folder'), _('Where the recordings are kept. The folder must start with a slash.'), recPath),
				u.field(_('Storage budget'), _('The recordings may fill this many megabytes before the oldest give way (1 to 4095)'),
					E('div', { 'class': 'av-inline' }, [ recMax, E('span', { 'class': 'av-mut' }, 'MB') ])),
				u.field(_('Stored recordings'), _('Everything recorded so far lives in the recording folder.'),
					E('div', { 'class': 'av-inline' }, [ recUsage,
						u.button(_('Delete all'), 'trash', 'cbi-button-remove important', function() { recDeleteAll(); }) ]))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Answering machine')),
				E('div', { 'class': 'av-note' }, _('An incoming call the handset does not take is answered by the machine: the caller hears the greeting and leaves a message, which appears on the Calls tab. Only the cellular line is answered. Picking up the handset hands the call over: the message ends at the lift and keeps what the caller said up to it — the classic answering-machine behaviour.')),
				u.field(_('Answer the phone'), _('Off by default. The greeting is played toward the caller.'),
					E('div', { 'class': 'av-inline' }, [ machOn.node ])),
				u.field(_('Greeting'), _('The machine plays one file, at the place below - it is not a path you type. The package puts a default there when the router has none, and it is never written over again.'),
					E('div', [
						greetState,
						E('div', { 'class': 'av-acts' }, [ greetPlay, greetUpload, greetRecord ]),
						greetBusy,
						greetFile,
						greetPath
					])),
				u.field(_('Answer after'), _('Seconds to ring before the machine answers (0 to 60); zero answers at once'),
					E('div', { 'class': 'av-inline' }, [ machDelay, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Message folder'), _('Where the messages are kept. The folder must start with a slash.'), machPath),
				u.field(_('Storage budget'), _('The messages may fill this many megabytes before the oldest give way (1 to 4095)'),
					E('div', { 'class': 'av-inline' }, [ machMax, E('span', { 'class': 'av-mut' }, 'MB') ])),
				u.field(_('Longest message'), _('The machine ends the call after this many seconds (10 to 600)'),
					E('div', { 'class': 'av-inline' }, [ machSecs, E('span', { 'class': 'av-mut' }, 's') ])),
				u.field(_('Stored messages'), _('Everything the machine kept lives in the message folder. A message whose call leaves the log can still be played and deleted on the Calls tab.'),
					E('div', { 'class': 'av-inline' }, [ msgUsage,
						u.button(_('Delete all'), 'trash', 'cbi-button-remove important', function() { msgDeleteAll(); }) ])),
				machNote
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('Speed dial'),
					E('span', { 'class': 'av-r' }, [ speedReady ? '' : tag(),
						u.button(_('Add'), 'plus', '', function() { editors.speedDial(ctx, drawSpeed); }, speedReady ? {} : { 'disabled': true }) ]) ]),
				E('div', { 'class': 'av-note' }, _('A speed dial is two digits followed by #. Without # the digits are an ordinary number.')),
				sdList
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', [ _('Indicator'), live.led ? '' : E('span', { 'class': 'av-r' }, tag()) ]),
				E('div', { 'class': 'av-note' }, _('The LED is solid while the line is ready for calls, blinks fast while a call rings, blinks slowly while the handset is off hook, and stays dark otherwise.')),
				offIf('led', [ u.field(_('Phone LED'), _('One of the router\'s LEDs that shows the phone state'), led),
					u.field(_('Call-blink rate'), _('The fast blink while a call rings, in milliseconds (100 to 3000)'),
						E('div', { 'class': 'av-inline' }, [ ringRate, E('span', { 'class': 'av-mut' }, 'ms') ])),
					u.field(_('Off-hook blink rate'), _('The slow blink while the handset is off hook, in milliseconds (200 to 10000)'),
						E('div', { 'class': 'av-inline' }, [ offhookRate, E('span', { 'class': 'av-mut' }, 'ms') ])) ])
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('details', [
					E('summary', _('Advanced')),
					u.field(_('Modem receive gain'), _('0 to 65535, 8192 is unity. Leave empty to keep the modem\'s own value.'), rxGain),
					u.field(_('Modem USB interface'), _('Change only if the modem is not found. The port in use is shown on the Health tab.'), usb),
					u.field(_('Live view refresh'), _('How often this page asks the phone service for the current state. One second shows a lifted handset or a ring almost at once.'), refresh)
				])
			]),
			err,
			E('div', { 'class': 'cbi-page-actions' }, [
				u.button(_('Save and apply'), '', 'cbi-button-apply important', save),
				u.button(_('Reset'), '', 'cbi-button-reset', function() { load(); loadRec(); })
			])
		]);

		load();
		loadRec();
		recRefreshUsage();
		msgRefreshUsage();
		drawSpeed();
		greetDraw();		/* the card starts with what is not known yet, and the health reply corrects it */
		return root;
	}
});
