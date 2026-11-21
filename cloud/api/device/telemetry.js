const { json, fail, method, db, deviceAuth, deviceId } = require('../../lib/api');
const HHMM = /^([01]\d|2[0-3]):[0-5]\d$/;
const text = (value, max) => String(value ?? '').slice(0, max);
function validSlot(slot) {
  return slot && typeof slot === 'object' && typeof slot.enabled === 'boolean' && (slot.used === undefined || typeof slot.used === 'boolean') && HHMM.test(slot.start) && HHMM.test(slot.stop);
}
module.exports = async (req, res) => {
  if (!method(req, res, 'POST')) return;
  if (!deviceAuth(req, res)) return;
  const body = req.body;
  if (!body || typeof body !== 'object' || Array.isArray(body)) return fail(res, 400, 'Telemetria incompleta ou inválida.');
  if (JSON.stringify(body).length > 20000) return fail(res, 413, 'Telemetria grande demais.');
  if (typeof body.pump !== 'boolean' || typeof body.low !== 'boolean' || typeof body.manual !== 'boolean' || !Array.isArray(body.slots) || ![8, 34].includes(body.slots.length) || !body.slots.every(validSlot)) return fail(res, 400, 'Telemetria incompleta ou inválida.');
  if (body.relay !== undefined && typeof body.relay !== 'boolean') return fail(res, 400, 'Telemetria incompleta ou inválida.');
  if (body.mode !== undefined && !['off', 'on', 'auto'].includes(body.mode)) return fail(res, 400, 'Telemetria incompleta ou inválida.');
  // Histórico: no máximo 20 eventos, textos curtos (o painel só exibe texto simples).
  const latest = { ...body, history: (Array.isArray(body.history) ? body.history : []).slice(0, 20).map(e => ({ time: text(e?.time, 40), text: text(e?.text, 120) })) };
  if (latest.ip !== undefined) latest.ip = text(latest.ip, 64);
  if (latest.now !== undefined) latest.now = text(latest.now, 64);
  try {
    const id = deviceId(req); const now = new Date().toISOString();
    await db('device_state?on_conflict=device_id', { method: 'POST', headers: { Prefer: 'resolution=merge-duplicates,return=minimal' }, body: JSON.stringify([{ device_id: id, latest, last_seen: now }]) });
    json(res, 200, { ok: true, receivedAt: now });
  } catch (error) { fail(res, 503, error.message || 'Não foi possível armazenar telemetria.'); }
};
