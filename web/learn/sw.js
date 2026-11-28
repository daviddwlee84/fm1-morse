// SPDX-License-Identifier: GPL-3.0-or-later
// MORSE practice offline: the app's files cached at install; the network first when there is one, so an update
// shows on the next open, the cache when there is not. Bump CACHE when the file list changes.
const CACHE = "fm1-morse-learn-1";
const FILES = ["./", "index.html", "app.js", "core.js", "progress.js", "core.wasm", "manifest.webmanifest",
               "icon.svg", "icon-180.png", "icon-192.png", "icon-512.png"];
self.addEventListener("install", e => e.waitUntil(caches.open(CACHE).then(c => c.addAll(FILES)).then(() => self.skipWaiting())));
self.addEventListener("activate", e => e.waitUntil(caches.keys()
  .then(ks => Promise.all(ks.filter(k => k.startsWith("fm1-morse-learn-") && k !== CACHE).map(k => caches.delete(k))))
  .then(() => self.clients.claim())));
self.addEventListener("fetch", e => {
  const u = new URL(e.request.url);
  if (e.request.method !== "GET" || u.origin !== location.origin) return;
  e.respondWith(fetch(e.request).then(r => {
    if (r.ok) { const copy = r.clone(); caches.open(CACHE).then(c => c.put(e.request, copy)); }
    return r;
  }).catch(() => caches.match(e.request, { ignoreSearch: true })));
});
