// Round-trip-тесты shproto: кодирование → декодирование → побайтная сверка.
// main() и тесты декодера — в test_shproto.c; здесь только roundtrip_suite(),
// вызываемый оттуда (обе .c компилируются в один test_runner).
#include "shproto.h"
#include "test_util.h"
#include <string.h>

#define T_CMD 0x42

// Round-trip cmd + payload через encoder shproto, затем декод в отдельный буфер.
// Проверяет, что cmd, длина и все байты payload восстановлены точно.
static void roundtrip(uint8_t cmd, const uint8_t *payload, size_t n,
                      uint8_t *wire, size_t wire_cap,
                      uint8_t *dec, size_t dec_cap)
{
    shproto_struct pe;
    shproto_init(&pe, wire, wire_cap);
    shproto_packet_start(&pe, cmd);
    for (size_t i = 0; i < n; i++) shproto_packet_add_data(&pe, payload[i]);
    shproto_packet_complete(&pe);

    shproto_struct pd;
    shproto_init(&pd, dec, dec_cap);
    for (size_t i = 0; i < pe.len; i++) shproto_byte_received(&pd, wire[i]);

    CHECK(pd.ready == true);
    CHECK(pd.cmd == cmd);
    CHECK(pd.len == n);
    CHECK(memcmp(pd.data, payload, n) == 0);
}

// Byte-stuffing: payload содержит сами маркеры 0xFE/0xFD/0xA5 → encoder
// экранирует (add_escaped), decoder разэкранирует → данные восстанавливаются.
static void test_stuffing_markers(void)
{
    uint8_t wire[128], dec[128];
    const uint8_t payload[] = {0x01, 0x02, SHPROTO_START, SHPROTO_ESC,
                               SHPROTO_FINISH, 0x03, 0xFF, 0x00};
    roundtrip(T_CMD, payload, sizeof(payload), wire, sizeof(wire), dec, sizeof(dec));
}

// cmd сам является маркером (0xFD) → тоже должен экранироваться и восстановиться.
static void test_cmd_is_marker(void)
{
    uint8_t wire[64], dec[64];
    const uint8_t payload[] = {0x10, 0x20};
    roundtrip(SHPROTO_ESC, payload, sizeof(payload), wire, sizeof(wire), dec, sizeof(dec));
}

// Пустой payload (только cmd): на FINISH len==3 (cmd + 2 байта CRC) → валидно,
// после декода payload-длина == 0. Указатель payload непустой (не NULL) —
// memcmp(.., NULL, 0) формально UB и ловится UBSan.
static void test_empty_payload(void)
{
    uint8_t wire[32], dec[32];
    const uint8_t empty[1] = {0};
    roundtrip(T_CMD, empty, 0, wire, sizeof(wire), dec, sizeof(dec));
}

// Все 256 значений байта в payload — проверка эскейпа/CRC на полном алфавите.
static void test_all_byte_values(void)
{
    uint8_t wire[600], dec[600];
    uint8_t payload[256];
    for (int i = 0; i < 256; i++) payload[i] = (uint8_t)i;
    roundtrip(T_CMD, payload, sizeof(payload), wire, sizeof(wire), dec, sizeof(dec));
}

void roundtrip_suite(void)
{
    test_stuffing_markers();
    test_cmd_is_marker();
    test_empty_payload();
    test_all_byte_values();
}
