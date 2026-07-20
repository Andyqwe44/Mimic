/**
 * Mimic signaling server — account, device presence, session mutex, SDP/LAN candidates.
 * Media never relays through this process.
 *
 * Symmetric nodes: every install is the same binary. On start, join BOOTSTRAP_URL;
 * bootstrap echoes public URL, reverse-probes /health, returns full cluster list.
 *
 *   node server.js [--port 8443] [--host 0.0.0.0]
 */
const http = require('http');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');
const { URL } = require('url');

const pkg = (() => {
  try {
    return JSON.parse(fs.readFileSync(path.join(__dirname, 'package.json'), 'utf8'));
  } catch {
    return { version: '0.0.0' };
  }
})();
const SERVER_VER = String(pkg.version || '0.0.0');

const BOOTSTRAP_URL = (process.env.MIMIC_BOOTSTRAP || 'http://47.107.43.5:8443').replace(/\/$/, '');
/** Distinguishes “I am answering BOOTSTRAP_URL” when public EIP is not on a local NIC. */
const INSTANCE_ID = crypto.randomBytes(8).toString('hex');

const PORT = (() => {
  const i = process.argv.indexOf('--port');
  return i >= 0 ? parseInt(process.argv[i + 1], 10) : 8443;
})();
const STUN_PORT = (() => {
  const i = process.argv.indexOf('--stun-port');
  if (i >= 0) return parseInt(process.argv[i + 1], 10) || 3478;
  const e = parseInt(process.env.MIMIC_STUN_PORT || '3478', 10);
  return Number.isFinite(e) && e > 0 ? e : 3478;
})();
const HOST = (() => {
  const i = process.argv.indexOf('--host');
  return i >= 0 ? process.argv[i + 1] : '0.0.0.0';
})();

const DATA_DIR = path.join(__dirname, 'data');
const USERS_FILE = path.join(DATA_DIR, 'users.json');
const HEARTBEAT_MS = 15000;
const NODE_TTL_MS = 45000;
/** Delay roster/session teardown after WS close (Android Home flaps). */
const OFFLINE_GRACE_MS = 20_000;
/** Token kept for re-login-less reconnect after confirmed offline. */
const TOKEN_GRACE_MS = 90_000;

/** Client wire secret: hex(SHA-256(UTF-8(password))). Never accept plaintext password. */
function clientPassHash(password) {
  return crypto.createHash('sha256').update(String(password), 'utf8').digest('hex');
}

/** Server stored hash: SHA256(salt + ":" + passHash). */
function hashPassHash(passHash, salt) {
  return crypto.createHash('sha256').update(String(salt) + ':' + String(passHash)).digest('hex');
}

function isValidPassHash(h) {
  return typeof h === 'string' && /^[0-9a-f]{64}$/i.test(h);
}

function isValidUser(user) {
  return typeof user === 'string' && /^[A-Za-z0-9_.-]{3,32}$/.test(user);
}

function ensureData() {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  if (!fs.existsSync(USERS_FILE)) {
    const salt = 'seed';
    const seed = {
      users: {
        demo: {
          salt,
          hash: hashPassHash(clientPassHash('demo'), salt),
          created: Date.now(),
        },
      },
    };
    saveUsers(seed);
    console.log('[signaling] seeded user demo / demo (passHash protocol)');
    return;
  }
  // Migrate legacy plaintext-era demo seed to passHash protocol.
  try {
    const db = loadUsers();
    const demo = db.users && db.users.demo;
    if (demo && demo.salt === 'seed') {
      const expected = hashPassHash(clientPassHash('demo'), 'seed');
      if (demo.hash !== expected) {
        demo.hash = expected;
        saveUsers(db);
        console.log('[signaling] migrated demo user to passHash protocol');
      }
    }
  } catch (e) {
    console.log('[signaling] users.json migrate skipped:', e.message || e);
  }
}

function loadUsers() {
  return JSON.parse(fs.readFileSync(USERS_FILE, 'utf8'));
}

function saveUsers(db) {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  const tmp = USERS_FILE + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(db, null, 2));
  fs.renameSync(tmp, USERS_FILE);
}

