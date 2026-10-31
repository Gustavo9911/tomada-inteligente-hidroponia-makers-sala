const { json, method, userAuth } = require('../lib/api');

module.exports = async (req, res) => {
  if (!method(req, res, 'GET')) return;
  const user = await userAuth(req, res);
  if (!user) return;
  json(res, 200, { ok: true, username: user.username });
};
