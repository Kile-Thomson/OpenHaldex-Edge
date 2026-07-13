// OpenHaldex Edge service worker.
//
// Scope is the whole origin (served from /sw.js). The job here is narrow: let the
// app open and show its shell even when the module is briefly unreachable, and
// pick up firmware UI updates as soon as it is reachable again.
//
// Strategy:
//   - /api/* and any non-GET request: never touched. Live CAN/telemetry and all
//     control POSTs must always hit the network - a cached dashboard would be a
//     dangerously stale view of a moving vehicle.
//   - Everything else (the HTML/CSS/JS/icon shell): network-first. A fresh copy
//     from the module wins and refreshes the cache; if the fetch fails we fall
//     back to the last cached copy so the UI still loads and can report the
//     disconnect itself. ignoreSearch means the ?v= cache-bust query never
//     causes a miss.
const CACHE = "openhaldex-shell-v1";
const SHELL = [
  "/",
  "/index.html",
  "/app.js",
  "/style.css",
  "/manifest.json",
  "/icon-192.png",
  "/icon-512.png",
  "/icon-maskable-512.png",
  "/apple-touch-icon.png",
];

self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(CACHE).then((cache) => cache.addAll(SHELL)).catch(() => {})
  );
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k)))
    ).then(() => self.clients.claim())
  );
});

self.addEventListener("fetch", (event) => {
  const req = event.request;
  const url = new URL(req.url);

  // Only handle same-origin GETs; leave API traffic and control POSTs to network.
  if (req.method !== "GET" || url.origin !== self.location.origin) return;
  if (url.pathname.startsWith("/api/")) return;

  event.respondWith(
    fetch(req)
      .then((res) => {
        // Cache a copy of good, basic responses for offline shell loading.
        if (res && res.ok && res.type === "basic") {
          const copy = res.clone();
          caches.open(CACHE).then((cache) => cache.put(req, copy)).catch(() => {});
        }
        return res;
      })
      .catch(() =>
        caches.match(req, { ignoreSearch: true }).then((hit) => {
          if (hit) return hit;
          // Last resort for a navigation with nothing cached yet.
          if (req.mode === "navigate") return caches.match("/index.html", { ignoreSearch: true });
          return Response.error();
        })
      )
  );
});
