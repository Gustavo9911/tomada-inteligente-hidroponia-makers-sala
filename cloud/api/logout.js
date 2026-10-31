const { json, method } = require('../lib/api');

module.exports = (req, res) => {
  if (!method(req, res, 'POST')) return;
  res.setHeader('Set-Cookie', 'hidro_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0');
  json(res, 200, { ok: true });
};
