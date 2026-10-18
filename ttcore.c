/* ===========================================================================
   ttcore.c - NOS Teletekst page model, streaming parser and renderer
   Portable C (also compiles on a PC for testing). Part of TELETEKS for SymbOS.

   Input  : raw HTTP response bytes (status line + headers + JSON body),
            fed in arbitrary chunks via tt_feed().
   Output : 40x25 teletext cell grid (tt_ch / tt_at / tt_lk) plus navigation
            fields (prev/next page, sub pages, fast text links).

   JSON layout (teletekst-data.nos.nl/json/<page>):
     { "prevPage":"..", "nextPage":"..", "prevSubPage":"..", "nextSubPage":"..",
       "fastTextLinks":[{"title":"..","page":".."}, ...],
       "content":"<span class=\"red bg-blue \">text</span>\n..." }
   Block graphics arrive as U+F020..U+F07F (contiguous) / U+F0A0..U+F0FF
   (separated) - same mapping as the MSX version.
   =========================================================================== */

#include <string.h>
#include "ttfont.h"
#ifndef STATICF
#define STATICF static
#endif

#define TT_COLS   40
#define TT_ROWS   25
#define TT_CELLS  1000
#define TT_MAXLNK 48

/* teletext colours */
#define TT_BLACK 0
#define TT_RED 1
#define TT_GREEN 2
#define TT_YELLOW 3
#define TT_BLUE 4
#define TT_MAGENTA 5
#define TT_CYAN 6
#define TT_WHITE 7

#define AT_MOSAIC 0x40
#define AT_SEP    0x80

unsigned char tt_ch[TT_CELLS];   /* glyph index 0..159, or 6 mosaic bits   */
unsigned char tt_at[TT_CELLS];   /* b0-2 fg, b3-5 bg, b6 mosaic, b7 separ. */
unsigned char tt_lk[TT_CELLS];   /* link number + 1, 0 = no link           */
char tt_links[TT_MAXLNK][8];
unsigned char tt_nlinks;
char tt_prev[8], tt_next[8], tt_prevsub[8], tt_nextsub[8];
char tt_ftitle[4][14];
char tt_fpage[4][8];
unsigned char tt_nfast;
int  tt_http;                    /* HTTP status code, 0 = none seen        */
unsigned char tt_gotjson;        /* 1 once the JSON body has started       */
unsigned short tt_clen, tt_body; /* Content-Length and body bytes so far   */
unsigned char tt_hasclen;
unsigned char tt_rows_used;      /* highest content row written + 1       */

