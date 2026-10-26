SPDX-License-Identifier: GPL-2.0-only. Copyright (C) 2026 Pr0Luci.

Render check for the phone page (no router needed).

  cd packages/luci-app-awvoice && python3 -m http.server 8099
  open http://localhost:8099/tests/harness/index.html?state=call

fixture.js replaces phone/live.js here; the links on the page switch the line state and the later features.
