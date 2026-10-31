const { randomUUID } = require('node:crypto');
const { json, fail, method, db, env, userAuth } = require('../lib/api');

function validSlots(slots) {
  return Array.isArray(slots) && slots.length === 8 && slots.every(s => s && typeof s.enabled === 'boolean' && /^([01]\d|2[0-3]):[0-5]\d$/.test(s.start) && /^([01]\d|2[0-3]):[0-5]\d$/.test(s.stop));
}
module.exports = async (req, res) => {
  if (!method(req, res, 'POST')) return;
  try {
    const user = await userAuth(req, res); if (!user) return;
    const { action, payload, idempotencyKey } = req.body || {};
    const allowed = ['on', 'off', 'auto', 'schedules'];
    if (!allowed.includes(action) || (action === 'schedules' && !validSlots(payload))) return fail(res, 400, 'Comando ou programação inválida.');
    if (action !== 'schedules' && payload != null) return fail(res, 400, 'O comando não aceita payload.');
    const key = typeof idempotencyKey === 'string' && /^[0-9a-f-]{36}$/i.test(idempotencyKey) ? idempotencyKey : randomUUID();
    const previous = await db(`commands?idempotency_key=eq.${encodeURIComponent(key)}&select=id,status&limit=1`);
    if (previous.length) return json(res, 200, { ...previous[0], message: 'Solicitação já registrada anteriormente.' });
    const id = randomUUID(); const expiresAt = new Date(Date.now() + 90000).toISOString();
    // A solicitação mais recente substitui qualquer comando ainda pendente;
    // comandos expirados nunca ficam disponíveis após reconexões tardias.
    const device = env('DEVICE_ID') || 'hidroponia-adib-01';
    await db(`commands?device_id=eq.${encodeURIComponent(device)}&status=in.(pending,received)`, { method: 'PATCH', headers: { Prefer: 'return=minimal' }, body: JSON.stringify({ status: 'superseded', result: { reason: 'Substituído por solicitação mais recente.' } }) });
    const rows = await db('commands', { method: 'POST', headers: { Prefer: 'return=representation' }, body: JSON.stringify([{ id, device_id: device, action, payload: payload || {}, status: 'pending', idempotency_key: key, created_by: user.username, expires_at: expiresAt }]) });
    json(res, 202, { id: rows?.[0]?.id || id, status: 'pending', message: 'Aguardando o ESP32 buscar e confirmar a execução.' });
  } catch (error) { fail(res, 503, error.message || 'Não foi possível registrar o comando.'); }
};
