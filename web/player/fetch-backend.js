// Bounded FetchFS reads for large games. Emscripten's default backend retains
// every fetched chunk for the lifetime of each file. This replacement uses a
// shared 64 MiB LRU; guest reads remain synchronous through the SDK proxy worker.
// Pieces are also kept in the browser's Cache Storage, keyed by the file's
// validator (ETag, else Last-Modified, else size), so a second visit reads
// them from disk instead of the network. A changed file gets new keys; the
// browser may evict the cache whenever it likes, which only costs a refetch.
// Uses the FetchFS bridge supplied by Emscripten 6.0.11.
addToLibrary({
  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends', '_wasmfs_fetch_get_file_url', '_wasmfs_fetch_get_chunk_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    const info = new Map();
    const cache = new Map();
    const limit = 64 * 1024 * 1024;
    let cachedBytes = 0;
    // Cache Storage needs a secure context (localhost counts). Any failure,
    // including a full quota, turns persistence off rather than the game.
    // It opens in the background: the backend must be registered before
    // this function first yields, or WasmFS allocates files on nothing.
    let store = null;
    let storeRoom = true;
    let puts = 0;
    async function checkRoom() {
      try {
        const {usage, quota} = await navigator.storage.estimate();
        storeRoom = !quota || usage < quota * 0.8;
      } catch {}
    }
    const storeReady = (async () => {
      try {
        if (self.caches) store = await caches.open('recomp-pieces-v1');
        if (store) await checkRoom();
      } catch { store = null; }
    })();
    function pieceKey(meta, index) {
      const key = new URL(meta.url);
      key.searchParams.set('recomp-piece', index + '.' + meta.chunkSize);
      key.searchParams.set('recomp-version', meta.version);
      return key.href;
    }
    async function stored(meta, index, start, end) {
      await storeReady;
      if (!store) return null;
      try {
        const response = await store.match(pieceKey(meta, index));
        if (!response) return null;
        const data = new Uint8Array(await response.arrayBuffer());
        return data.byteLength === end - start + 1 ? data : null;
      } catch { return null; }
    }
    function keep(meta, index, data) {
      if (!store || !storeRoom) return;
      // Not awaited: the guest read continues while the browser writes.
      store.put(pieceKey(meta, index), new Response(data.slice(), {
        headers: {'Content-Type': 'application/octet-stream'},
      })).catch(() => { storeRoom = false; });
      if (++puts % 64 === 0) checkRoom();
    }
    async function details(file) {
      if (info.has(file)) return info.get(file);
      const path = UTF8ToString(__wasmfs_fetch_get_file_url(file));
      const url = new URL(path, self.location.origin).href;
      const response = await fetch(url, {method: 'HEAD'});
      const size = Number(response.headers.get('Content-Length'));
      if (!response.ok || !response.headers.has('Content-Length') ||
          !Number.isSafeInteger(size) || size < 0 || response.headers.get('Accept-Ranges') !== 'bytes')
        throw new Error('Game server must support sized HTTP ranges.');
      const version = response.headers.get('ETag') || response.headers.get('Last-Modified') || String(size);
      const result = {url, size, version,
                      chunkSize: Math.min(__wasmfs_fetch_get_chunk_size(file), 1024 * 1024)};
      info.set(file, result);
      return result;
    }
    async function chunk(file, meta, index) {
      const key = file + ':' + index;
      if (cache.has(key)) {
        const data = cache.get(key);
        cache.delete(key);
        cache.set(key, data);
        return data;
      }
      const start = index * meta.chunkSize;
      const end = Math.min(start + meta.chunkSize, meta.size) - 1;
      let data = await stored(meta, index, start, end);
      if (!data) {
        const response = await fetch(meta.url, {headers: {'Range': `bytes=${start}-${end}`}});
        if (response.status !== 206 || response.headers.get('Content-Range') !== `bytes ${start}-${end}/${meta.size}`)
          throw new Error('Game range request failed.');
        data = new Uint8Array(await response.arrayBuffer());
        if (data.byteLength !== end - start + 1) throw new Error('Incomplete game range.');
        keep(meta, index, data);
      }
      while (cachedBytes + data.byteLength > limit && cache.size) {
        const oldest = cache.keys().next().value;
        cachedBytes -= cache.get(oldest).byteLength;
        cache.delete(oldest);
      }
      cache.set(key, data);
      cachedBytes += data.byteLength;
      return data;
    }
    wasmFS$backends[backend] = {
      allocFile: async () => {},
      freeFile: async file => {
        info.delete(file);
        for (const [key, data] of cache) if (key.startsWith(file + ':')) {
          cachedBytes -= data.byteLength;
          cache.delete(key);
        }
      },
      write: async () => -{{{ cDefs.EROFS }}},
      getSize: async file => {
        try { return (await details(file)).size; }
        catch { return -{{{ cDefs.EIO }}}; }
      },
      read: async (file, buffer, length, offset) => {
        if (offset < 0 || length <= 0) return 0;
        try {
          const meta = await details(file);
          const count = Math.max(0, Math.min(length, meta.size - offset));
          let copied = 0;
          while (copied < count) {
            const at = offset + copied;
            const index = Math.floor(at / meta.chunkSize);
            const data = await chunk(file, meta, index);
            const within = at - index * meta.chunkSize;
            const take = Math.min(data.byteLength - within, count - copied);
            HEAPU8.set(data.subarray(within, within + take), buffer + copied);
            copied += take;
          }
          return copied;
        } catch { return -{{{ cDefs.EIO }}}; }
      },
    };
  },
});
