#include "np_peer_store.h"
#include <string.h>
static bool valid(const np_peer_record_t *r)
{
    return r->name[0] && memchr(r->name, 0, sizeof(r->name)) && r->port &&
           (r->family == 4 || r->family == 6);
}
bool np_peer_store_decode(const uint8_t *data, size_t len, unsigned version,
                          np_peer_record_t *records, size_t *count)
{
    if (!data || !records || !count) { return false; }
    size_t entries, entry;
    if (version == 3) {
        if (len < 4 || data[0] != 3 || data[2] != 27 || data[3] != 0) { return false; }
        entries = data[1]; entry = 27;
        if (len != 4 + entries * entry) { return false; }
        data += 4;
    } else if (version == 1 || version == 2) {
        entry = version == 1 ? 15 : 27;
        if (!len || len % entry) { return false; }
        entries = len / entry;
    } else { return false; }
    if (entries > NP_PEER_LIMIT) { return false; }
    size_t used = 0;
    for (size_t i = 0; i < entries; ++i) {
        const uint8_t *p = data + i * entry;
        if (!p[0]) { continue; }
        np_peer_record_t r = {0};
        memcpy(r.name, p, 8);
        r.family = version == 1 ? 4 : p[8];
        memcpy(r.address, p + (version == 1 ? 8 : 9), version == 1 ? 4 : 16);
        const size_t port = version == 1 ? 12 : 25;
        r.port = (uint16_t)((p[port] << 8) | p[port + 1]);
        if (!valid(&r)) { return false; }
        records[used++] = r;
    }
    *count = used;
    return true;
}
size_t np_peer_store_encode(const np_peer_record_t *records, size_t count, uint8_t *out, size_t cap)
{
    if (!out || count > NP_PEER_LIMIT || (count && !records)) { return 0; }
    const size_t size = 4 + count * 27;
    if (size > cap) { return 0; }
    for (size_t i = 0; i < count; ++i) { if (!valid(&records[i])) { return 0; } }
    memset(out, 0, size); out[0] = 3; out[1] = (uint8_t)count; out[2] = 27;
    for (size_t i = 0; i < count; ++i) {
        uint8_t *p = out + 4 + i * 27;
        memcpy(p, records[i].name, 8); p[8] = records[i].family;
        memcpy(p + 9, records[i].address, 16);
        p[25] = (uint8_t)(records[i].port >> 8); p[26] = (uint8_t)records[i].port;
    }
    return size;
}
