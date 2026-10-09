import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

async function fixture() {
  let library;
  const gets = [];
  let short = false;
  const heap = new Uint8Array(3 * 1024 * 1024);
  const context = {
    addToLibrary: value => library = value, wasmFS$backends: {},
    UTF8ToString: value => value,
    __wasmfs_fetch_get_file_url: file => '/assets/' + file,
    __wasmfs_fetch_get_chunk_size: () => 1024 * 1024,
    self: {location: {origin: 'https://game.test'}}, URL, HEAPU8: heap,
    fetch: async (url, options) => {
      const size = 3 * 1024 * 1024 + 17;
      if (options.method === 'HEAD') return {ok: true, headers: new Headers({
        'Content-Length': String(size), 'Accept-Ranges': 'bytes'})};
      gets.push(url + ':' + options.headers.Range);
      const [, left, right] = options.headers.Range.match(/bytes=(\d+)-(\d+)/);
      const start = Number(left), end = Number(right);
      const data = new Uint8Array(end - start + 1 - (short ? 1 : 0));
      for (let i = 0; i < data.length; i++) data[i] = (start + i) % 251;
      return {status: 206, headers: new Headers({'Content-Range': `bytes ${start}-${end}/${size}`}),
              arrayBuffer: async () => data.buffer};
    },
  };
  const source = (await readFile(new URL('../../player/fetch-backend.js', import.meta.url), 'utf8'))
    .replaceAll('{{{ cDefs.EROFS }}}', '30').replaceAll('{{{ cDefs.EIO }}}', '5');
  vm.runInNewContext(source, context);
  await library._wasmfs_create_fetch_backend_js(1);
  return {backend: context.wasmFS$backends[1], heap, gets, truncate: () => short = true};
}

test('bounded fetch reads cross chunk boundaries, caches and clips at EOF', async () => {
  const f = await fixture();
  assert.equal(await f.backend.getSize(1), 3 * 1024 * 1024 + 17);
  assert.equal(f.gets.length, 0);
  const offset = 1024 * 1024 - 23;
  assert.equal(await f.backend.read(1, 0, 100, offset), 100);
  for (let i = 0; i < 100; i++) assert.equal(f.heap[i], (offset + i) % 251);
  assert.equal(f.gets.length, 2);
  assert.equal(await f.backend.read(1, 0, 100, offset), 100);
  assert.equal(f.gets.length, 2);
  assert.equal(await f.backend.read(1, 0, 100, 3 * 1024 * 1024), 17);
  assert.equal(await f.backend.read(1, 0, 100, 4 * 1024 * 1024), 0);
  assert.equal(await f.backend.write(), -30);
});

test('64 MiB LRU evicts old ranges, touches recent reads and releases files', async () => {
  const f = await fixture();
  for (let file = 1; file <= 64; file++) assert.equal(await f.backend.read(file, 0, 1, 0), 1);
  await f.backend.read(1, 0, 1, 0);
  await f.backend.read(65, 0, 1, 0);
  assert.equal(f.gets.length, 65);
  await f.backend.read(1, 0, 1, 0);
  assert.equal(f.gets.length, 65);
  await f.backend.read(2, 0, 1, 0);
  assert.equal(f.gets.length, 66);
  await f.backend.freeFile(1);
  await f.backend.read(1, 0, 1, 0);
  assert.equal(f.gets.length, 67);
});

test('incomplete ranges fail instead of supplying truncated game data', async () => {
  const f = await fixture();
  f.truncate();
  assert.equal(await f.backend.read(1, 0, 100, 0), -5);
});
