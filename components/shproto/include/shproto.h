#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SHPROTO_START  0xFE
#define SHPROTO_ESC    0xFD
#define SHPROTO_FINISH 0xA5

typedef struct {
    uint8_t *data;
    size_t   buf_size;
    size_t   len;
    uint8_t  cmd;
    uint16_t crc_tx;
    bool     ready;
    bool     dropped;
    bool     esc;
    bool     started;
} shproto_struct;

void shproto_init(shproto_struct *p, uint8_t *buf, size_t buf_size);
void shproto_byte_received(shproto_struct *p, uint8_t byte);
void shproto_packet_start(shproto_struct *p, uint8_t cmd);
void shproto_packet_add_data(shproto_struct *p, uint8_t byte);
void shproto_packet_complete(shproto_struct *p);