/*
 * THE SESSION PAGE: what every game's page between runtimes does the same --
 * the terminal and the status line, the runtime and its calls, who you are,
 * hosting, joining and leaving (NetworkProvider/scripts/session_*.etcs filled
 * in and run), the hall, the lobby's settings, and the boot. A game's page
 * (KartProvider's, GolfProvider's) is its own HTML and a few hooks:
 *
 *   EtcsSessionPage({
 *     game:    'battle',            the boot's global the page drives
 *     kind:    'kart',              what the hall lists it under
 *     noun:    'battle',            what one is called, in the page's words
 *     nameKey: 'etcs-kart-name',    where this tab keeps its name
 *     type:    'KartProvider::KartBattle'   the game's type: a guest's surface of
 *                                   the host's game is one
 *     status(text)  -> { mode, slot, phase, next, secs, seeded, round, seat }
 *                      the game's Status read: mode host|guest|practice|idle,
 *                      phase lobby|... (lobby is the only one settings move in)
 *     scores(text, api)             the game's Scores drawn into #scores
 *     poll(api)                     anything else to ask each half second
 *     ready(api)                    once, when the runtime first answers
 *   })
 *
 * The page's elements, by id: status canvas shell name host leave link turn
 * seats round note scores no-scores scores-title map length reroll start
 * abandon who-sets hall no-hall bootthrob, and if the game has a look (its
 * Livery, Rename, Recolour) look shown-as look-note. The canvas is the runtime's (a
 * Window's surface); what is HTML is what a window has no DOM for.
 */
