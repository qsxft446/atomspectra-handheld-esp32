#pragma once
// #ACQ-1 (перенос из апстрима 1.2.23, acq_intent.h/acq_watch.h): намерение набора по
// текстовой команде, ушедшей прибору, и распознавание команд сброса прибора.
// Свободно от ESP-IDF.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define ACQ_INTENT_UNKNOWN 0u   // чужой клиент (TCP-мост) или -sta с параметрами
#define ACQ_INTENT_RUN     1u   // плата сама послала голый -sta
#define ACQ_INTENT_STOP    2u   // -sto
#define ACQ_WATCH_MS       20000u   // нет гистограмм столько при RUN -> повторить -sta

static inline bool acq_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Голый -sta -> RUN; -sta с параметрами (таймер, тихий режим) -> UNKNOWN: сторож не
// должен превращать ограниченный набор в бесконечный; -sto -> STOP; прочее — как было.
static inline uint8_t acq_intent_for_cmd(const char *cmd, uint8_t cur)
{
    size_t n = strlen(cmd);
    while (n > 0 && acq_is_ws(cmd[n - 1])) n--;
    if (n < 4 || strncmp(cmd, "-st", 3) != 0) return cur;
    if (cmd[3] == 'o' && (n == 4 || acq_is_ws(cmd[4]))) return ACQ_INTENT_STOP;
    if (cmd[3] == 'a' && n == 4) return ACQ_INTENT_RUN;
    if (cmd[3] == 'a' && acq_is_ws(cmd[4])) return ACQ_INTENT_UNKNOWN;
    return cur;
}

// Команда обнуляет прибор: -rst или -sta ... -r (сброс перед стартом) в любой позиции.
static inline bool cmd_is_device_reset(const char *cmd)
{
    size_t n = strlen(cmd);
    while (n > 0 && acq_is_ws(cmd[n - 1])) n--;
    if (n == 4 && strncmp(cmd, "-rst", 4) == 0) return true;
    if (!(n >= 4 && strncmp(cmd, "-sta", 4) == 0 && (n == 4 || acq_is_ws(cmd[4])))) return false;
    size_t i = 4;
    while (i < n) {
        while (i < n && acq_is_ws(cmd[i])) i++;
        size_t start = i;
        while (i < n && !acq_is_ws(cmd[i])) i++;
        if (i - start == 2 && cmd[start] == '-' && cmd[start + 1] == 'r') return true;
    }
    return false;
}

// Только разности штампов (переполнение uint32 мс безопасно); гистограмма старше
// открытия CDC считается от открытия.
static inline bool acq_watch_resend_due(uint8_t intent, uint32_t now_ms, uint32_t open_ts_ms,
                                        uint32_t hist_ts_ms, uint32_t last_resend_ts_ms)
{
    if (intent != ACQ_INTENT_RUN) return false;
    uint32_t open_age = now_ms - open_ts_ms, hist_age = now_ms - hist_ts_ms;
    if (hist_age > open_age) hist_age = open_age;
    if (hist_age < ACQ_WATCH_MS) return false;
    return now_ms - last_resend_ts_ms >= ACQ_WATCH_MS;
}