/* ---------------------------------------------------------------- helpers */
STATICF unsigned char lc(unsigned char c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

STATICF unsigned char streq(char* a, char* b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}

/* ============================================================ cell output */
static unsigned char h_row, h_fg, h_bg;
unsigned char h_col, h_link;           /* shared with ttfast.s */
static unsigned short h_base;           /* h_row * TT_COLS */
unsigned short h_idx;                   /* index of the next cell */
unsigned char h_room;                   /* cells left on this row (0 = off page) */
unsigned char h_attr;                   /* h_fg | h_bg << 3 */
unsigned char cls[256];                 /* fast-path class per byte: 1 text, 2 tag */
static unsigned char cls_ready;
static unsigned char h_sfg[16], h_sbg[16], h_sp;

STATICF void put_cell(unsigned char ch, unsigned char at) {
    if (h_room) {
        tt_ch[h_idx] = ch;
        tt_at[h_idx] = at | h_fg | (h_bg << 3);
        tt_lk[h_idx] = h_link;
        ++h_idx;
        --h_room;
    }
    ++h_col;
}

STATICF void set_pos(unsigned char row, unsigned char col) {
    h_row = row; h_col = col;
    h_base = (unsigned short)row * TT_COLS;
    h_idx = h_base + col;
    h_room = (row < TT_ROWS && col < TT_COLS) ? TT_COLS - col : 0;
}

STATICF void newline(void) { if (h_row < 200) set_pos(h_row + 1, 0); else h_room = 0; }

/* map a Unicode code point to a cell */
STATICF void put_cp(unsigned short cp) {
    unsigned char b;
    if (cp == '\n') { newline(); return; }
    if (cp == '\r') return;
    if (cp == '\t') cp = ' ';
    if (cp >= 0x20 && cp < 0x7F) { put_cell((unsigned char)(cp - 0x20), 0); return; }
    if (cp >= 0xC0 && cp <= 0xFF) { put_cell((unsigned char)(cp - 0xC0 + 0x60), 0); return; }
    if ((cp & 0xFF00) == 0xF000) {             /* teletext block graphics */
        b = (unsigned char)cp;
        if ((b >= 0x20 && b < 0x40) || (b >= 0x60 && b < 0x80)) {
            put_cell((b & 0x1F) | ((b & 0x40) >> 1), AT_MOSAIC);
            return;
        }
        if ((b >= 0xA0 && b < 0xC0) || b >= 0xE0) {
            b -= 0x80;
            put_cell((b & 0x1F) | ((b & 0x40) >> 1), AT_MOSAIC | AT_SEP);
            return;
        }
        if (b >= 0x40 && b < 0x60) {                  /* as MSXTT: mosaic too */
            put_cell((b & 0x1F) | 0x20, AT_MOSAIC);
            return;
        }
        put_cell(0, 0);
        return;
    }
    switch (cp) {
        case 0xA0: b = ' '; break;
        case 0x128: b = 'I'; break;                    /* as in MSX version */
        case 0xB0: b = 'o'; break;                     /* degree sign */
        case 0x2018: case 0x2019: case 0xB4: b = '\''; break;
        case 0x201C: case 0x201D: b = '"'; break;
        case 0x2013: case 0x2014: b = '-'; break;
        case 0x20AC: b = 'E'; break;
        case 0x2588: put_cell(0x3F, AT_MOSAIC); return;
        default: b = '?';
    }
    put_cell(b - 0x20, 0);
}

/* ============================================================ HTML layer */
static unsigned char h_state;          /* 0 text, 1 tag, 2 entity */
char h_buf[100];                        /* shared with ttfast.s */
unsigned char h_len;

STATICF void push_colour(void) {
    if (h_sp < 16) { h_sfg[h_sp] = h_fg; h_sbg[h_sp] = h_bg; }
    ++h_sp;
}
STATICF void pop_colour(void) {
    if (!h_sp) return;
    --h_sp;
    if (h_sp < 16) { h_fg = h_sfg[h_sp]; h_bg = h_sbg[h_sp]; }
}

STATICF void add_link(char* href);

/* teletext colour from a class name (NOS uses lower case) */
STATICF unsigned char colour_n(char* t, unsigned char n) {
    switch (t[0]) {
        case 'r': if (n == 3) return TT_RED; break;
        case 'g': if (n == 5) return TT_GREEN; break;
        case 'y': if (n == 6) return TT_YELLOW; break;
        case 'm': if (n == 7) return TT_MAGENTA; break;
        case 'c': if (n == 4 && t[1] == 'y') return TT_CYAN; break;
        case 'w': if (n == 5) return TT_WHITE; break;
        case 'b':
            if (n == 4 && t[2] == 'u') return TT_BLUE;
            if (n == 5 && t[2] == 'a') return TT_BLACK;
            break;
    }
    return 255;
}

/* q points at an attribute value; kind 1 = class, 2 = href.
   Returns pointer just past the value. */
STATICF char* attr_val(char* q, unsigned char kind) {
    char quote = 0;
    char* t;
    unsigned char n, c;
    if (*q == '"' || *q == '\'') quote = *q++;
    if (kind == 2) {
        t = q;
        while (*q && (quote ? *q != quote : *q != ' ')) ++q;
        c = *q; *q = 0;
        add_link(t);
        *q = c;
    } else {
        for (;;) {
            while (*q == ' ') ++q;
            if (!*q || *q == quote) break;
            t = q;
            while (*q && *q != ' ' && *q != quote) ++q;
            n = (unsigned char)(q - t);
            if (n > 3 && t[0] == 'b' && t[1] == 'g' && t[2] == '-') {
                c = colour_n(t + 3, n - 3);
                if (c != 255) h_bg = c;
            } else {
                c = colour_n(t, n);
                if (c != 255) h_fg = c;
            }
            if (!quote) break;
        }
    }
    if (quote && *q == quote) ++q;
    return q;
}

STATICF void add_link(char* href) {
    unsigned char n = 0;
    char* d;
    if (tt_nlinks >= TT_MAXLNK) return;
    while (*href && (*href < '0' || *href > '9')) ++href;   /* skip "#", "/" ... */
    d = tt_links[tt_nlinks];
    while (n < 7 && ((*href >= '0' && *href <= '9') || (*href == '-' && n))) { d[n++] = *href++; }
    d[n] = 0;
    if (n >= 3) h_link = ++tt_nlinks;
}

STATICF void do_tag(void) {
    char* p = h_buf;
    char* q;
    char c, quote;
    unsigned char n, closing = 0;
    h_buf[h_len] = 0;
    if (*p == '/') { closing = 1; ++p; }
    c = *p | 0x20;
    q = p;
    while (*q && *q != ' ' && *q != '/') ++q;
    n = (unsigned char)(q - p);
    /* same rules as MSXTT parse.c: any closing tag restores the colours,
       br/hr start a new line, any other opening tag saves them */
    if (closing) {
        pop_colour();
        if (c == 'a' && n == 1) h_link = 0;
        return;
    }
    if ((c == 'b' || c == 'h') && n == 2 && (p[1] | 0x20) == 'r') { newline(); return; }
    push_colour();
    while (*q) {
        if (*q == ' ' || *q == '/') { ++q; continue; }
        if (q[0] == 'c' && q[1] == 'l' && q[2] == 'a' && q[3] == 's' && q[4] == 's' && q[5] == '=') {
            q = attr_val(q + 6, 1);
            continue;
        }
        if (c == 'a' && q[0] == 'h' && q[1] == 'r' && q[2] == 'e' && q[3] == 'f' && q[4] == '=') {
            q = attr_val(q + 5, 2);
            continue;
        }
        /* skip any other attribute */
        quote = 0;
        while (*q && (quote || *q != ' ')) {
            if (!quote && (*q == '"' || *q == '\'')) quote = *q;
            else if (quote && *q == quote) quote = 0;
            ++q;
        }
    }
}

STATICF void do_entity(void) {
    char* e = h_buf;
    unsigned short cp = 0;
    unsigned char hex = 0;
    h_buf[h_len] = 0;
    if (*e == '#') {
        ++e;
        if (*e == 'x' || *e == 'X') { hex = 1; ++e; }
        while (*e) {
            if (*e >= '0' && *e <= '9') cp = cp * (hex ? 16 : 10) + (*e - '0');
            else if (hex && lc(*e) >= 'a' && lc(*e) <= 'f') cp = cp * 16 + (lc(*e) - 'a' + 10);
            ++e;
        }
        put_cp(cp);
        return;
    }
    if (streq(e, "amp"))  { put_cp('&'); return; }
    if (streq(e, "lt"))   { put_cp('<'); return; }
    if (streq(e, "gt"))   { put_cp('>'); return; }
    if (streq(e, "quot")) { put_cp('"'); return; }
    if (streq(e, "apos")) { put_cp('\''); return; }
    if (streq(e, "nbsp")) { put_cp(' '); return; }
    /* accented letters: &<letter><accent>; */
    if (h_len >= 4) {
        static char* acc[] = {"grave", "acute", "circ", "tilde", "uml", "ring", "cedil", 0};
        /* Latin-1 offsets of A-grave etc. per base letter */
        static char* base = "AaEeIiOoUuCcNnYy";
        static unsigned char tab[16][7] = {
            /* grave acute circ tilde uml ring cedil */
            {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0},{0xE0,0xE1,0xE2,0xE3,0xE4,0xE5,0},
            {0xC8,0xC9,0xCA,0,0xCB,0,0},      {0xE8,0xE9,0xEA,0,0xEB,0,0},
            {0xCC,0xCD,0xCE,0,0xCF,0,0},      {0xEC,0xED,0xEE,0,0xEF,0,0},
            {0xD2,0xD3,0xD4,0xD5,0xD6,0,0},   {0xF2,0xF3,0xF4,0xF5,0xF6,0,0},
            {0xD9,0xDA,0xDB,0,0xDC,0,0},      {0xF9,0xFA,0xFB,0,0xFC,0,0},
            {0,0,0,0,0,0,0xC7},               {0,0,0,0,0,0,0xE7},
            {0,0,0,0xD1,0,0,0},               {0,0,0,0xF1,0,0,0},
            {0,0xDD,0,0,0,0,0},               {0,0xFD,0,0,0xFF,0,0}};
        unsigned char i, j;
        for (i = 0; base[i]; ++i) {
            if (base[i] != e[0]) continue;
            for (j = 0; acc[j]; ++j) {
                if (streq(e + 1, acc[j])) {
                    if (tab[i][j]) { put_cp(tab[i][j]); return; }
                    if (e[0] == 'I' && j == 3) { put_cp('I'); return; }
                    put_cp(e[0]); return;
                }
            }
        }
        if (streq(e, "szlig")) { put_cp(0xDF); return; }
        if (streq(e, "euro"))  { put_cp(0x20AC); return; }
        if (streq(e, "deg"))   { put_cp(0xB0); return; }
    }
    put_cp('?');
}

/* one decoded code point of the "content" HTML */
STATICF void html_cp(unsigned short cp) {
    if (h_state == 1) {
        if (cp == '>') { do_tag(); h_state = 0; }
        else if (h_len < sizeof(h_buf) - 1) h_buf[h_len++] = (cp < 128) ? (char)cp : '?';
        return;
    }
    if (h_state == 2) {
        if (cp == ';') { do_entity(); h_state = 0; return; }
        if (h_len < 10 && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                           (cp >= '0' && cp <= '9') || cp == '#')) {
            h_buf[h_len++] = (char)cp;
            return;
        }
        /* not an entity after all: print literally */
        h_state = 0;
        put_cp('&');
        { unsigned char i; for (i = 0; i < h_len; ++i) put_cp(h_buf[i]); }
        /* fall through and handle cp as normal text */
    }
    if (cp == '<') { h_state = 1; h_len = 0; return; }
    if (cp == '&') { h_state = 2; h_len = 0; return; }
    put_cp(cp);
}

