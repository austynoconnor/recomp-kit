import { test } from 'node:test';
import assert from 'node:assert/strict';

globalThis.__recompNoPadInstall = true;
await import('../../player/pad-filter.js');
const { create } = globalThis.RecompPadFilter;

const btn = v => ({ pressed: v > 0.5, touched: v > 0, value: v });
function pad(over = {}) {
  return { index: 0, connected: true, timestamp: 1, mapping: 'standard',
           id: 'Xbox 360 Controller (XInput STANDARD GAMEPAD)',
           axes: [0, 0, 0, 0], buttons: Array.from({ length: 17 }, () => btn(0)), ...over };
}

test('standard pad passes with a dead zone', () => {
  const f = create();
  const p = pad({ axes: [0, 0, 0, 1] });
  f.virtual(p);
  p.axes[0] = 0.1; p.axes[1] = -0.5;
  const v = f.virtual(p);
  assert.equal(v.mapping, 'standard');
  assert.equal(v.axes[0], 0);
  assert.ok(v.axes[1] < -0.4);
  assert.equal(v.axes[3], 0, 'an axis resting at +1 is ignored');
});

test('an axis resting at -1 stays ignored until it returns to centre', () => {
  const f = create();
  const p = pad({ axes: [0, -1, 0, 0] });
  assert.equal(f.virtual(p).axes[1], 0);
  assert.equal(f.virtual(p).axes[1], 0, 'still at rest value: no phantom up');
  p.axes[1] = 0;
  assert.equal(f.virtual(p).axes[1], 0);
  p.axes[1] = -1;
  assert.equal(f.virtual(p).axes[1], -1, 'counts after it has been seen centred');
});

test('a button held at connect counts only after a release', () => {
  const f = create();
  const p = pad();
  p.buttons[13] = btn(1);
  assert.equal(f.virtual(p).buttons[13].pressed, false);
  p.buttons[13] = btn(0);
  f.virtual(p);
  p.buttons[13] = btn(1);
  assert.equal(f.virtual(p).buttons[13].pressed, true);
});

test('unknown unmapped devices are hidden unless raw pads are allowed', () => {
  const wheel = pad({ mapping: '', id: 'Wheel (Vendor: 046d Product: c24f)', axes: [0, -1, -1, 1] });
  assert.equal(create().virtual(wheel), null);
  assert.equal(create({ allowRaw: true }).virtual(wheel), wheel);
  assert.equal(create().virtual(pad({ connected: false })), null);
});

test('Bluetooth LE Xbox pad (unmapped, triggers resting at -1) becomes standard', () => {
  const f = create();
  const axes = [0, 0, 0, -1, -1, 0, 0, 0, 0, 3.2857];
  const buttons = Array.from({ length: 16 }, () => btn(0));
  const p = pad({ mapping: '', id: 'Xbox Wireless Controller (Vendor: 045e Product: 0b20)', axes, buttons });
  let v = f.virtual(p);
  assert.match(v.id, /STANDARD GAMEPAD Vendor: 045e Product: 0b20/);
  assert.deepEqual(v.axes, [0, 0, 0, 0]);
  assert.equal(v.buttons[6].value, 0, 'LT at rest');
  assert.equal(v.buttons[13].pressed, false, 'no phantom d-pad down');
  p.axes[4] = 1; // right trigger fully in
  p.axes[9] = 1 / 7; // hat down
  p.buttons[0] = btn(1); // A
  p.buttons[11] = btn(1); // Menu
  v = f.virtual(p);
  assert.equal(v.buttons[7].value, 1);
  assert.equal(v.buttons[13].pressed, true);
  assert.equal(v.buttons[0].pressed, true);
  assert.equal(v.buttons[9].pressed, true, 'Menu is Start');
});

test('classic Bluetooth Xbox pad: shared trigger axis and Rx/Ry stick', () => {
  const f = create();
  const p = pad({ mapping: '', id: '045e-02e0-Xbox Wireless Controller',
                  axes: [0, 0, 0, 0, 0], buttons: Array.from({ length: 10 }, () => btn(0)) });
  let v = f.virtual(p);
  assert.equal(v.buttons[6].value, 0);
  assert.equal(v.buttons[7].value, 0);
  p.axes[2] = 1; p.axes[4] = 0.9; p.buttons[7] = btn(1);
  v = f.virtual(p);
  assert.equal(v.buttons[6].value, 1, 'LT from Z+');
  assert.equal(v.buttons[7].value, 0);
  assert.ok(v.axes[3] > 0.8, 'right stick Y from Ry');
  assert.equal(v.buttons[9].pressed, true, 'Menu is Start');
});

test('a speaker bar in slot 0 is hidden and the Xbox pad in slot 1 passes', () => {
  const f = create();
  const bar = pad({ index: 0, mapping: '', id: 'CA-2890 BT Speaker Bar (Vendor: 3391 Product: 0008)',
                    axes: [0, 1, 0, 0], buttons: Array.from({ length: 8 }, () => btn(0)) });
  const xbox = pad({ index: 1 });
  const out = f.filter([bar, xbox, null, null]);
  assert.equal(out[0], null);
  assert.equal(out[1].index, 1);
  assert.equal(out[2], null);
  // Even if a browser called it standard, the name keeps it out.
  assert.equal(create().virtual({ ...bar, mapping: 'standard' }), null);
});
