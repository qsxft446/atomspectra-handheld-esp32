#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
//  #REC-11-A2: автономная выгрузка завершённых сегментов водопада по HTTP POST.
//
//  Плата сама (push) отправляет каждый завершённый сегмент /storage/wf/seg_*.aswf
//  на адрес, заданный ОПЕРАТОРОМ в конфиге (NVS namespace "wfofl"), и удаляет его
//  с Flash только после подтверждённого 2xx-ответа приёмника. Координация с
//  кольцом keep-last — через пин сегмента в spectrogram.c (claim/done/release).
//
//  БАН Народмон (CLAUDE.md, HARD HOLD): аплоадер НЕ имеет дефолтного адреса и
//  шлёт ИСКЛЮЧИТЕЛЬНО на url из конфига. Конфиг с host'ом, содержащим
//  "narodmon", отвергается (wf_offload_set_cfg вернёт WF_OFFLOAD_ERR_BLOCKED).
//  Транспорт A2 — только plain HTTP (LAN ПК/NAS). HTTPS/SMB — фаза B (#REC-11-B).
// ============================================================================

typedef struct {
    bool     enabled;       // выгрузка включена
    char     url[160];      // полный URL приёмника, напр. "http://<pc-host>:8080/wf"
    char     user[48];      // опц. HTTP Basic-auth логин ("" = без авторизации)
    char     pass[48];      // опц. HTTP Basic-auth пароль
} wf_offload_cfg_t;

typedef struct {
    uint32_t sent_ok;       // успешно выгружено сегментов с момента boot
    uint32_t failed;        // неуспешных попыток выгрузки с момента boot
    int      last_status;   // последний HTTP-код (>0) или отрицательный код ошибки
    long     last_ok_at;    // epoch последней успешной выгрузки (0 = ещё не было)
    bool     busy;          // прямо сейчас идёт POST
} wf_offload_stat_t;

// Коды возврата wf_offload_set_cfg.
#define WF_OFFLOAD_OK            0
#define WF_OFFLOAD_ERR_NVS      (-1)   // ошибка записи NVS
#define WF_OFFLOAD_ERR_BLOCKED  (-2)   // адрес запрещён (Народмон-бан)
#define WF_OFFLOAD_ERR_INVALID  (-3)   // невалидный конфиг (url не "http://..." при enabled)

// Загрузить конфиг из NVS и поднять задачу-аплоадер. Вызывать на boot ПОСЛЕ
// web_server_init() (см. main.c). Идемпотентно: повторный вызов — no-op.
void wf_offload_init(void);

// Снимок текущего конфига. ВНИМАНИЕ: out->pass заполняется реальным паролем —
// веб-слой НЕ должен отдавать его наружу (вернуть только has_pass).
void wf_offload_get_cfg(wf_offload_cfg_t *out);

// Сохранить конфиг в NVS (write-on-change: NVS не трогается, если ничего не
// изменилось — #NVS-1) и применить к работающей задаче. Возвращает WF_OFFLOAD_OK
// или отрицательный WF_OFFLOAD_ERR_*.
int  wf_offload_set_cfg(const wf_offload_cfg_t *in);

// Снимок рантайм-статистики для веб-статуса.
void wf_offload_get_stat(wf_offload_stat_t *out);

// Разбудить задачу немедленно (после смены конфига или появления нового сегмента).
void wf_offload_kick(void);
