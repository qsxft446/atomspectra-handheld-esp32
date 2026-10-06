#pragma once
// #FS-1: браузер файлов для страницы «Файлы» (web/saved.html, запрос 2026-10-06):
// папки SD-карты (спектры, мониторинг, водопад). Флеш не показывается (#FS-2).
//   GET  /api/fs/roots                         — папки: есть ли, файлов, байт; место SD
//   GET  /api/fs/list?root=ID                  — файлы папки: имя, размер, дата; занятые
//   GET  /api/fs/file?root=ID&name=N[&head=B][&dl=1] — файл (head — первые B байт)
//   GET  /api/fs/spectrum?root=ID&name=N&fmt=json|xml|csv — spec_NNNN.bin как спектр
//   POST /api/fs/delete?root=ID   тело: имена через '\n' (CSRF) — пакетное удаление
// Файл адресуется парой «папка + имя без каталогов» — путь собирает плата, так что
// выйти за пределы папок нельзя.
#include "esp_http_server.h"
#include "atomspectra.h"

void web_files_register(httpd_handle_t server);

// Реализовано в web_server.c (там static-рендеры экспорта): fmt "xml" | "csv" | иначе JSON.
esp_err_t web_render_spectrum(httpd_req_t *req, const spectrum_data_t *sp,
                              const char *fmt, const char *filename);
