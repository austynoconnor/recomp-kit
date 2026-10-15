// pad-filter.js - what the game sees of the browser's game pads.
//
// The game reads pads through SDL, which reads navigator.getGamepads(). SDL
// trusts a pad's resting values and, for a pad the browser did not map to the
// "standard" layout, guesses a layout from a database written for other
// platforms. A pad whose trigger or stick rests at -1 or +1 (a Bluetooth Xbox
// pad Chrome leaves unmapped, a wheel, a flight stick, a virtual device) then
// holds a direction forever: menus that scroll by themselves, buttons that do
// nothing. This filter sits in front of SDL:
//  - a pad the browser maps to "standard" passes, with a dead zone;
//  - a known unmapped layout (Microsoft's Bluetooth Xbox pads) is rebuilt as
//    a standard pad;
//  - any other unmapped device is hidden (RECOMP_RAW_PADS=1 lets it through);
//  - every axis and button must first be seen at rest before it counts, so a
//    value that rests away from zero, or a button held at connect, is ignored.
// Classic script: it must run before the game's loader installs SDL's
// listeners. Also importable for tests (globalThis.RecompPadFilter).
(function (root) {
  "use strict";
  const DEAD = 0.15;   // stick dead zone, of full deflection
  const REST = 0.3;    // how near its rest value an input must be to arm
  const NOT_PADS = /speaker|sound ?bar|headset|headphone|earbud|audio|keyboard|mouse|remote control/i;
  const HAT = [[-1, "u"], [-5 / 7, "ur"], [-3 / 7, "r"], [-1 / 7, "dr"],
               [1 / 7, "d"], [3 / 7, "dl"], [5 / 7, "l"], [1, "ul"]];

  function vendorProduct(id) {
    const m = /Vendor:\s*([0-9a-f]{4}).*?Product:\s*([0-9a-f]{4})/i.exec(id || "") ||
              /^([0-9a-f]{4})-([0-9a-f]{4})-/i.exec(id || "");
    return m ? [m[1].toLowerCase(), m[2].toLowerCase()] : ["", ""];
  }
  function hatDirs(v) {
    for (const [h, d] of HAT) if (Math.abs(v - h) < 0.05) return d;
    return "";
  }
  function value(b) {
    return typeof b === "number" ? b : b ? (b.value || (b.pressed ? 1 : 0)) : 0;
  }
  function deadzone(v) {
    const a = Math.abs(v);
    return a < DEAD ? 0 : Math.sign(v) * Math.min(1, (a - DEAD) / (1 - DEAD));
  }

  // How a pad becomes standard inputs: `layout` names it; `buttons[i]` is
  // the source button of standard button i (or -1), and each axis source is
  // {axis, kind}: "stick" (rests at 0), "trigger" (rests at -1, reads -1..1),
  // "zpos"/"zneg" (one half of a combined trigger axis resting at 0).
  function plan(pad, allowRaw) {
    const nb = pad.buttons.length, na = pad.axes.length;
    // Bluetooth audio and other HID devices Windows lists as game controllers
    // (a "BT Speaker Bar" with an axis resting at +1 scrolled menus by itself).
    if (NOT_PADS.test(pad.id || "") && !allowRaw) return null;
    if (pad.mapping === "standard") {
      return { layout: "standard", buttons: [...Array(17).keys()].map(i => (i < nb ? i : -1)),
               sticks: [0, 1, 2, 3].map(i => (i < na ? { axis: i, kind: "stick" } : null)),
               triggers: null, hat: -1 };
    }
    const [vendor] = vendorProduct(pad.id);
    if (vendor === "045e" && nb >= 10 && na >= 4) {
      const hat = na > 9 ? 9 : -1;
      if (nb >= 15) {
        // Bluetooth LE firmware (0b13, 0b20, 0b12...): A B _ X Y _ LB RB _ _
        // View Menu Xbox LS RS; sticks X Y and Z Rz; triggers are axes that
        // rest at -1.
        const resting = [];
        for (let i = 2; i < na; ++i)
          if (i !== 5 && i !== hat && Math.abs(pad.axes[i] + 1) < 0.05) resting.push(i);
        const b = [0, 1, 3, 4, 6, 7, -1, -1, 10, 11, 13, 14, -1, -1, -1, -1, 12];
        return { layout: "xbox-bluetooth-le", buttons: b.map(i => (i < nb ? i : -1)),
                 sticks: [{ axis: 0, kind: "stick" }, { axis: 1, kind: "stick" },
                          { axis: 2, kind: "stick" }, na > 5 ? { axis: 5, kind: "stick" } : null],
                 triggers: resting.length >= 2
                   ? [{ axis: resting[0], kind: "trigger" }, { axis: resting[1], kind: "trigger" }] : null,
                 hat };
      }
      // Classic Bluetooth firmware (02e0, 02fd): A B X Y LB RB View Menu LS
      // RS; sticks X Y and Rx Ry; both triggers share Z.
      const b = [0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, -1, -1, -1, -1, -1];
      return { layout: "xbox-bluetooth", buttons: b,
               sticks: [{ axis: 0, kind: "stick" }, { axis: 1, kind: "stick" },
                        { axis: 3, kind: "stick" }, { axis: 4, kind: "stick" }],
               triggers: [{ axis: 2, kind: "zpos" }, { axis: 2, kind: "zneg" }], hat };
    }
    if (allowRaw)
      return { layout: "raw", raw: true };
    return null;
  }

  function create(options = {}) {
    const allowRaw = !!options.allowRaw;
    const log = options.log || (() => {});
    const state = new Map(); // index -> {id, plan, armed axes/buttons, virtual}
    const rest = { stick: 0, trigger: -1, zpos: 0, zneg: 0 };

    function track(pad) {
      let s = state.get(pad.index);
      if (s && s.id === pad.id) return s;
      const p = plan(pad, allowRaw);
      s = { id: pad.id, plan: p, axisArmed: new Map(), buttonArmed: new Map(), hatArmed: false };
      state.set(pad.index, s);
      log(`[pads] ${pad.index}: "${pad.id}" mapping "${pad.mapping || "none"}", ` +
          `${pad.axes.length} axes [${pad.axes.map(a => a.toFixed(2)).join(" ")}], ` +
          `${pad.buttons.length} buttons -> ${p ? p.layout : "ignored (unmapped device)"}`);
      return s;
    }
    // An input counts once it has been seen near its rest value.
    function axisValue(s, pad, src) {
      if (!src || src.axis >= pad.axes.length) return src && src.kind === "stick" ? 0 : -1;
      const v = pad.axes[src.axis];
      const key = src.axis + src.kind;
      if (!s.axisArmed.get(key)) {
        if (Math.abs(v - rest[src.kind]) < REST) s.axisArmed.set(key, true);
        else return src.kind === "stick" ? 0 : -1;
      }
      switch (src.kind) {
      case "stick": return deadzone(v);
      case "trigger": return v;
      case "zpos": return Math.max(0, v) * 2 - 1;
      default: return Math.max(0, -v) * 2 - 1;
      }
    }
    function buttonValue(s, pad, i) {
      if (i < 0 || i >= pad.buttons.length) return 0;
      const v = value(pad.buttons[i]);
      if (!s.buttonArmed.get(i)) {
        if (v < 0.1) s.buttonArmed.set(i, true);
        else return 0;
      }
      return v;
    }

    function virtual(pad) {
      if (!pad || !pad.connected) return null;
      const s = track(pad);
      const p = s.plan;
      if (!p) return null;
      if (p.raw) return pad;
      const buttons = p.buttons.map(i => buttonValue(s, pad, i));
      if (p.triggers) {
        buttons[6] = (axisValue(s, pad, p.triggers[0]) + 1) / 2;
        buttons[7] = (axisValue(s, pad, p.triggers[1]) + 1) / 2;
      }
      if (p.hat >= 0 && p.hat < pad.axes.length) {
        const h = pad.axes[p.hat];
        const d = hatDirs(h);
        if (!s.hatArmed && !d) s.hatArmed = true; // seen centred
        if (s.hatArmed) {
          buttons[12] = d.includes("u") ? 1 : 0;
          buttons[13] = d.includes("d") ? 1 : 0;
          buttons[14] = d.includes("l") ? 1 : 0;
          buttons[15] = d.includes("r") ? 1 : 0;
        }
      }
      const axes = p.sticks.map(src => axisValue(s, pad, src));
      const [vendor, product] = vendorProduct(pad.id);
      const id = p.layout === "standard" || !vendor ? pad.id
        : `Xbox Wireless Controller (STANDARD GAMEPAD Vendor: ${vendor} Product: ${product})`;
      return {
        id, index: pad.index, connected: true, mapping: "standard", timestamp: pad.timestamp,
        axes, buttons: buttons.map(v => ({ pressed: v > 0.5, touched: v > 0, value: v })),
        vibrationActuator: pad.vibrationActuator || null, hapticActuators: pad.hapticActuators || [],
      };
    }

    function filter(list) {
      return Array.from(list || [], pad => virtual(pad));
    }
    function describe(list) {
      return Array.from(list || []).filter(Boolean).map(pad => {
        const s = state.get(pad.index);
        return { index: pad.index, id: pad.id, mapping: pad.mapping, connected: pad.connected,
                 timestamp: Math.round(pad.timestamp || 0),
                 axes: Array.from(pad.axes, a => +a.toFixed(3)),
                 pressed: Array.from(pad.buttons, (b, i) => (value(b) > 0.5 ? i : -1)).filter(i => i >= 0),
                 handling: s && s.id === pad.id ? (s.plan ? s.plan.layout : "ignored") : "not seen" };
      });
    }
    return { filter, virtual, describe, plan: pad => plan(pad, allowRaw) };
  }

  // Wraps the page's Gamepad API: getGamepads() and the connect events.
  function install(win, options = {}) {
    const nav = win.navigator;
    if (!nav || !nav.getGamepads || nav.__recompPads) return null;
    const raw = nav.getGamepads.bind(nav);
    const f = create(options);
    nav.__recompPads = f;
    nav.getGamepads = () => f.filter(raw());
    const resend = (event) => {
      if (event.__recomp) return;
      event.stopImmediatePropagation();
      const v = event.type === "gamepadconnected" ? f.virtual(event.gamepad) : null;
      if (event.type === "gamepadconnected" && !v) return; // hidden device
      const copy = new Event(event.type);
      Object.defineProperty(copy, "gamepad", { value: v || event.gamepad });
      copy.__recomp = true;
      win.dispatchEvent(copy);
    };
    win.addEventListener("gamepadconnected", resend, true);
    win.addEventListener("gamepaddisconnected", resend, true);
    win.recompPads = () => f.describe(raw());
    return f;
  }

  root.RecompPadFilter = { create, install, plan, hatDirs, vendorProduct };
  if (root.window === root && root.navigator && !root.__recompNoPadInstall) {
    const allowRaw = new URLSearchParams(root.location.search).get("RECOMP_RAW_PADS") === "1";
    install(root, { allowRaw, log: (text) => (root.recompPadLog || console.info)(text) });
  }
})(globalThis);