/** Extract passHash from body; reject plaintext password field. */
function readPassHash(body) {
  if (body && Object.prototype.hasOwnProperty.call(body, 'password') && body.password !== undefined && body.password !== null && String(body.password).length > 0) {
    return { error: 'plaintext password rejected; send passHash (SHA-256 hex)' };
  }
  const passHash = String(body.passHash || body.pass_hash || '').trim().toLowerCase();
  if (!isValidPassHash(passHash)) {
    return { error: 'passHash required (64 hex chars, SHA-256 of password)' };
  }
  return { passHash };
}

function json(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, {
    'Content-Type': 'application/json',
    'Access-Control-Allow-Origin': '*',
    'Access-Control-Allow-Headers': 'Content-Type, Authorization',
    'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
  });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', () => {
      try {
        const raw = Buffer.concat(chunks).toString('utf8');
        resolve(raw ? JSON.parse(raw) : {});
      } catch (e) {
        reject(e);
      }
    });
    req.on('error', reject);
  });
}

function normalizeUrl(u) {
  let s = String(u || '').trim().replace(/\/$/, '');
  return s;
}

function clientIp(req) {
  const xf = req.headers['x-forwarded-for'];
  if (xf) {
    const first = String(xf).split(',')[0].trim();
    if (first) return first.replace(/^::ffff:/, '');
  }
  let ip = req.socket?.remoteAddress || '';
  ip = ip.replace(/^::ffff:/, '');
  if (ip === '::1') ip = '127.0.0.1';
  return ip;
}

function httpGetJson(urlStr, timeoutMs = 5000) {
  return new Promise((resolve, reject) => {
    let u;
    try {
      u = new URL(urlStr);
    } catch (e) {
      reject(e);
      return;
    }
    const lib = u.protocol === 'https:' ? require('https') : http;
    const req = lib.get(
      {
        hostname: u.hostname,
        port: u.port || (u.protocol === 'https:' ? 443 : 80),
        path: u.pathname + u.search,
        timeout: timeoutMs,
        headers: { Accept: 'application/json' },
      },
      (res) => {
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => {
          const raw = Buffer.concat(chunks).toString('utf8');
          try {
            resolve({ status: res.statusCode || 0, body: raw ? JSON.parse(raw) : {} });
          } catch {
            resolve({ status: res.statusCode || 0, body: { raw } });
          }
        });
      },
    );
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('timeout'));
    });
    req.on('error', reject);
  });
}

function httpPostJson(urlStr, obj, timeoutMs = 8000) {
  return new Promise((resolve, reject) => {
    let u;
    try {
      u = new URL(urlStr);
    } catch (e) {
      reject(e);
      return;
    }
    const payload = Buffer.from(JSON.stringify(obj), 'utf8');
    const lib = u.protocol === 'https:' ? require('https') : http;
    const req = lib.request(
      {
        hostname: u.hostname,
        port: u.port || (u.protocol === 'https:' ? 443 : 80),
        path: u.pathname + u.search,
        method: 'POST',
        timeout: timeoutMs,
        headers: {
          'Content-Type': 'application/json',
          'Content-Length': payload.length,
          Accept: 'application/json',
        },
      },
      (res) => {
        const chunks = [];
        res.on('data', (c) => chunks.push(c));
        res.on('end', () => {
          const raw = Buffer.concat(chunks).toString('utf8');
          try {
            resolve({ status: res.statusCode || 0, body: raw ? JSON.parse(raw) : {} });
          } catch {
            resolve({ status: res.statusCode || 0, body: { raw } });
          }
        });
      },
    );
    req.on('timeout', () => {
      req.destroy();
      reject(new Error('timeout'));
    });
    req.on('error', reject);
    req.write(payload);
    req.end();
  });
}

// ── Cluster registry (bootstrap holds SSOT; members keep a mirror from join/heartbeat) ──
/** @type {Map<string, { url: string, joinedAt: number, lastSeen: number }>} */
const clusterNodes = new Map();
let selfUrl = '';
let isBootstrapRole = false;

function pruneCluster() {
  const now = Date.now();
  for (const [url, n] of clusterNodes) {
    if (now - n.lastSeen > NODE_TTL_MS) {
      clusterNodes.delete(url);
      console.log(`[cluster] expired ${url}`);
    }
  }
}

function clusterList() {
  pruneCluster();
  return [...clusterNodes.values()]
    .sort((a, b) => a.joinedAt - b.joinedAt)
    .map((n) => ({ url: n.url, joinedAt: n.joinedAt, lastSeen: n.lastSeen }));
}

