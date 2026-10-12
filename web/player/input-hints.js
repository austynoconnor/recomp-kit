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

export function startInputHints(render, readPads = () => navigator.getGamepads?.() || [], custom = null) {
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
    const labels = active === "controller" ? controllerLabels(selected?.id) : null;
    const text = hintText(labels, custom);
    if (text !== last) { render(text); last = text; }
  }, 150);
  return () => { clearInterval(timer); window.removeEventListener("keydown", keyboard, true); };
}
