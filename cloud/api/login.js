const { createHmac, randomBytes, pbkdf2 } = require('node:crypto');
const { promisify } = require('node:util');
const { json, fail, method, env, constantEqual, guardOrigin, clientIp } = require('../lib/api');

const derive = promisify(pbkdf2);
const SESSION_MS = 8 * 60 * 60 * 1000;
// Limite de tentativas erradas por IP (vale enquanto a função está ativa; a derivação PBKDF2 já deixa cada tentativa lenta).
const MAX_FAILURES = 8, WINDOW_MS = 10 * 60 * 1000;
const failures = new Map();
function tooMany(ip) { const f = failures.get(ip); return Boolean(f && f.count >= MAX_FAILURES && Date.now() - f.first < WINDOW_MS); }
function recordFailure(ip) {
  const now = Date.now(); const f = failures.get(ip);
  if (!f || now - f.first >= WINDOW_MS) failures.set(ip, { count: 1, first: now }); else f.count += 1;
  if (failures.size > 500) for (const [key, value] of failures) if (now - value.first >= WINDOW_MS) failures.delete(key);
}

module.exports = async (req, res) => {
  if (!method(req, res, 'POST')) return;
  if (!guardOrigin(req, res)) return;
  const ip = clientIp(req);
  if (tooMany(ip)) { res.setHeader('Retry-After', '600'); return fail(res, 429, 'Muitas tentativas. Aguarde alguns minutos e tente de novo.'); }
  const username = env('REMOTE_USERNAME');
  const saltText = env('REMOTE_PASSWORD_SALT');
  const expectedText = env('REMOTE_PASSWORD_HASH');
  const sessionSecret = env('REMOTE_SESSION_SECRET');
  if (!username || !saltText || !expectedText || sessionSecret.length < 32) return fail(res, 503, 'Autenticação do painel ainda não foi configurada.');

  const body = req.body || {};
  if (typeof body.username !== 'string' || typeof body.password !== 'string' || body.password.length > 256) return fail(res, 400, 'Informe usuário e senha.');
  const salt = Buffer.from(saltText, 'base64');
  const expected = Buffer.from(expectedText, 'base64');
  if (salt.length < 16 || expected.length !== 32) return fail(res, 503, 'Configuração de autenticação inválida.');

  const actual = await derive(body.password, salt, 210000, 32, 'sha256');
  const userOk = constantEqual(body.username, username); const passOk = constantEqual(actual, expected);
  if (!(userOk && passOk)) { recordFailure(ip); return fail(res, 401, 'Usuário ou senha incorretos.'); }
  failures.delete(ip);

  const expiresAt = Date.now() + SESSION_MS;
  const payload = Buffer.from(JSON.stringify({ exp: expiresAt, nonce: randomBytes(18).toString('hex') })).toString('base64url');
  const signature = createHmac('sha256', sessionSecret).update(payload).digest('base64url');
  res.setHeader('Set-Cookie', `hidro_session=${payload}.${signature}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${SESSION_MS / 1000}`);
  json(res, 200, { ok: true, expiresAt });
};