function upsertNode(url) {
  const key = normalizeUrl(url);
  if (!key) return null;
  const prev = clusterNodes.get(key);
  const now = Date.now();
  const rec = {
    url: key,
    joinedAt: prev ? prev.joinedAt : now,
    lastSeen: now,
  };
  clusterNodes.set(key, rec);
  return rec;
}

async function reverseProbe(url) {
  const key = normalizeUrl(url);
  // Self / bootstrap URL: prefer loopback (cloud public-IP hairpin often fails).
  if (key === BOOTSTRAP_URL || (selfUrl && key === selfUrl)) {
    try {
      const local = await httpGetJson(`http://127.0.0.1:${PORT}/health`, 2000);
      if (local.status === 200 && local.body && local.body.ok === true) return { ok: true };
    } catch {
      /* fall through to public URL */
    }
  }
  const healthUrl = `${key}/health`;
  try {
    const { status, body } = await httpGetJson(healthUrl, 5000);
    if (status !== 200 || !body || body.ok !== true) {
      return { ok: false, error: `health status=${status}` };
    }
    return { ok: true };
  } catch (e) {
    return { ok: false, error: String(e.message || e) };
  }
}

function bootstrapHost() {
  try {
    return new URL(BOOTSTRAP_URL).hostname;
  } catch {
    return '';
  }
}

/** True when BOOTSTRAP_URL's host is a local interface (this machine is the registry). */
function isLocalBootstrapHost() {
  const host = bootstrapHost();
  if (!host || host === 'localhost' || host === '127.0.0.1') return true;
  try {
    const os = require('os');
    const ifs = os.networkInterfaces();
    for (const list of Object.values(ifs)) {
      for (const a of list || []) {
        if (a && a.address === host) return true;
      }
    }
  } catch {
    /* ignore */
  }
  return false;
}

function looksLikeLoopback(ip) {
  return ip === '127.0.0.1' || ip === 'localhost' || ip === '::1';
}

// token -> { user, deviceId, deviceName, lanIps[], ws, lastSeen }
const sessions = new Map();
// user -> { controllerId, controlledId } | null
const activeSessions = new Map();

function devicesForUser(user) {
  const list = [];
  for (const [, s] of sessions) {
    if (s.user !== user) continue;
    const live = !!(s.ws && s.ws.readyState === 1);
    // Still list during offline grace (away) so peers do not flicker to "0 devices".
    if (!live && !s.offlineTimer) continue;
    list.push({
      deviceId: s.deviceId,
      deviceName: s.deviceName,
      lanIps: s.lanIps || [],
      platform: s.platform || 'unknown',
      peerProto: s.peerProto || 1,
      // Honest online — invite needs live WS (findByDevice). Grace = away.
      online: live,
      state: live ? 'online' : 'away',
    });
  }
  return list;
}

/** End active call for this device immediately; notify the other side. */
function endActiveSessionForDevice(user, deviceId, reason) {
  const cur = activeSessions.get(user);
  if (!cur) return false;
  if (cur.controllerId !== deviceId && cur.controlledId !== deviceId) return false;
  activeSessions.delete(user);
  const otherId = cur.controllerId === deviceId ? cur.controlledId : cur.controllerId;
  const other = findByDevice(user, otherId);
  send(other, { type: 'session_end', reason: reason || 'peer_disconnect', session: cur });
  return true;
}

function findByDevice(user, deviceId) {
  for (const [, s] of sessions) {
    if (s.user === user && s.deviceId === deviceId && s.ws && s.ws.readyState === 1)
      return s;
  }
  return null;
}

function broadcastDevices(user) {
  const list = devicesForUser(user);
  const msg = JSON.stringify({ type: 'devices', devices: list });
  for (const [, s] of sessions) {
    if (s.user === user && s.ws && s.ws.readyState === 1) {
      try { s.ws.send(msg); } catch { /* drop dead socket; close will clean */ }
    }
  }
}

function sameLanIps(a, b) {
  const aa = Array.isArray(a) ? a.map(String) : [];
  const bb = Array.isArray(b) ? b.map(String) : [];
  if (aa.length !== bb.length) return false;
  for (let i = 0; i < aa.length; i++) if (aa[i] !== bb[i]) return false;
  return true;
}

