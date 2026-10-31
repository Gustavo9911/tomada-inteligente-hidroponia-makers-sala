const { json, fail, method, db, deviceAuth, deviceId } = require('../../lib/api');
module.exports = async (req, res) => {
  if (!method(req, res, 'POST')) return;
  if (!deviceAuth(req, res)) return;
  const { id, stage, ok, error } = req.body || {};
  if (typeof id !== 'string' || !/^[0-9a-f-]{36}$/i.test(id) || !['received', 'executed'].includes(stage) || typeof ok !== 'boolean') return fail(res, 400, 'Confirmação inválida.');
  const status = stage === 'received' ? (ok ? 'received' : 'rejected') : (ok ? 'executed' : 'rejected');
  try {
    const rows = await db(`commands?id=eq.${encodeURIComponent(id)}&device_id=eq.${encodeURIComponent(deviceId(req))}&status=in.(pending,received)&select=id`, { method: 'PATCH', headers: { Prefer: 'return=representation' }, body: JSON.stringify({ status, result: { stage, ok, error: typeof error === 'string' ? error.slice(0, 160) : null }, updated_at: new Date().toISOString() }) });
    if (!rows?.length) return fail(res, 409, 'Comando ausente, expirado ou já finalizado.');
    json(res, 200, { ok: true, status });
  } catch (failure) { fail(res, 503, failure.message || 'Falha ao registrar a confirmação.'); }
};
