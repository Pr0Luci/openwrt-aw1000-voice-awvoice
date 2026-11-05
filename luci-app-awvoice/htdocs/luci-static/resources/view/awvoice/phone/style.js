// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Pr0Luci
'use strict';
'require baseclass';

/*
 * style.js - the stylesheet of the phone page, injected once as a <style> element.
 *
 * Every rule is scoped under .avp and every colour or size comes from a token that resolves in this order:
 *   1. the in-house theme's --ax-* variable (when that theme is active),
 *   2. the stock theme's variable (--background-color-*, ...),
 *   3. a neutral system value.
 * So the page follows whichever theme is selected, light or dark, with no theme-specific layout code.
 *
 * Layout reacts to the width of the page container (@container), not the viewport, because the side menu changes how
 * much room the content gets. Breakpoints: 1023 (detail panel -> inline), 700 (single column), 420.
 */

var STYLE_ID = 'avp-style';

var CSS = [
'.avp{--av-fg:var(--ax-fg,var(--text-color-high,CanvasText));',
'--av-mut:var(--ax-muted,var(--text-color-medium,GrayText));',
'--av-card:var(--ax-card,var(--background-color-high,Canvas));',
'--av-sub:var(--ax-card2,var(--background-color-medium,rgba(128,128,128,.1)));',
'--av-bd:var(--ax-border,var(--border-color-medium,rgba(128,128,128,.35)));',
'--av-bd2:var(--ax-border-strong,var(--border-color-high,rgba(128,128,128,.55)));',
'--av-ac:var(--ax-accent-text,var(--primary-color-high,LinkText));',
'--av-dim:var(--ax-accent-dim,rgba(128,128,128,.14));',
'--av-ok:var(--ax-ok,var(--success-color-medium,#2e7d32));',
'--av-warn:var(--ax-warn,var(--warn-color-medium,#b26a00));',
'--av-err:var(--ax-err,var(--error-color-medium,#c62828));',
'--av-mono:var(--ax-mono,ui-monospace,SFMono-Regular,Menlo,Consolas,monospace);',
'--av-r:var(--ax-r,4px);--av-chip:0px;--av-row:44px;--av-it:40px;',
'container-type:inline-size;container-name:avp;color:var(--av-fg);min-width:0}',
'.avp *{box-sizing:border-box}',
'.avp [hidden]{display:none!important}',
/* the in-house theme, like the stock themes, sets its content text at 13px, which the page then inherits */
'.avp .av-mono{font-family:var(--av-mono)}',
'.avp .av-mut{color:var(--av-mut)}',
'.avp .av-ok{color:var(--av-ok)}.avp .av-warn{color:var(--av-warn)}.avp .av-err{color:var(--av-err)}',
'.avp .av-i{display:inline-flex;vertical-align:-.2em;flex:none}',
'.avp .av-i svg{width:1.2em;height:1.2em;fill:none;stroke:currentColor;stroke-width:2;stroke-linecap:round;stroke-linejoin:round;',
'border:0;border-radius:0;padding:0;margin:0;background:none;box-shadow:none}',
/* undo the in-house theme's generic "#view svg" graph frame for icons */
'#view .avp .av-i svg{border:0;border-radius:0;background:none}',

/* a form group whose controls are all disabled together */
'.avp fieldset.av-fs{border:0;padding:0;margin:0;min-width:0}',
'.avp fieldset.av-fs[disabled]{opacity:.55}',

/* the SIP call panel: a dial row, the action strip and the keypad */
'.avp .av-dial{display:flex;gap:8px;align-items:center;margin:10px 0 0}',
'.avp .av-dial input{flex:1;min-width:0}',
'.avp .av-pad{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:6px;max-width:300px;margin:10px 0 0}',
'.avp .av-pad button{min-height:38px;font-size:15px;font-variant-numeric:tabular-nums}',
'.avp .av-h4{font-size:12px;font-weight:600;letter-spacing:.4px;text-transform:uppercase;color:var(--av-mut);margin:16px 0 2px}',
/* the phone's call grid and keypad, and the SIP tab's cards */
'.avp .av-sub{margin:0 0 14px}',
'.avp .av-callgrid{display:grid;gap:12px;grid-template-columns:minmax(0,7fr) minmax(0,5fr);align-items:start}',
'.avp .av-svcgrid{display:grid;gap:12px;grid-template-columns:repeat(2,minmax(0,1fr));align-items:start}',
'.avp .av-pad{max-width:340px}',
'.avp .av-key{display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:46px;padding:2px 0;',
'font-size:17px;font-weight:600;line-height:1.1}',
'.avp .av-key .l{font-size:9px;font-weight:400;letter-spacing:.5px;color:var(--av-mut);min-height:10px}',
'.avp .av-dial .cbi-button{white-space:nowrap}',
/* the installed service: status chips, one collapsible card per direction */
'.avp .av-statuschips{display:flex;flex-wrap:wrap;gap:6px;margin:12px 0 0}',
'.avp .av-statuschip{display:inline-flex;gap:6px;align-items:center;padding:4px 10px;border:1px solid var(--av-bd2);',
'border-radius:12px;background:transparent;color:var(--av-fg);font:inherit;font-size:12px;cursor:pointer}',
'.avp .av-statuschip .dot{width:8px;height:8px;border-radius:50%;background:var(--av-mut)}',
'.avp .av-statuschip.ok .dot{background:var(--av-ok)}.avp .av-statuschip.unk .dot{background:var(--av-warn)}',
'.avp .av-statuschip.err .dot{background:var(--av-err)}',
'.avp .av-statuschip.static{cursor:default}',
'.avp .av-dirgrid{display:grid;gap:12px;grid-template-columns:repeat(2,minmax(0,1fr));align-items:start}',
'.avp details.av-dir>summary{display:flex;flex-wrap:wrap;gap:4px 10px;align-items:center;justify-content:flex-start;',
'font-weight:600;cursor:pointer;color:var(--av-fg)}',
'.avp details.av-dir>summary .av-dir-t{flex:0 1 auto}',
'.avp details.av-dir>summary .av-dir-s{color:var(--av-mut);font-weight:400;font-size:12px}',
'.avp details.av-dir[open]>summary{margin-bottom:8px}',
/* a card that is always open: its state line and its switch sit in the title row's right slot,
   where another card keeps its refresh button */
'.avp .cbi-section h3 .av-dir-s{font-weight:400;font-size:12px}',
'.avp .cbi-section h3 .av-r .av-sw{margin:0}',
/* a folded part inside a card: a rule above it, not a second card */
'.avp .cbi-section details.av-dir{margin-top:12px;border-top:1px solid var(--av-bd);padding-top:6px}',
/* a form's actions stay in the framework's own action bar: the theme draws the row and the buttons the way it draws
   every page's. the one row that is not a page footer (the mode pick and its button on one line) takes the
   container's padding and alignment back to its own */
'.avp .av-moderow{display:flex;gap:10px;align-items:flex-end;flex-wrap:wrap;margin:10px 0 0;padding:0;text-align:start}',
'.avp .av-moderow.cbi-page-actions{justify-content:flex-start}',
'.avp .av-modesel{flex:0 1 320px;min-width:0}',
'.avp .av-modesel label{display:block;margin:0 0 4px;font-weight:600}',
'.avp .av-modesel select{width:100%}',
/* the form of a direction: the fields in a grid, the label above the control */
'.avp .av-fgrid{display:grid;gap:12px 16px;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));margin:10px 0 0}',
'.avp .av-ffld{display:flex;flex-direction:column;gap:3px;min-width:0}',
'.avp .av-ffld-t{font-size:12px;font-weight:600;color:var(--av-mut)}',
'.avp .av-ffld-c{min-width:0}',
'.avp .av-ffld-c input[type=text],.avp .av-ffld-c input[type=number],.avp .av-ffld-c input[type=password],',
'.avp .av-ffld-c select{width:100%}',
'.avp .av-ffld-c .av-inline{width:100%}',
'.avp .av-ffld-c .av-inline input[type=text],.avp .av-ffld-c .av-inline input[type=password]{flex:1 1 120px;min-width:0}',
'.avp .av-ffld-c .av-inline input[type=number]{flex:0 0 88px;width:88px}',
'.avp .av-ffld-c .av-inline .av-port{flex:0 0 96px;width:96px}',
'.avp .av-ffld-h{font-size:11px;color:var(--av-mut);line-height:1.35}',
/* a field that carries two switches and a name under them */
'.avp .av-stack{display:flex;flex-direction:column;gap:6px;align-items:flex-start;width:100%}',
'.avp .av-stack input[type=text]{width:100%}',
/* the switch in a card's summary: the theme draws the switch, the page gives it the place */
'.avp .av-sw{display:inline-flex;align-items:center;margin-left:auto;cursor:pointer}',
'.avp .av-sw input[type=checkbox]{margin:0}',
/* the number field and the keypad end on the same right bound; the two call controls share the rail's whole
   height beside them, one half each */
'.avp .av-padwrap{display:grid;grid-template-columns:minmax(0,1fr) 108px;gap:10px;align-items:stretch;margin:10px 0 0}',
'.avp .av-dialcol{display:flex;flex-direction:column;min-width:0}',
'.avp .av-dialcol .av-dial{margin:0 0 10px}',
'.avp .av-padwrap .av-pad{margin:0;max-width:none}',
'.avp .av-rail{display:grid;grid-template-rows:repeat(3,minmax(0,1fr));gap:6px;min-height:0}',
'.avp .av-railbtn{height:100%;min-height:0;flex-direction:column;gap:4px;padding:6px}',
'.avp .av-railbtn .av-i{font-size:24px}',
'.avp .av-railbtn span:last-child{font-size:14px;font-weight:600;line-height:1.15}',
'.avp .av-railbtn:disabled{opacity:.6;filter:grayscale(1);background:var(--av-sub);border-color:var(--av-bd);',
'color:var(--av-mut);cursor:not-allowed}',
/* the browser audio's level meter */
'.avp .av-meter{height:6px;margin:8px 0 0;border:1px solid var(--av-bd);border-radius:3px;background:var(--av-sub);overflow:hidden}',
'.avp .av-meter span{display:block;height:100%;width:0;background:var(--av-ok);transition:width .1s linear}',

/* status banner: the box, its colours and the state tint belong to the framework's own message box, which
   every theme paints - the page only lays the icon and the two lines out inside it */
'.avp .av-ban{display:flex;gap:10px;align-items:center;margin:0 0 12px}',
'.avp .av-ban .av-i{font-size:20px;flex:none}.avp .av-ban>div:not(.av-ban-num){flex:1;min-width:0}',
'.avp .av-ban .av-sub{color:var(--av-mut)}',
/* the number at the end of the state card: one column of two rows, stuck to the card's right edge, read left */
'.avp .av-ban-num{margin-left:auto;text-align:start;min-width:0;padding-inline-start:12px}',
'.avp .av-ban-num>div:first-child{font-size:11px;color:var(--av-mut)}',
'.avp .av-ban-num>div:last-child{font-weight:600}',

/* cards and grids */
'.avp .cbi-section h3,.avp .av-h{display:flex;flex-wrap:wrap;gap:8px;align-items:center}',
'.avp .av-r{margin-left:auto;font-weight:400;color:var(--av-mut);display:inline-flex;gap:8px;align-items:center}',
'.avp .av-g2,.avp .av-g3{display:grid;gap:12px;grid-template-columns:repeat(2,minmax(0,1fr))}',
'.avp .av-g3{grid-template-columns:repeat(3,minmax(0,1fr))}',
'.avp .av-tiles{display:grid;gap:10px;grid-template-columns:repeat(4,minmax(0,1fr));margin:0 0 12px}',
/* the box, its border and its radius are the framework card's; the page sets only the measure of the inside */
'.avp .av-tile{min-width:0;padding:9px 11px}',
'.avp .av-tile small{display:block;color:var(--av-mut);font-size:11px}',
'.avp .av-tile b{display:flex;gap:6px;align-items:center;font-weight:600;font-size:13px}',
'.avp .av-dot{width:8px;height:8px;border-radius:50%;flex:none;background:var(--av-ok)}',
'.avp .av-dot.warn{background:var(--av-warn)}.avp .av-dot.err{background:var(--av-err)}.avp .av-dot.off{background:var(--av-mut)}',
'.avp .av-bars{display:inline-flex;gap:2px;align-items:flex-end;height:16px}',
'.avp .av-bars i{display:block;width:4px;border-radius:1px;background:var(--av-bd2)}',
'.avp .av-bars i.on{background:var(--av-ok)}',
'.avp .av-bars.q3 i.on{background:var(--av-warn)}.avp .av-bars.q2 i.on{background:var(--av-err)}',
'.avp .av-bars i:nth-child(1){height:5px}.avp .av-bars i:nth-child(2){height:8px}',
'.avp .av-bars i:nth-child(3){height:11px}.avp .av-bars i:nth-child(4){height:14px}',
'.avp .av-kv{display:flex;justify-content:space-between;gap:10px;padding:7px 0;border-top:1px solid var(--av-bd)}',
'.avp .av-kv:first-of-type{border-top:0}.avp .av-kv>span:first-child{color:var(--av-mut)}',
'.avp .av-kv>span:last-child{text-align:right;min-width:0;overflow-wrap:anywhere}',
'.avp .av-big{font-size:28px;font-weight:600;letter-spacing:1px;line-height:1.2}',
'.avp .av-tag{display:inline-block;padding:1px 7px;border:1px solid var(--av-bd2);border-radius:9px;font-size:10px;',
'font-weight:600;letter-spacing:.3px;text-transform:uppercase;color:var(--av-mut);white-space:nowrap}',
'.avp .av-badge{display:inline-flex;align-items:center;justify-content:center;min-width:18px;height:18px;padding:0 5px;margin-left:6px;',
'border-radius:9px;background:var(--av-err);color:#fff;font-size:11px;font-weight:600}',
'.avp .av-badge[hidden]{display:none}',
'.avp .av-note{margin:8px 0 0;color:var(--av-mut);font-size:12px}',
/* the recording controls' busy state: the icon hides and a ring spins in its place */
'.avp .av-busy svg{display:none}',
'.avp .av-busy::after{content:\'\';display:inline-block;width:11px;height:11px;margin:0 6px 0 2px;',
'\tborder:2px solid currentColor;border-right-color:transparent;border-radius:50%;vertical-align:-2px;',
'\tanimation:av-rot .7s linear infinite}',
'@keyframes av-rot{to{transform:rotate(360deg)}}',

/* buttons, chips, filter bar */
'.avp .av-chip{min-height:var(--av-chip)}',
/* a row of pills is a tab row: the theme gives it the same panel it gives the page's own tab bar above it */
'.avp .av-chips{display:flex;flex-wrap:wrap;gap:6px;margin:0 0 10px}',
'.avp .av-chip{display:inline-flex;gap:6px;align-items:center;padding:3px 12px;border:1px solid var(--av-bd2);border-radius:14px;',
'background:transparent;color:var(--av-fg);font:inherit;font-size:12px;cursor:pointer}',
'.avp .av-chip[aria-pressed=true]{background:var(--av-dim);border-color:var(--av-ac);color:var(--av-ac)}',
'.avp .av-chip:focus-visible,.avp .av-row:focus-visible{outline:2px solid var(--av-ac);outline-offset:1px}',
/* the box, its border and its radius are the framework card's; here only the measure of the row is the page's */
'.avp .av-fl{display:grid;gap:8px;align-items:center;margin:0 0 10px;grid-template-columns:minmax(0,1fr) 150px auto auto}',
'.avp .av-fl input,.avp .av-fl select{width:100%;min-width:0;max-width:100%}',

/* call list */
'.avp .av-split{display:grid;gap:12px;align-items:start;grid-template-columns:minmax(0,1fr)}',
'.avp .av-list{padding-top:4px;padding-bottom:4px}',
'.avp .av-row{display:grid;width:100%;gap:0 10px;align-items:center;text-align:left;cursor:pointer;color:inherit;font:inherit;',
'grid-template-columns:28px minmax(0,1fr) auto;grid-template-areas:"i n d" "i m t";',
'padding:6px 4px;min-height:var(--av-row);background:transparent;border:0;border-top:1px solid var(--av-bd);border-radius:0}',
'.avp .av-row:first-child{border-top:0}',
'.avp .av-row:hover,.avp .av-row[aria-current=true]{background:var(--av-sub)}',
'.avp .av-row .av-i{grid-area:i;font-size:19px}',
'.avp .av-row .n{grid-area:n;font-size:13px;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
'.avp .av-row .d{grid-area:d;color:var(--av-mut);font-size:12px;text-align:right}',
'.avp .av-row .m{grid-area:m;color:var(--av-mut);font-size:12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}',
'.avp .av-row .t{grid-area:t;font-size:12px;text-align:right}',
'.avp .av-in{color:var(--av-ok)}.avp .av-out{color:var(--av-ac)}.avp .av-miss{color:var(--av-err)}',
'.avp .av-empty{padding:22px 8px;text-align:center;color:var(--av-mut)}',
'.avp .av-detail{display:none;position:sticky;top:8px}',
'.avp .av-inl{display:block;margin:0 -4px;padding:10px 8px 12px;background:var(--av-sub);border-top:1px solid var(--av-ac)}',
'.avp .av-acts{display:flex;flex-wrap:wrap;gap:8px;margin-top:10px}',

/* simple lists (phone book, speed dial, alarms) */
'.avp .av-it{display:flex;gap:10px;align-items:center;padding:6px 0;border-top:1px solid var(--av-bd);min-height:var(--av-it)}',
'.avp .av-it:first-child{border-top:0}.avp .av-it>.av-i{font-size:20px;color:var(--av-mut)}',
'.avp .av-it .t{flex:1;min-width:0;overflow-wrap:anywhere}.avp .av-it .t small{display:block;color:var(--av-mut)}',

/* settings rows reuse .cbi-value; keep inputs compact and aligned */
'.avp .av-inline{display:flex;flex-wrap:wrap;gap:6px;align-items:center}',
'.avp .av-inline input[type=number]{width:80px}',
'.avp .av-inline input.wide{width:170px}',
'.avp .av-modalrow .av-inline input.cbi-input-text{width:auto;flex:1 1 140px;min-width:0}',
'.avp .av-modalrow{display:grid;gap:4px;margin:0 0 10px}',
'.avp .av-ta{width:100%}.avp .av-modalrow input.cbi-input-text,.avp .av-modalrow select{width:100%;max-width:none}',
'.avp .av-modalrow>label{font-weight:600}',
'.avp .av-days{display:flex;flex-wrap:wrap;gap:6px 12px}',
'.avp .av-days label{display:inline-flex;gap:4px;align-items:center}',
'.avp .av-err-msg{color:var(--av-err);min-height:1.2em;margin:4px 0 0}',

/* log */
/* the log block takes its surface from the framework's own text field: only the reading of it is the page's */
'.avp .av-log{max-height:160px;overflow:auto;white-space:pre-wrap;word-break:break-all;font-size:12px;',
'color:var(--av-mut)}',
'.avp details>summary{cursor:pointer;color:var(--av-ac);display:flex;align-items:center;min-height:var(--av-chip)}',
/* the code a phone scans: the framework's own renderer draws the square, and the page only sizes and centres
   it - the code carries its own light ground, so the theme's panel shows around it, not through it */
'.avp .av-qr{display:flex;justify-content:center;margin:6px 0 0}',
'.avp .av-qr svg{width:212px;height:212px}',
'.avp .av-qr-url{margin:8px 0 0;text-align:center;overflow-wrap:anywhere;font-size:12px}',

/* the message of a moment: the theme paints its box like every other message box, the page only places it */
'.av-toast{position:fixed;left:50%;bottom:18px;transform:translateX(-50%);z-index:3000;max-width:90vw;margin:0}',
'.av-toast[hidden]{display:none}',

/* tab bar: keep it on one scrolling line on narrow screens */
'.avp .cbi-tabmenu{overflow-x:auto;overflow-y:hidden;white-space:nowrap;flex-wrap:nowrap}',
'.avp .cbi-tabmenu li{flex:none}',
'.avp .cbi-tabmenu li a .av-i{margin-right:6px;opacity:.85}',
/* the tab pane's own breathing room, on the pane that is in view only: every pane lives in the page at once, and a
   rule that reached the hidden ones would add its space to each of them, so the gap under the tab bar would grow
   with the tab's place in the row */
'.avp div[data-tab][data-tab-active=true]{padding-top:20px}',

/* wide: side detail panel, inline expander off */
'@container avp (min-width:1024px){',
'.avp .av-split{grid-template-columns:minmax(0,1fr) 300px}',
'.avp .av-detail{display:block}.avp .av-inl{display:none}}',

'@container avp (max-width:1023px){',
'.avp .av-tiles{grid-template-columns:repeat(2,minmax(0,1fr))}}',
/* touch sizing follows the viewport, like the in-house theme (<= 1023px) */
'@media (max-width:1023px){.avp{--av-chip:32px;--av-row:52px;--av-it:48px}}',

'@container avp (max-width:700px){',
'.avp .av-g2,.avp .av-g3,.avp .av-callgrid,.avp .av-svcgrid,.avp .av-dirgrid{grid-template-columns:minmax(0,1fr)}',
'.avp .av-fl{grid-template-columns:minmax(0,1fr) minmax(0,1fr)}',
'.avp .av-fl .q{grid-column:1/-1}',
'.avp .av-big{font-size:24px}',
'.avp .av-inline input.wide{width:100%}}',

'@container avp (max-width:420px){',
'.avp .av-kv{flex-direction:column;gap:0}.avp .av-kv>span:last-child{text-align:left}',
'.avp .av-key{min-height:54px;font-size:19px}',
'.avp .av-padwrap{grid-template-columns:minmax(0,1fr) 96px}',
'.avp .av-railbtn .av-i{font-size:21px}',
'.avp .av-railbtn span:last-child{font-size:13px}}',

'@media (prefers-reduced-motion:reduce){.avp *{transition:none!important;animation:none!important}}'
].join('');

return baseclass.extend({
	/* Inject once; safe to call on every render. */
	inject: function() {
		if (document.getElementById(STYLE_ID))
			return;
		var st = E('style', { 'id': STYLE_ID });
		st.appendChild(document.createTextNode(CSS));
		document.head.appendChild(st);
	}
});