/* ============================================================ JSON layer */
#define TG_NONE 0
#define TG_KEY 1
#define TG_CONTENT 2
#define TG_BUF 3

static unsigned char js_depth, js_expkey, js_instr, js_esc, js_uhex, js_bare;
static char js_stack[8];
static unsigned short js_ucp;
static char js_key[16];
static unsigned char js_klen;
static char js_k1[16];               /* last key at depth 1 */
static unsigned char js_target;
static char* js_dst;
static unsigned char js_dmax, js_dlen;
static unsigned char js_inlink;      /* inside a fastTextLinks entry */
static unsigned short u8cp;
static unsigned char u8need;

STATICF void choose_target(void) {
    js_target = TG_NONE;
    if (js_depth == 1) {
        js_dmax = 7;
        if (streq(js_key, "content")) { js_target = TG_CONTENT; tt_gotjson = 1; return; }
        if (streq(js_key, "prevPage"))    { js_dst = tt_prev; }
        else if (streq(js_key, "nextPage"))    { js_dst = tt_next; }
        else if (streq(js_key, "prevSubPage")) { js_dst = tt_prevsub; }
        else if (streq(js_key, "nextSubPage")) { js_dst = tt_nextsub; }
        else return;
    } else if (js_inlink && js_depth == 3 && tt_nfast < 4) {
        if (streq(js_key, "title")) { js_dst = tt_ftitle[tt_nfast]; js_dmax = 13; }
        else if (streq(js_key, "page")) { js_dst = tt_fpage[tt_nfast]; js_dmax = 7; }
        else return;
    } else {
        return;
    }
    js_target = TG_BUF;
    js_dlen = 0;
    js_dst[0] = 0;
}

