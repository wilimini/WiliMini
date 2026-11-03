#include "stream_server.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Same list as server/internal/proxy: API hosts are exact, images any *.hdslb.com. */
static int proxied_host(const char *host, size_t length) {
    static const char *const exact[] = {
        "api.bilibili.com", "app.bilibili.com", "api.vc.bilibili.com", "api.live.bilibili.com",
        "passport.bilibili.com", "bangumi.bilibili.com", "s.search.bilibili.com",
        "message.bilibili.com", "www.bilibili.com"};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); ++i)
        if (strlen(exact[i]) == length && !memcmp(exact[i], host, length)) return 1;
    static const char suffix[] = ".hdslb.com";
    const size_t suffix_length = sizeof(suffix) - 1;
    if (length <= suffix_length || memcmp(host + length - suffix_length, suffix, suffix_length)) return 0;
    for (size_t i = 0; i < length; ++i) {
        const char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return 0;
        if (c == '.' && i + 1 < length && host[i + 1] == '.') return 0;
    }
    return 1;
}

static int valid_host_char(int c) {
    return isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_';
}

int wili_server_parse_address(const char *text, char *host, size_t host_cap, int *port) {
    if (!text || !host || !host_cap || !port) return -1;
    while (*text == ' ') ++text;
    size_t length = strlen(text);
    while (length && text[length - 1] == ' ') --length;
    if (!length) return -1;
    size_t host_length = length;
    int parsed_port = WILI_SERVER_PORT;
    const char *colon = memchr(text, ':', length);
    if (colon) {
        host_length = (size_t)(colon - text);
        const char *digits = colon + 1;
        size_t digit_count = length - host_length - 1;
        if (!digit_count || digit_count > 5) return -1;
        long value = 0;
        for (size_t i = 0; i < digit_count; ++i) {
            if (digits[i] < '0' || digits[i] > '9') return -1;
            value = value * 10 + (digits[i] - '0');
        }
        if (value < 1 || value > 65535) return -1;
        parsed_port = (int)value;
    }
    if (!host_length || host_length >= host_cap || host_length >= WILI_SERVER_HOST_MAX) return -1;
    for (size_t i = 0; i < host_length; ++i)
        if (!valid_host_char((unsigned char)text[i])) return -1;
    if (text[0] == '.' || text[0] == '-' || text[host_length - 1] == '.' || text[host_length - 1] == '-') return -1;
    memcpy(host, text, host_length);
    host[host_length] = 0;
    *port = parsed_port;
    return 0;
}

int wili_server_valid_token(const char *token) {
    if (!token) return 0;
    size_t length = strlen(token);
    if (length < 4 || length >= WILI_SERVER_TOKEN_MAX) return 0;
    for (size_t i = 0; i < length; ++i)
        if (!isalnum((unsigned char)token[i]) && token[i] != '_' && token[i] != '-') return 0;
    return 1;
}

int wili_server_ready(const WiliServer *s) {
    return s && s->enabled && s->host[0] && s->port > 0 && s->port <= 65535 &&
           wili_server_valid_token(s->token);
}

/* Appends without overflowing; remembers failure in *ok. */
typedef struct { char *out; size_t cap, len; int ok; } Builder;
static void put(Builder *b, const char *text) {
    size_t n = strlen(text);
    if (!b->ok || b->len + n + 1 > b->cap) { b->ok = 0; return; }
    memcpy(b->out + b->len, text, n + 1);
    b->len += n;
}
static void put_int(Builder *b, long long value) {
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lld", value);
    put(b, tmp);
}
static void put_escaped(Builder *b, const char *text) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *text; ++text) {
        const unsigned char c = (unsigned char)*text;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            char one[2] = {(char)c, 0};
            put(b, one);
        } else {
            char three[4] = {'%', hex[c >> 4], hex[c & 15], 0};
            put(b, three);
        }
    }
}
static void put_base(Builder *b, const WiliServer *s) {
    put(b, "http://");
    put(b, s->host);
    put(b, ":");
    put_int(b, s->port);
}

