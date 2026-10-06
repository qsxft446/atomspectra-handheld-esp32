# AtomSpectra Web UI — дизайн-mock v2026-06-27

Статический комплект из 4 HTML-страниц для предпросмотра и обсуждения
визуального стиля будущего Web UI прошивки **AtomSpectra (ESP32-S3)**.

| Файл | Назначение |
|---|---|
| [`index.html`](index.html) | Главная — спектр (Sigma/Channels/Energy) |
| [`waterfall.html`](waterfall.html) | Водопад спектров во времени |
| [`saved.html`](saved.html) | Сохранённые на устройстве .n42 / .aswf |
| [`system.html`](system.html) | Системная страница (BLE/WiFi/Flash/uptime) |

## Публичный preview (без локального устройства)

Ссылки рендерят страницы напрямую из этого репо в любом браузере:

| Страница | raw.githack.com | jsDelivr CDN |
|---|---|---|
| Спектр | [Открыть](https://raw.githack.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/main/design/v2026-06-27/index.html) | [Открыть](https://cdn.jsdelivr.net/gh/VibeEngineering-LLC/atomspectra-waterfall-esp32@main/design/v2026-06-27/index.html) |
| Водопад | [Открыть](https://raw.githack.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/main/design/v2026-06-27/waterfall.html) | [Открыть](https://cdn.jsdelivr.net/gh/VibeEngineering-LLC/atomspectra-waterfall-esp32@main/design/v2026-06-27/waterfall.html) |
| Сохранённые | [Открыть](https://raw.githack.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/main/design/v2026-06-27/saved.html) | [Открыть](https://cdn.jsdelivr.net/gh/VibeEngineering-LLC/atomspectra-waterfall-esp32@main/design/v2026-06-27/saved.html) |
| Система | [Открыть](https://raw.githack.com/VibeEngineering-LLC/atomspectra-waterfall-esp32/main/design/v2026-06-27/system.html) | [Открыть](https://cdn.jsdelivr.net/gh/VibeEngineering-LLC/atomspectra-waterfall-esp32@main/design/v2026-06-27/system.html) |

`raw.githack.com` обновляется почти мгновенно после `git push`.
`jsDelivr` агрессивно кэширует — обновление в течение ~10 минут.

## Статус

**Mock, не прошивка.** Это статические HTML без подключения к реальному устройству:
все цифры/графики/чипы — фиксированные плейсхолдеры, кнопки и формы не работают.
Backend в текущей прошивке (см. корень репо) пока живёт по старому UI; этот
комплект — предложение по редизайну для обсуждения.

IP `192.168.x.x` в чипах — плейсхолдер; реальный IP подставляется
прошивкой на работающем устройстве.
