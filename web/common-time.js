// #FIELD-5: авто-синхронизация времени платы от браузера телефона.
// В полевом AP (Outdoor) интернета нет → SNTP не работает → плата стартует с
// near-epoch (1970). Любая открытая страница шлёт текущее время браузера на
// POST /api/time; прошивка сама решает, принимать ли (net_time_should_accept):
// при активном SNTP — откажет (война источников), иначе примет при расхождении
// > 5 с. manual=false → авто-режим (для явного ручного ввода — форма в /system).
// #MON-2: заодно передаёт пояс отображения (tz_min) — им пользуется экран платы.
// #CSRF-1 (апстрим 1.2.25): токен CSRF плата выдаёт заново при каждой загрузке. Вкладка,
// открытая до перезагрузки платы, слала команды со старым токеном — плата отвечала 403,
// а страница молчала (Стоп/Сброс/Сохранить «не работали» до F5). Обёртка fetch: на 403
// у запроса с X-CSRF-Token берёт новый токен (и кладёт его в глобальный csrfToken
// страницы) и повторяет запрос один раз. Файл подключён на всех страницах.
(function () {
  const orig = window.fetch;
  if (!orig || orig.__aswfCsrf) return;
  const isTok = k => k.toLowerCase() === 'x-csrf-token';
  function hasTok(h) {
    if (!h) return false;
    if (typeof Headers !== 'undefined' && h instanceof Headers) return h.has('X-CSRF-Token');
    return Object.keys(h).some(isTok);
  }
  function withTok(h, tok) {
    if (typeof Headers !== 'undefined' && h instanceof Headers) { const n = new Headers(h); n.set('X-CSRF-Token', tok); return n; }
    const o = {};
    Object.keys(h).forEach(k => { if (!isTok(k)) o[k] = h[k]; });
    o['X-CSRF-Token'] = tok;
    return o;
  }
  const wrapped = async function (input, init) {
    const r = await orig(input, init);
    if (r.status !== 403 || !init || !hasTok(init.headers)) return r;
    try {
      const t = await orig('/api/csrf-token').then(x => x.json());
      if (!t || !t.token) return r;
      try { if ('csrfToken' in window) window.csrfToken = t.token; } catch (e) {}
      return orig(input, Object.assign({}, init, { headers: withTok(init.headers, t.token) }));
    } catch (e) {
      return r;
    }
  };
  wrapped.__aswfCsrf = true;
  window.fetch = wrapped;
})();

(function () {
  function tzMin() {  // как tzOffMin() в monitor.html: aswf-tz ("auto" | минуты от UTC)
    let m = 'auto';
    try { m = localStorage.getItem('aswf-tz') || 'auto'; } catch (e) {}
    const o = parseInt(m, 10);
    return (m !== 'auto' && isFinite(o)) ? o : -new Date().getTimezoneOffset();
  }
  async function syncTime() {
    try {
      const t = await fetch('/api/csrf-token').then(r => r.json());
      const res = await fetch('/api/time', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': t.token },
        body: JSON.stringify({ epoch_ms: Date.now(), manual: false, tz_min: tzMin() })
      }).then(r => r.json());
      if (res && res.accepted)
        console.log('[time] синхронизировано от браузера, источник=' + res.source);
    } catch (e) {
      // Оффлайн или рано при загрузке — молча, авто-синк не должен мешать UI.
    }
  }
  window.aswfSyncTime = syncTime;   // system.html: сразу после смены пояса
  if (document.readyState === 'loading')
    document.addEventListener('DOMContentLoaded', syncTime);
  else
    syncTime();
  // Повтор раз в час с открытой вкладки: корректирует дрейф часов платы (нет RTC,
  // нет SNTP в поле) без перезагрузки страницы. Приём — по той же guard-логике.
  setInterval(syncTime, 3600000);
})();