function send(s, obj) {
  if (s && s.ws && s.ws.readyState === 1) {
    try { s.ws.send(JSON.stringify(obj)); } catch { /* */ }
  }
}

ensureData();

const server = http.createServer(async (req, res) => {
  if (req.method === 'OPTIONS') {
    json(res, 204, {});
    return;
  }

  const url = new URL(req.url || '/', `http://${req.headers.host}`);

  if (req.method === 'GET' && url.pathname === '/health') {
    pruneCluster();
    json(res, 200, {
      ok: true,
      service: 'mimic-signaling',
      ver: SERVER_VER,
      role: isBootstrapRole ? 'bootstrap' : 'member',
      nodeCount: clusterNodes.size,
      selfUrl: selfUrl || null,
      bootstrap: BOOTSTRAP_URL,
      instanceId: INSTANCE_ID,
      stunPort: STUN_PORT,
    });
    return;
  }

  // ICE config for clients — STUN only (no TURN / no media relay).
  if (req.method === 'GET' && url.pathname === '/api/ice') {
    let host = '47.107.43.5';
    try {
      const u = new URL(selfUrl || BOOTSTRAP_URL);
      if (u.hostname) host = u.hostname;
    } catch { /* keep default */ }
    json(res, 200, {
      ok: true,
      stunUrls: [`stun:${host}:${STUN_PORT}`],
      stunHost: host,
      stunPort: STUN_PORT,
      turnUrls: [],
      iceTransportPolicy: 'all',
      note: 'STUN only — media is peer direct (LAN or UDP hole-punch); no TURN relay',
    });
    return;
  }

  if (req.method === 'GET' && url.pathname === '/api/cluster') {
    json(res, 200, {
      ok: true,
      bootstrap: BOOTSTRAP_URL,
      selfUrl: selfUrl || null,
      role: isBootstrapRole ? 'bootstrap' : 'member',
      nodes: clusterList(),
    });
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/cluster/join') {
    try {
      const body = await readBody(req);
      const port = parseInt(body.port, 10) || PORT;
      let candidate = normalizeUrl(body.url);
      if (!candidate) {
        const ip = clientIp(req);
        if (!ip || looksLikeLoopback(ip)) {
          // Joiner is local to bootstrap (or bootstrap self-join over loopback).
          candidate = BOOTSTRAP_URL;
        } else {
          candidate = `http://${ip}:${port}`;
        }
      }

      // Cold-start: only the real bootstrap host may claim BOOTSTRAP_URL.
      if (!isBootstrapRole) {
        if (
          normalizeUrl(candidate) === BOOTSTRAP_URL &&
          (looksLikeLoopback(clientIp(req)) || isLocalBootstrapHost())
        ) {
          isBootstrapRole = true;
          selfUrl = BOOTSTRAP_URL;
          upsertNode(selfUrl);
          console.log(`[cluster] auto-claim bootstrap via self-join`);
        } else {
          json(res, 200, {
            ok: true,
            selfUrl: candidate,
            nodes: clusterList(),
            bootstrap: BOOTSTRAP_URL,
            note: 'not_bootstrap',
          });
          return;
        }
      }

      const probe = await reverseProbe(candidate);
      if (!probe.ok) {
        json(res, 400, {
          ok: false,
          error: 'reverse_probe_failed',
          detail: probe.error,
          selfUrl: candidate,
        });
        return;
      }

      upsertNode(candidate);
      console.log(`[cluster] join ok ${candidate} (nodes=${clusterNodes.size})`);
      json(res, 200, {
        ok: true,
        selfUrl: candidate,
        nodes: clusterList(),
        bootstrap: BOOTSTRAP_URL,
      });
    } catch (e) {
      json(res, 400, { ok: false, error: String(e.message || e) });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/cluster/heartbeat') {
    try {
      const body = await readBody(req);
      const u = normalizeUrl(body.url);
      if (!u) {
        json(res, 400, { ok: false, error: 'url required' });
        return;
      }
      if (!isBootstrapRole) {
        json(res, 200, {
          ok: true,
          nodes: clusterList(),
          bootstrap: BOOTSTRAP_URL,
          note: 'not_bootstrap',
        });
        return;
      }
      if (!clusterNodes.has(u)) {
        // Accept heartbeat as soft re-join only after reverse probe.
        const probe = await reverseProbe(u);
        if (!probe.ok) {
          json(res, 400, { ok: false, error: 'unknown_node', detail: probe.error });
          return;
        }
      }
      upsertNode(u);
      json(res, 200, { ok: true, nodes: clusterList(), bootstrap: BOOTSTRAP_URL });
    } catch (e) {
      json(res, 400, { ok: false, error: String(e.message || e) });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/register') {
    try {
      const body = await readBody(req);
      const user = String(body.user || '').trim();
      if (!isValidUser(user)) {
        json(res, 400, { ok: false, error: 'user required (3-32: A-Za-z0-9_.-)' });
        return;
      }
      const ph = readPassHash(body);
      if (ph.error) {
        json(res, 400, { ok: false, error: ph.error });
        return;
      }
      const db = loadUsers();
      if (db.users[user]) {
        json(res, 409, { ok: false, error: 'user exists' });
        return;
      }
      const salt = crypto.randomBytes(8).toString('hex');
      db.users[user] = { salt, hash: hashPassHash(ph.passHash, salt), created: Date.now() };
      saveUsers(db);
      json(res, 200, { ok: true });
    } catch (e) {
      json(res, 400, { ok: false, error: String(e.message || e) });
    }
    return;
  }

  if (req.method === 'POST' && url.pathname === '/api/login') {
    try {
      const body = await readBody(req);
      const user = String(body.user || '').trim();
      if (!isValidUser(user)) {
        json(res, 400, { ok: false, error: 'user required (3-32: A-Za-z0-9_.-)' });
        return;
      }
      const ph = readPassHash(body);
      if (ph.error) {
        json(res, 400, { ok: false, error: ph.error });
        return;
      }
      const deviceId = String(body.deviceId || crypto.randomBytes(8).toString('hex'));
      const deviceName = String(body.deviceName || 'PC');
      const platform = String(body.platform || 'unknown');
      const peerProto = Number(body.peerProto || body.peer_proto || 1) || 1;
      const db = loadUsers();
      const rec = db.users[user];
      if (!rec || rec.hash !== hashPassHash(ph.passHash, rec.salt)) {
        json(res, 401, { ok: false, error: 'invalid credentials' });
        return;
      }
      const token = crypto.randomBytes(24).toString('hex');
      for (const [t, s] of sessions) {
        if (s.user === user && s.deviceId === deviceId) {
          try { s.ws?.close(); } catch { /* */ }
          sessions.delete(t);
        }
      }
      sessions.set(token, {
        user,
        deviceId,
        deviceName,
        lanIps: Array.isArray(body.lanIps) ? body.lanIps : [],
        platform,
        peerProto,
        ws: null,
        lastSeen: Date.now(),
      });
      json(res, 200, { ok: true, token, deviceId, user, platform, peerProto });
    } catch (e) {
      json(res, 400, { ok: false, error: String(e.message || e) });
    }
    return;
  }

  json(res, 404, { ok: false, error: 'not found' });
});

const wss = new WebSocketServer({ server, path: '/ws' });

// Drop half-open sockets so presence stays accurate (NAT / sleep / firewall).
const WS_PING_MS = 20000;
setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) {
      try { ws.terminate(); } catch { /* */ }
      continue;
    }
    ws.isAlive = false;
    try { ws.ping(); } catch { /* */ }
  }
}, WS_PING_MS);

