// Host-тесты конечного автомата shproto (components/shproto/shproto.c).
// Компилируется обычным gcc/clang — компонент зависит только от stdint/stddef/
// stdbool/string, ESP-IDF не требуется. Здесь живёт main() и тесты декодера
// shproto_byte_received; round-trip-тесты — в test_roundtrip.c.
#include "shproto.h"
#include "test_util.h"
#include <string.h>
#include <stdio.h>

int g_failures = 0;

// Round-trip-набор из test_roundtrip.c (одна общая сборка → один main).
void roundtrip_suite(void);
// #FIELD-6: guard-логика источника времени из test_net_time.c (main/net_time.c).
void nettime_suite(void);

// Тестовая команда. CMD_HISTOGRAM (0x01) объявлена в main/atomspectra.h, но она
// вне include-path host-сборки; shproto трактует cmd как обычный uint8_t.
#define T_CMD 0x42

// Кодирует кадр (cmd + payload) в проволочный буфер через encoder shproto.
// Возвращает длину кадра на проводе (ведущий 0xFF + START + тело + FINISH).
static size_t encode_frame(uint8_t cmd, const uint8_t *payload, size_t n,
                           uint8_t *wire, size_t wire_cap)
{
    shproto_struct pe;
    shproto_init(&pe, wire, wire_cap);
    shproto_packet_start(&pe, cmd);
    for (size_t i = 0; i < n; i++) shproto_packet_add_data(&pe, payload[i]);
    shproto_packet_complete(&pe);
    return pe.len;
}

// Скармливает поток байт в декодер побайтно.
static void feed(shproto_struct *pd, const uint8_t *wire, size_t n)
{
    for (size_t i = 0; i < n; i++) shproto_byte_received(pd, wire[i]);
}

// 1. Happy path: валидный кадр декодируется, cmd и данные совпадают с исходными.
static void test_happy_path(void)
{
    uint8_t wire[64], dec[64];
    const uint8_t payload[] = {0x10, 0x11, 0x22, 0x33};
    size_t wlen = encode_frame(T_CMD, payload, sizeof(payload), wire, sizeof(wire));

    shproto_struct pd;
    shproto_init(&pd, dec, sizeof(dec));
    feed(&pd, wire, wlen);

    CHECK(pd.ready == true);
    CHECK(pd.dropped == false);
    CHECK(pd.cmd == T_CMD);
    CHECK(pd.len == sizeof(payload));
    CHECK(memcmp(pd.data, payload, sizeof(payload)) == 0);
}

// 2. CRC corruption: инверсия одного байта payload → dropped, не ready.
static void test_crc_corruption(void)
{
    uint8_t wire[64], dec[64];
    const uint8_t payload[] = {0x11, 0x22, 0x33};  // без маркеров → без escape
    size_t wlen = encode_frame(T_CMD, payload, sizeof(payload), wire, sizeof(wire));

    // Контроль: неиспорченный кадр декодируется чисто.
    shproto_struct ctrl;
    shproto_init(&ctrl, dec, sizeof(dec));
    feed(&ctrl, wire, wlen);
    CHECK(ctrl.ready == true);

    // Проволочный кадр: [0]=0xFF [1]=0xFE(START) [2]=cmd [3]=0x11 [4]=0x22 ...
    // Портим payload-байт по индексу 4. XOR 0x01 → 0x23: не маркер, поэтому
    // фрейминг цел, но CRC ломается.
    uint8_t bad[64];
    memcpy(bad, wire, wlen);
    bad[4] ^= 0x01;
    CHECK(bad[4] != SHPROTO_START && bad[4] != SHPROTO_ESC && bad[4] != SHPROTO_FINISH);

    shproto_struct pd;
    shproto_init(&pd, dec, sizeof(dec));
    feed(&pd, bad, wlen);
    CHECK(pd.ready == false);
    CHECK(pd.dropped == true);
}

// 4. Потеря START: байты до первого 0xFE игнорируются (started остаётся false),
//    а следующий валидный кадр после мусора всё равно декодируется.
static void test_lost_start(void)
{
    uint8_t dec[64];
    shproto_struct pd;
    shproto_init(&pd, dec, sizeof(dec));

    const uint8_t garbage[] = {0x01, 0x02, 0x03, 0xAA, 0xBB};  // нет 0xFE
    feed(&pd, garbage, sizeof(garbage));
    CHECK(pd.started == false);
    CHECK(pd.ready == false);
    CHECK(pd.len == 0);

    uint8_t wire[64];
    const uint8_t payload[] = {0x55, 0x66};
    size_t wlen = encode_frame(T_CMD, payload, sizeof(payload), wire, sizeof(wire));
    feed(&pd, wire, wlen);
    CHECK(pd.ready == true);
    CHECK(pd.cmd == T_CMD);
    CHECK(pd.len == sizeof(payload));
    CHECK(memcmp(pd.data, payload, sizeof(payload)) == 0);
}

// 5. Два пакета подряд в одном потоке байт — оба декодируются.
static void test_two_packets(void)
{
    uint8_t w1[64], w2[64], dec[64];
    const uint8_t p1[] = {0x01, 0x02, 0x03};
    const uint8_t p2[] = {0xA0, 0xB0};
    size_t l1 = encode_frame(0x11, p1, sizeof(p1), w1, sizeof(w1));
    size_t l2 = encode_frame(0x22, p2, sizeof(p2), w2, sizeof(w2));

    shproto_struct pd;
    shproto_init(&pd, dec, sizeof(dec));

    feed(&pd, w1, l1);
    CHECK(pd.ready == true);
    CHECK(pd.cmd == 0x11);
    CHECK(pd.len == sizeof(p1));
    CHECK(memcmp(pd.data, p1, sizeof(p1)) == 0);

    feed(&pd, w2, l2);
    CHECK(pd.ready == true);
    CHECK(pd.cmd == 0x22);
    CHECK(pd.len == sizeof(p2));
    CHECK(memcmp(pd.data, p2, sizeof(p2)) == 0);
}

// 6. Переполнение буфера: payload длиннее buf_size → декодер обрезает по границе,
//    не пишет за пределы (canary после буфера цела), не падает; усечённый буфер →
//    CRC не сходится → dropped, не ready.
static void test_buffer_overflow(void)
{
    struct { uint8_t buf[8]; uint8_t canary[8]; } g;
    memset(g.canary, 0xAA, sizeof(g.canary));

    uint8_t wire[128];
    uint8_t payload[40];
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)(i + 1);
    size_t wlen = encode_frame(T_CMD, payload, sizeof(payload), wire, sizeof(wire));

    shproto_struct pd;
    shproto_init(&pd, g.buf, sizeof(g.buf));  // buf_size = 8 << payload
    feed(&pd, wire, wlen);

    for (size_t i = 0; i < sizeof(g.canary); i++)
        CHECK(g.canary[i] == 0xAA);
    CHECK(pd.len <= sizeof(g.buf));
    CHECK(pd.ready == false);
}

int main(void)
{
    test_happy_path();
    test_crc_corruption();
    test_lost_start();
    test_two_packets();
    test_buffer_overflow();
    roundtrip_suite();
    nettime_suite();

    if (g_failures) {
        printf("\n%d CHECK(S) FAILED\n", g_failures);
        return 1;
    }
    printf("\nALL PASSED\n");
    return 0;
}
