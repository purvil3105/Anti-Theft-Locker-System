/* Smart Anti-Theft Locker Alarm - owner web dashboard
 *
 * - Reads the ThingSpeak channel (fields 1-8 + status) every 5 s.
 * - Sends "ARM <pin>" / "DISARM <pin>" through ThingSpeak TalkBack; the ESP32 checks the PIN.
 * - Raises an alert (banner, sound, browser notification) when the locker reports
 *   TAMPER (2) or THEFT (3). The email alert itself is sent by the ESP32.
 * - Open index.html?demo for a simulated locker (no ThingSpeak needed, PIN 1234).
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const params = new URLSearchParams(location.search);
  const DEMO = params.has('demo');
  const KEY = DEMO ? 'smartlocker.demo.' : 'smartlocker.';

  const POLL_MS = DEMO ? 1500 : 5000;          // channel read interval
  const OFFLINE_MS = DEMO ? 15000 : 60000;     // no new entry for this long = locker offline
  const IDLE_LOGOUT_MS = 10 * 60 * 1000;       // lock the dashboard after 10 min without activity
  const MAX_TRIES = 5;                         // wrong PINs before the keypad locks
  const LOCKOUT_MS = 60 * 1000;                // first lockout; grows with each round
  const CMD_SLOW_MS = 45 * 1000;
  const CMD_GIVEUP_MS = 150 * 1000;            // locker ignores commands older than 120 s
  const RESULTS = 120;                         // about 40 min of history at one entry per 20 s

  const STATES = [
    { name: 'Disarmed', short: 'Disarmed', desc: 'The alarm is off. Arm it before you leave the locker.' },
    { name: 'Armed', short: 'Armed', desc: 'The locker is protected. The vibration and tilt sensors are active.' },
    { name: 'Tamper alarm', short: 'Tamper', desc: 'Repeated vibration: someone is forcing or hammering the locker. The buzzer is beeping.' },
    { name: 'Theft alarm', short: 'Theft', desc: 'The locker has been tilted, lifted or moved. The buzzer is sounding continuously.' },
  ];
  const CMD_TEXT = {
    1: ['Locker armed. It is protected 10 s after the command arrived.', 'ok'],
    2: ['Locker disarmed.', 'ok'],
    3: ['The locker rejected the PIN. The dashboard PIN must match OWNER_PIN in the firmware.', 'bad'],
    4: ['The locker refused the command: too many wrong PINs. It accepts commands again after 5 minutes.', 'bad'],
    5: ['The command reached the locker more than 2 minutes late and was ignored. Try again.', 'bad'],
    6: ['The locker did not understand the command.', 'bad'],
  };
  const MAIL_TEXT = {
    0: ['None', ''], 1: ['Sending', 'warn'], 2: ['Sent', 'ok'], 3: ['Retrying', 'warn'], 4: ['Not set up', 'bad'],
  };

  // ---------------------------------------------------------------- storage
  const mem = new Map();
  const store = {
    get(k, fallback = null) {
      try {
        const v = localStorage.getItem(KEY + k);
        return v == null ? fallback : JSON.parse(v);
      } catch { return mem.has(k) ? mem.get(k) : fallback; }
    },
    set(k, v) { mem.set(k, v); try { localStorage.setItem(KEY + k, JSON.stringify(v)); } catch { /* memory only */ } },
    del(k) { mem.delete(k); try { localStorage.removeItem(KEY + k); } catch { /* ignore */ } },
    clearAll() {
      mem.clear();
      try { Object.keys(localStorage).filter((k) => k.startsWith(KEY)).forEach((k) => localStorage.removeItem(k)); } catch { /* ignore */ }
    },
  };
  let memSession = null;
  const session = {
    get() { try { return JSON.parse(sessionStorage.getItem(KEY + 'session')); } catch { return memSession; } },
    set(v) { memSession = v; try { sessionStorage.setItem(KEY + 'session', JSON.stringify(v)); } catch { /* memory only */ } },
    del() { memSession = null; try { sessionStorage.removeItem(KEY + 'session'); } catch { /* ignore */ } },
  };

  // ---------------------------------------------------------------- SHA-256 (works on file:// too)
  const SHA_K = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ];
  const ror = (x, n) => (x >>> n) | (x << (32 - n));
  function sha256Hex(text) {
    const data = new TextEncoder().encode(text);
    const total = Math.ceil((data.length + 9) / 64) * 64;
    const buf = new Uint8Array(total);
    buf.set(data);
    buf[data.length] = 0x80;
    const dv = new DataView(buf.buffer);
    const bits = data.length * 8;
    dv.setUint32(total - 8, Math.floor(bits / 0x100000000));
    dv.setUint32(total - 4, bits >>> 0);
    const H = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19];
    const w = new Uint32Array(64);
    for (let off = 0; off < total; off += 64) {
      for (let i = 0; i < 16; i++) w[i] = dv.getUint32(off + i * 4);
      for (let i = 16; i < 64; i++) {
        const s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >>> 3);
        const s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >>> 10);
        w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
      }
      let [a, b, c, d, e, f, g, h] = H;
      for (let i = 0; i < 64; i++) {
        const t1 = (h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i]) | 0;
        const t2 = ((ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c))) | 0;
        h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
      }
      H[0] = (H[0] + a) | 0; H[1] = (H[1] + b) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
      H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
    }
    return H.map((x) => (x >>> 0).toString(16).padStart(8, '0')).join('');
  }
  const hashPin = (salt, pin) => sha256Hex(`smartlocker:${salt}:${pin}`);
  function newSalt() {
    const a = new Uint8Array(16);
    crypto.getRandomValues(a);
    return [...a].map((x) => x.toString(16).padStart(2, '0')).join('');
  }

  // ---------------------------------------------------------------- PIN guard (wrong-PIN lockout)
  const guard = () => store.get('guard', { fails: 0, until: 0, rounds: 0 });
  const lockedFor = () => Math.max(0, guard().until - Date.now());
  function failPin() {
    const g = guard();
    g.fails += 1;
    if (g.fails >= MAX_TRIES) { g.rounds += 1; g.until = Date.now() + LOCKOUT_MS * g.rounds; g.fails = 0; }
    store.set('guard', g);
    return g;
  }
  const passPin = () => store.set('guard', { fails: 0, until: 0, rounds: 0 });
  const checkPin = (pin) => !!settings && hashPin(settings.salt, pin) === settings.pinHash;

  // ---------------------------------------------------------------- small helpers
  const pad2 = (n) => String(n).padStart(2, '0');
  const fmtTime = (d) => `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  function fmtWhen(d) {
    if (d.toDateString() === new Date().toDateString()) return fmtTime(d);
    return `${d.toLocaleDateString([], { day: '2-digit', month: 'short' })} ${fmtTime(d)}`;
  }
  function ago(ms) {
    const s = Math.max(0, Math.round(ms / 1000));
    if (s < 5) return 'just now';
    if (s < 60) return `${s} s ago`;
    const m = Math.round(s / 60);
    if (m < 60) return `${m} min ago`;
    const h = Math.round(m / 60);
    return h < 48 ? `${h} h ago` : `${Math.round(h / 24)} days ago`;
  }
  function setMsg(el, text, kind) { el.textContent = text || ''; el.className = 'msg' + (kind ? ' ' + kind : ''); }
  function el(tag, cls, text) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }
  const num = (v) => (v == null || String(v).trim() === '' ? null : Number(v));
  function norm(f) {
    return {
      id: Number(f.entry_id), t: new Date(f.created_at),
      state: num(f.field1), armed: num(f.field2), vib: num(f.field3), tilt: num(f.field4),
      alarms: num(f.field5), cmd: num(f.field6), cmdId: f.field7 == null ? '' : String(f.field7).trim(),
      mail: num(f.field8), status: (f.status || '').trim(),
    };
  }

  // ---------------------------------------------------------------- data sources
  function thingSpeak(s) {
    const base = 'https://api.thingspeak.com';
    const tb = `${base}/talkbacks/${encodeURIComponent(s.tbId)}/commands`;
    return {
      label: `ThingSpeak channel ${s.channelId}`,
      async feeds() {
        const q = new URLSearchParams({ results: RESULTS, status: 'true' });
        if (s.readKey) q.set('api_key', s.readKey);
        const r = await fetch(`${base}/channels/${encodeURIComponent(s.channelId)}/feeds.json?${q}`, { cache: 'no-store' });
        if (!r.ok) throw new Error(r.status === 404 ? 'channel not found' : `ThingSpeak error ${r.status}`);
        const j = await r.json();
        if (!j || !Array.isArray(j.feeds)) throw new Error('no access to the channel (check the Channel ID and Read API key)');
        return j.feeds;
      },
      async checkTalkBack() {
        const r = await fetch(`${tb}.json?api_key=${encodeURIComponent(s.tbKey)}`, { cache: 'no-store' });
        if (!r.ok) throw new Error(r.status === 401 ? 'TalkBack refused the API key' : `TalkBack error ${r.status}`);
      },
      async command(text) {
        // Only the newest command should run, so clear anything still queued (best effort).
        try { await fetch(`${tb}.json?api_key=${encodeURIComponent(s.tbKey)}`, { method: 'DELETE' }); } catch { /* ignore */ }
        const r = await fetch(`${tb}.json`, {
          method: 'POST',
          body: new URLSearchParams({ api_key: s.tbKey, command_string: text }),
        });
        if (!r.ok) throw new Error(r.status === 401 ? 'TalkBack refused the API key' : `TalkBack error ${r.status}`);
        const j = await r.json();
        if (!j || j.id == null) throw new Error('TalkBack did not return a command id');
        return String(j.id);
      },
    };
  }

  // Simulated locker that follows the same rules as the firmware (faster uploads).
  function Simulator() {
    const feeds = [];
    const s = { state: 0, armed: 0, vib: 0, tilt: 0, alarms: 0, cmd: 0, cmdId: 0, mail: 0, status: 'Started' };
    let entry = 0, cmdSeq = 48210, armTime = 0, online = true, dirty = false, lastUp = 0;
    let mailAt = 0, wrong = 0, vibTimes = [];
    const queue = [];

    function rec(t) {
      entry += 1;
      feeds.push({
        entry_id: entry, created_at: new Date(t).toISOString(),
        field1: String(s.state), field2: String(s.armed), field3: String(s.vib), field4: String(s.tilt),
        field5: String(s.alarms), field6: String(s.cmd), field7: String(s.cmdId), field8: String(s.mail),
        status: s.status,
      });
      if (feeds.length > RESULTS) feeds.shift();
      s.vib = 0;
    }
    function setStatus(m) { s.status = m; dirty = true; }
    function alarm(st) {
      s.alarms += 1; s.state = st; s.mail = 1; mailAt = Date.now() + 2500;
      setStatus(st === 3 ? 'THEFT alarm: locker tilted or lifted' : 'TAMPER alarm: locker being forced');
    }
    function armNow(by) {
      if (s.armed) { setStatus(`Already armed (${by})`); return; }
      s.armed = 1; s.state = 0; armTime = Date.now();
      setStatus(s.tilt ? `Arming in 10 s (${by}) - WARNING: locker is tilted, level it` : `Arming in 10 s (${by}) - close the locker`);
    }
    function run(c) {
      const [verb, pin] = c.text.trim().toUpperCase().split(/[\s:]+/);
      s.cmdId = c.id;
      if (verb !== 'ARM' && verb !== 'DISARM') { s.cmd = 6; setStatus('Unknown command from dashboard'); return; }
      if (pin !== '1234') { wrong += 1; s.cmd = 3; setStatus(`Wrong PIN from dashboard (${wrong} of 5 tries)`); return; }
      wrong = 0;
      if (verb === 'ARM') { s.cmd = 1; armNow('dashboard'); return; }
      s.cmd = 2;
      const was = s.state >= 2;
      s.armed = 0; s.state = 0;
      setStatus(was ? 'Disarmed by dashboard - alarm cleared' : 'Disarmed by dashboard');
    }
    function tamperCheck() {
      const now = Date.now();
      vibTimes = vibTimes.filter((t) => now - t <= 2000);
      if (vibTimes.length >= 3 && s.state < 2) alarm(2); else dirty = true;
    }

    // Seed about 40 minutes of history: armed from the dashboard, one tamper alarm, re-armed with BOOT.
    (function seed() {
      const now = Date.now();
      const plan = [
        [37.0, () => { s.armed = 1; s.cmd = 1; s.cmdId = ++cmdSeq; s.status = 'Arming in 10 s (dashboard) - close the locker'; }],
        [36.6, () => { s.state = 1; s.status = 'Armed - locker protected'; }],
        [24.0, () => { s.vib = 1; }],
        [22.0, () => { s.alarms += 1; s.state = 2; s.vib = 4; s.mail = 1; s.status = 'TAMPER alarm: locker being forced'; }],
        [21.6, () => { s.mail = 2; }],
        [19.0, () => { s.state = 0; s.armed = 0; s.cmd = 2; s.cmdId = ++cmdSeq; s.status = 'Disarmed by dashboard - alarm cleared'; }],
        [11.0, () => { s.armed = 1; s.status = 'Arming in 10 s (BOOT button) - close the locker'; }],
        [10.6, () => { s.state = 1; s.status = 'Armed - locker protected'; }],
      ];
      let i = 0;
      for (let t = now - 40 * 60000; t < now - 2000; t += 20000) {
        while (i < plan.length && now - plan[i][0] * 60000 <= t) { plan[i][1](); i += 1; }
        rec(t);
      }
      armTime = now - 11 * 60000;
      lastUp = now;
    })();

    setInterval(() => {
      const now = Date.now();
      if (online && queue.length && now - queue[0].at > 1500) run(queue.shift());
      if (s.armed && s.state === 0 && now - armTime >= 10000) {
        if (s.tilt) alarm(3); else { s.state = 1; setStatus('Armed - locker protected'); }
      }
      if (s.mail === 1 && online && now >= mailAt) { s.mail = 2; dirty = true; }
      if (online && ((dirty && now - lastUp >= 1500) || now - lastUp >= 4000)) { rec(now); lastUp = now; dirty = false; }
    }, 250);

    return {
      label: 'Simulated locker',
      async feeds() { return feeds.map((f) => ({ ...f })); },
      async checkTalkBack() { /* nothing to check */ },
      async command(text) {
        const id = String(++cmdSeq);
        queue.length = 0;
        queue.push({ id, text, at: Date.now() });
        return id;
      },
      act(kind) {
        const active = s.armed && s.state >= 1;
        if (kind === 'knock' && active) { s.vib += 1; vibTimes.push(Date.now()); tamperCheck(); }
        if (kind === 'tamper' && active) { for (let i = 0; i < 3; i++) { s.vib += 1; vibTimes.push(Date.now()); } tamperCheck(); }
        if (kind === 'theft') { s.tilt = 1; dirty = true; if (active && s.state < 3) alarm(3); }
        if (kind === 'level') { s.tilt = 0; dirty = true; }
        if (kind === 'boot' && !s.armed) armNow('BOOT button');
        if (kind === 'wifi') online = !online;
        return online;
      },
    };
  }

  // ---------------------------------------------------------------- keypad
  function keypad(padEl, dotsEl, onDone) {
    let value = '';
    const dots = [...dotsEl.children];
    ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'C', '0', '<'].forEach((k) => {
      const b = el('button', 'key' + (k === 'C' || k === '<' ? ' fn' : ''), k === 'C' ? 'Clear' : k === '<' ? '⌫' : k);
      b.type = 'button';
      b.setAttribute('aria-label', k === 'C' ? 'Clear' : k === '<' ? 'Delete' : k);
      b.addEventListener('click', () => press(k));
      padEl.appendChild(b);
    });
    const draw = () => dots.forEach((d, i) => d.classList.toggle('on', i < value.length));
    function press(k) {
      unlockAudio();
      if (padEl.classList.contains('locked')) return;
      if (k === 'C') value = '';
      else if (k === '<') value = value.slice(0, -1);
      else if (/^\d$/.test(k) && value.length < 4) value += k;
      draw();
      if (value.length === 4) {
        const v = value;
        value = '';
        setTimeout(() => { draw(); onDone(v); }, 140);
      }
    }
    return {
      press,
      clear() { value = ''; draw(); },
      shake() { dotsEl.classList.remove('shake'); void dotsEl.offsetWidth; dotsEl.classList.add('shake'); },
    };
  }

  // ---------------------------------------------------------------- sound + notifications
  let audio = null, sirenTimer = null, sirenLevel = 0;
  function unlockAudio() {
    try {
      if (!audio) { const AC = window.AudioContext || window.webkitAudioContext; if (AC) audio = new AC(); }
      if (audio && audio.state === 'suspended') audio.resume();
    } catch { /* no audio */ }
  }
  function beep(freq, dur) {
    if (!audio) return;
    const o = audio.createOscillator(), g = audio.createGain(), t = audio.currentTime;
    o.type = 'square';
    o.frequency.value = freq;
    g.gain.setValueAtTime(0.0001, t);
    g.gain.exponentialRampToValueAtTime(0.06, t + 0.01);
    g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
    o.connect(g); g.connect(audio.destination);
    o.start(t); o.stop(t + dur + 0.02);
  }
  function startSiren(level) {
    if (sirenTimer && sirenLevel === level) return;
    stopSiren();
    sirenLevel = level;
    const play = () => {
      if (level === 3) { beep(1800, 0.22); setTimeout(() => beep(1350, 0.22), 280); } else beep(1600, 0.14);
    };
    play();
    sirenTimer = setInterval(play, level === 3 ? 800 : 1200);
  }
  function stopSiren() { clearInterval(sirenTimer); sirenTimer = null; sirenLevel = 0; }
  const canNotify = () => 'Notification' in window && window.isSecureContext;
  function updateNotifyBtn() { $('notifyBtn').hidden = !(canNotify() && Notification.permission === 'default'); }
  function notify(title, body) {
    if (!canNotify() || Notification.permission !== 'granted') return;
    try { new Notification(title, { body, tag: 'smartlocker-alarm', requireInteraction: true }); } catch { /* ignore */ }
  }

  // ---------------------------------------------------------------- app state
  let settings = null, source = null, sim = null;
  let entries = [], running = false, pollTimer = null, lastError = null, busy = false, pending = null;
  let loginPad, modalPad, modalVerb = null;

  function show(view) { ['setupView', 'loginView', 'dashView'].forEach((v) => { $(v).hidden = v !== view; }); }
  const sessionOk = () => { const s = session.get(); return !!s && s.exp > Date.now(); };
  const startSession = () => session.set({ exp: Date.now() + IDLE_LOGOUT_MS });

  // ---------------------------------------------------------------- setup / settings
  const settingsComplete = (s) => !!s && (s.demo || (s.channelId && s.tbId && s.tbKey)) && s.pinHash && s.salt;

  function openSetup(first) {
    show('setupView');
    const f = $('setupForm'), s = settings || {};
    f.channelId.value = s.channelId || '';
    f.readKey.value = s.readKey || '';
    f.tbId.value = s.tbId || '';
    f.tbKey.value = s.tbKey || '';
    f.pin1.value = ''; f.pin2.value = '';
    $('setupTitle').textContent = first ? 'Set up the dashboard' : 'Settings';
    $('pinLegend').textContent = first ? 'Owner PIN' : 'Change PIN (leave empty to keep it)';
    $('setupBack').hidden = first;
    $('resetBtn').hidden = first;
    $('demoLink').hidden = !first;
    setMsg($('setupMsg'), '', '');
  }
  function formValues() {
    const f = $('setupForm');
    return {
      channelId: f.channelId.value.trim(), readKey: f.readKey.value.trim(),
      tbId: f.tbId.value.trim(), tbKey: f.tbKey.value.trim(), pin1: f.pin1.value.trim(), pin2: f.pin2.value.trim(),
    };
  }
  function validate(v, needPin) {
    if (!/^\d+$/.test(v.channelId)) return 'The Channel ID is a number (ThingSpeak > Channel > Channel ID).';
    if (!/^\d+$/.test(v.tbId)) return 'The TalkBack ID is a number (Apps > TalkBack).';
    if (!v.tbKey) return 'Enter the TalkBack API key.';
    if (needPin || v.pin1 || v.pin2) {
      if (!/^\d{4}$/.test(v.pin1)) return 'The PIN must be exactly 4 digits.';
      if (v.pin1 !== v.pin2) return 'The two PINs do not match.';
    }
    return null;
  }
  async function testConnection() {
    const v = formValues();
    const msg = $('setupMsg');
    const err = validate(v, false);
    if (err) return setMsg(msg, err, 'bad');
    setMsg(msg, 'Testing…', 'wait');
    const ts = thingSpeak(v);
    try {
      const feeds = await ts.feeds();
      const last = feeds.length ? norm(feeds[feeds.length - 1]) : null;
      let text = last ? `Channel OK, last update ${ago(Date.now() - last.t)}.` : 'Channel OK, but it has no entries yet (power on the locker).';
      try { await ts.checkTalkBack(); text += ' TalkBack OK.'; setMsg(msg, text, 'ok'); }
      catch (e) { setMsg(msg, `${text} TalkBack failed: ${e.message}.`, 'bad'); }
    } catch (e) {
      setMsg(msg, `Channel failed: ${e.message}.`, 'bad');
    }
  }
  function saveSetup(ev) {
    ev.preventDefault();
    const first = !settingsComplete(settings);
    const v = formValues();
    const err = validate(v, first);
    if (err) return setMsg($('setupMsg'), err, 'bad');
    const next = { channelId: v.channelId, readKey: v.readKey, tbId: v.tbId, tbKey: v.tbKey,
      salt: settings && settings.salt, pinHash: settings && settings.pinHash };
    if (v.pin1) { next.salt = newSalt(); next.pinHash = hashPin(next.salt, v.pin1); passPin(); }
    settings = next;
    store.set('settings', settings);
    if (first) { showLogin('Saved. Now enter your new PIN.'); return; }
    enterDash();
  }
  function resetAll() {
    stopDash();
    store.clearAll();
    session.del();
    settings = null;
    openSetup(true);
  }

  // ---------------------------------------------------------------- login
  function showLogin(msg) {
    stopDash();
    show('loginView');
    loginPad.clear();
    setMsg($('loginMsg'), msg || '', msg ? 'wait' : '');
    refreshLock();
  }
  function refreshLock() {
    const ms = lockedFor(), padEl = $('loginPad');
    if (ms > 0) {
      padEl.classList.add('locked');
      setMsg($('loginMsg'), `Too many wrong PINs. Try again in ${Math.ceil(ms / 1000)} s.`, 'bad');
    } else if (padEl.classList.contains('locked')) {
      padEl.classList.remove('locked');
      setMsg($('loginMsg'), '', '');
    }
  }
  function tryLogin(pin) {
    if (lockedFor() > 0) return refreshLock();
    if (checkPin(pin)) { passPin(); startSession(); enterDash(); return; }
    const g = failPin();
    loginPad.shake();
    if (lockedFor() > 0) refreshLock();
    else setMsg($('loginMsg'), `Wrong PIN. ${MAX_TRIES - g.fails} ${MAX_TRIES - g.fails === 1 ? 'try' : 'tries'} left.`, 'bad');
  }
  function logout(msg) { session.del(); showLogin(msg); }

  // ---------------------------------------------------------------- dashboard
  function enterDash() {
    show('dashView');
    source = DEMO ? sim : thingSpeak(settings);
    $('footSource').textContent = source.label;
    $('demoPanel').hidden = !DEMO;
    $('settingsBtn').hidden = DEMO;
    updateNotifyBtn();
    if (!running) { running = true; poll(); }
  }
  function stopDash() {
    running = false;
    clearTimeout(pollTimer);
    stopSiren();
    closeModal();
    entries = []; pending = null; lastError = null;
    document.title = 'Smart Locker Dashboard';
    $('alertBanner').hidden = true;
    setMsg($('cmdMsg'), '', '');
  }
  async function poll() {
    if (!running) return;
    try {
      const feeds = await source.feeds();
      if (!running) return;
      entries = feeds.map(norm).filter((e) => e.state != null && !Number.isNaN(e.t.getTime())).sort((a, b) => a.id - b.id);
      lastError = null;
      renderAll();
    } catch (e) {
      lastError = e.message || 'network error';
      renderPill();
    } finally {
      if (running) pollTimer = setTimeout(poll, POLL_MS);
    }
  }

  function renderPill() {
    const pill = $('linkPill'), text = pill.querySelector('span');
    const last = entries[entries.length - 1];
    if (lastError) { pill.className = 'pill error'; text.textContent = `Cannot reach ThingSpeak: ${lastError}`; return; }
    if (!last) { pill.className = 'pill'; text.textContent = 'Waiting for the first update'; return; }
    const age = Date.now() - last.t;
    if (age < OFFLINE_MS) { pill.className = 'pill online'; text.textContent = `Locker online · updated ${ago(age)}`; }
    else { pill.className = 'pill offline'; text.textContent = `Locker offline · last seen ${ago(age)}`; }
  }

  function renderAll() {
    renderPill();
    const last = entries[entries.length - 1];
    const card = $('stateCard');
    if (!last) {
      card.dataset.state = 'none';
      $('stateName').textContent = 'No data yet';
      $('stateDesc').textContent = 'The channel has no entries. Power on the locker and wait about 20 s.';
      $('armBtn').disabled = $('disarmBtn').disabled = true;
      return;
    }
    const online = Date.now() - last.t < OFFLINE_MS;
    const st = Math.min(3, Math.max(0, last.state | 0));
    const arming = st === 0 && last.armed === 1;
    card.dataset.state = arming ? 'arming' : String(st);
    $('stateName').textContent = arming ? 'Arming…' : STATES[st].name;
    $('stateDesc').textContent = (arming ? 'Exit delay: close the locker. It is armed 10 s after the command.' : STATES[st].desc) +
      (online ? '' : ' This is the last known state: the locker has stopped reporting.');
    $('armBtn').disabled = busy || last.armed === 1;
    $('disarmBtn').disabled = busy || last.armed !== 1;
    $('armBtn').className = 'btn big' + (last.armed === 1 ? '' : ' primary');
    $('disarmBtn').className = 'btn big' + (last.armed === 1 ? ' primary' : '');

    // tiles
    const tilt = $('tTilt');
    tilt.textContent = last.tilt ? 'Tilted' : 'Level';
    tilt.className = last.tilt ? 'bad' : 'ok';
    $('tTiltSub').textContent = last.tilt ? 'locker moved or lifted' : 'locker in place';
    $('tVib').textContent = last.vib == null ? '–' : String(last.vib);
    $('tAlarms').textContent = last.alarms == null ? '–' : String(last.alarms);
    const [mailText, mailCls] = MAIL_TEXT[last.mail] || ['–', ''];
    $('tMail').textContent = mailText;
    $('tMail').className = mailCls;
    $('tMailSub').textContent = last.mail === 4 ? 'set ALERTS_KEY in the firmware' : 'to the owner\'s email';

    renderAlarm(last, online);
    renderTimeline();
    renderHistory();
    checkPending();
  }

  function renderAlarm(last, online) {
    const banner = $('alertBanner');
    if (last.state < 2) {
      banner.hidden = true;
      stopSiren();
      document.title = 'Smart Locker Dashboard';
      return;
    }
    let i = entries.length - 1;
    while (i > 0 && entries[i - 1].state >= 2) i -= 1;
    const start = entries[i];
    const key = `${start.id}:${last.state}`;           // escalation from tamper to theft alerts again
    const theft = last.state >= 3;
    const title = theft ? 'Theft alert' : 'Tamper alert';
    const what = theft ? 'Locker tilted, lifted or moved' : 'Someone is forcing the locker';
    const mail = (MAIL_TEXT[last.mail] || ['–'])[0].toLowerCase();
    banner.hidden = false;
    $('alertTitle').textContent = title;
    $('alertDetail').textContent = `${what} · since ${fmtWhen(start.t)} · email ${mail}` +
      (online ? ' · disarm with your PIN to stop the buzzer' : ' · locker offline, last known state');
    document.title = `(!) ${title} - Smart Locker`;

    const silenced = store.get('ack') === key;
    banner.classList.toggle('quiet', silenced);
    $('silenceBtn').hidden = silenced;
    if (silenced) stopSiren(); else startSiren(theft ? 3 : 2);
    if (store.get('notified') !== key) {
      store.set('notified', key);
      notify(`${title}: Smart Locker`, `${what} at ${fmtTime(start.t)}.`);
    }
  }

  function renderTimeline() {
    const box = $('timeline');
    box.replaceChildren();
    entries.forEach((e, i) => {
      const c = el('span', `cell s${Math.min(3, Math.max(0, e.state | 0))}`);
      c.dataset.i = String(i);
      box.appendChild(c);
    });
    if (entries.length) {
      $('timelineRange').textContent = `${fmtWhen(entries[0].t)} to ${fmtWhen(entries[entries.length - 1].t)} · ${entries.length} updates`;
      const counts = [0, 0, 0, 0];
      entries.forEach((e) => { counts[Math.min(3, Math.max(0, e.state | 0))] += 1; });
      box.setAttribute('aria-label', `Locker state over the last ${entries.length} updates: ` +
        STATES.map((s, k) => `${counts[k]} ${s.short.toLowerCase()}`).join(', '));
    }
  }
  function timelineHover(ev) {
    const tip = $('tip');
    const cell = ev.target.closest('.cell');
    if (!cell) { tip.hidden = true; return; }
    const e = entries[Number(cell.dataset.i)];
    if (!e) { tip.hidden = true; return; }
    const st = STATES[Math.min(3, Math.max(0, e.state | 0))].short;
    tip.textContent = `${fmtWhen(e.t)} · ${e.state === 0 && e.armed === 1 ? 'Arming' : st}${e.tilt ? ' · tilted' : ''}${e.vib ? ` · ${e.vib} vibration${e.vib > 1 ? 's' : ''}` : ''}`;
    const cardBox = cell.closest('.card').getBoundingClientRect();
    const r = cell.getBoundingClientRect();
    tip.hidden = false;
    const half = tip.offsetWidth / 2 + 8;
    const x = Math.min(Math.max(r.left + r.width / 2 - cardBox.left, half), cardBox.width - half);
    tip.style.left = `${x}px`;
    tip.style.top = `${r.top - cardBox.top - 8}px`;
  }

  function listItem(when, whatNode) {
    const li = el('li');
    li.append(el('span', 'when', fmtWhen(when)));
    const w = el('span', 'what');
    w.append(whatNode);
    li.append(w);
    return li;
  }
  function renderHistory() {
    const alarms = [], acts = [];
    let prev = null;
    for (const e of entries) {
      if (e.state >= 2 && (!prev || prev.state < 2 || e.state > prev.state)) {
        const tag = el('span', `tag s${e.state >= 3 ? 3 : 2}`);
        tag.append(el('i'), document.createTextNode(e.state >= 3 ? 'Theft' : 'Tamper'));
        const frag = document.createDocumentFragment();
        frag.append(tag, document.createTextNode(e.state >= 3
          ? (prev && prev.state === 2 ? ' · escalated: locker lifted or tilted' : ' · locker lifted or tilted')
          : ` · forcing detected (${e.vib || 0} vibrations)`));
        alarms.push(listItem(e.t, frag));
      } else if (prev && prev.state >= 2 && e.state < 2) {
        alarms.push(listItem(e.t, document.createTextNode('Alarm cleared · system disarmed')));
      }
      if (e.status && (!prev || e.status !== prev.status)) acts.push(listItem(e.t, document.createTextNode(e.status)));
      prev = e;
    }
    const fill = (id, items, emptyText) => {
      const ul = $(id);
      ul.replaceChildren();
      if (!items.length) { ul.append(el('li', 'empty', emptyText)); return; }
      items.reverse().slice(0, 40).forEach((li) => ul.append(li));
    };
    fill('alarmList', alarms, 'No alarms in the recent history.');
    fill('activityList', acts, 'No activity in the recent history.');
  }

  // ---------------------------------------------------------------- commands
  function checkPending() {
    if (!pending) return;
    const msg = $('cmdMsg');
    const hit = entries.find((e) => e.cmdId === pending.id);
    if (hit) {
      const [text, kind] = CMD_TEXT[hit.cmd] || [`The locker replied with code ${hit.cmd}.`, 'bad'];
      setMsg(msg, text, kind);
      pending = null;
      return;
    }
    const waited = Date.now() - pending.at, secs = Math.round(waited / 1000);
    if (waited > CMD_GIVEUP_MS) {
      setMsg(msg, 'No reply from the locker. It may be offline; the locker ignores commands older than 2 minutes.', 'bad');
      pending = null;
    } else if (waited > CMD_SLOW_MS) {
      setMsg(msg, `Still waiting for the locker (${secs} s). Check that it is powered and on Wi-Fi.`, 'wait');
    } else {
      setMsg(msg, `${pending.verb === 'ARM' ? 'Arm' : 'Disarm'} command sent. Waiting for the locker to confirm (${secs} s, usually under 20 s)…`, 'wait');
    }
  }
  async function send(verb, pin) {
    busy = true;
    renderAll();
    setMsg($('cmdMsg'), 'Sending…', 'wait');
    try {
      const id = await source.command(`${verb} ${pin}`);
      pending = { id, verb, at: Date.now() };
      checkPending();
    } catch (e) {
      pending = null;
      setMsg($('cmdMsg'), `Could not send the command: ${e.message}.`, 'bad');
    } finally {
      busy = false;
      if (running) renderAll();
    }
  }
  function openModal(verb) {
    if (lockedFor() > 0) { logout('Too many wrong PINs.'); return; }
    modalVerb = verb;
    $('pinTitle').textContent = verb === 'ARM' ? 'Arm the locker' : 'Disarm the locker';
    $('pinSub').textContent = verb === 'ARM'
      ? 'Enter your PIN. The locker arms 10 s after it receives the command.'
      : 'Enter your PIN to switch the alarm off.';
    setMsg($('modalMsg'), '', '');
    modalPad.clear();
    $('pinModal').hidden = false;
  }
  function closeModal() { $('pinModal').hidden = true; modalVerb = null; }
  function modalDone(pin) {
    if (!modalVerb) return;
    if (checkPin(pin)) {
      passPin();
      const verb = modalVerb;
      closeModal();
      send(verb, pin);
      return;
    }
    const g = failPin();
    modalPad.shake();
    if (lockedFor() > 0) logout('Too many wrong PINs. The dashboard is locked.');
    else setMsg($('modalMsg'), `Wrong PIN. ${MAX_TRIES - g.fails} ${MAX_TRIES - g.fails === 1 ? 'try' : 'tries'} left.`, 'bad');
  }

  // ---------------------------------------------------------------- wiring
  function init() {
    const theme = params.get('theme');                   // ?theme=light or ?theme=dark overrides the OS setting
    if (theme === 'light' || theme === 'dark') document.documentElement.dataset.theme = theme;
    loginPad = keypad($('loginPad'), $('loginDots'), tryLogin);
    modalPad = keypad($('modalPad'), $('modalDots'), modalDone);

    $('setupForm').addEventListener('submit', saveSetup);
    $('testBtn').addEventListener('click', testConnection);
    $('setupBack').addEventListener('click', enterDash);
    $('resetBtn').addEventListener('click', () => {
      if (confirm('Forget the ThingSpeak keys and the PIN saved in this browser?')) resetAll();
    });
    $('forgotBtn').addEventListener('click', () => {
      if (DEMO) { setMsg($('loginMsg'), 'The demo PIN is 1234.', 'wait'); return; }
      if (confirm('Reset this dashboard? You will need to enter the ThingSpeak keys and choose the PIN again. The locker itself keeps OWNER_PIN from the firmware.')) resetAll();
    });
    $('settingsBtn').addEventListener('click', () => { stopDash(); openSetup(false); });
    $('logoutBtn').addEventListener('click', () => logout('Dashboard locked.'));
    $('armBtn').addEventListener('click', () => openModal('ARM'));
    $('disarmBtn').addEventListener('click', () => openModal('DISARM'));
    $('modalCancel').addEventListener('click', closeModal);
    $('silenceBtn').addEventListener('click', () => {
      const last = entries[entries.length - 1];
      if (!last || last.state < 2) return;
      let i = entries.length - 1;
      while (i > 0 && entries[i - 1].state >= 2) i -= 1;
      store.set('ack', `${entries[i].id}:${last.state}`);
      renderAll();
    });
    $('notifyBtn').addEventListener('click', () => {
      if (canNotify()) Notification.requestPermission().then(updateNotifyBtn);
    });
    $('timeline').addEventListener('mousemove', timelineHover);
    $('timeline').addEventListener('mouseleave', () => { $('tip').hidden = true; });
    $('demoPanel').addEventListener('click', (ev) => {
      const b = ev.target.closest('[data-sim]');
      if (!b || !sim) return;
      const online = sim.act(b.dataset.sim);
      if (b.dataset.sim === 'wifi') b.textContent = online ? 'Wi-Fi on / off' : 'Wi-Fi is OFF (click to restore)';
    });

    document.addEventListener('keydown', (ev) => {
      const modalOpen = !$('pinModal').hidden;
      const pad = modalOpen ? modalPad : (!$('loginView').hidden ? loginPad : null);
      if (ev.key === 'Escape' && modalOpen) { closeModal(); return; }
      if (!pad) return;
      if (/^\d$/.test(ev.key)) pad.press(ev.key);
      else if (ev.key === 'Backspace') pad.press('<');
    });

    // keep the session alive while the owner is using the page
    let lastTouch = 0;
    const touch = () => {
      if (Date.now() - lastTouch < 5000 || $('dashView').hidden || !sessionOk()) return;
      lastTouch = Date.now();
      startSession();
    };
    ['click', 'keydown', 'pointermove', 'touchstart'].forEach((t) => document.addEventListener(t, touch, { passive: true }));

    setInterval(() => {
      if (!$('dashView').hidden) {
        if (!sessionOk()) { logout('Locked after 10 minutes without activity.'); return; }
        renderPill();
        checkPending();
      }
      if (!$('loginView').hidden) refreshLock();
    }, 1000);

    if (DEMO) {
      $('demoBar').hidden = false;
      $('demoLink').hidden = true;
      sim = Simulator();
      settings = { demo: true, salt: 'demo', pinHash: hashPin('demo', '1234') };
    } else {
      settings = store.get('settings');
    }
    if (!settingsComplete(settings)) { openSetup(true); return; }

    const scene = DEMO ? params.get('scene') : null;     // demo shortcuts for screenshots
    if (scene && scene !== 'login') {
      startSession();
      enterDash();
      if (scene === 'theft' || scene === 'tamper') setTimeout(() => sim.act(scene), 300);
      return;
    }
    if (sessionOk()) enterDash(); else showLogin();
  }

  init();
})();