STATICF void js_emit(unsigned short cp) {
    if (js_target == TG_CONTENT) { html_cp(cp); return; }
    if (js_target == TG_KEY) {
        if (js_klen < 15) { js_key[js_klen++] = (char)cp; js_key[js_klen] = 0; }
        return;
    }
    if (js_target == TG_BUF && js_dlen < js_dmax) {
        if (cp >= 0xC0 && cp <= 0xFF) cp = cp - 0xC0 + 0x80 + 0x20; /* keep font index+0x20 */
        else if (cp >= 0x80 || cp < 0x20) cp = '?';
        js_dst[js_dlen++] = (char)cp;
        js_dst[js_dlen] = 0;
    }
}

/* raw byte inside a JSON string: UTF-8 decode */
STATICF void js_strbyte(unsigned char c) {
    if (u8need) {
        if ((c & 0xC0) == 0x80) {
            u8cp = (u8cp << 6) | (c & 0x3F);
            if (--u8need == 0) js_emit(u8cp);
            return;
        }
        u8need = 0;
        js_emit('?');
    }
    if (c < 0x80) js_emit(c);
    else if ((c & 0xE0) == 0xC0) { u8cp = c & 0x1F; u8need = 1; }
    else if ((c & 0xF0) == 0xE0) { u8cp = c & 0x0F; u8need = 2; }
    else if ((c & 0xF8) == 0xF0) { u8cp = 0; u8need = 3; }  /* > BMP: shown as '?' */
    else js_emit(c);                                         /* stray Latin-1 byte */
}

STATICF void js_endbare(void) {
    if (js_bare) { js_bare = 0; js_target = TG_NONE; }
}

