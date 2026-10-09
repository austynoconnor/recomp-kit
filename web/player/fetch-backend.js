// Bounded FetchFS reads for large games. Emscripten's default backend retains
// every fetched chunk for the lifetime of each file. This replacement uses a
// shared 64 MiB LRU; guest reads remain synchronous through the SDK proxy worker.
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
    async function details(file) {
      if (info.has(file)) return info.get(file);
      const path = UTF8ToString(__wasmfs_fetch_get_file_url(file));
      const url = new URL(path, self.location.origin).href;
      const response = await fetch(url, {method: 'HEAD'});
      const size = Number(response.headers.get('Content-Length'));
      if (!response.ok || !response.headers.has('Content-Length') ||
          !Number.isSafeInteger(size) || size < 0 || response.headers.get('Accept-Ranges') !== 'bytes')
        throw new Error('Game server must support sized HTTP ranges.');
      const result = {url, size, chunkSize: Math.min(__wasmfs_fetch_get_chunk_size(file), 1024 * 1024)};
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
      const response = await fetch(meta.url, {headers: {'Range': `bytes=${start}-${end}`}});
      if (response.status !== 206 || response.headers.get('Content-Range') !== `bytes ${start}-${end}/${meta.size}`)
        throw new Error('Game range request failed.');
      const data = new Uint8Array(await response.arrayBuffer());
      if (data.byteLength !== end - start + 1) throw new Error('Incomplete game range.');
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
