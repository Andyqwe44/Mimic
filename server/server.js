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
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');

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
const HOST = (() => {
  const i = process.argv.indexOf('--host');
  return i >= 0 ? process.argv[i + 1] : '0.0.0.0';
})();

const DATA_DIR = path.join(__dirname, 'data');
const USERS_FILE = path.join(DATA_DIR, 'users.json');
const HEARTBEAT_MS = 15000;
const NODE_TTL_MS = 45000;

function ensureData() {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  if (!fs.existsSync(USERS_FILE)) {
    const seed = {
      users: {
        demo: {
          salt: 'seed',
          hash: hashPassword('demo', 'seed'),
          created: Date.now(),
        },
      },
    };
    fs.writeFileSync(USERS_FILE, JSON.stringify(seed, null, 2));
    console.log('[signaling] seeded user demo / demo');
  }
}

function hashPassword(password, salt) {
  return crypto.createHash('sha256').update(salt + ':' + password).digest('hex');
}

function loadUsers() {
  return JSON.parse(fs.readFileSync(USERS_FILE, 'utf8'));
}

function saveUsers(db) {
  fs.writeFileSync(USERS_FILE, JSON.stringify(db, null, 2));
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
    if (s.user === user && s.ws && s.ws.readyState === 1) {
      list.push({
        deviceId: s.deviceId,
        deviceName: s.deviceName,
        lanIps: s.lanIps || [],
        online: true,
      });
    }
  }
  return list;
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
    if (s.user === user && s.ws && s.ws.readyState === 1) s.ws.send(msg);
  }
}

function send(s, obj) {
  if (s && s.ws && s.ws.readyState === 1) s.ws.send(JSON.stringify(obj));
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
      const password = String(body.password || '');
      if (!user || password.length < 3) {
        json(res, 400, { ok: false, error: 'user/password required (min 3)' });
        return;
      }
      const db = loadUsers();
      if (db.users[user]) {
        json(res, 409, { ok: false, error: 'user exists' });
        return;
      }
      const salt = crypto.randomBytes(8).toString('hex');
      db.users[user] = { salt, hash: hashPassword(password, salt), created: Date.now() };
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
      const password = String(body.password || '');
      const deviceId = String(body.deviceId || crypto.randomBytes(8).toString('hex'));
      const deviceName = String(body.deviceName || 'PC');
      const db = loadUsers();
      const rec = db.users[user];
      if (!rec || rec.hash !== hashPassword(password, rec.salt)) {
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
        ws: null,
        lastSeen: Date.now(),
      });
      json(res, 200, { ok: true, token, deviceId, user });
    } catch (e) {
      json(res, 400, { ok: false, error: String(e.message || e) });
    }
    return;
  }

  json(res, 404, { ok: false, error: 'not found' });
});

const wss = new WebSocketServer({ server, path: '/ws' });

wss.on('connection', (ws, req) => {
  const url = new URL(req.url || '/', `http://${req.headers.host}`);
  const token = url.searchParams.get('token') || '';
  const sess = sessions.get(token);
  if (!sess) {
    ws.close(4001, 'unauthorized');
    return;
  }
  sess.ws = ws;
  sess.lastSeen = Date.now();
  console.log(`[signaling] online ${sess.user}/${sess.deviceName} (${sess.deviceId})`);
  send(sess, { type: 'hello', user: sess.user, deviceId: sess.deviceId });
  broadcastDevices(sess.user);
  const active = activeSessions.get(sess.user);
  if (active) send(sess, { type: 'session_state', session: active });

  ws.on('message', (raw) => {
    let msg;
    try { msg = JSON.parse(String(raw)); } catch { return; }
    sess.lastSeen = Date.now();
    const type = msg.type;

    if (type === 'presence') {
      if (Array.isArray(msg.lanIps)) sess.lanIps = msg.lanIps;
      if (typeof msg.deviceName === 'string' && msg.deviceName) sess.deviceName = msg.deviceName;
      broadcastDevices(sess.user);
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
    console.log(`[signaling] offline ${sess.user}/${sess.deviceName}`);
    const cur = activeSessions.get(sess.user);
    if (cur && (cur.controllerId === sess.deviceId || cur.controlledId === sess.deviceId)) {
      activeSessions.delete(sess.user);
      const otherId =
        cur.controllerId === sess.deviceId ? cur.controlledId : cur.controllerId;
      const other = findByDevice(sess.user, otherId);
      send(other, { type: 'session_end', reason: 'peer_disconnect', session: cur });
    }
    if (sessions.get(token) === sess) sessions.delete(token);
    broadcastDevices(sess.user);
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
  console.log(`[signaling] WS path /ws?token=...  health GET /health`);
  console.log(`[signaling] bootstrap ${BOOTSTRAP_URL}  ver=${SERVER_VER}`);
  console.log(`[signaling] default login demo / demo`);
  // Defer join so /health is already accepting (reverse probe).
  setImmediate(() => {
    joinCluster()
      .then(() => startHeartbeat())
      .catch((e) => console.log(`[cluster] init error: ${e.message || e}`));
  });
});