STATICF void json_byte(unsigned char c) {
    if (js_instr) {
        if (js_uhex) {
            unsigned char v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (lc(c) >= 'a' && lc(c) <= 'f') v = lc(c) - 'a' + 10;
            else v = 0;
            js_ucp = (js_ucp << 4) | v;
            if (--js_uhex == 0) {
                if (js_ucp >= 0xDC00 && js_ucp < 0xE000) return;      /* low surrogate */
                if (js_ucp >= 0xD800 && js_ucp < 0xDC00) js_ucp = '?';
                js_emit(js_ucp);
            }
            return;
        }
        if (js_esc) {
            js_esc = 0;
            switch (c) {
                case 'n': js_emit('\n'); break;
                case 't': js_emit(' '); break;
                case 'r': case 'b': case 'f': break;
                case 'u': js_uhex = 4; js_ucp = 0; break;
                default: js_emit(c);
            }
            return;
        }
        if (c == '\\') { js_esc = 1; return; }
        if (c == '"') {
            js_instr = 0;
            u8need = 0;
            if (js_target == TG_KEY && js_depth == 1) {
                unsigned char i;
                for (i = 0; i <= js_klen; ++i) js_k1[i] = js_key[i];
            }
            js_target = TG_NONE;
            return;
        }
        js_strbyte(c);
        return;
    }
    switch (c) {
        case '"':
            js_endbare();
            js_instr = 1;
            if (js_expkey && js_depth && js_stack[js_depth - 1] == 'o') {
                js_target = TG_KEY;
                js_klen = 0;
                js_key[0] = 0;
            } else {
                choose_target();
            }
            return;
        case ':': js_endbare(); js_expkey = 0; return;
        case ',':
            js_endbare();
            if (js_depth && js_stack[js_depth - 1] == 'o') js_expkey = 1;
            return;
        case '{': case '[':
            js_endbare();
            if (js_depth < 8) js_stack[js_depth] = (c == '{') ? 'o' : 'a';
            ++js_depth;
            js_expkey = (c == '{');
            if (c == '{' && js_depth == 3 && streq(js_k1, "fastTextLinks")) js_inlink = 1;
            return;
        case '}': case ']':
            js_endbare();
            if (js_inlink && js_depth == 3 && c == '}') {
                js_inlink = 0;
                if (tt_nfast < 4 && tt_fpage[tt_nfast][0]) ++tt_nfast;
            }
            if (js_depth) --js_depth;
            js_expkey = 0;
            return;
        case ' ': case '\t': case '\r': case '\n':
            js_endbare();
            return;
    }
    /* bare value (number/true/false/null) */
    if (!js_bare) { js_bare = 1; choose_target(); if (js_target == TG_CONTENT) js_target = TG_NONE; }
    if (c >= '0' && c <= '9') js_emit(c);
}

/* ============================================================ HTTP layer */
static unsigned char hs;              /* 0 status line, 1 headers, 2 body */
static char hl[64];
static unsigned char hll, chunked, cs;
static unsigned short csize;

STATICF void header_line(void) {
    char* p = hl;
    hl[hll] = 0;
    if (hs == 0) {                    /* "HTTP/1.1 200 OK" */
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        tt_http = 0;
        while (*p >= '0' && *p <= '9') tt_http = tt_http * 10 + (*p++ - '0');
        hs = 1;
        return;
    }
    if (!hll) { hs = 2; return; }     /* empty line: body starts */
    {
        static char* te = "transfer-encoding:";
        static char* cl = "content-length:";
        unsigned char i = 0;
        unsigned long v;
        while (cl[i] && lc(hl[i]) == cl[i]) ++i;
        if (!cl[i]) {
            while (hl[i] == ' ') ++i;
            v = 0;
            while (hl[i] >= '0' && hl[i] <= '9' && v < 65535) v = v * 10 + (hl[i++] - '0');
            tt_clen = (v > 65535) ? 65535 : (unsigned short)v;
            tt_hasclen = 1;
            return;
        }
        i = 0;
        while (te[i] && lc(hl[i]) == te[i]) ++i;
        if (!te[i]) {
            while (hl[i]) {
                if (lc(hl[i]) == 'c' && lc(hl[i + 1]) == 'h' && lc(hl[i + 2]) == 'u') chunked = 1;
                ++i;
            }
        }
    }
}

STATICF unsigned char hexval(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 255;
}

/* body byte, de-chunking if needed */
STATICF void body_byte(unsigned char c) {
    unsigned char v;
    if (!chunked) { json_byte(c); return; }
    switch (cs) {
        case 0:                        /* chunk size */
            v = hexval(c);
            if (v != 255) { csize = (csize << 4) | v; return; }
            cs = (c == '\n') ? 2 : 1;
            if (cs == 2 && !csize) cs = 4;
            return;
        case 1:                        /* extension until LF */
            if (c == '\n') cs = csize ? 2 : 4;
            return;
        case 2:                        /* data */
            json_byte(c);
            if (--csize == 0) cs = 3;
            return;
        case 3:                        /* CRLF after data */
            if (c == '\n') { cs = 0; csize = 0; }
            return;
        default:                       /* trailer: ignore */
            return;
    }
}