wss.on('connection', (ws, req) => {
  const url = new URL(req.url || '/', `http://${req.headers.host}`);
  const token = url.searchParams.get('token') || '';
  const sess = sessions.get(token);
  if (!sess) {
    ws.close(4001, 'unauthorized');
    return;
  }
  const prevWs = sess.ws;
  sess.ws = ws;
  sess.lastSeen = Date.now();
  if (sess.offlineTimer) {
    try { clearTimeout(sess.offlineTimer); } catch { /* */ }
    sess.offlineTimer = null;
  }
  // Close previous socket without treating it as logout (close handler checks sess.ws).
  if (prevWs && prevWs !== ws) {
    try { prevWs.close(4000, 'replaced'); } catch { /* */ }
  }
  ws.isAlive = true;
  ws.on('pong', () => { ws.isAlive = true; });
  console.log(`[signaling] online ${sess.user}/${sess.deviceName} (${sess.deviceId})`);
  send(sess, { type: 'hello', user: sess.user, deviceId: sess.deviceId });
  broadcastDevices(sess.user);
  const active = activeSessions.get(sess.user);
  if (active) send(sess, { type: 'session_state', session: active });

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(String(raw)); } catch { return; }
    sess.lastSeen = Date.now();
    ws.isAlive = true;
    const type = msg.type;

    if (type === 'presence') {
      // Keep lastSeen above; only push devices when visible metadata changes.
      let metaChanged = false;
      if (Array.isArray(msg.lanIps) && !sameLanIps(msg.lanIps, sess.lanIps)) {
        sess.lanIps = msg.lanIps;
        metaChanged = true;
      }
      if (typeof msg.deviceName === 'string' && msg.deviceName && msg.deviceName !== sess.deviceName) {
        sess.deviceName = msg.deviceName;
        metaChanged = true;
      }
      if (typeof msg.platform === 'string' && msg.platform && msg.platform !== sess.platform) {
        sess.platform = msg.platform;
        metaChanged = true;
      }
      if (msg.peerProto != null || msg.peer_proto != null) {
        const nextProto = Number(msg.peerProto || msg.peer_proto) || sess.peerProto || 1;
        if (nextProto !== (sess.peerProto || 1)) {
          sess.peerProto = nextProto;
          metaChanged = true;
        }
      }
      if (metaChanged) broadcastDevices(sess.user);
      return;
    }

    // Client pull — recovery if a previous devices push was missed.
    if (type === 'list_devices') {
      send(sess, { type: 'devices', devices: devicesForUser(sess.user) });
      return;
    }

    if (type === 'invite') {
      const targetId = String(msg.targetDeviceId || '');
      const peer = findByDevice(sess.user, targetId);
      if (!peer) {
        send(sess, { type: 'error', error: 'peer offline', code: 'peer_offline' });
        return;
      }
      if (peer.deviceId === sess.deviceId) {
        send(sess, { type: 'error', error: 'cannot invite self', code: 'self' });
        return;
      }
      const cur = activeSessions.get(sess.user);
      if (cur) {
        send(sess, { type: 'error', error: 'session busy', code: 'busy', session: cur });
        return;
      }
      send(peer, {
        type: 'invite',
        fromDeviceId: sess.deviceId,
        fromDeviceName: sess.deviceName,
        fromLanIps: sess.lanIps || [],
      });
      send(sess, { type: 'invite_sent', targetDeviceId: targetId });
      return;
    }

    if (type === 'invite_reject') {
      const fromId = String(msg.fromDeviceId || '');
      const peer = findByDevice(sess.user, fromId);
      send(peer, {
        type: 'invite_rejected',
        byDeviceId: sess.deviceId,
        reason: msg.reason || 'rejected',
      });
      return;
    }

    if (type === 'invite_accept') {
      const fromId = String(msg.fromDeviceId || '');
      const peer = findByDevice(sess.user, fromId);
      if (!peer) {
        send(sess, { type: 'error', error: 'peer offline', code: 'peer_offline' });
        return;
      }
      if (activeSessions.get(sess.user)) {
        send(sess, { type: 'error', error: 'session busy', code: 'busy' });
        return;
      }
      const session = {
        controllerId: peer.deviceId,
        controlledId: sess.deviceId,
        started: Date.now(),
      };
      activeSessions.set(sess.user, session);
      const payload = {
        type: 'session_start',
        session,
        transportHint: 'lan_or_p2p',
        controller: {
          deviceId: peer.deviceId,
          deviceName: peer.deviceName,
          lanIps: peer.lanIps || [],
        },
        controlled: {
          deviceId: sess.deviceId,
          deviceName: sess.deviceName,
          lanIps: sess.lanIps || [],
        },
      };
      send(peer, payload);
      send(sess, payload);
      return;
    }

    if (type === 'hangup') {
      const cur = activeSessions.get(sess.user);
      if (!cur) return;
      activeSessions.delete(sess.user);
      const otherId =
        cur.controllerId === sess.deviceId ? cur.controlledId : cur.controllerId;
      const other = findByDevice(sess.user, otherId);
      const done = { type: 'session_end', reason: msg.reason || 'hangup', session: cur };
      send(sess, done);
      send(other, done);
      return;
    }

    if (type === 'signal') {
      const toId = String(msg.toDeviceId || '');
      const peer = findByDevice(sess.user, toId);
      if (!peer) {
        send(sess, { type: 'error', error: 'peer offline', code: 'peer_offline' });
        return;
      }
      send(peer, {
        type: 'signal',
        fromDeviceId: sess.deviceId,
        payload: msg.payload,
      });
      return;
    }
  });

  ws.on('close', () => {
    // Stale socket after token rebind — do NOT delete session or end peer call.
    if (sess.ws !== ws) {
      console.log(`[signaling] stale close ignored ${sess.user}/${sess.deviceName}`);
      return;
    }
    console.log(`[signaling] offline ${sess.user}/${sess.deviceName} (grace ${OFFLINE_GRACE_MS}ms)`);
    sess.ws = null;

    // Call teardown is immediate (state table #7) — do not wait for roster grace.
    // Roster stays "away" during OFFLINE_GRACE so Android Home flap does not flicker.
    if (endActiveSessionForDevice(sess.user, sess.deviceId, 'peer_disconnect')) {
      console.log(`[signaling] session_end peer_disconnect ${sess.user}/${sess.deviceName}`);
    }
    broadcastDevices(sess.user);

    if (sess.offlineTimer) {
      try { clearTimeout(sess.offlineTimer); } catch { /* */ }
      sess.offlineTimer = null;
    }
    sess.offlineTimer = setTimeout(() => {
      sess.offlineTimer = null;
      if (sess.ws != null) return; // reconnected within grace
      console.log(`[signaling] offline confirmed ${sess.user}/${sess.deviceName}`);
      // Keep token briefly so the same device can re-open /ws?token=... without re-login.
      const tok = token;
      setTimeout(() => {
        const curSess = sessions.get(tok);
        if (curSess === sess && curSess.ws == null) {
          sessions.delete(tok);
          console.log(`[signaling] token expired ${sess.user}/${sess.deviceName}`);
          broadcastDevices(sess.user);
        }
      }, TOKEN_GRACE_MS);
      broadcastDevices(sess.user);
    }, OFFLINE_GRACE_MS);
  });
});

