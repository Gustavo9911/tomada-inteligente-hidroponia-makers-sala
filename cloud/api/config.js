const { json, method, env } = require('../lib/api');
module.exports = (req, res) => {
  if (!method(req, res, 'GET')) return;
  json(res, 200, { deviceId: env('DEVICE_ID') || 'hidroponia-adib-01' });
};