void tt_reset(void) {
    unsigned short i;
    memset(tt_ch, 0, TT_CELLS);
    memset(tt_at, 0, TT_CELLS);
    memset(tt_lk, 0, TT_CELLS);
    tt_nlinks = 0; tt_nfast = 0; tt_http = 0; tt_gotjson = 0; tt_rows_used = 0;
    tt_prev[0] = tt_next[0] = tt_prevsub[0] = tt_nextsub[0] = 0;
    for (i = 0; i < 4; ++i) { tt_ftitle[i][0] = 0; tt_fpage[i][0] = 0; }
    h_fg = TT_WHITE; set_pos(0, 0);
    if (!cls_ready) {
        for (i = 0x20; i < 0x7F; ++i) cls[i] = 3;
        cls['"'] = 0; cls['\\'] = 0;
        cls['<'] = 2; cls['&'] = 2;  /* not plain text, but fine inside a tag */
        cls['>'] = 1;                /* plain text, but ends a tag */
        cls_ready = 1;
    }
    h_fg = TT_WHITE; h_bg = TT_BLACK; h_link = 0; h_sp = 0; h_state = 0;
    js_depth = js_expkey = js_instr = js_esc = js_uhex = js_bare = js_inlink = 0;
    js_target = TG_NONE; js_k1[0] = 0; u8need = 0;
    hs = 0; hll = 0; chunked = 0; cs = 0; csize = 0;
    tt_clen = 0; tt_body = 0; tt_hasclen = 0;
}

extern unsigned short tt_fastrun(char* buf, unsigned short len, unsigned char mode);

#ifdef TT_HOST
/* C model of ttfast.s (same behaviour), used for PC testing */
static unsigned char hexv(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 255;
}
static void fput(unsigned char ch, unsigned char at) {
    if (h_room) { tt_ch[h_idx] = ch; tt_at[h_idx] = h_attr | at; tt_lk[h_idx] = h_link; ++h_idx; --h_room; }
    ++h_col;
}
static void fbuf(unsigned char c) { if (h_len < 99) h_buf[h_len++] = c; }
unsigned short tt_fastrun(char* buf, unsigned short len, unsigned char mode) {
    unsigned char* p = (unsigned char*)buf;
    unsigned char* start = p;
    unsigned char d, e;
    while (len) {
        if (cls[*p] & mode) {
            if (mode == 1) fput(*p - 0x20, 0); else fbuf(*p);
            ++p; --len; continue;
        }
        if (*p != '\\' || len < 2) break;
        if (p[1] == '"' || p[1] == '/') {
            if (mode == 1) fput(p[1] - 0x20, 0); else fbuf(p[1]);
            p += 2; len -= 2; continue;
        }
        if (p[1] != 'u' || mode != 1 || len < 6) break;
        if ((p[2] | 0x20) != 'f' || p[3] != '0') break;
        d = hexv(p[4]); if (d == 255 || (d & 0xFA) != 2) break;
        e = hexv(p[5]); if (e == 255) break;
        fput(e | ((d & 1) << 4) | ((d & 4) ? 0x20 : 0), AT_MOSAIC);
        p += 6; len -= 6;
    }
    return (unsigned short)(p - start);
}
#endif

/* 1 when the complete HTTP body has arrived (Content-Length or last chunk) */
unsigned char tt_complete(void) {
    if (hs != 2) return 0;
    if (chunked) return cs == 4;
    return tt_hasclen && tt_body >= tt_clen;
}

void tt_feed(char* buf, unsigned short len) {
    unsigned char c, fast;
    unsigned short n, total, hb;
    total = len;
    hb = 0;
    fast = 0;
    while (len) {
#ifndef TT_NOFAST
        /* fast path (ttfast.s): plain text / tag bytes inside "content" */
        if (fast) {
            n = tt_fastrun(buf, len, fast);
            buf += n;
            len -= n;
            if (!len) break;
        }
#endif
        c = (unsigned char)*buf++;
        --len;
#ifndef TT_NOFAST
        /* shortcuts for tag boundaries and line breaks inside "content"
           (same effect as the full JSON -> HTML path below) */
        if (fast == 1) {
            if (c == '<') { h_state = 1; h_len = 0; fast = 2; continue; }
            if (c == '\\' && len && *buf == 'n') { ++buf; --len; newline(); continue; }
        } else if (fast == 2 && c == '>') {
            do_tag(); h_state = 0; h_attr = h_fg | (h_bg << 3); fast = 1; continue;
        }
#endif
        fast = 0;
        if (hs < 2) {
            ++hb;
            if (c == '\r') continue;
            if (c == '\n') { header_line(); hll = 0; continue; }
            if (hll < sizeof(hl) - 1) hl[hll++] = c;
            continue;
        }
        body_byte(c);
        if (!chunked && js_instr && js_target == TG_CONTENT && !js_esc && !js_uhex && !u8need) {
            if (h_state == 0) { fast = 1; h_attr = h_fg | (h_bg << 3); }
            else if (h_state == 1) fast = 2;
        }
    }
    if (hs == 2) tt_body += total - hb;
}