int wili_server_rewrite(const WiliServer *s, const char *url, char *out, size_t cap) {
    if (!wili_server_ready(s) || !url || !out || !cap) return 0;
    static const char scheme[] = "https://";
    const char *rest = url;
    if (!strncmp(url, "//", 2)) rest = url + 2;
    else if (!strncmp(url, scheme, sizeof(scheme) - 1)) rest = url + sizeof(scheme) - 1;
    else return 0;
    size_t host_length = strcspn(rest, "/?#:@");
    if (rest[host_length] != '/' && rest[host_length] != '?' && rest[host_length] != 0) return 0;
    if (!proxied_host(rest, host_length)) return 0;
    const char *path = rest + host_length;
    const char *query = strchr(path, '?');
    const char *fragment = strchr(path, '#');
    size_t path_length = query ? (size_t)(query - path) : fragment ? (size_t)(fragment - path) : strlen(path);
    Builder b = {out, cap, 0, 1};
    out[0] = 0;
    put_base(&b, s);
    put(&b, "/p/");
    {
        char host[WILI_SERVER_HOST_MAX + 1];
        memcpy(host, rest, host_length);
        host[host_length] = 0;
        put(&b, host);
    }
    if (!path_length) put(&b, "/");
    else if (b.ok && b.len + path_length + 1 <= b.cap) {
        memcpy(b.out + b.len, path, path_length);
        b.len += path_length;
        b.out[b.len] = 0;
    } else b.ok = 0;
    int has_query = 0;
    if (query) {
        size_t query_length = fragment && fragment > query ? (size_t)(fragment - query) : strlen(query);
        if (query_length > 1 && b.ok && b.len + query_length + 1 <= b.cap) {
            memcpy(b.out + b.len, query, query_length);
            b.len += query_length;
            b.out[b.len] = 0;
            has_query = 1;
        } else if (query_length > 1) b.ok = 0;
    }
    put(&b, has_query ? "&k=" : "?k=");
    put_escaped(&b, s->token);
    if (!b.ok) { out[0] = 0; return -1; }
    return 1;
}

int wili_server_hello_url(const WiliServer *s, char *out, size_t cap) {
    if (!wili_server_ready(s) || !out || !cap) return -1;
    Builder b = {out, cap, 0, 1};
    out[0] = 0;
    put_base(&b, s);
    put(&b, "/wili/v1/hello?k=");
    put_escaped(&b, s->token);
    if (!b.ok) { out[0] = 0; return -1; }
    return 0;
}

int wili_server_logout_url(const WiliServer *s, char *out, size_t cap) {
    if (!wili_server_ready(s) || !out || !cap) return -1;
    Builder b = {out, cap, 0, 1};
    out[0] = 0;
    put_base(&b, s);
    put(&b, "/wili/v1/logout?k=");
    put_escaped(&b, s->token);
    if (!b.ok) { out[0] = 0; return -1; }
    return 0;
}

WiliStreamParams wili_stream_params(WiliStreamPreset preset) {
    switch (preset) {
    case WILI_PRESET_PERFORMANCE: return (WiliStreamParams){384, 216, 24, 700, 64};
    case WILI_PRESET_QUALITY: return (WiliStreamParams){480, 272, 30, 1500, 128};
    default: return (WiliStreamParams){480, 272, 30, 1000, 96};
    }
}

int wili_server_stream_url(const WiliServer *s, const WiliStreamRequest *r, char *out, size_t cap) {
    if (!wili_server_ready(s) || !r || !out || !cap || r->cid <= 0) return -1;
    if (!r->pgc && (!r->bvid || !r->bvid[0])) return -1;
    const WiliStreamParams p = wili_stream_params(r->preset);
    Builder b = {out, cap, 0, 1};
    out[0] = 0;
    put_base(&b, s);
    put(&b, "/wili/v1/stream?");
    if (r->pgc) put(&b, "pgc=1");
    else { put(&b, "bvid="); put_escaped(&b, r->bvid); }
    put(&b, "&cid="); put_int(&b, r->cid);
    if (r->start_ms > 0) {
        put(&b, "&t=");
        put_int(&b, r->start_ms / 1000);
        put(&b, ".");
        char ms[8];
        snprintf(ms, sizeof(ms), "%03d", (int)(r->start_ms % 1000));
        put(&b, ms);
    }
    put(&b, "&profile=");
    put(&b, r->profile == WILI_PROFILE_CABAC ? "cabac" : r->profile == WILI_PROFILE_CAVLC ? "cavlc" : "auto");
    put(&b, "&w="); put_int(&b, p.width);
    put(&b, "&h="); put_int(&b, p.height);
    put(&b, "&fps="); put_int(&b, p.fps);
    put(&b, "&vb="); put_int(&b, p.video_kbps);
    put(&b, "&ab="); put_int(&b, p.audio_kbps);
    put(&b, "&k="); put_escaped(&b, s->token);
    if (!b.ok) { out[0] = 0; return -1; }
    return 0;
}

/* ---- discovery -------------------------------------------------------- */

int wili_beacon_probe(unsigned char *out, size_t cap) {
    static const char probe[] = "WILIMINI-DISCOVER 1\n";
    if (!out || cap < sizeof(probe) - 1) return -1;
    memcpy(out, probe, sizeof(probe) - 1);
    return (int)(sizeof(probe) - 1);
}

int wili_beacon_parse(const unsigned char *packet, size_t length, int *port, char *name, size_t name_cap) {
    static const char prefix[] = "WILIMINI-SERVER ";
    if (!packet || !port || !name || !name_cap || length <= sizeof(prefix) - 1 || length > 200) return -1;
    if (memcmp(packet, prefix, sizeof(prefix) - 1)) return -1;
    char text[201];
    memcpy(text, packet, length);
    text[length] = 0;
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r' || text[length - 1] == ' ')) text[--length] = 0;
    char *cursor = text + sizeof(prefix) - 1;
    char *end = NULL;
    long version = strtol(cursor, &end, 10);
    if (end == cursor || *end != ' ' || version < 1) return -1;
    cursor = end + 1;
    long value = strtol(cursor, &end, 10);
    if (end == cursor || *end != ' ' || value < 1 || value > 65535) return -1;
    cursor = end + 1;
    size_t n = strlen(cursor);
    if (n >= name_cap) n = name_cap - 1;
    memcpy(name, cursor, n);
    name[n] = 0;
    *port = (int)value;
    return 0;
}

