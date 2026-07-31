#!/usr/bin/env node
// The dev server.
//
//   node test/serve.mjs [port]      default 8173
//
// It exists for one line of it: `Cache-Control: no-store`.
//
// `python3 -m http.server` sends Last-Modified and no Cache-Control at all. A
// response with a validator but no freshness directive is not uncacheable — the
// browser is allowed to invent a freshness lifetime from how old the file looks
// (RFC 9111 §4.2.2), and Chrome does. So a tab left open across a work session
// keeps serving the copy it already has, and never asks whether index.html
// changed.
//
// Which means a fix can be written, committed, verified in a fresh headless
// browser, and still be absent from the window the user is looking at — with
// nothing anywhere saying so. That happened: space-to-play, wheel scrolling and
// the track buttons were all reported dead while passing against this same
// server, because the reports and the tests were looking at different builds of
// the page. A stale asset is not a small annoyance in a loop where someone is
// checking my work by hand; it makes the check meaningless in a way that looks
// exactly like the work being wrong.
//
// no-store rather than no-cache: no-cache still permits a stored copy revalidated
// by ETag, which is fine in principle and one more thing to be wrong in practice.
// This server has one job and it is not throughput.

import { createServer } from 'node:http';
import { readFile, stat } from 'node:fs/promises';
import { join, extname, normalize } from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = fileURLToPath(new URL('..', import.meta.url));
const PORT = Number(process.argv[2]) || 8173;

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'text/javascript; charset=utf-8',
  '.mjs':  'text/javascript; charset=utf-8',
  '.css':  'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.woff2': 'font/woff2',
  '.woff': 'font/woff',
  '.svg':  'image/svg+xml',
  '.png':  'image/png',
  '.wasm': 'application/wasm',
};

const server = createServer(async (req, res) => {
  // Strip the query and decode before touching the filesystem; `?v=` cache
  // busters and spaces in names both arrive here.
  const url = decodeURIComponent((req.url || '/').split('?')[0]);
  // normalize() collapses `..` BEFORE the join, so a path cannot climb out of
  // the served root. Serving a directory to localhost is not a reason to serve
  // the whole disk.
  const rel = normalize(url === '/' ? '/index.html' : url).replace(/^(\.\.[/\\])+/, '');
  const path = join(ROOT, rel);
  if (!path.startsWith(ROOT)) { res.writeHead(403).end('no'); return; }

  try {
    const s = await stat(path);
    if (s.isDirectory()) { res.writeHead(403).end('no'); return; }
    const body = await readFile(path);
    res.writeHead(200, {
      'Content-Type': TYPES[extname(path).toLowerCase()] || 'application/octet-stream',
      'Content-Length': body.length,
      // The whole point of this file.
      'Cache-Control': 'no-store, must-revalidate',
      'Pragma': 'no-cache',
      'Expires': '0',
    });
    res.end(body);
  } catch {
    res.writeHead(404, { 'Content-Type': 'text/plain', 'Cache-Control': 'no-store' });
    res.end('404');
  }
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`serving ${ROOT} on http://127.0.0.1:${PORT}/index.html (no-store)`);
});
