const { json, fail, method, db, deviceAuth, deviceId } = require('../../lib/api');
module.exports = async (req, res) => {
  if (!method(req, res, 'GET')) return;
  if (!deviceAuth(req, res)) return;
  try {
    const id = deviceId(req); const now = new Date().toISOString();
    await db(`commands?device_id=eq.${encodeURIComponent(id)}&status=eq.pending&expires_at=lte.${encodeURIComponent(now)}`, { method: 'PATCH', headers: { Prefer: 'return=minimal' }, body: JSON.stringify({ status: 'expired', result: { reason: 'Comando expirou antes de ser recebido pelo ESP32.' } }) });
    const rows = await db(`commands?device_id=eq.${encodeURIComponent(id)}&status=eq.pending&expires_at=gt.${encodeURIComponent(now)}&select=id,action,payload,created_at,expires_at&order=created_at.desc&limit=1`);
    json(res, 200, { ok: true, command: rows[0] || null });
  } catch (error) { fail(res, 503, error.message || 'Falha ao consultar comandos.'); }
};