/* write plain ASCII text into the grid (status / error screens) */
void tt_text(unsigned char row, unsigned char col, char* t, unsigned char fg, unsigned char bg) {
    set_pos(row, col);
    h_fg = fg; h_bg = bg; h_link = 0;
    while (*t) {
        unsigned char c = (unsigned char)*t++;
        put_cell((c >= 0x20 && c < 0x7F) ? c - 0x20 : '?' - 0x20, 0);
    }
}

/* add fast-text bar on row 24 if the page itself did not use it */
void tt_finish(void) {
    static unsigned char fc[4] = {TT_RED, TT_GREEN, TT_YELLOW, TT_CYAN};
    unsigned char i, j;
    char* t;
    if (!tt_nfast) return;
    for (i = 0; i < TT_COLS; ++i)               /* page uses row 24 itself? */
        if (tt_ch[24 * TT_COLS + i] || (tt_at[24 * TT_COLS + i] & 0x38)) return;
    for (i = 0; i < tt_nfast; ++i) {
        if (tt_nlinks >= TT_MAXLNK) break;
        for (j = 0; j < 8; ++j) tt_links[tt_nlinks][j] = tt_fpage[i][j];
        ++tt_nlinks;
        set_pos(24, i * 10); h_fg = fc[i]; h_bg = TT_BLACK; h_link = tt_nlinks;
        t = tt_ftitle[i][0] ? tt_ftitle[i] : tt_fpage[i];
        for (j = 0; j < 9 && t[j]; ++j) put_cell((unsigned char)(t[j] - 0x20), 0);
        while (h_col < i * 10 + 10) put_cell(0, 0);
    }
}

/* link page for a cell: explicit <a href>, else a 3-digit number under it */
char* tt_link_at(unsigned char col, unsigned char row) {
    static char num[4];
    unsigned short i = (unsigned short)row * TT_COLS + col;
    unsigned char a, b, k;
    if (row >= TT_ROWS || col >= TT_COLS) return 0;
    if (tt_lk[i]) return tt_links[tt_lk[i] - 1];
    #define ISDIG(n) (!(tt_at[n] & AT_MOSAIC) && tt_ch[n] >= 0x10 && tt_ch[n] <= 0x19)
    if (!ISDIG(i)) return 0;
    a = col; b = col;
    while (a > 0 && ISDIG(i - (col - a) - 1)) --a;
    while (b < TT_COLS - 1 && ISDIG(i + (b - col) + 1)) ++b;
    if (b - a != 2) return 0;
    for (k = 0; k < 3; ++k) num[k] = (char)(tt_ch[(unsigned short)row * TT_COLS + a + k] + 0x20);
    num[3] = 0;
    if (num[0] < '1' || num[0] > '8') return 0;
    return num;
}

/* ============================================================ rendering */
/* 6x7 cells, canvas split: rows 0-12 in 'top', rows 13-24 in 'bot'.
   dat = canvas + 24, bw = bytes per canvas line (canvas[0]).               */

extern const unsigned char ttfont3[];
STATICF unsigned char cell_row_mask(unsigned char ch, unsigned char at, unsigned char r) {
    unsigned char m = 0, sep;
    if (!(at & AT_MOSAIC)) {                 /* rebuild 6-bit row from ttfont3 */
        unsigned char* f = (unsigned char*)ttfont3 + (unsigned short)ch * 21 + r * 3;
        return ((f[0] & 0xF0) ? 0x20 : 0) | ((f[0] & 0x0F) ? 0x10 : 0)
             | ((f[1] & 0xF0) ? 0x08 : 0) | ((f[1] & 0x0F) ? 0x04 : 0)
             | ((f[2] & 0xF0) ? 0x02 : 0) | ((f[2] & 0x0F) ? 0x01 : 0);
    }
    sep = at & AT_SEP;
    if (r < 2)      { if (sep && r == 1) return 0; if (ch & 1) m |= 0x38; if (ch & 2) m |= 0x07; }
    else if (r < 5) { if (sep && r == 4) return 0; if (ch & 4) m |= 0x38; if (ch & 8) m |= 0x07; }
    else            { if (sep && r == 6) return 0; if (ch & 16) m |= 0x38; if (ch & 32) m |= 0x07; }
    if (sep) m &= 0x36;
    return m;
}

