#pragma once

#include "esp_http_server.h"
#include <stdbool.h>

// Регистрирует все HTTP/WS-обработчики водопада на уже запущенном сервере
// и подписывает WS-броадкаст на новые строки спектрограммы.
void web_waterfall_register(httpd_handle_t server);

// Колбэк httpd_config_t.close_fn: чистит WS-реестр при ЛЮБОМ закрытии сокета
// (graceful close, RST, LRU-purge при F5 и пр.), затем закрывает сокет вручную
// (требование esp_http_server: если close_fn задан — пользователь обязан вызвать close()).
void web_waterfall_on_close(httpd_handle_t hd, int sockfd);

// Экспорт CSRF-проверки из web_server.c (csrf_check там static).
bool web_csrf_check(httpd_req_t *req);