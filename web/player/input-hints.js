// Display the device used most recently, rather than treating a connected but
// idle controller as the player's choice. Only labels change; SDL owns input.
export function controllerLabels(id = "") {
  if (/dualsense|dualshock|playstation|sony|054c/i.test(id))
    return { name: "PlayStation controller", confirm: "✕", back: "○" };
  if (/nintendo|switch|pro controller|057e/i.test(id))
    return { name: "Nintendo controller", confirm: "B (bottom)", back: "A (right)" };
  return { name: /xbox|xinput|045e/i.test(id) ? "Xbox controller" : "Controller",
           confirm: "A / bottom button", back: "B / right button" };
}

// Actions are supplied by the game, while bindings come from its native mapped
// pad configuration. Face-button position must not imply a menu action.
export function inputHintText(id, config) {
  const labels = id === null ? null : controllerLabels(id);
  if (typeof config?.keyboard === "string" || typeof config?.controller === "string")
    return hintText(labels, config);
  if (!config?.actions || !Object.keys(config.actions).length)
    return labels ? `${labels.name} · D-pad: browse · ${labels.confirm}: confirm · ${labels.back}: back · Use keyboard to type names`
      : "Keyboard · Arrow keys: browse · Enter: confirm · Esc: back · Type names · Backspace: delete";
  const playstation = labels?.name.startsWith("PlayStation");
  const nintendo = labels?.name.startsWith("Nintendo");
  const buttons = playstation ? {cross:"✕",circle:"○",square:"□",triangle:"△",r2:"R2",l2:"L2",r1:"R1",l1:"L1",start:"Options"}
    : nintendo ? {cross:"B",circle:"A",square:"Y",triangle:"X",r2:"ZR",l2:"ZL",r1:"R",l1:"L",start:"+"}
    : {cross:"A",circle:"B",square:"X",triangle:"Y",r2:"RT",l2:"LT",r1:"RB",l1:"LB",start:labels?.name === "Xbox controller" ? "Menu" : "Start"};
  if (labels?.name === "Controller") Object.assign(buttons, {
    cross:"Bottom button",circle:"Right button",square:"Left button",triangle:"Top button",
    r2:"Right trigger",l2:"Left trigger",r1:"Right shoulder",l1:"Left shoulder",
  });
  const keyboard = value => ({"key:Return":"Enter", "key:Escape":"Esc", "key:Up":"↑", "key:Down":"↓",
    "key:Space":"Space", "key:LShift":"Shift", "key:RShift":"Shift", "horizontal_arrows":"←/→"})[value] || value.replace(/^key:/, "");
  const order = ["r2","l2","start","triangle","cross","square","circle","l1","r1","select","l3","r3"];
  const text = [labels ? labels.name : "Keyboard"];
  if (!labels || config.bindings?.dpad === "arrows") text.push(labels ? "D-pad: menus" : "Arrow keys: menus");
  for (const [action, value] of Object.entries(config.actions)) {
    let button = keyboard(value);
    if (labels) {
      const binding = order.find(name => config.bindings?.[name] === value);
      if (binding) button = buttons[binding] || binding.toUpperCase();
      else if (config.bindings?.left_stick === value) button = "Left stick";
      else if (config.bindings?.right_stick === value) button = "Right stick";
      else button += " key";
    }
    text.push(`${button}: ${action === "back" ? "back/pause" : action}`);
  }
  text.push(labels ? "Keyboard: names" : "Type names · Backspace: delete");
  return text.join(" · ");
}

// `custom` ({keyboard, controller}, game.toml [launcher] input_hints) replaces
// the generic lines; {name}, {confirm} and {back} in the controller line take
// the active pad's labels.
export function hintText(labels, custom = null) {
  if (labels) {
    const line = custom?.controller ||
      "{name} · D-pad: browse · {confirm}: confirm · {back}: back · Use keyboard to type names";
    return line.replace(/\{(name|confirm|back)\}/g, (_, k) => labels[k]);
  }
  return custom?.keyboard ||
    "Keyboard · Arrow keys: browse · Enter: confirm · Esc: back · Type names · Backspace: delete";
}

// The third argument is the hint config itself or a function returning it.
export function startInputHints(render, readPads = () => navigator.getGamepads?.() || [], config = null) {
  const readConfig = typeof config === "function" ? config : () => config;
  let active = "keyboard";
  let selected = null;
  let first = true;
  let previous = new Map();
  let last = "";
  const keyboard = event => {
    if (!event.ctrlKey && !event.altKey && !event.metaKey) active = "keyboard";
  };
  window.addEventListener("keydown", keyboard, true);
  const timer = setInterval(() => {
    const pads = Array.from(readPads()).filter(pad => pad?.connected);
    if (first && pads.length) { selected = pads[0]; active = "controller"; }
    first = false;
    const next = new Map();
    for (const pad of pads) {
      const signature = pad.buttons.map(b => b.pressed ? 1 : 0).join("") + ":" +
        pad.axes.map(a => Math.abs(a) > .25 ? Math.round(a * 10) : 0).join(",");
      const engaged = pad.buttons.some(b => b.pressed) || pad.axes.some(a => Math.abs(a) > .25);
      if (engaged && signature !== previous.get(pad.index)) { selected = pad; active = "controller"; }
      next.set(pad.index, signature);
    }
    previous = next;
    if (active === "controller" && !pads.some(pad => pad.index === selected?.index)) active = "keyboard";
    const text = inputHintText(active === "controller" ? selected?.id || "" : null, readConfig());
    if (text !== last) { render(text); last = text; }
  }, 150);
  return () => { clearInterval(timer); window.removeEventListener("keydown", keyboard, true); };
}
