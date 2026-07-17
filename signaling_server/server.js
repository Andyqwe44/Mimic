/**
 * Mimic signaling server — account, device presence, session mutex, SDP/LAN candidates.
 * Media never relays through this process.
 *
 *   node signaling_server/server.js [--port 8443] [--host 0.0.0.0]
 */
const http = require('http');
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');

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

// token -> { user, deviceId, deviceName, lanIps[], ws, lastSeen }
const sessions = new Map();
// user -> { controllerId, controlledId } | null
const activeSessions = new Map();

function devicesForUser(user) {
  const list = [];
  for (const [token, s] of sessions) {
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
    json(res, 200, { ok: true, service: 'mimic-signaling', ver: '0.1.0' });
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
      // Kick previous connection for same deviceId
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
      // Acceptor becomes Controlled; inviter becomes Controller
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

    // Opaque relay for SDP / LAN candidates / peer app messages (not media frames).
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

server.listen(PORT, HOST, () => {
  console.log(`[signaling] MimicServer listening http://${HOST}:${PORT}`);
  console.log(`[signaling] WS path /ws?token=...  health GET /health`);
  console.log(`[signaling] default login demo / demo`);
});
