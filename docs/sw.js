const CACHE_NAME = 'qt-wasm-cache-v1';

// Add the exact names of your generated Qt files here
const ASSETS_TO_CACHE = [
  'firebird-emu.html',
  'firebird-emu.js',
  'firebird-emu.png',
  'qtloader.js',
  'firebird-emu.wasm',
  'sw.js',
  'qtlogo.svg',
  'manifest.json',
];

// Install Event: Cache all essential application files
self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => {
      console.log('Caching application shell and WASM module');
      // Use standard map catch to prevent a single missing asset from breaking deployment
      return Promise.all(
        ASSETS_TO_CACHE.map(url => {
          return cache.add(url).catch(err => console.warn(`Failed to cache asset: ${url}`, err));
        })
      );
    }).then(() => self.skipWaiting()) // Force activation instantly
  );
});

// Activate Event: Clear old caches when updating versions
self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((cacheNames) => {
      return Promise.all(
        cacheNames.map((cache) => {
          if (cache !== CACHE_NAME) {
            console.log('Clearing old cache bundle');
            return caches.delete(cache);
          }
        })
      );
    }).then(() => self.clients.claim())
  );
});

// Fetch Event: Serve from local cache instead of making network calls
self.addEventListener('fetch', (event) => {
  event.respondWith(
    caches.match(event.request).then((cachedResponse) => {
      // Fetch fresh version from network if it isn't explicitly cached
      let responsePromise = cachedResponse ? Promise.resolve(cachedResponse) : fetch(event.request);

      return responsePromise.then((response) => {
        // Opfs, external APIs, or null responses cannot be mutated
        if (!response || response.status === 0 || response.type === 'opaque') {
          return response;
        }

        // Clone the headers because the original response headers are read-only
        const newHeaders = new Headers(response.headers);
        
        // Critical headers required to activate self.crossOriginIsolated
        newHeaders.set('Cross-Origin-Opener-Policy', 'same-origin');
        newHeaders.set('Cross-Origin-Embedder-Policy', 'require-corp');

        // Build a fresh response object using the cloned body stream and mutated headers
        return new Response(response.body, {
          status: response.status,
          statusText: response.statusText,
          headers: newHeaders
        });
      });
    }).catch(() => {
      // Fallback network routing context for cross-origin assets
      return fetch(event.request);
    })
  );
});