async function becomeBootstrap(url) {
  isBootstrapRole = true;
  selfUrl = normalizeUrl(url) || BOOTSTRAP_URL;
  upsertNode(selfUrl);
  console.log(`[cluster] role=bootstrap selfUrl=${selfUrl}`);
}

async function joinCluster() {
  if (process.env.MIMIC_IS_BOOTSTRAP === '1' || isLocalBootstrapHost()) {
    await becomeBootstrap(process.env.MIMIC_PUBLIC_URL || BOOTSTRAP_URL);
    return;
  }

  let bootHealth = null;
  try {
    bootHealth = await httpGetJson(`${BOOTSTRAP_URL}/health`, 4000);
  } catch (e) {
    console.log(`[cluster] bootstrap unreachable (${e.message || e}) — becoming local bootstrap`);
    await becomeBootstrap(BOOTSTRAP_URL);
    return;
  }

  const bootBody = bootHealth && bootHealth.body;
  // Public EIP often missing from NICs: if BOOTSTRAP_URL /health is this process, we are registry.
  if (bootBody && bootBody.instanceId === INSTANCE_ID) {
    await becomeBootstrap(process.env.MIMIC_PUBLIC_URL || BOOTSTRAP_URL);
    return;
  }

  try {
    const { status, body } = await httpPostJson(`${BOOTSTRAP_URL}/api/cluster/join`, {
      port: PORT,
      url: process.env.MIMIC_PUBLIC_URL || undefined,
    });
    if (status === 200 && body && body.ok && body.note !== 'not_bootstrap') {
      selfUrl = normalizeUrl(body.selfUrl) || selfUrl;
      if (Array.isArray(body.nodes)) {
        for (const n of body.nodes) {
          if (n && n.url) upsertNode(n.url);
        }
      }
      isBootstrapRole = false;
      console.log(`[cluster] role=member selfUrl=${selfUrl} nodes=${clusterNodes.size}`);
      return;
    }
    console.log(`[cluster] join rejected status=${status} ${JSON.stringify(body)}`);
  } catch (e) {
    console.log(`[cluster] join error: ${e.message || e}`);
  }

  if (bootBody && bootBody.ok) {
    console.log('[cluster] bootstrap up but join failed — running standalone until heartbeat retry');
  }
}