/* 16-colour fast path: every glyph row is pre-expanded into 3 nibble-mask
   bytes (00/0F/F0/FF); the Z80 routine tt_cell16 (ttcell.s) writes
   out = bgfill ^ ((fg^bg) & mask). Mosaic patterns are expanded at start-up. */
extern const unsigned char ttfont3[];
extern unsigned short tt_step, tt_rowskip;
extern void tt_render16a(unsigned char* top, unsigned char* bot, unsigned char* pal);

extern const unsigned char ttmos3[];
static unsigned char zero21[21];

#ifdef TT_HOST                              /* C version for PC testing */
unsigned short tt_step = 118, tt_rowskip = 720;
void tt_cell16(unsigned short attr, unsigned char* dst, unsigned char* src) {
    unsigned char fill = attr & 0xFF, x = attr >> 8, r;
    for (r = 0; r < 7; ++r) {
        dst[0] = fill ^ (x & src[0]);
        dst[1] = fill ^ (x & src[1]);
        dst[2] = fill ^ (x & src[2]);
        src += 3;
        dst += tt_step + 2;
    }
}
#endif

/* 16-colour canvases: 2 pixels per byte, left pixel in high nibble */
void tt_render16(unsigned char* top, unsigned char* bot, unsigned char bw, unsigned char* pal) {
#ifndef TT_HOST
    tt_step = bw - 2;
    tt_rowskip = 7 * (unsigned short)bw - TT_COLS * 3;
    tt_render16a(top, bot, pal);
#else
    unsigned char row, col, at, ch, f, b, lastat;
    unsigned short attr = 0;
    unsigned char* line;
    unsigned char* src;
    unsigned char* chp = tt_ch;
    unsigned char* atp = tt_at;
    tt_step = bw - 2;
    lastat = 0xFF;
    line = top;
    for (row = 0; row < TT_ROWS; ++row) {
        if (row == 13) line = bot;
        for (col = 0; col < TT_COLS; ++col) {
            at = *atp++;
            ch = *chp++;
            if (at != lastat) {
                lastat = at;
                f = pal[at & 7]; b = pal[(at >> 3) & 7];
                attr = ((unsigned short)(((f ^ b) << 4) | (f ^ b)) << 8) | ((b << 4) | b);
            }
            if (!ch)
                src = zero21;
            else if (at & AT_MOSAIC)
                src = (unsigned char*)ttmos3 + (unsigned short)((at & AT_SEP) ? ch + 64 : ch) * 21;
            else
                src = (unsigned char*)ttfont3 + (unsigned short)ch * 21;
            tt_cell16(attr, line, src);
            line += 3;
        }
        line += 7 * (unsigned short)bw - TT_COLS * 3;   /* next cell row */
    }
#endif
}

/* 4-colour canvases (CPC-style: 4 pixels per byte, bit0 at 7-p, bit1 at 3-p) */
void tt_render4(unsigned char* top, unsigned char* bot, unsigned char bw, unsigned char* pal) {
    unsigned char row, col, r, m, at, f, b, k, c, px;
    unsigned char* line;
    unsigned char* p;
    unsigned short i = 0, x;
    for (row = 0; row < TT_ROWS; ++row) {
        line = (row < 13) ? top + (unsigned short)row * 7 * bw
                          : bot + (unsigned short)(row - 13) * 7 * bw;
        for (col = 0; col < TT_COLS; ++col, ++i) {
            at = tt_at[i];
            f = pal[at & 7] & 3; b = pal[(at >> 3) & 7] & 3;
            for (r = 0; r < 7; ++r) {
                m = cell_row_mask(tt_ch[i], at, r);
                for (k = 0; k < 6; ++k) {
                    x = col * 6 + k;
                    p = line + (unsigned short)r * bw + (x >> 2);
                    px = x & 3;
                    c = (m & (0x20 >> k)) ? f : b;
                    *p = (*p & ~((0x80 >> px) | (0x08 >> px)))
                       | ((c & 1) ? (0x80 >> px) : 0) | ((c & 2) ? (0x08 >> px) : 0);
                }
            }
        }
    }
}
