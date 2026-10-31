const { json, fail, method, db, env, userAuth } = require('../lib/api');
module.exports = async (req, res) => {
  if (!method(req, res, 'GET')) return;
  try {
    const user = await userAuth(req, res); if (!user) return;
    const id = env('DEVICE_ID') || 'hidroponia-adib-01';
    const rows = await db(`device_state?device_id=eq.${encodeURIComponent(id)}&select=device_id,latest,last_seen&limit=1`);
    const commands = await db(`commands?device_id=eq.${encodeURIComponent(id)}&created_by=eq.${encodeURIComponent(user.username)}&select=id,action,status,result,created_at,expires_at&order=created_at.desc&limit=10`);
    const events = rows[0]?.latest?.history || [];
    json(res, 200, { device: rows[0]?.latest || null, lastSeen: rows[0]?.last_seen || null, events, commands, pendingCommand: commands.find(c => ['pending', 'received'].includes(c.status)) || null });
  } catch (error) { fail(res, 503, error.message || 'Falha ao consultar o estado remoto.'); }
};
