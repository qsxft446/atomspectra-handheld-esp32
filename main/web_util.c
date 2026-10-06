#include "web_util.h"
#include "boot_config.h"
#include <stdio.h>

// P3-9: значения из прибора (serial) уходят в XML/N42-разметку → экранировать &<>"'.
void web_xml_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 6 < cap; p++) {
        switch (*p) {
        case '&':  o += snprintf(out + o, cap - o, "&amp;");  break;
        case '<':  o += snprintf(out + o, cap - o, "&lt;");   break;
        case '>':  o += snprintf(out + o, cap - o, "&gt;");   break;
        case '"':  o += snprintf(out + o, cap - o, "&quot;"); break;
        case '\'': o += snprintf(out + o, cap - o, "&apos;"); break;
        default:   out[o++] = *p; break;
        }
    }
    out[o] = '\0';
}

// #FW-42: prefix + base → out. Пустой префикс = чистый base (обратная совместимость).
void web_build_export_name(char *out, size_t cap, const char *base)
{
    if (!out || cap == 0) return;
    boot_config_t bc;
    boot_config_load(&bc);                       // читает NVS "boot"/"nprefix" (санит.)
    if (bc.name_prefix[0]) {
        snprintf(out, cap, "%s%s", bc.name_prefix, base ? base : "");
    } else {
        snprintf(out, cap, "%s", base ? base : "");
    }
}