/* DNS name helpers. */
static int read_name(const unsigned char *msg, size_t size, size_t offset,
                     char *out, size_t cap, size_t *next) {
    size_t used = 0, jumps = 0;
    int followed = 0;
    for (;;) {
        if (offset >= size) return -1;
        unsigned len = msg[offset];
        if (len == 0) {
            if (!followed && next) *next = offset + 1;
            if (used) out[used - 1] = 0; else out[0] = 0; /* drop the trailing dot */
            return 0;
        }
        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= size) return -1;
            size_t target = ((len & 0x3F) << 8) | msg[offset + 1];
            if (!followed && next) *next = offset + 2;
            followed = 1;
            if (++jumps > 8 || target >= size) return -1;
            offset = target;
            continue;
        }
        if (len > 63 || offset + 1 + len > size) return -1;
        if (used + len + 2 > cap) return -1;
        memcpy(out + used, msg + offset + 1, len);
        used += len;
        out[used++] = '.';
        out[used] = 0;
        offset += 1 + len;
    }
}

static int name_equals(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}

static unsigned be16(const unsigned char *p) { return (unsigned)(p[0] << 8 | p[1]); }

int wili_mdns_query(unsigned char *out, size_t cap) {
    static const unsigned char query[] = {
        0x1e, 0xe7, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        9, '_', 'w', 'i', 'l', 'i', 'm', 'i', 'n', 'i',
        4, '_', 't', 'c', 'p',
        5, 'l', 'o', 'c', 'a', 'l', 0,
        0x00, 0x0c, 0x00, 0x01};
    if (!out || cap < sizeof(query)) return -1;
    memcpy(out, query, sizeof(query));
    return (int)sizeof(query);
}

int wili_mdns_parse(const unsigned char *msg, size_t size, int *port, char *name, size_t name_cap) {
    if (!msg || !port || !name || !name_cap || size < 12 || size > 9000) return -1;
    if (!(msg[2] & 0x80)) return -1; /* not a response */
    unsigned questions = be16(msg + 4);
    unsigned records = be16(msg + 6) + be16(msg + 8) + be16(msg + 10);
    size_t offset = 12;
    char buffer[256];
    for (unsigned i = 0; i < questions; ++i) {
        if (read_name(msg, size, offset, buffer, sizeof(buffer), &offset) < 0) return -1;
        offset += 4;
        if (offset > size) return -1;
    }
    char instance[256] = "";
    int found_port = 0;
    char txt_name[WILI_SERVER_NAME_MAX] = "";
    int have_ptr = 0;
    /* PTR first, so SRV/TXT can be matched to its instance regardless of order. */
    for (int pass = 0; pass < 2; ++pass) {
        size_t cursor = offset;
        for (unsigned i = 0; i < records; ++i) {
            char owner[256];
            if (read_name(msg, size, cursor, owner, sizeof(owner), &cursor) < 0) return -1;
            if (cursor + 10 > size) return -1;
            unsigned type = be16(msg + cursor);
            unsigned rdlength = be16(msg + cursor + 8);
            size_t rdata = cursor + 10;
            if (rdata + rdlength > size) return -1;
            cursor = rdata + rdlength;
            if (pass == 0 && type == 12 && name_equals(owner, "_wilimini._tcp.local")) {
                if (read_name(msg, size, rdata, instance, sizeof(instance), NULL) == 0) have_ptr = 1;
            } else if (pass == 1 && have_ptr && name_equals(owner, instance)) {
                if (type == 33 && rdlength >= 6) {
                    int p = (int)be16(msg + rdata + 4);
                    if (p > 0) found_port = p;
                } else if (type == 16) {
                    size_t at = rdata;
                    while (at < rdata + rdlength) {
                        size_t n = msg[at++];
                        if (at + n > rdata + rdlength) break;
                        if (n > 5 && !memcmp(msg + at, "name=", 5)) {
                            size_t m = n - 5;
                            if (m >= sizeof(txt_name)) m = sizeof(txt_name) - 1;
                            memcpy(txt_name, msg + at + 5, m);
                            txt_name[m] = 0;
                        }
                        at += n;
                    }
                }
            }
        }
    }
    if (!have_ptr || !found_port) return -1;
    if (txt_name[0]) {
        snprintf(name, name_cap, "%s", txt_name);
    } else {
        /* Instance label, up to the service type. */
        size_t label = 0;
        while (instance[label] && !(instance[label] == '.' && instance[label + 1] == '_')) ++label;
        if (label >= name_cap) label = name_cap - 1;
        memcpy(name, instance, label);
        name[label] = 0;
    }
    *port = found_port;
    return 0;
}