function startHeartbeat() {
  setInterval(async () => {
    pruneCluster();
    if (!selfUrl) return;
    if (isBootstrapRole) {
      upsertNode(selfUrl);
      return;
    }
    try {
      const { status, body } = await httpPostJson(`${BOOTSTRAP_URL}/api/cluster/heartbeat`, {
        url: selfUrl,
      });
      if (status === 200 && body && body.ok && Array.isArray(body.nodes)) {
        clusterNodes.clear();
        for (const n of body.nodes) {
          if (n && n.url) upsertNode(n.url);
        }
      }
    } catch (e) {
      console.log(`[cluster] heartbeat failed: ${e.message || e}`);
    }
  }, HEARTBEAT_MS);
}

server.listen(PORT, HOST, () => {
  console.log(`[signaling] MimicServer listening http://${HOST}:${PORT}`);
  console.log(`[signaling] WS path /ws?token=...  health GET /health  ice GET /api/ice`);
  console.log(`[signaling] bootstrap ${BOOTSTRAP_URL}  ver=${SERVER_VER}`);
  console.log(`[signaling] default login demo / demo`);
  startStunServer();
  setImmediate(() => {
    joinCluster()
      .then(() => startHeartbeat())
      .catch((e) => console.log(`[cluster] init error: ${e.message || e}`));
  });
});

