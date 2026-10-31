const { timingSafeEqual } = require('node:crypto');
const { createHmac } = require('node:crypto');

const env = name => (process.env[name] || '').trim();
function json(res, status, body) { res.status(status).setHeader('Cache-Control', 'no-store').json(body); }
function fail(res, status, message) { json(res, status, { error: message }); }
function method(req, res, expected) { if (req.method !== expected) { res.setHeader('Allow', expected); fail(res, 405, 'Método não permitido.'); return false; } return true; }
function constantEqual(a, b) {
  const left = Buffer.isBuffer(a) ? a : Buffer.from(String(a || ''));
  const right = Buffer.isBuffer(b) ? b : Buffer.from(String(b || ''));
  return left.length === right.length && timingSafeEqual(left, right);
}
function databaseReady() {
  return Boolean(env('SUPABASE_URL') && (env('SUPABASE_SECRET_KEY') || env('SUPABASE_SERVICE_ROLE_KEY')));
}
async function db(path, options = {}) {
  if (!databaseReady()) throw new Error('O backend ainda não foi configurado.');
  const base = env('SUPABASE_URL').replace(/\/$/, '');
  const secretKey = env('SUPABASE_SECRET_KEY');
  const legacyServiceRoleKey = env('SUPABASE_SERVICE_ROLE_KEY');
  const apiKey = secretKey || legacyServiceRoleKey;
  const headers = {
    apikey: apiKey,
    'Content-Type': 'application/json',
    ...(options.headers || {})
  };
  // Legacy service_role keys are JWTs. New sb_secret keys are opaque and
  // must be sent only as apikey, never as Authorization: Bearer.
  if (!secretKey && legacyServiceRoleKey) headers.Authorization = `Bearer ${legacyServiceRoleKey}`;
  const response = await fetch(`${base}/rest/v1/${path}`, { ...options, headers: {
    ...headers
  } });
  const text = await response.text(); let body = null;
  try { body = text ? JSON.parse(text) : null; } catch { body = text; }
  if (!response.ok) throw new Error(body?.message || body?.hint || `Banco remoto respondeu ${response.status}.`);
  return body;
}
function deviceId(req) { return req.headers['x-device-id'] || req.query?.deviceId || env('DEVICE_ID') || 'hidroponia-adib-01'; }
function deviceAuth(req, res) {
  const supplied = (req.headers.authorization || '').replace(/^Bearer\s+/i, '');
  const secret = env('DEVICE_TOKEN');
  if (!secret || !constantEqual(supplied, secret) || deviceId(req) !== (env('DEVICE_ID') || 'hidroponia-adib-01')) { fail(res, 401, 'Dispositivo não autenticado.'); return false; }
  return true;
}
function validSession(req) {
  const secret = env('REMOTE_SESSION_SECRET');
  const cookies = req.headers.cookie || '';
  const match = cookies.match(/(?:^|;\s*)hidro_session=([^;]+)/);
  if (!secret || !match) return null;
  const parts = match[1].split('.');
  if (parts.length !== 2) return null;
  const [payload, signature] = parts;
  const expected = createHmac('sha256', secret).update(payload).digest('base64url');
  if (!constantEqual(signature, expected)) return null;
  try {
    const session = JSON.parse(Buffer.from(payload, 'base64url').toString('utf8'));
    if (!Number.isFinite(session.exp) || session.exp <= Date.now()) return null;
    return { username: env('REMOTE_USERNAME') || 'admin' };
  } catch { return null; }
}
async function userAuth(req, res) {
  const user = validSession(req);
  if (!user) { fail(res, 401, 'Sessão inválida ou expirada. Entre novamente.'); return null; }
  return user;
}
module.exports = { env, json, fail, method, db, deviceId, deviceAuth, userAuth, constantEqual };
