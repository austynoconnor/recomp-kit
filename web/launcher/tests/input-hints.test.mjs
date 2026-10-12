import { test } from 'node:test';
import assert from 'node:assert/strict';
import { controllerLabels, hintText } from '../../player/input-hints.js';

test('generic lines without game hints', () => {
  assert.match(hintText(null), /^Keyboard · Arrow keys: browse/);
  assert.match(hintText(controllerLabels('Xbox 360 Controller')), /^Xbox controller · D-pad/);
});

test('game hints replace the lines and take the pad labels', () => {
  const custom = { keyboard: 'WASD: drive', controller: '{name} · {confirm}: Drive · {back}: Reverse' };
  assert.equal(hintText(null, custom), 'WASD: drive');
  assert.equal(hintText(controllerLabels('DualSense Wireless Controller'), custom),
               'PlayStation controller · ✕: Drive · ○: Reverse');
  // Only the keyboard line set: the controller line stays generic.
  assert.match(hintText(controllerLabels('xinput'), { keyboard: 'k' }), /D-pad: browse/);
});
