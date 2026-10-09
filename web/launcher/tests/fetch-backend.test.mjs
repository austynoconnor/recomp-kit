import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import vm from 'node:vm';

// A Cache Storage stand-in shared between page visits (fixture calls).
function fakeCaches({full = false} = {}) {
  const entries = new Map();
  return {entries, caches: {open: async () => ({
    match: async key => entries.has(key) ? new Response(entries.get(key)) : undefined,
    put: async (key, response) => {
      if (full) throw new Error('QuotaExceededError');
      entries.set(key, new Uint8Array(await response.arrayBuffer()));
    },
  })}};
}

async function fixture({store = null, etag = null} = {}) {
  let library;
  const gets = [];
  let short = false;
  const heap = new Uint8Array(3 * 1024 * 1024);
  const context = {
    addToLibrary: value => library = value, wasmFS$backends: {},
    UTF8ToString: value => value,
    __wasmfs_fetch_get_file_url: file => '/assets/' + file,
    __wasmfs_fetch_get_chunk_size: () => 1024 * 1024,
    self: {location: {origin: 'https://game.test'}, ...(store ? {caches: store.caches} : {})},
    caches: store?.caches, URL, Response, HEAPU8: heap,
    navigator: {storage: {estimate: async () => ({usage: 0, quota: 1e9})}},
    fetch: async (url, options) => {
      const size = 3 * 1024 * 1024 + 17;
      if (options.method === 'HEAD') return {ok: true, headers: new Headers({
        'Content-Length': String(size), 'Accept-Ranges': 'bytes', ...(etag ? {ETag: etag} : {})})};
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

test('Cache Storage keeps pieces for the next visit and drops them when the file changes', async () => {
  const store = fakeCaches();
  const first = await fixture({store, etag: '"v1"'});
  assert.equal(await first.backend.read(1, 0, 100, 5), 100);
  assert.equal(first.gets.length, 1);
  await new Promise(resolve => setTimeout(resolve, 0)); // let the background put land
  assert.equal(store.entries.size, 1);
  const again = await fixture({store, etag: '"v1"'});
  assert.equal(await again.backend.read(1, 0, 100, 5), 100);
  for (let i = 0; i < 100; i++) assert.equal(again.heap[i], (5 + i) % 251);
  assert.equal(again.gets.length, 0);
  const changed = await fixture({store, etag: '"v2"'});
  assert.equal(await changed.backend.read(1, 0, 100, 5), 100);
  assert.equal(changed.gets.length, 1);
});

test('a full or missing Cache Storage still serves reads from the network', async () => {
  const full = await fixture({store: fakeCaches({full: true})});
  assert.equal(await full.backend.read(1, 0, 10, 0), 10);
  assert.equal(await full.backend.read(2, 0, 10, 0), 10);
  assert.equal(full.gets.length, 2);
  const none = await fixture();
  assert.equal(await none.backend.read(1, 0, 10, 0), 10);
});

test('the backend exists before Cache Storage finishes opening', async () => {
  let library;
  let open;
  const context = {
    addToLibrary: value => library = value, wasmFS$backends: {}, UTF8ToString: v => v,
    __wasmfs_fetch_get_file_url: file => '/assets/' + file, __wasmfs_fetch_get_chunk_size: () => 1024 * 1024,
    self: {location: {origin: 'https://game.test'}, caches: {open: () => new Promise(r => open = r)}},
    caches: {open: () => new Promise(r => open = r)}, URL, Response, HEAPU8: new Uint8Array(16),
    navigator: {storage: {estimate: async () => ({usage: 0, quota: 1e9})}},
  };
  const source = (await readFile(new URL('../../player/fetch-backend.js', import.meta.url), 'utf8'))
    .replaceAll('{{{ cDefs.EROFS }}}', '30').replaceAll('{{{ cDefs.EIO }}}', '5');
  vm.runInNewContext(source, context);
  library._wasmfs_create_fetch_backend_js(1); // WasmFS does not wait for it
  assert.equal(typeof context.wasmFS$backends[1]?.allocFile, 'function');
  open({match: async () => undefined, put: async () => {}});
});