// ─── STUN Binding (RFC 5389 subset) — same process, UDP STUN_PORT; no TURN ───
const STUN_MAGIC = 0x2112A442;
const STUN_BINDING_REQUEST = 0x0001;
const STUN_BINDING_SUCCESS = 0x0101;
const STUN_ATTR_XOR_MAPPED = 0x0020;

function startStunServer() {
  const sock = dgram.createSocket('udp4');
  sock.on('message', (msg, rinfo) => {
    try {
      if (!msg || msg.length < 20) return;
      const type = msg.readUInt16BE(0);
      const length = msg.readUInt16BE(2);
      const magic = msg.readUInt32BE(4);
      if (magic !== STUN_MAGIC) return;
      if ((type & 0x3fff) !== STUN_BINDING_REQUEST) return;
      if (20 + length > msg.length) return;

      const tid = msg.subarray(8, 20);
      // XOR-MAPPED-ADDRESS: IPv4
      const xorPort = rinfo.port ^ ((STUN_MAGIC >> 16) & 0xffff);
      const parts = String(rinfo.address).split('.').map((x) => parseInt(x, 10) & 0xff);
      if (parts.length !== 4 || parts.some((n) => Number.isNaN(n))) return;
      const ipBuf = Buffer.from(parts);
      for (let i = 0; i < 4; i++) ipBuf[i] ^= (STUN_MAGIC >> (24 - 8 * i)) & 0xff;

      const attrVal = Buffer.alloc(8);
      attrVal.writeUInt8(0, 0);
      attrVal.writeUInt8(0x01, 1); // IPv4
      attrVal.writeUInt16BE(xorPort, 2);
      ipBuf.copy(attrVal, 4);

      const attr = Buffer.alloc(4 + attrVal.length);
      attr.writeUInt16BE(STUN_ATTR_XOR_MAPPED, 0);
      attr.writeUInt16BE(attrVal.length, 2);
      attrVal.copy(attr, 4);

      const resp = Buffer.alloc(20 + attr.length);
      resp.writeUInt16BE(STUN_BINDING_SUCCESS, 0);
      resp.writeUInt16BE(attr.length, 2);
      resp.writeUInt32BE(STUN_MAGIC, 4);
      tid.copy(resp, 8);
      attr.copy(resp, 20);
      sock.send(resp, rinfo.port, rinfo.address);
    } catch (e) {
      console.log(`[stun] handle error: ${e.message || e}`);
    }
  });
  sock.on('error', (e) => console.log(`[stun] error: ${e.message || e}`));
  sock.bind(STUN_PORT, HOST, () => {
    console.log(`[stun] Binding UDP ${HOST}:${STUN_PORT} (STUN-only, no TURN/media relay)`);
  });
}
