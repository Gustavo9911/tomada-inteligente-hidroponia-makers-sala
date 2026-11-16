const { json, fail, method, db, deviceAuth, deviceId } = require('../../lib/api');
module.exports = async (req, res) => {
  if (!method(req, res, 'POST')) return;
  if (!deviceAuth(req, res)) return;
  const body = req.body || {};
  if (typeof body.pump !== 'boolean' || typeof body.low !== 'boolean' || typeof body.manual !== 'boolean' || !Array.isArray(body.slots) || ![8, 34].includes(body.slots.length)) return fail(res, 400, 'Telemetria incompleta ou inválida.');
  try {
    const id = deviceId(req); const now = new Date().toISOString();
    await db('device_state?on_conflict=device_id', { method: 'POST', headers: { Prefer: 'resolution=merge-duplicates,return=minimal' }, body: JSON.stringify([{ device_id: id, latest: body, last_seen: now }]) });
    json(res, 200, { ok: true, receivedAt: now });
  } catch (error) { fail(res, 503, error.message || 'Não foi possível armazenar telemetria.'); }
};