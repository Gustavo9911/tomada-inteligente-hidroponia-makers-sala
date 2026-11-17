(() => {
  'use strict';

  const BLE_SERVICE = '7b4a0001-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_STATUS  = '7b4a0002-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_COMMAND = '7b4a0003-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_ACK     = '7b4a0004-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_SCHEDULE = '7b4a0005-7f3a-4b69-9f33-3a4f8c2c1000';
  const MAX_SLOTS = 34;
  const $ = id => document.getElementById(id);
  // Algumas versões antigas da página podem coexistir por cache/CDN.
  // Evita que um erro secundário no tratamento de rede esconda a falha real.
  function setText(id, value) { const element = $(id); if (element) element.textContent = value ?? '—'; }
  function setClass(id, className, enabled) { const element = $(id); if (element) element.classList.toggle(className, enabled); }
  const state = { config: null, authenticated: false, refreshTimer: 0,
    bleDevice: null, bleStatus: null, bleCommand: null, bleAck: null, bleSchedule: null, bleReading: false, bleConnected: false, bleRefreshTimer: 0,
    lastPayload: null, schedulesDirty: false, pendingScheduleCommandId: null, busy: false, toastTimer: 0 };

  function toast(message, error = false) {
    const el = $('toast'); if (!el) return; el.textContent = message; el.classList.toggle('error', error); el.classList.add('visible');
    clearTimeout(state.toastTimer); state.toastTimer = setTimeout(() => el.classList.remove('visible'), 3600);
  }
  function setBusy(button, busy, label) {
    button.disabled = busy; button.dataset.original ||= button.textContent;
    button.textContent = busy ? label : button.dataset.original;
  }
  function authHeaders(extra = {}) { return extra; }
  async function api(path, options = {}) {
    const response = await fetch(path, { cache: 'no-store', ...options, headers: authHeaders(options.headers || {}) });
    const data = await response.json().catch(() => ({}));
    if (response.status === 401) { signOut(false); throw new Error('Sessão encerrada. Entre novamente.'); }
    if (!response.ok) throw new Error(data.error || `Falha de comunicação (${response.status})`);
    return data;
  }
  function showLogin(message = '') {
    $('loginView').classList.remove('hidden'); $('appView').classList.add('hidden'); $('loginMessage').textContent = message;
  }
  function showApp() { $('loginView').classList.add('hidden'); $('appView').classList.remove('hidden'); }
  function signOut(revoke = true) {
    if (revoke) fetch('/api/logout', { method: 'POST', cache: 'no-store' }).catch(() => {});
    clearInterval(state.refreshTimer); clearInterval(state.bleRefreshTimer); state.authenticated = false;
    if (state.bleDevice?.gatt?.connected) state.bleDevice.gatt.disconnect(); showLogin('');
  }
  function localDate(value) {
    if (!value) return '—'; const date = new Date(value);
    return Number.isNaN(date.getTime()) ? '—' : date.toLocaleString('pt-BR', { dateStyle: 'short', timeStyle: 'medium' });
  }
  function safeText(id, value) { setText(id, value); }
  // Modo do sistema: 'off' (tudo parado), 'on' (ligado, bomba manual) ou 'auto' (fila de horários).
  function deviceMode(device) {
    if (!device) return null;
    if (device.mode === 'off' || device.mode === 'on' || device.mode === 'auto') return device.mode;
    if (device.manual === true) return 'on';
    if (device.manual === false) return 'auto';
    return null;
  }
  function scheduleIsUsed(slot) {
    if (typeof slot.used === 'boolean') return slot.used;
    return Boolean(slot.enabled) || (slot.start !== '08:00' || slot.stop !== '08:15');
  }
  function renderSchedules(slots) {
    const root = $('scheduleRows'); root.replaceChildren();
    const configured = (slots || []).filter(scheduleIsUsed).slice(0, 34);
    configured.forEach((slot, index) => {
      const row = document.createElement('div'); row.className = 'schedule-row';
      const label = document.createElement('label'); label.className = 'schedule-name';
      const name = document.createElement('span'); name.textContent = `Período ${String(index + 1).padStart(2, '0')}`;
      const enabled = document.createElement('input'); enabled.type = 'checkbox'; enabled.name = `en${index}`; enabled.checked = Boolean(slot.enabled); enabled.setAttribute('aria-label', `Ativar período ${index + 1}`);
      label.append(name);
      const toggle = document.createElement('label'); toggle.className = 'schedule-toggle'; toggle.append(enabled); const toggleText = document.createElement('span'); toggleText.textContent = 'Ativo'; toggle.append(toggleText);
      const start = document.createElement('input'); start.type = 'time'; start.name = `start${index}`; start.value = slot.start || '08:00'; start.required = true; start.setAttribute('aria-label', `Horário para ligar no período ${index + 1}`);
      const arrow = document.createElement('span'); arrow.className = 'schedule-arrow'; arrow.setAttribute('aria-hidden', 'true'); arrow.textContent = '→';
      const stop = document.createElement('input'); stop.type = 'time'; stop.name = `stop${index}`; stop.value = slot.stop || '08:15'; stop.required = true; stop.setAttribute('aria-label', `Horário para desligar no período ${index + 1}`);
      const remove = document.createElement('button'); remove.type = 'button'; remove.className = 'remove-schedule'; remove.textContent = '×'; remove.title = 'Remover horário'; remove.setAttribute('aria-label', `Remover período ${index + 1}`);
      remove.addEventListener('click', () => { const current = readScheduleRows(); current.splice(index, 1); renderSchedules(current); markScheduleDirty(); });
      row.append(label, toggle, start, arrow, stop, remove);
      row.querySelectorAll('input').forEach(input => input.addEventListener('input', markScheduleDirty));
      root.appendChild(row);
    });
    if (!configured.length) { const empty = document.createElement('div'); empty.className = 'schedule-empty'; empty.textContent = 'Nenhum horário cadastrado. Toque em Adicionar horário.'; root.appendChild(empty); }
    $('slotCount').textContent = `${configured.length} de 34 horários`;
    $('addSchedule').disabled = configured.length >= 34;
    if (!configured.length) $('scheduleStatus').textContent = 'Adicione um horário para começar';
    updateQueue();
  }
  // Só redesenha a lista quando os dados mudaram: assim a atualização automática (a cada 5 s)
  // não tira o foco do campo que a pessoa está editando.
  function scheduleSignature(slots) {
    return JSON.stringify((slots || []).filter(scheduleIsUsed).slice(0, 34).map(s => [Boolean(s.enabled), s.start || '08:00', s.stop || '08:15']));
  }
  function syncSchedules(slots) {
    const root = $('scheduleRows');
    if (root && root.querySelector('.schedule-row') && scheduleSignature(readScheduleRows()) === scheduleSignature(slots)) return;
    renderSchedules(slots);
  }
  function readScheduleRows() {
    return [...$('scheduleRows').querySelectorAll('.schedule-row')].map(row => ({
      used: true,
      enabled: row.querySelector('input[type="checkbox"]').checked,
      start: row.querySelector('input[name^="start"]').value,
      stop: row.querySelector('input[name^="stop"]').value
    }));
  }
  function markScheduleDirty() {
    state.schedulesDirty = true;
    $('scheduleStatus').textContent = 'Alterações não salvas';
  }
  // BLE: agenda em texto compacto (10 caracteres por horário usado: usado, ativo, HHMM ligar, HHMM desligar).
  function compactSchedule(slots) {
    return (slots || []).filter(s => s.used).map(s => `1${s.enabled ? 1 : 0}${s.start.replace(':', '')}${s.stop.replace(':', '')}`).join('');
  }
  function parseCompactSchedule(text) {
    const slots = [];
    for (let i = 0; i + 10 <= text.length; i += 10) {
      const p = text.slice(i, i + 10);
      slots.push({ used: p[0] === '1', enabled: p[1] === '1', start: `${p.slice(2, 4)}:${p.slice(4, 6)}`, stop: `${p.slice(6, 8)}:${p.slice(8, 10)}` });
    }
    return slots;
  }
  // ---- Fila de horários: mostra a ordem de execução e o que está rodando agora ----
  function hhmmToSec(text) { const [h, m] = String(text || '').split(':').map(Number); return (h || 0) * 3600 + (m || 0) * 60; }
  function fmtDuration(sec) {
    sec = Math.max(0, Math.round(sec));
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    if (h) return `${h} h ${String(m).padStart(2, '0')} min`;
    if (m) return `${m} min ${String(s).padStart(2, '0')} s`;
    return `${s} s`;
  }
  // Hora do ESP32 agora (segundos do dia), estimada a partir do último estado recebido; null se sem comunicação recente.
  function deviceSeconds() {
    const payload = state.lastPayload; if (!payload) return null;
    const device = payload.device || payload;
    const match = /(\d{2}):(\d{2}):(\d{2})\s*$/.exec(device.now || '');
    if (!match) return null;
    const receivedAt = payload.lastSeen || payload.last_seen || device.updatedAt || device.updated_at;
    const elapsed = receivedAt ? (Date.now() - new Date(receivedAt).getTime()) / 1000 : 0;
    if (!(elapsed >= 0 && elapsed < 45)) return null;
    return (Number(match[1]) * 3600 + Number(match[2]) * 60 + Number(match[3]) + elapsed) % 86400;
  }
  function updateQueue() {
    const rowsRoot = $('scheduleRows'); if (!rowsRoot) return;
    let box = $('queueSummary');
    if (!box) { box = document.createElement('div'); box.id = 'queueSummary'; box.className = 'queue-summary'; rowsRoot.before(box); }
    const device = state.lastPayload ? (state.lastPayload.device || state.lastPayload) : {};
    const now = deviceSeconds();
    const mode = deviceMode(device);
    const items = [...rowsRoot.querySelectorAll('.schedule-row')].map((row, index) => {
      const startText = row.querySelector('input[name^="start"]').value, stopText = row.querySelector('input[name^="stop"]').value;
      const start = hhmmToSec(startText), stop = hhmmToSec(stopText);
      const enabled = row.querySelector('input[type="checkbox"]').checked;
      const valid = Boolean(startText && stopText && start !== stop);
      const item = { row, index, name: `Período ${String(index + 1).padStart(2, '0')}`, startText, stopText, start, stop, enabled, valid, status: 'off', label: 'Desativado', remaining: 0, delta: Infinity };
      if (!enabled || !valid) return item;
      if (now === null) { item.status = 'wait'; item.label = 'Aguardando ESP32'; return item; }
      if (mode !== 'auto') { item.status = 'paused'; item.label = 'Fila parada'; return item; }
      const inWindow = start < stop ? (now >= start && now < stop) : (now >= start || now < stop);
      if (inWindow) {
        item.remaining = start < stop ? stop - now : (now >= start ? 86400 - now + stop : stop - now);
        if (device.levelSensor === true && device.low === true) { item.status = 'blocked'; item.label = 'Bloqueado · nível baixo'; }
        else if (device.pump === true) { item.status = 'running'; item.label = `Rodando agora · faltam ${fmtDuration(item.remaining)}`; }
        else { item.status = 'running'; item.label = 'Na janela agora'; }
      } else {
        item.delta = (start - now + 86400) % 86400;
        item.status = start < stop && now >= stop ? 'done' : 'queued';
        item.label = item.status === 'done' ? 'Concluído hoje' : 'Na fila';
      }
      return item;
    });
    const upcoming = items.filter(i => i.delta !== Infinity).sort((a, b) => a.delta - b.delta);
    if (upcoming.length) { const next = upcoming[0]; next.status = 'next'; next.label = `Próximo · em ${fmtDuration(next.delta)}`; }
    items.forEach(item => {
      let badge = item.row.querySelector('.slot-badge');
      if (!badge) { badge = document.createElement('span'); item.row.querySelector('.schedule-name').append(badge); }
      badge.className = `slot-badge ${item.status}`; badge.textContent = item.label;
      item.row.classList.toggle('is-running', item.status === 'running');
      item.row.classList.toggle('is-next', item.status === 'next');
    });
    // Mensagem "o que está acontecendo agora" (painel da bomba) e resumo da fila (seção Horários)
    const active = items.filter(i => i.enabled && i.valid);
    const running = items.find(i => i.status === 'running' || i.status === 'blocked');
    const next = upcoming[0];
    const first = active[0], last = active[active.length - 1];
    const loopLine = active.length > 1 ? `Ao terminar o último horário (${last.name}, ${last.stopText}), a fila recomeça no ${first.name} (${first.startText}).` : active.length === 1 ? 'Esse horário se repete todos os dias.' : '';
    const hasData = device.pump !== undefined;
    const modeLabel = mode === 'off' ? 'desligado' : mode === 'on' ? 'ligado (manual)' : mode === 'auto' ? 'automático' : 'desconhecido';
    let title = '', text = '', tone = '';
    if (!hasData) { title = 'Aguardando a ESP32'; text = 'Ainda não chegou nenhum estado do dispositivo. Quando ela se conectar, o que está acontecendo aparece aqui.'; tone = 'warn'; }
    else if (now === null) { title = 'Sem comunicação recente'; text = `Último modo conhecido: ${modeLabel}. Não dá para confirmar o que está rodando agora.`; tone = 'warn'; }
    else if (mode === 'off') { title = 'Sistema desligado'; text = 'Tudo parado: bomba desligada e fila pausada. Toque em Ligar para liberar o modo Automático.'; }
    else if (mode === 'on') {
      title = 'Sistema ligado · modo manual';
      text = device.pump ? 'Bomba funcionando agora. Por segurança (ainda sem chave boia) ela para sozinha após 30 minutos. Toque em Automático para iniciar a fila de horários.' : 'Bomba parada por segurança (passou o limite de 30 minutos). O sistema continua ligado: toque em Ligar para religar a bomba ou em Automático para iniciar a fila.';
    } else if (mode === 'auto') {
      if (!active.length) { title = 'Automático ativado · sem horários'; text = 'Nenhum horário ativo na fila. Marque pelo menos um como Ativo e salve para a fila começar.'; tone = 'warn'; }
      else if (running && running.status === 'blocked') { title = 'Automático · bomba bloqueada'; text = `${running.name} está na janela, mas a bomba está bloqueada por nível baixo.`; tone = 'warn'; }
      else if (running) {
        const chained = next && next.start === running.stop;
        title = 'Automático · fila rodando';
        text = `▶ ${running.name} (${running.startText} → ${running.stopText}) · faltam ${fmtDuration(running.remaining)}. ` + (chained ? `Em seguida, sem pausa: ${next.name} (${next.startText} → ${next.stopText}). ` : next ? `Próximo: ${next.name} às ${next.startText} (em ${fmtDuration(next.delta)}). ` : '') + loopLine;
        tone = 'running';
      } else {
        title = 'Automático · aguardando o próximo horário';
        text = (next ? `Bomba desligada até o ${next.name}, às ${next.startText} (em ${fmtDuration(next.delta)}). ` : '') + loopLine;
      }
    } else { title = 'Modo não confirmado'; text = 'Aguardando a ESP32 informar o modo.'; tone = 'warn'; }
    let nowBox = $('nowMessage');
    if (!nowBox) { nowBox = document.createElement('div'); nowBox.id = 'nowMessage'; nowBox.className = 'now-message'; const anchor = document.querySelector('.pump-controls'); if (anchor) anchor.after(nowBox); }
    if (nowBox) { nowBox.replaceChildren(); const strong = document.createElement('strong'); strong.textContent = title; const span = document.createElement('span'); span.textContent = text.trim(); nowBox.append(strong, span); nowBox.className = `now-message ${tone}`.trim(); }
    // Resumo da fila
    box.replaceChildren();
    box.className = 'queue-summary';
    if (!items.length) return;
    const line = value => { const div = document.createElement('div'); div.textContent = value; box.append(div); };
    if (!hasData || now === null) { box.classList.add('warn'); line(active.length ? 'Sem comunicação recente com a ESP32: não dá para confirmar qual horário está rodando agora.' : 'Nenhum horário ativo na fila.'); }
    else if (!active.length) { line('Nenhum horário ativo na fila.'); }
    else if (mode !== 'auto') { box.classList.add('warn'); line(mode === 'off' ? 'Sistema desligado: a fila está parada.' : 'Sistema ligado em modo manual: a fila está parada. Toque em Automático para iniciar.'); line(`${active.length} ${active.length === 1 ? 'horário ativo' : 'horários ativos'} na fila.`); }
    else {
      if (running && running.status === 'running') { box.classList.add('running'); line(`▶ Rodando agora: ${running.name} · ${running.startText} → ${running.stopText} · faltam ${fmtDuration(running.remaining)}`); }
      else if (running) { box.classList.add('warn'); line(`${running.name} está na janela, mas a bomba está bloqueada por nível baixo.`); }
      else line('Nenhum horário rodando agora.');
      if (next) { const chained = running && running.status === 'running' && next.start === running.stop; line(chained ? `Em seguida, sem pausa: ${next.name} (${next.startText} → ${next.stopText}).` : `Próximo na fila: ${next.name} às ${next.startText} (em ${fmtDuration(next.delta)}).`); }
      line(`${active.length} ${active.length === 1 ? 'horário ativo' : 'horários ativos'} na fila · ${loopLine || 'repete todos os dias.'}`);
    }
  }
  // Ordenação opcional (só quando a pessoa clica): o dia começa às 08:00, então 08:15 vem antes de 00:15.
  function sortSchedulesFrom8() {
    const current = readScheduleRows();
    const key = s => (hhmmToSec(s.start) - 8 * 3600 + 86400) % 86400;
    current.sort((x, y) => key(x) - key(y));
    renderSchedules(current); markScheduleDirty();
  }
  function addSchedule() {
    const current = readScheduleRows();
    if (current.length >= 34) return;
    current.push({ used: true, enabled: false, start: '08:00', stop: '08:15' });
    renderSchedules(current); markScheduleDirty();
  }
  function actionText(command) {
    const names = { on: 'Ligou o sistema · bomba em modo manual', off: 'Desligou o sistema · tudo parado', auto: 'Ativou o modo automático · fila em execução', schedules: 'Salvou a agenda de horários' };
    return names[command.action] || `Comando ${command.action}`;
  }
  function actionResult(command) {
    switch (command.status) {
      case 'executed': return ['ok', '✓ confirmado pela ESP32'];
      case 'rejected': return ['bad', `✗ recusado${command.result?.error ? ': ' + command.result.error : ''}`];
      case 'pending': case 'received': return ['wait', '⏳ aguardando a ESP32'];
      case 'expired': return ['bad', 'expirou sem ser executado'];
      case 'superseded': return ['', 'substituído por um comando mais novo'];
      default: return ['', command.status || ''];
    }
  }
  function renderActions(commands = []) {
    const root = $('actionList'); if (!root) return;
    root.replaceChildren();
    if (!commands.length) { const li = document.createElement('li'); li.className = 'muted'; li.textContent = 'Nenhuma ação registrada ainda.'; root.appendChild(li); return; }
    commands.slice(0, 30).forEach(command => {
      const li = document.createElement('li'); const time = document.createElement('time'); time.textContent = localDate(command.created_at);
      const text = document.createElement('span'); text.textContent = actionText(command);
      const [kind, label] = actionResult(command); const tag = document.createElement('em'); tag.className = `act-tag ${kind}`; tag.textContent = label; text.append(tag);
      li.append(time, text); root.appendChild(li);
    });
  }
  function renderHistory(events = []) {
    const root = $('eventList'); root.replaceChildren();
    if (!events.length) { const li = document.createElement('li'); li.className = 'muted'; li.textContent = 'Ainda não há eventos recebidos do dispositivo.'; root.appendChild(li); return; }
    events.forEach(event => { const li = document.createElement('li'); const time = document.createElement('time'); time.textContent = event.time || localDate(event.created_at); const text = document.createElement('span'); text.textContent = event.text || event.message || event.action || 'Evento do dispositivo'; li.append(time, text); root.appendChild(li); });
  }
  function renderStatus(payload, source = 'cloud') {
    state.lastPayload = payload; const device = payload.device || payload;
    const receivedAt = payload.lastSeen || payload.last_seen || device.updatedAt || device.updated_at;
    const age = receivedAt ? Date.now() - new Date(receivedAt).getTime() : Infinity;
    const fresh = age < 45000;
    const remotePill = $('remotePill'); if (remotePill) { remotePill.classList.toggle('online', fresh); remotePill.classList.toggle('offline', !fresh); const label = remotePill.querySelector('span'); if (label) label.textContent = fresh ? 'ESP32 conectado' : 'Dispositivo offline'; }
    setClass('staleBanner', 'hidden', fresh);
    safeText('pumpState', device.pump ? 'Bomba ligada' : device.pump === false ? 'Bomba desligada' : 'Aguardando estado');
    const mode = deviceMode(device);
    safeText('modeState', mode === 'off' ? 'Sistema desligado · tudo parado' : mode === 'on' ? 'Sistema ligado · modo manual' : mode === 'auto' ? 'Modo automático · fila de horários' : 'Modo não confirmado');
    safeText('controlMode', mode === 'off' ? 'Desligado' : mode === 'on' ? 'Ligado (manual)' : mode === 'auto' ? 'Automático' : 'Aguardando estado');
    const autoButton = document.querySelector('[data-command="auto"]');
    if (autoButton && !state.busy) { const locked = mode !== 'on' && mode !== 'auto'; autoButton.disabled = locked; autoButton.title = locked ? 'Ligue o sistema primeiro para liberar o modo automático' : ''; }
    document.querySelectorAll('[data-command]').forEach(b => b.classList.toggle('is-active', b.dataset.command === mode || (b.dataset.command === 'on' && mode === 'on')));
    const levelReady = device.levelSensor === true;
    $('waterState').textContent = !levelReady ? 'Em breve' : device.low === true ? 'Nível baixo · partida bloqueada' : device.low === false ? 'Nível normal' : 'Nível indisponível';
    $('waterState').className = `state-chip ${!levelReady ? 'neutral' : device.low === true ? 'alert' : device.low === false ? 'safe' : 'neutral'}`;
    $('lowWaterWarning').classList.toggle('hidden', !(levelReady && device.low === true));
    safeText('waterNote', levelReady ? 'Reservatório · chave boia' : 'Chave boia ainda não instalada');
    safeText('sensorWaterNote', levelReady ? 'Chave boia digital · GPIO 5' : 'A chave boia ainda não foi instalada');
    safeText('temperature', Number.isFinite(device.temp) ? `${device.temp.toFixed(1)} °C` : 'Indisponível');
    safeText('humidity', Number.isFinite(device.hum) ? `${device.hum.toFixed(1)} %` : 'Indisponível');
    safeText('temperatureUpdated', device.dhtOk ? `Atualizado ${localDate(receivedAt)}` : 'Leitura do DHT22 indisponível');
    safeText('humidityUpdated', device.dhtOk ? `Atualizado ${localDate(receivedAt)}` : 'Leitura do DHT22 indisponível');
    safeText('sensorTemperature', Number.isFinite(device.temp) ? `${device.temp.toFixed(1)} °C` : 'Leitura indisponível');
    safeText('sensorHumidity', Number.isFinite(device.hum) ? `${device.hum.toFixed(1)} %` : 'Leitura indisponível');
    safeText('sensorTemperatureTime', device.dhtOk ? `Recebida ${localDate(receivedAt)}` : 'Última leitura indisponível');
    safeText('sensorHumidityTime', device.dhtOk ? `Recebida ${localDate(receivedAt)}` : 'Última leitura indisponível');
    safeText('sensorWater', !levelReady ? 'Em breve' : device.low === true ? 'Baixo · bomba protegida' : device.low === false ? 'Normal' : 'Leitura indisponível');
    safeText('cycles', device.cycles); safeText('totalTime', Number.isFinite(device.totalMin) ? `${device.totalMin} min` : '—');
    safeText('deviceIp', device.ip || '—');
    safeText('currentTime', device.now || '—');
    safeText('deviceTime', device.now || 'Horário do dispositivo indisponível');
    $('topClock').textContent = device.now ? device.now.slice(-8, -3) : '--:--';
    $('topDate').textContent = device.now && device.now.includes(' ') ? device.now.split(' ')[0] : '--/--/----';
    safeText('sidebarStatus', fresh ? 'Conectado' : 'Sem comunicação');
    safeText('freshness', fresh ? `Recebido ${localDate(receivedAt)}` : 'Sem comunicação recente');
    safeText('lastSeen', fresh ? `Última comunicação ${localDate(receivedAt)}` : `Última comunicação ${localDate(receivedAt)}`);
    safeText('wifiState', device.wifi ? (device.apMode ? 'AP de emergência' : 'Conectado') : 'Desconectado');
    safeText('diagWifi', device.wifi ? `Conectado${device.apMode ? ' · AP de emergência' : ''}` : 'Desconectado');
    safeText('diagIp', device.ip ? `IP ${device.ip}${device.rssi != null ? ` · RSSI ${device.rssi} dBm` : ''}` : 'Endereço IP indisponível');
    safeText('cloudState', fresh ? 'Sincronizado' : 'Sem comunicação'); safeText('diagCloud', fresh ? 'Online' : 'Offline');
    safeText('diagTime', device.timeSynced ? 'Sincronizado' : 'Aguardando sincronização');
    safeText('diagSensors', device.dhtOk ? (levelReady ? 'DHT22 e boia ativos' : 'DHT22 ativo · chave boia em breve') : 'Falha ou leitura pendente');
    safeText('sensorHealth', device.dhtOk ? 'DHT22 respondendo' : 'DHT22 sem leitura válida'); safeText('diagUpdated', localDate(receivedAt));
    $('wifiState').classList.toggle('connection-status', true);
    if (!state.schedulesDirty) {
      const pending = Array.isArray(payload.pendingSchedule?.payload) ? payload.pendingSchedule : null;
      if (pending) { syncSchedules(pending.payload); $('scheduleStatus').textContent = 'Agenda pendente na nuvem · aguardando a ESP32'; }
      else if (device.slots) syncSchedules(device.slots);
    }
    if (state.pendingScheduleCommandId) {
      const saved = (payload.commands || []).find(command => command.id === state.pendingScheduleCommandId);
      if (saved?.status === 'executed') {
        state.pendingScheduleCommandId = null; state.schedulesDirty = false;
        $('scheduleStatus').textContent = 'Agenda confirmada pela ESP32';
        if (device.slots) renderSchedules(device.slots);
      } else if (saved && ['rejected', 'expired', 'superseded'].includes(saved.status)) {
        state.pendingScheduleCommandId = null;
        $('scheduleStatus').textContent = saved.result?.error || 'A ESP32 não confirmou a agenda. Revise e salve novamente.';
      }
    }
    renderHistory(payload.events || device.history || []);
    if (source !== 'ble' && Array.isArray(payload.commands)) renderActions(payload.commands);
    if (payload.pendingCommand) safeText('commandState', `Comando ${payload.pendingCommand.action}: aguardando confirmação do ESP32`);
    else if (source === 'ble') safeText('commandState', 'Conectado diretamente por Bluetooth LE');
    updateQueue();
  }
  async function refresh() {
    if (!state.authenticated || state.bleConnected) return;
    try { const payload = await api('/api/status'); renderStatus(payload); }
    catch (error) { const pill = $('remotePill'); if (pill) { pill.classList.add('offline'); const label = pill.querySelector('span'); if (label) label.textContent = 'Falha de comunicação'; } const banner = $('staleBanner'); if (banner) { banner.classList.remove('hidden'); banner.textContent = `${error.message} · exibindo o último estado recebido, se disponível.`; } }
  }
  async function signIn(event) {
    event.preventDefault(); const button = $('loginButton'); setBusy(button, true, 'Validando…'); $('loginMessage').textContent = '';
    try {
      const response = await fetch('/api/login', { method: 'POST', cache: 'no-store', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username: $('username').value.trim(), password: $('password').value }) });
      const data = await response.json().catch(() => ({})); if (!response.ok) throw new Error(data.error || 'Não foi possível validar esse acesso.');
      state.authenticated = true; showApp(); toast('Acesso confirmado'); await refresh(); startPolling();
    } catch (error) { $('loginMessage').textContent = error.message; }
    finally { setBusy(button, false); }
  }
  function startPolling() { clearInterval(state.refreshTimer); state.refreshTimer = setInterval(refresh, 5000); }
  async function sendCommand(action, payload = null) {
    if (state.busy) return; state.busy = true; const button = document.querySelector(`[data-command="${action}"]`); if (button) setBusy(button, true, 'Enviando…');
    try {
      if (action === 'on' && state.lastPayload?.device?.levelSensor === true && state.lastPayload?.device?.low === true) throw new Error('Nível de água baixo: o ESP32 não permitirá ligar a bomba.');
      if (state.bleConnected) {
        const blePayload = action === 'schedules' ? compactSchedule(payload) : payload;
        const command = { id: crypto.randomUUID(), action, ...(blePayload !== null && blePayload !== undefined ? { payload: blePayload } : {}) };
        const result = await bleExclusive(async () => {
          await state.bleCommand.writeValue(new TextEncoder().encode(JSON.stringify(command)));
          for (let attempt = 0; attempt < 20; attempt++) {
            await new Promise(resolve => setTimeout(resolve, 150));
            const ack = new TextDecoder().decode((await state.bleAck.readValue()).buffer);
            try { const candidate = JSON.parse(ack); if (candidate.id === command.id) return candidate; } catch {}
          }
          return null;
        });
        if (!result || !result.ok) throw new Error(result?.error || 'O ESP32 não confirmou o comando.');
        toast('Comando confirmado pelo ESP32'); await readBleStatus();
      } else {
        const result = await api('/api/command', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ action, payload, idempotencyKey: crypto.randomUUID() }) });
        if (action === 'schedules') {
          state.pendingScheduleCommandId = result.id;
          safeText('commandState', 'Agenda salva na nuvem · aguardando a ESP32');
          toast('Agenda salva na fila. A ESP32 aplicará quando estiver conectada.');
          return { queued: true, id: result.id };
        }
        safeText('commandState', 'Solicitação registrada · aguardando confirmação do ESP32'); toast('Solicitação enviada; aguardando execução no dispositivo.');
        return await waitForCommand(result.id);
      }
      return true;
    } catch (error) { toast(error.message, true); }
    finally { state.busy = false; if (button) setBusy(button, false); }
  }
  async function waitForCommand(id) {
    for (let i = 0; i < 10; i++) {
      await new Promise(resolve => setTimeout(resolve, 1800)); await refresh();
      const command = state.lastPayload?.commands?.find(item => item.id === id);
      if (command?.status === 'executed') { safeText('commandState', 'Comando executado e confirmado pelo ESP32'); toast('ESP32 confirmou a execução.'); return true; }
      if (command?.status === 'rejected') { const why = command.result?.error || 'Comando recusado pelo ESP32'; safeText('commandState', why); toast(why, true); return false; }
    }
    safeText('commandState', 'Aguardando confirmação · verifique a conexão do dispositivo');
    return false;
  }
  async function saveSchedule(event) {
    event.preventDefault(); if (state.busy) return; const button = event.submitter || $('scheduleForm').querySelector('button[type="submit"]'); const slots = [];
    const configured = readScheduleRows();
    if (configured.length > 34) { toast('O limite é de 34 horários.', true); return; }
    if (configured.some(s => !/^([01]\d|2[0-3]):[0-5]\d$/.test(s.start) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(s.stop) || s.start === s.stop)) { toast('Informe horários válidos e diferentes para ligar e desligar.', true); return; }
    for (let i = 0; i < 34; i++) slots.push(configured[i] || { used: false, enabled: false, start: '00:00', stop: '00:00' });
    setBusy(button, true, 'Salvando…');
    try {
      const result = await sendCommand('schedules', slots);
      if (result === true) { state.schedulesDirty = false; $('scheduleStatus').textContent = 'Agenda confirmada pela ESP32'; }
      else if (result?.queued) { state.schedulesDirty = true; $('scheduleStatus').textContent = 'Salva na nuvem · aguardando a ESP32'; }
      else $('scheduleStatus').textContent = 'Não foi possível confirmar. Revise e tente novamente.';
    }
    finally { setBusy(button, false); }
  }
  async function connectBle() {
    if (!('bluetooth' in navigator)) { toast('Este navegador não oferece Web Bluetooth. Use Chrome/Edge em contexto seguro ou um app BLE compatível.', true); return; }
    try {
      const device = await navigator.bluetooth.requestDevice({ filters: [{ name: 'Hidroponia-ESP32' }], optionalServices: [BLE_SERVICE] });
      state.bleDevice = device; device.addEventListener('gattserverdisconnected', () => { state.bleConnected = false; clearInterval(state.bleRefreshTimer); safeText('bleState', 'Desconectado'); safeText('diagBle', 'Desconectado'); $('bleConnect').textContent = 'Conectar por Bluetooth'; $('remotePill').classList.remove('online'); toast('Bluetooth desconectado.'); startPolling(); });
      const server = await device.gatt.connect(); const service = await server.getPrimaryService(BLE_SERVICE);
      state.bleStatus = await service.getCharacteristic(BLE_STATUS); state.bleCommand = await service.getCharacteristic(BLE_COMMAND); state.bleAck = await service.getCharacteristic(BLE_ACK); state.bleSchedule = await service.getCharacteristic(BLE_SCHEDULE).catch(() => null); state.bleConnected = true;
      safeText('bleState', 'Conectado'); safeText('diagBle', device.name || 'Conectado'); $('bleConnect').textContent = 'Desconectar Bluetooth'; clearInterval(state.refreshTimer); await readBleStatus(); clearInterval(state.bleRefreshTimer); state.bleRefreshTimer = setInterval(readBleStatus, 3000); toast('Conexão BLE estabelecida.');
    } catch (error) { if (error.name !== 'NotFoundError') toast(error.message || 'Não foi possível conectar por BLE.', true); }
  }
  // Web Bluetooth só aceita UMA operação GATT por vez: tudo (leituras e escritas) passa por esta fila.
  let bleQueue = Promise.resolve();
  function bleExclusive(task) { const run = bleQueue.then(task, task); bleQueue = run.catch(() => {}); return run; }
  async function readBleStatus() {
    if (!state.bleConnected || state.bleReading) return;
    state.bleReading = true;
    try { await bleExclusive(readBleStatusNow); } finally { state.bleReading = false; }
  }
  async function readBleStatusNow() {
    if (!state.bleConnected) return; try { const value = await state.bleStatus.readValue(); const data = JSON.parse(new TextDecoder().decode(value.buffer)); if (Array.isArray(data.slots)) data.slots = data.slots.map(slot => slot.length >= 4 ? ({ used: Boolean(slot[0]), enabled: Boolean(slot[1]), start: slot[2], stop: slot[3] }) : ({ used: Boolean(slot[0]), enabled: Boolean(slot[0]), start: slot[1], stop: slot[2] })); if (state.bleSchedule) { try { data.slots = parseCompactSchedule(new TextDecoder().decode((await state.bleSchedule.readValue()).buffer)); } catch {} } renderStatus({ device: data, lastSeen: new Date().toISOString(), events: data.history || [] }, 'ble'); $('remotePill').classList.add('online'); $('remotePill').querySelector('span').textContent = 'Bluetooth conectado'; }
    catch (error) { if (!/already in progress/i.test(error.message || '')) toast(`Falha ao ler o estado BLE: ${error.message}`, true); }
  }
  async function toggleBle() { if (state.bleDevice?.gatt?.connected) { state.bleDevice.gatt.disconnect(); return; } await connectBle(); }

  document.addEventListener('DOMContentLoaded', async () => {
    $('loginForm').addEventListener('submit', signIn); $('logoutButton').addEventListener('click', signOut); $('scheduleForm').addEventListener('submit', saveSchedule); $('addSchedule').addEventListener('click', addSchedule); { const sortButton = document.createElement('button'); sortButton.type = 'button'; sortButton.id = 'sortSchedule'; sortButton.className = 'button secondary'; sortButton.textContent = 'Ordenar a partir das 08:00'; sortButton.addEventListener('click', sortSchedulesFrom8); $('addSchedule').before(sortButton); } renderSchedules([]); setInterval(updateQueue, 1000);
    $('togglePassword').addEventListener('click', () => { const input = $('password'); const visible = input.type === 'password'; input.type = visible ? 'text' : 'password'; $('togglePassword').textContent = visible ? 'Ocultar' : 'Ver'; });
    document.querySelectorAll('[data-command]').forEach(button => button.addEventListener('click', () => sendCommand(button.dataset.command)));
    $('bleConnect').addEventListener('click', toggleBle); $('bleConnectCard').addEventListener('click', connectBle);
    document.querySelectorAll('.nav-item').forEach(item => item.addEventListener('click', () => { document.querySelectorAll('.nav-item').forEach(link => link.classList.remove('active')); item.classList.add('active'); safeText('pageTitle', item.textContent.trim()); }));
    try { state.config = await (await fetch('/api/config', { cache: 'no-store' })).json(); safeText('deviceIdLabel', state.config.deviceId || '—'); } catch {}
    if (!('bluetooth' in navigator)) { safeText('bleState', 'Não suportado neste navegador'); safeText('diagBle', 'Web Bluetooth indisponível'); $('bleConnect').disabled = true; }
    try { const response = await fetch('/api/session', { cache: 'no-store' }); state.authenticated = response.ok; } catch { state.authenticated = false; }
    if (state.authenticated) { showApp(); await refresh(); startPolling(); } else showLogin();
  });
})();