window.EtcsSessionPage = function (cfg) {
  const $ = function (id) { return document.getElementById(id); };
  const statusEl = $('status'), canvas = $('canvas'), shellFrame = $('shell');
  const ORIGIN = window.location.origin;
  const QS = new URLSearchParams(window.location.search);
  const GAME = cfg.game, KIND = cfg.kind, NOUN = cfg.noun || 'game', TYPE = cfg.type;

  // ── the log and the status line ─────────────────────────────────────────
  const ANSI_STRIP = /\x1b\[[0-9;?]*[A-Za-z]/g;
  const STATUS_TTL_MS = 6000;
  let statusTimer = 0;
  function setStatus(s, cls, ttl) {
    statusEl.textContent = s; statusEl.className = cls || '';
    if (statusTimer) { clearTimeout(statusTimer); statusTimer = 0; }
    if (ttl && s) statusTimer = setTimeout(function () { statusTimer = 0; if (statusEl.textContent === s) setStatus('', ''); }, ttl);
  }
  function log(text, cls) {
    termWrite(text);
    if (cls === 'err' || cls === 'ok') setStatus(String(text).replace(ANSI_STRIP, '').trim().split('\n')[0], cls, STATUS_TTL_MS);
  }
  let shellReady = false;
  const pending = [];
  function toShell(msg) { if (shellFrame.contentWindow) shellFrame.contentWindow.postMessage(msg, ORIGIN); }
  function termWrite(text) {
    if (!shellReady) { pending.push(text); if (pending.length > 2000) pending.shift(); return; }
    toShell({ etcs: 'out', text: text });
  }
  window.etcsTermWrite = function (t) { if (t != null) termWrite(String(t)); };
  // Where the pictures are drawn, and why when it is the CPU (RenderProvider's
  // WebGpuJs.h says, once each time it changes): the status line and the terminal.
  window.etcsGpuNote = function (text, onGpu) { log('[etcs] ' + text + '\n', onGpu ? 'ok' : 'err'); };
  const SHELL_DRAIN_MS = 250;
  let shellDrainTimer = 0, lastMenuJson = '';
  function drainShellOutput() {
    const mod = window.Module;
    if (!mod || !mod.ccall) return;
    let text = '';
    try { text = mod.ccall('etcs_web_shell_drain', 'string', [], []); } catch (e) { return; }
    if (text) termWrite(text);
    let menu = '';
    try { menu = mod.ccall('etcs_web_shell_menu', 'string', [], []); } catch (e) { menu = ''; }
    if (menu && menu !== lastMenuJson) { lastMenuJson = menu; toShell({ etcs: 'menu', json: menu }); }
    takeNotice(mod);
  }

  /*
   * A NOTICE FROM THE RUNTIME (etcs_web_notice, core/CommandExecutor.h): a
   * re-sync, said above the ETCS loading mark while the mark is up, in the
   * ring's blue and violet, fading after 8s. With the mark gone the terminal
   * has the same line, in colour, and nothing more is shown. Its look is here
   * rather than in each game's CSS, so every session page has the same one.
   */
  let noticeSerial = 0;
  function takeNotice(mod) {
    let json = '';
    try { json = mod.ccall('etcs_web_notice', 'string', [], []); } catch (e) { return; }
    if (!json) return;
    let n = null;
    try { n = JSON.parse(json); } catch (e) { return; }
    if (!n || n.serial === noticeSerial) return;
    noticeSerial = n.serial;
    const mark = $('bootthrob');
    if (!mark || !mark.parentNode) return;
    if (!$('bootnotice-style')) {
      const st = document.createElement('style');
      st.id = 'bootnotice-style';
      st.textContent = '#bootnotice { position: absolute; left: 50%; bottom: calc(50% + 60px); transform: translateX(-50%);'
        + ' max-width: 80%; padding: 8px 12px; z-index: 6; pointer-events: none; background: rgba(27, 28, 20, 0.92);'
        + ' color: rgb(240, 227, 199); border: 1px solid rgb(100, 0, 228); box-shadow: 0 0 0 1px rgba(0, 0, 255, 0.35);'
        + ' border-radius: 6px; font-size: 13px; line-height: 1.35; text-align: center; transition: opacity 0.6s; opacity: 1; }'
        + ' #bootnotice.gone { opacity: 0; }';
      document.head.appendChild(st);
    }
    let el = $('bootnotice');
    if (!el) { el = document.createElement('div'); el.id = 'bootnotice'; mark.parentNode.appendChild(el); }
    el.textContent = String(n.text || '');
    el.classList.remove('gone');
    clearTimeout(el.hideTimer);
    el.hideTimer = setTimeout(function () { el.classList.add('gone'); }, 8000);
  }
  function startShellDrain() { if (!shellDrainTimer) shellDrainTimer = setInterval(drainShellOutput, SHELL_DRAIN_MS); }
  function pushLine(line) {
    const mod = window.Module;
    if (!mod) { log('[etcs] line dropped, runtime not up: ' + line + '\n', 'err'); return; }
    try { mod.ccall('etcs_web_shell_push_line', null, ['string'], [line]); }
    catch (e) { log('[etcs] push_line failed: ' + e + '\n', 'err'); }
  }
  window.addEventListener('message', function (ev) {
    if (ev.origin !== ORIGIN || ev.source !== shellFrame.contentWindow) return;
    const m = ev.data;
    if (!m || typeof m !== 'object') return;
    if (m.etcs === 'view-ready') {
      shellReady = true;
      for (const t of pending) toShell({ etcs: 'out', text: t });
      pending.length = 0;
      toShell({ etcs: 'enable', enabled: !!(window.Module && window.Module.calledRun) });
      if (lastMenuJson) toShell({ etcs: 'menu', json: lastMenuJson });
      return;
    }
    if (m.etcs === 'line') { pushLine(String(m.line == null ? '' : m.line)); return; }
  });

  // ── the runtime ─────────────────────────────────────────────────────────
  function etcsFS() { return (typeof FS !== 'undefined') ? FS : (window.Module && window.Module.FS); }
  let runtimeIsUp = false, sceneUp = false;
  function runtimeUp() { return runtimeIsUp && !!(window.Module && window.Module.ccall); }
  // A call that answers, off the main thread -- the thread GLFW delivers every
  // press on, and the one a verb that waits on the link must not hold.
  let __seq = 1; const __pending = new Map();
  window.__etcs_call_done = function (handle, ok, text) {
    const p = __pending.get(handle); if (!p) return;
    __pending.delete(handle); p.resolve(ok ? text : null);
  };
  // An answer that never comes is no answer after a while: the poll waits on
  // these, and one lost would stop it for good.
  const CALL_WAIT_MS = 10000;
  function callResult(name, work, args) {
    if (!runtimeUp()) return Promise.resolve(null);
    const handle = __seq++;
    return new Promise(function (resolve) {
      __pending.set(handle, { resolve: resolve });
      setTimeout(function () { if (__pending.delete(handle)) resolve(null); }, CALL_WAIT_MS);
      window.Module.ccall('etcs_web_call_async', null, ['number', 'string', 'string', 'string'], [handle, name, work, args == null ? '' : args]);
    });
  }
  function ask(work, args) { return callResult(GAME, work, args); }
  function nameLive(name) {
    try { return !!window.Module.ccall('etcs_web_has', 'number', ['string'], [name]); } catch (e) { return false; }
  }
  canvas.addEventListener('contextmenu', function (ev) { ev.preventDefault(); });

  // ── who you are, where the hub is ───────────────────────────────────────
  function hubUrl() {
    const given = QS.get('hub');
    if (given) return given.replace(/\/+$/, '');
    return window.location.origin.replace(/^http/, 'ws') + '/link';
  }
  const NAME_A = ['amber', 'brisk', 'calm', 'deep', 'eager', 'fern', 'gold', 'hazy', 'ivory', 'jade', 'keen', 'lunar',
                  'mossy', 'north', 'opal', 'plum', 'quiet', 'rusty', 'sly', 'tidal', 'umber', 'vivid', 'wild', 'zesty'];
  const NAME_B = ['otter', 'heron', 'lynx', 'wren', 'moth', 'finch', 'badger', 'newt', 'crane', 'fox', 'marten', 'owl',
                  'raven', 'seal', 'stoat', 'swift', 'toad', 'vole', 'yak', 'gecko', 'ibis', 'koi', 'lark', 'mole'];
  // A name the hall knows this page by, kept across reloads (the room refuses
  // a name already linked, so a page coming back as itself is the point).
  function myName() {
    try { const kept = window.localStorage.getItem(cfg.nameKey); if (kept) return kept; } catch (e) {}
    const pick = function (a) { return a[Math.floor(Math.random() * a.length)]; };
    const made = pick(NAME_A) + '-' + pick(NAME_B) + '-' + Math.floor(Math.random() * 90 + 10);
    keepName(made); return made;
  }
  function keepName(n) { try { window.localStorage.setItem(cfg.nameKey, n); } catch (e) {} }
  const nameEl = $('name');
  nameEl.value = myName();
  nameEl.addEventListener('change', function () {
    const n = nameEl.value.trim().replace(/[^A-Za-z0-9_-]/g, '').slice(0, 24);
    if (!n) { nameEl.value = myName(); return; }
    keepName(n); nameEl.value = n;
    log('[etcs] you are ' + n + ' from the next ' + NOUN + ' on\n', 'ok');
  });
  function mintId() {
    const b = new Uint8Array(8); window.crypto.getRandomValues(b);
    return Array.from(b, function (x) { return x.toString(16).padStart(2, '0'); }).join('');
  }

  /*
   * A TEMPLATE, FILLED IN AND RUN (ChessProvider's page, the same): the
   * staged script read out of the runtime's filesystem, every {{field}}
   * replaced, written under a fresh name and pushed to the shell as a line.
   * `inject` hands the boot's globals in (game=battle ...), which a later
   * root script cannot name.
   */
  let templateSeq = 0;
  function runTemplate(path, fields, inject) {
    const fs = etcsFS();
    const name = path.slice(path.lastIndexOf('/') + 1);
    let text = '';
    try { text = new TextDecoder().decode(fs.readFile('/' + path)); }
    catch (e) { log('[etcs] ' + path + ' is not staged -- ' + e + '\n', 'err'); return false; }
    for (const k in fields) text = text.split('{{' + k + '}}').join(String(fields[k]));
    const out = '/run_' + name.replace(/\.etcs$/, '') + '_' + (++templateSeq) + '.etcs';
    fs.writeFile(out, new TextEncoder().encode(text));
    pushLine(out + (inject ? ' ' + inject : ''));
    return true;
  }

  // ── the session ─────────────────────────────────────────────────────────
  //
  // THE TEMPLATES THE OS RUNS: hosting is session_host.etcs filled in,
  // joining is session_join.etcs, and the hall is the site's --
  // chess_hall.etcs, run once when the runtime is up and mirrored here. Out of
  // a session the game is your own: practice, alone.
  const session = { n: 0, mode: null, id: null, owner: null, tick: 0, beating: false, hallN: 0, hallUp: false, practising: false };
  const hostBtn = $('host'), leaveBtn = $('leave'), linkEl = $('link');
  const SESSION = 'NetworkProvider/scripts/session_';

  function beat() {
    if (session.beating || !session.mode) return;
    session.beating = true;
    ask('Tick', '').then(function () { session.beating = false; });
  }
  function practise() {
    session.practising = true;
    ask('Practice', myName());
  }
  function openHall() {
    if (session.hallUp || !runtimeUp() || !nameLive(GAME)) return;
    if (!session.practising && !session.mode) practise();
    session.hallN = 0;
    if (!runTemplate('ChessProvider/scripts/chess_hall.etcs', { n: 0, name: myName(), hub: hubUrl() }, '')) return;
    session.hallUp = true;
    log('[etcs] listing the hall at ' + hubUrl() + '/hall as ' + myName() + '\n');
  }
  function host() {
    if (session.mode || !session.hallUp) return;
    session.n += 1; session.mode = 'hosting'; session.id = mintId(); session.owner = myName();
    if (!runTemplate(SESSION + 'host.etcs', { n: session.n, name: myName(), id: session.id, hub: hubUrl() },
                     'game=' + GAME + ' hall=halllist' + session.hallN)) { session.mode = null; return; }
    session.tick = setInterval(beat, 1000);
    linkEl.textContent = linkFor(session.id, session.owner); linkEl.classList.remove('hidden');
    hostBtn.classList.add('hidden'); leaveBtn.classList.remove('hidden');
    log('[etcs] your ' + NOUN + ' is open: ' + linkFor(session.id, session.owner) + '\n', 'ok');
    canvas.focus();
  }
  function join(id, owner) {
    if (session.mode) return;
    session.n += 1; session.mode = 'joined'; session.id = id; session.owner = owner;
    if (!runTemplate(SESSION + 'join.etcs', { n: session.n, name: myName(), id: id, owner: owner, hub: hubUrl(), type: TYPE },
                     'game=' + GAME)) { session.mode = null; return; }
    session.tick = setInterval(beat, 1000);
    hostBtn.classList.add('hidden'); leaveBtn.classList.remove('hidden');
    log('[etcs] in ' + owner + "'s " + NOUN + ' as ' + myName() + '\n', 'ok');
    canvas.focus();
  }
  function leave() {
    if (!session.mode) return;
    // Launched with the names handed in, as host and join are: a `run` line
    // of a later root script cannot name the boot's globals.
    if (session.mode === 'hosting') runTemplate(SESSION + 'close.etcs', {}, 'game=' + GAME + ' room=room' + session.n);
    else                            runTemplate(SESSION + 'leave.etcs', {}, 'game=' + GAME + ' far=far' + session.n + ' rfar=rfar' + session.n + ' peer=peer' + session.n);
    clearInterval(session.tick); session.tick = 0;
    session.mode = null; session.id = null; session.owner = null;
    linkEl.classList.add('hidden'); hostBtn.classList.remove('hidden'); leaveBtn.classList.add('hidden');
    setTimeout(practise, 300);
  }
  function linkFor(id, owner) { return window.location.origin + window.location.pathname + '?s=' + id + '&owner=' + encodeURIComponent(owner); }
  hostBtn.addEventListener('click', host);
  leaveBtn.addEventListener('click', leave);
  linkEl.addEventListener('click', function () {
    navigator.clipboard.writeText(linkEl.textContent).then(function () {
      linkEl.classList.add('copied'); setTimeout(function () { linkEl.classList.remove('copied'); }, 1200);
    }).catch(function () {});
  });
  // The keys are the window's (GLFW listens on the page): a click on the
  // canvas is how you say you are playing rather than typing your name. The
  // keys a game uses may not move the focus off it or scroll the page.
  canvas.addEventListener('mousedown', function () { canvas.focus(); });
  const KEEP = { 'Tab': 1, ' ': 1, 'ArrowUp': 1, 'ArrowDown': 1, 'ArrowLeft': 1, 'ArrowRight': 1 };
  canvas.addEventListener('keydown', function (ev) { if (KEEP[ev.key]) ev.preventDefault(); });

  // ── what the page shows, polled ─────────────────────────────────────────
  const turnEl = $('turn'), seatsEl = $('seats'), roundEl = $('round'), noteEl = $('note'), hallEl = $('hall');
  const mapEl = $('map'), lengthEl = $('length'), startBtn = $('start'), whoSetsEl = $('who-sets');
  const abandonBtn = $('abandon'), rerollBtn = $('reroll');
  let shownScores = '', shownHall = '', shownMaps = '', polling = false, authority = false, readied = false;
  function esc(s) { return String(s).replace(/[&<>]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]; }); }
  function clock(secs) { return Math.floor(secs / 60) + ':' + String(secs % 60).padStart(2, '0'); }
  const api = { $: $, log: log, setStatus: setStatus, ask: ask, callResult: callResult, nameLive: nameLive, myName: myName,
                esc: esc, clock: clock, session: session, runtimeUp: runtimeUp, canvas: canvas };

  async function poll() {
    if (polling || !runtimeUp() || !nameLive(GAME)) return;
    polling = true;
    try {
      if (!readied) { readied = true; if (cfg.ready) cfg.ready(api); }
      const st = await ask('Status', '');
      if (st) {
        sceneUp = true;
        const v = cfg.status(st.trim());
        turnEl.textContent = v.mode === 'host' ? 'hosting a ' + NOUN : v.mode === 'guest' ? 'in ' + session.owner + "'s " + NOUN
                           : v.mode === 'practice' ? 'practising, alone' : '—';
        seatsEl.textContent = v.seat || (session.mode ? 'watching -- every place is taken' : '');
        roundEl.textContent = v.round || '';
        roundEl.className = v.phase !== 'lobby' ? 'playing' : '';
        renderSettings(v.mode === 'host' || v.mode === 'practice', v.phase, v.next, v.secs, v.seeded);
        phaseNow = v.phase;
        const title = $('scores-title');
        if (title) title.textContent = v.phase === 'lobby' ? 'Last round' : 'Scores';
      }
      const note = await ask('Note', '');
      if (note != null) noteEl.textContent = note;
      const scores = await ask('Scores', '');
      if (scores != null && scores !== shownScores) { shownScores = scores; cfg.scores(scores, api); }
      if (lookEl) { const lv = await ask('Livery', ''); if (lv != null) renderLook(lv.trim()); }
      // Every poll, not once: a page asks before its boot has made every map.
      const maps = await ask('Maps', '');
      if (maps && maps !== shownMaps) { shownMaps = maps; renderMaps(maps); }
      if (cfg.poll) await cfg.poll(api);
      if (session.hallUp) {
        const list = await callResult('hall' + session.hallN, 'List', '');
        if (list != null && list !== shownHall) { shownHall = list; renderHall(list); }
      }
    } finally { polling = false; }
  }
  /*
   * THE LOBBY'S SETTINGS: the host's (or yours alone, practising), each a line
   * of the trace (`config`, `round`, `abandon`), so a guest sees them change
   * and cannot change them -- and nobody can while a round runs. A select
   * being used is not overwritten by a poll.
   */
  function renderMaps(text) {
    const keep = mapEl.value;
    mapEl.innerHTML = '';
    for (const m of text.split('\n').map(function (l) { return l.trim(); }).filter(Boolean)) {
      const o = document.createElement('option'); o.value = m; o.textContent = m; mapEl.appendChild(o);
    }
    if (keep) mapEl.value = keep;
  }
  function renderSettings(deciding, phase, next, secs, seeded) {
    authority = deciding;
    const open = deciding && phase === 'lobby';
    mapEl.disabled = lengthEl.disabled = startBtn.disabled = rerollBtn.disabled = !open;
    rerollBtn.classList.toggle('hidden', !seeded);
    startBtn.classList.toggle('hidden', phase !== 'lobby');
    abandonBtn.classList.toggle('hidden', !(deciding && phase !== 'lobby'));
    whoSetsEl.textContent = phase !== 'lobby' ? 'locked until the round is over' : deciding ? 'yours to set' : 'the host sets them';
    if (document.activeElement !== mapEl && next && mapEl.value !== next) mapEl.value = next;
    if (document.activeElement !== lengthEl && secs) {
      if (!Array.from(lengthEl.options).some(function (o) { return o.value === secs; })) {
        const o = document.createElement('option'); o.value = secs; o.textContent = clock(parseInt(secs, 10)); lengthEl.appendChild(o);
      }
      lengthEl.value = secs;
    }
  }
  function configure() {
    if (!authority || !mapEl.value) return;
    ask('Configure', mapEl.value + ' ' + lengthEl.value).then(function (r) {
      if (r !== 'ok') log('[etcs] the settings were refused\n', 'err');
    });
    canvas.focus();
  }
  mapEl.addEventListener('change', configure);
  lengthEl.addEventListener('change', configure);
  startBtn.addEventListener('click', function () { if (authority) ask('StartRound', ''); canvas.focus(); });
  abandonBtn.addEventListener('click', function () { if (authority) ask('Abandon', ''); canvas.focus(); });
  rerollBtn.addEventListener('click', function () { if (authority) ask('Reroll', ''); canvas.focus(); });

  /*
   * YOUR LOOK: your colour and the name shown for you, each a `look` line of
   * the trace -- the same line the canvas's menu says, so changing either
   * shows in both: neither one is where the state is. Anyone seated sets
   * their own, in the lobby only. "<colour> <name> <rrggbb,...>" or "-".
   */
  const lookEl = $('look'), shownAsEl = $('shown-as'), lookNoteEl = $('look-note');
  let phaseNow = '', shownLook = '';
  function renderLook(text) {
    const open = phaseNow === 'lobby' && text !== '-';
    if (text !== shownLook) {
      shownLook = text;
      lookEl.innerHTML = '';
      const p = text === '-' ? [] : text.split(' ');
      const chosen = parseInt(p[0], 10), palette = p[2] ? p[2].split(',') : [];
      palette.forEach(function (hex, n) {
        const b = document.createElement('button');
        b.style.background = '#' + hex; b.className = n === chosen ? 'on' : ''; b.title = '#' + hex;
        b.onclick = function () { ask('Recolour', String(n)); canvas.focus(); };
        lookEl.appendChild(b);
      });
      if (shownAsEl && document.activeElement !== shownAsEl) shownAsEl.value = p[1] || '';
    }
    for (const b of lookEl.children) b.disabled = !open;
    if (shownAsEl) shownAsEl.disabled = !open;
    if (lookNoteEl) lookNoteEl.textContent = text === '-' ? 'take a place to choose a look'
      : open ? 'the canvas\'s menu says the same: change either' : 'locked until the round is over';
  }
  if (shownAsEl) shownAsEl.addEventListener('change', function () {
    const n = shownAsEl.value.trim().replace(/[^A-Za-z0-9_-]/g, '').slice(0, 16);
    ask('Rename', n).then(function (r) { if (r !== 'ok') log('[etcs] that name was refused\n', 'err'); shownLook = ''; });
    canvas.focus();
  });
  // "<name> <kind> <id> <players> <places>" per session of this kind. Mine is
  // marked; one with a place free is a join.
  function renderHall(list) {
    hallEl.innerHTML = '';
    let n = 0;
    for (const line of list.split('\n')) {
      const p = line.trim().split(/\s+/);
      if (p.length < 5 || p[1] !== KIND) continue;
      const [name, , id, players, places] = p;
      const tr = document.createElement('tr');
      tr.innerHTML = '<td class="key">' + esc(name + "'s " + NOUN) + '</td><td>' + esc(players + '/' + places) + '</td>';
      const td = document.createElement('td');
      if (name === myName()) td.textContent = 'you';
      else if (!session.mode) {
        const btn = document.createElement('button');
        btn.textContent = parseInt(players, 10) < parseInt(places, 10) ? 'join' : 'watch';
        btn.onclick = function () { join(id, name); };
        td.appendChild(btn);
      }
      tr.appendChild(td); hallEl.appendChild(tr); n++;
    }
    $('no-hall').classList.toggle('hidden', n > 0);
    landFromQuery(list);
  }
  // What the address asked for, once the hall can answer it: ?s=<id>&owner=
  // joins that session; ?quick=1 joins the first with a place free, or hosts
  // one; ?host=1 hosts one.
  let landed = false;
  function landFromQuery(list) {
    if (landed || session.mode) return;
    const s = QS.get('s'), owner = QS.get('owner');
    if (s && owner) { landed = true; join(s, owner); return; }
    const rows = list.split('\n').map(function (l) { return l.trim().split(/\s+/); })
                     .filter(function (p) { return p.length >= 5 && p[1] === KIND; });
    if (QS.get('quick') === '1') {
      landed = true;
      const r = rows.find(function (p) { return p[0] !== myName() && parseInt(p[3], 10) < parseInt(p[4], 10); });
      if (r) join(r[2], r[0]); else host();
      return;
    }
    if (QS.get('host') === '1') { landed = true; host(); }
  }
  // An empty hall answers nothing (renderHall runs on a change), so an
  // address that hosts when there is nothing to join waits a few seconds for
  // the hall's first listing before deciding there is none.
  let hallSince = 0;
  function startPolling() {
    setInterval(function () {
      if (!session.hallUp) openHall();
      else if (!hallSince) hallSince = Date.now();
      else if (!landed && Date.now() - hallSince > 8000) landFromQuery(shownHall);
      poll();
    }, 500);
  }

  // ── the include list, the base, the boot ────────────────────────────────
  function normalizeWasmList(raw) {
    let arr = [];
    if (Array.isArray(raw)) arr = raw; else if (raw && Array.isArray(raw.modules)) arr = raw.modules; else if (raw && Array.isArray(raw.wasm)) arr = raw.wasm;
    return arr.map(function (x) {
      x = String(x).trim(); if (!x) return null;
      if (!/\.wasm$/i.test(x)) x += '.wasm';
      x = x.replace(/^\.\//, '');
      if (/^etcs(\.wasm)?$/i.test(x) || /worker/i.test(x)) return null;
      return x;
    }).filter(Boolean);
  }
  let man_module_count = 0;
  async function fetchManifest() {
    const r = await fetch('modules.json', { cache: 'no-store' });
    if (!r.ok) throw new Error('modules.json HTTP ' + r.status + ' -- required (no fallback list)');
    const j = await r.json();
    const wasm = normalizeWasmList(j);
    if (!wasm.length) throw new Error('modules.json names no modules');
    const strip = function (s) { return String(s).trim().replace(/^\.\//, ''); };
    const scripts = (j && Array.isArray(j.scripts) ? j.scripts : []).map(strip).filter(Boolean);
    const boot = (j && j.boot) ? strip(j.boot) : '';
    const glue = (j && j.glue) ? strip(j.glue) : '';
    const base = (j && j.base) ? String(j.base).trim() : '';
    log('[etcs] modules.json -> ' + wasm.join(', ') + '\n');
    if (scripts.length) log('[etcs] scripts -> ' + scripts.join(', ') + '\n');
    man_module_count = wasm.length;
    return { wasm: wasm, scripts: scripts, boot: boot, glue: glue, base: base };
  }
  const WASM_MAGIC = [0x00, 0x61, 0x73, 0x6d];
  function checkStaged(name, kind, buf) {
    if (kind === 'module') {
      const ok = buf.byteLength >= 4 && WASM_MAGIC.every(function (b, i) { return buf[i] === b; });
      if (!ok) throw new Error(name + ' is not a WebAssembly module -- the server answered 200 with something else');
      return;
    }
    let i = 0;
    while (i < buf.byteLength && (buf[i] === 32 || buf[i] === 9 || buf[i] === 10 || buf[i] === 13)) i++;
    if (i < buf.byteLength && buf[i] === 0x3c)
      throw new Error(name + ' is markup, not an .etcs script -- check what the page\'s mounts mount at that path (FileHtmlPage::ListPaths in the server log)');
  }
  let workerDeathExplained = false;
  function explainWorkerDeath(t) {
    if (workerDeathExplained || !/worker sent an error|could not load dynamic lib|ASM_CONSTS is undefined/.test(t)) return;
    workerDeathExplained = true;
    log('[etcs] A Worker died. This is browser memory, not the wasm heap: each Worker re-instantiates every open module, '
      + 'so the cost is workers x modules (' + (man_module_count || '?') + ' here). Any "index out of bounds" after this line is the consequence.\n', 'err');
  }
  const WASM_BASE_CANDIDATES = ['/wasm/', './', 'https://anticurrententropy.com/wasm/'];
  let wasm_base = null, wasm_probe = null;
  function joinBase(base, name) { return (base === './') ? name : base.replace(/\/*$/, '/') + name; }
  async function resolveWasmBase(declared, firstModule) {
    const candidates = declared ? [declared] : WASM_BASE_CANDIDATES, tried = [];
    for (const base of candidates) {
      const url = joinBase(base, firstModule);
      try {
        const r = await fetch(url, { cache: 'no-store' });
        if (!r.ok) { tried.push(base + ' HTTP ' + r.status); continue; }
        const buf = new Uint8Array(await r.arrayBuffer());
        checkStaged(firstModule, 'module', buf);
        wasm_base = base; wasm_probe = { name: firstModule, buf: buf };
        log('[etcs] wasm base -> ' + base + '\n', 'ok');
        return base;
      } catch (e) { tried.push(base + ' ' + (e && e.message ? e.message : e)); }
    }
    throw new Error('no wasm base serves ' + firstModule + ' -- tried ' + tried.join('; '));
  }
  async function fetchAll(names, kind, base) {
    const out = {};
    for (const name of names) {
      if (wasm_probe && wasm_probe.name === name) { out[name] = wasm_probe.buf; continue; }
      setStatus('fetch ' + name);
      const url = base ? joinBase(base, name) : name;
      const r = await fetch(url, { cache: 'no-store' });
      if (!r.ok) throw new Error('fetch ' + url + ' failed HTTP ' + r.status);
      const buf = new Uint8Array(await r.arrayBuffer());
      if (!buf.byteLength) throw new Error(url + ' is empty');
      checkStaged(name, kind, buf);
      out[name] = buf;
      log('[etcs] staged ' + name + ' (' + buf.byteLength + ' bytes)\n');
    }
    return out;
  }
  function loadGlue(src) {
    return new Promise(function (resolve, reject) {
      const s = document.createElement('script'); s.src = src; s.async = true;
      s.onload = resolve; s.onerror = reject; document.body.appendChild(s);
    });
  }
  async function boot() {
    setStatus('fetching…');
    const man = await fetchManifest();
    setStatus('locating runtime…');
    await resolveWasmBase(man.base, man.wasm[0]);
    const blobs = Object.assign({}, await fetchAll(man.wasm, 'module', wasm_base), await fetchAll(man.scripts, 'script', null));
    if (man.boot && !blobs[man.boot]) throw new Error('modules.json boot="' + man.boot + '" is not in its scripts list');
    var Module = {
      canvas: canvas,
      // argv[1] is the boot script: main() runs it and falls through to the
      // navigator, so the window and its pumps are up before the first prompt.
      arguments: man.boot ? [man.boot] : [],
      print: function (t) { window.etcsTermWrite(t + (String(t).endsWith('\n') ? '' : '\n')); },
      printErr: function (t) { log(t + (String(t).endsWith('\n') ? '' : '\n'), 'err'); explainWorkerDeath(String(t)); },
      dynamicLibraries: [],
      locateFile: function (path) { return joinBase(wasm_base, path); },
      preRun: [function () {
        try {
          for (const name of Object.keys(blobs)) {
            const slash = name.lastIndexOf('/');
            if (slash > 0) { try { FS.mkdirTree(name.slice(0, slash)); } catch (e) {} }
            try { FS.writeFile(name, blobs[name]); }
            catch (e) { FS.createDataFile('/', name.replace(/^\.\//, ''), blobs[name], true, true, true); }
          }
          log('[etcs] MEMFS preload complete (' + Object.keys(blobs).length + ' files)\n');
        } catch (e) { log('[etcs] preRun FS error: ' + e + '\n', 'err'); }
      }],
      onRuntimeInitialized: function () {
        setStatus('runtime ready', 'ok');
        runtimeIsUp = true;
        startShellDrain();
        startPolling();
        log('[etcs] runtime initialized; boot script: ' + (man.boot || '(none)') + '\n', 'ok');
        if (shellReady) toShell({ etcs: 'enable', enabled: true });
      },
      onAbort: function (r) { setStatus('aborted', 'err'); log('[abort] ' + r + '\n', 'err'); if (shellReady) toShell({ etcs: 'enable', enabled: false }); }
    };
    window.Module = Module;
    if (man.glue) { setStatus('loading ' + man.glue + '…'); await loadGlue(joinBase(wasm_base, man.glue)); }
    else { try { await loadGlue(joinBase(wasm_base, 'etcs.js')); } catch (_) { await loadGlue(joinBase(wasm_base, 'etcs')); } }
  }

  // WHAT IT LEAVES BEHIND, when its element names something (data-leave): the
  // canvas's own CSS background, which paints under the canvas's pixels. The
  // runtime's canvas is transparent until it first presents and opaque after,
  // so this fills the wait between the throbber going and the first frame, and
  // everything the runtime draws is over it.
  function leaveBehind(el, under) {
    const url = el.dataset.leave;
    if (!url || !under) return;
    under.style.background = 'url("' + url + '") center / auto 45% no-repeat';
  }
  // The boot throbber, page-side: the runtime's first frame lands seconds
  // after main() starts. Not by reading the canvas, as the chess page does: a
  // 2D context asked for before the runtime's WebGPU one takes the canvas from
  // the device. The game is up when it first answers (poll).
  function bootThrobber() {
    const el = $('bootthrob'); if (!el) return;
    const g = el.getContext('2d'); if (!g) { el.remove(); return; }
    const RING = el.width, DOTS = 12, TAIL = 0.45, STEP = 5.4 / 360;
    const HEAD = [0.35, 0.55, 0.95], TAILC = [0.58, 0.40, 0.95];
    const cx = RING / 2, cy = RING / 2, dotR = RING * 0.08, orbit = RING * 0.5 - dotR - RING / 24;
    let phase = 0, stopped = false;
    g.font = '600 16px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace'; g.textAlign = 'center'; g.textBaseline = 'middle';
    function frame() {
      if (stopped) return;
      g.clearRect(0, 0, el.width, el.height);
      g.fillStyle = 'rgba(240, 227, 199, 0.95)'; g.fillText('ETCS', cx + 1, cy + 2);
      for (let i = 0; i < DOTS; i++) {
        const at = i / DOTS; let d = phase - at; d = d - Math.floor(d); if (d > 0.5) d = 1 - d;
        let t = 1 - (d / TAIL); if (t < 0) t = 0; t = t * t;
        const a = 0.14 + (1 - 0.14) * t, c = HEAD.map(function (h, k) { return Math.round(255 * (TAILC[k] + (h - TAILC[k]) * t)); });
        const ang = at * Math.PI * 2 - Math.PI / 2;
        g.beginPath(); g.arc(cx + orbit * Math.cos(ang), cy + orbit * Math.sin(ang), dotR, 0, Math.PI * 2);
        g.fillStyle = 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + a.toFixed(3) + ')'; g.fill();
      }
      phase = (phase + STEP) % 1; requestAnimationFrame(frame);
    }
    requestAnimationFrame(frame);
    const p = setInterval(function () {
      if (!sceneUp) return;
      stopped = true; clearInterval(p); leaveBehind(el, canvas); el.remove(); log('[etcs] the ' + KIND + ' is up\n');
    }, 200);
  }
  bootThrobber();
  boot().catch(function (e) { setStatus('boot failed', 'err'); log('[etcs] boot: ' + e + '\n', 'err'); });
  return api;
};
