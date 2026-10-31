(() => {
  'use strict';

  const BLE_SERVICE = '7b4a0001-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_STATUS  = '7b4a0002-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_COMMAND = '7b4a0003-7f3a-4b69-9f33-3a4f8c2c1000';
  const BLE_ACK     = '7b4a0004-7f3a-4b69-9f33-3a4f8c2c1000';
  const $ = id => document.getElementById(id);
  const state = { config: null, authenticated: false, refreshTimer: 0,
    bleDevice: null, bleStatus: null, bleCommand: null, bleAck: null, bleConnected: false, bleRefreshTimer: 0,
    lastPayload: null, schedulesDirty: false, pendingScheduleCommandId: null, busy: false, toastTimer: 0 };

  function toast(message, error = false) {
    const el = $('toast'); el.textContent = message; el.classList.toggle('error', error); el.classList.add('visible');
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
  function safeText(id, value) { $(id).textContent = value ?? '—'; }
  function scheduleIsUsed(slot) {
    if (typeof slot.used === 'boolean') return slot.used;
    return Boolean(slot.enabled) || (slot.start !== '08:00' || slot.stop !== '08:15');
  }
  function renderSchedules(slots) {
    const root = $('scheduleRows'); root.replaceChildren();
    const configured = (slots || []).filter(scheduleIsUsed).slice(0, 8);
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
    $('slotCount').textContent = `${configured.length} de 8 horários`;
    $('addSchedule').disabled = configured.length >= 8;
    if (!configured.length) $('scheduleStatus').textContent = 'Adicione um horário para começar';
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
  function addSchedule() {
    const current = readScheduleRows();
    if (current.length >= 8) return;
    current.push({ used: true, enabled: false, start: '08:00', stop: '08:15' });
    renderSchedules(current); markScheduleDirty();
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
    $('remotePill').classList.toggle('online', fresh); $('remotePill').classList.toggle('offline', !fresh);
    $('remotePill').querySelector('span').textContent = fresh ? 'ESP32 conectado' : 'Dispositivo offline';
    $('staleBanner').classList.toggle('hidden', fresh);
    safeText('pumpState', device.pump ? 'Bomba ligada' : device.pump === false ? 'Bomba desligada' : 'Aguardando estado');
    safeText('modeState', device.manual ? 'Modo manual' : device.manual === false ? 'Automático · agenda local' : 'Modo não confirmado');
    safeText('controlMode', device.manual ? 'Modo manual' : 'Modo automático');
    $('waterState').textContent = device.low === true ? 'Nível baixo · partida bloqueada' : device.low === false ? 'Nível normal' : 'Nível indisponível';
    $('waterState').className = `state-chip ${device.low === true ? 'alert' : device.low === false ? 'safe' : 'neutral'}`;
    $('lowWaterWarning').classList.toggle('hidden', device.low !== true);
    safeText('temperature', Number.isFinite(device.temp) ? `${device.temp.toFixed(1)} °C` : 'Indisponível');
    safeText('humidity', Number.isFinite(device.hum) ? `${device.hum.toFixed(1)} %` : 'Indisponível');
    safeText('temperatureUpdated', device.dhtOk ? `Atualizado ${localDate(receivedAt)}` : 'Leitura do DHT22 indisponível');
    safeText('humidityUpdated', device.dhtOk ? `Atualizado ${localDate(receivedAt)}` : 'Leitura do DHT22 indisponível');
    safeText('sensorTemperature', Number.isFinite(device.temp) ? `${device.temp.toFixed(1)} °C` : 'Leitura indisponível');
    safeText('sensorHumidity', Number.isFinite(device.hum) ? `${device.hum.toFixed(1)} %` : 'Leitura indisponível');
    safeText('sensorTemperatureTime', device.dhtOk ? `Recebida ${localDate(receivedAt)}` : 'Última leitura indisponível');
    safeText('sensorHumidityTime', device.dhtOk ? `Recebida ${localDate(receivedAt)}` : 'Última leitura indisponível');
    safeText('sensorWater', device.low === true ? 'Baixo · bomba protegida' : device.low === false ? 'Normal' : 'Leitura indisponível');
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
    safeText('diagSensors', device.dhtOk ? (device.low == null ? 'DHT22 ativo · boia indisponível' : 'DHT22 e boia ativos') : 'Falha ou leitura pendente');
    safeText('sensorHealth', device.dhtOk ? 'DHT22 respondendo' : 'DHT22 sem leitura válida'); safeText('diagUpdated', localDate(receivedAt));
    $('wifiState').classList.toggle('connection-status', true);
    if (!state.schedulesDirty && device.slots) renderSchedules(device.slots);
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
    if (payload.pendingCommand) safeText('commandState', `Comando ${payload.pendingCommand.action}: aguardando confirmação do ESP32`);
    else if (source === 'ble') safeText('commandState', 'Conectado diretamente por Bluetooth LE');
  }
  async function refresh() {
    if (!state.authenticated || state.bleConnected) return;
    try { const payload = await api('/api/status'); renderStatus(payload); }
    catch (error) { $('remotePill').classList.add('offline'); $('remotePill').querySelector('span').textContent = 'Falha de comunicação'; $('staleBanner').classList.remove('hidden'); $('staleBanner').textContent = `${error.message} · exibindo o último estado recebido, se disponível.`; }
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
      if (action === 'on' && state.lastPayload?.device?.low === true) throw new Error('Nível de água baixo: o ESP32 não permitirá ligar a bomba.');
      if (state.bleConnected) {
        const command = { id: crypto.randomUUID(), action, ...(payload ? { payload } : {}) };
        await state.bleCommand.writeValue(new TextEncoder().encode(JSON.stringify(command)));
        let result = null;
        for (let attempt = 0; attempt < 20; attempt++) {
          await new Promise(resolve => setTimeout(resolve, 150));
          const ack = new TextDecoder().decode((await state.bleAck.readValue()).buffer);
          try { const candidate = JSON.parse(ack); if (candidate.id === command.id) { result = candidate; break; } } catch {}
        }
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
    event.preventDefault(); if (state.busy) return; const button = event.submitter; const slots = [];
    const configured = readScheduleRows();
    if (configured.length > 8) { toast('O limite é de 8 horários.', true); return; }
    if (configured.some(s => !/^([01]\d|2[0-3]):[0-5]\d$/.test(s.start) || !/^([01]\d|2[0-3]):[0-5]\d$/.test(s.stop) || s.start === s.stop)) { toast('Informe horários válidos e diferentes para ligar e desligar.', true); return; }
    for (let i = 0; i < 8; i++) slots.push(configured[i] || { used: false, enabled: false, start: '00:00', stop: '00:00' });
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
      state.bleStatus = await service.getCharacteristic(BLE_STATUS); state.bleCommand = await service.getCharacteristic(BLE_COMMAND); state.bleAck = await service.getCharacteristic(BLE_ACK); state.bleConnected = true;
      safeText('bleState', 'Conectado'); safeText('diagBle', device.name || 'Conectado'); $('bleConnect').textContent = 'Desconectar Bluetooth'; clearInterval(state.refreshTimer); await readBleStatus(); clearInterval(state.bleRefreshTimer); state.bleRefreshTimer = setInterval(readBleStatus, 3000); toast('Conexão BLE estabelecida.');
    } catch (error) { if (error.name !== 'NotFoundError') toast(error.message || 'Não foi possível conectar por BLE.', true); }
  }
  async function readBleStatus() {
    if (!state.bleConnected) return; try { const value = await state.bleStatus.readValue(); const data = JSON.parse(new TextDecoder().decode(value.buffer)); if (Array.isArray(data.slots)) data.slots = data.slots.map(slot => slot.length >= 4 ? ({ used: Boolean(slot[0]), enabled: Boolean(slot[1]), start: slot[2], stop: slot[3] }) : ({ used: Boolean(slot[0]), enabled: Boolean(slot[0]), start: slot[1], stop: slot[2] })); renderStatus({ device: data, lastSeen: new Date().toISOString(), events: data.history || [] }, 'ble'); $('remotePill').classList.add('online'); $('remotePill').querySelector('span').textContent = 'Bluetooth conectado'; }
    catch (error) { toast(`Falha ao ler o estado BLE: ${error.message}`, true); }
  }
  async function toggleBle() { if (state.bleDevice?.gatt?.connected) { state.bleDevice.gatt.disconnect(); return; } await connectBle(); }

  document.addEventListener('DOMContentLoaded', async () => {
    $('loginForm').addEventListener('submit', signIn); $('logoutButton').addEventListener('click', signOut); $('scheduleForm').addEventListener('submit', saveSchedule); $('addSchedule').addEventListener('click', addSchedule); renderSchedules([]);
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
