/* ===========================================================================
   TELETEKS - NOS Teletekst for SymbOS
   SymbOS port of MSXTTPP (rubikonlab.com), written for the SymbOS C Compiler.

   Connection modes (Setup):
     Auto  - try HTTPS directly (needs TLS-capable network hardware, e.g.
             ESP8266-UNAPI / MSXPico); if that fails, use the relay.
     HTTPS - direct HTTPS only.
     Relay - plain HTTP to ttrelay.py on a computer in your own network
             (for DenYoNet, GR8NET, openMSX, ... without TLS).
   Settings are stored in TELETEKS.CFG next to TELETEKS.EXE.

   Keys: Left/Right = previous/next page, Up/Down = sub pages,
         F1-F4 = fast text (red/green/yellow/cyan), R = reload,
         0-9 = type a page number. Click a page number on screen to go there.

   Build: cc -N "Teletekst" -h 512 -o teleteks.exe teleteks.c ttcore.c ttcell.s ttfast.s -lnet -lgfx
   =========================================================================== */

#include <symbos.h>
#include <string.h>
#include <stdlib.h>
#include <network.h>
#include <graphics.h>

#include "ttcore.h"

#define VERSION "1.3"
#define NOS_HOST "teletekst-data.nos.nl"

#define MODE_AUTO  1
#define MODE_HTTPS 2
#define MODE_RELAY 3

/* library internals (libnet) used for the streaming receive loop */
extern unsigned char Net_Wait(unsigned char id);
extern char _netmsg[];

/* ------------------------------------------------------------- canvases */
#define CV_W   240
#define CV_HT  91     /* rows 0-12  */
#define CV_HB  84     /* rows 13-24 */
#define CV_X   38     /* canvas position in window (controls column on the left) */
#define CV_Y   0
_data     char cv_top[CV_W * CV_HT / 2 + 24];
_transfer char cv_bot[CV_W * CV_HB / 2 + 24];

/* teletext colour -> SymbOS palette (16 colours) */
unsigned char pal16[8] = {COLOR_BLACK, COLOR_LRED, COLOR_LGREEN, COLOR_LYELLOW,
                          COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_WHITE};
/* 4-colour fallback: 0 light, 1 black, 2 orange, 3 red */
unsigned char pal4[8] = {1, 3, 2, 0, 3, 3, 0, 0};
unsigned char colours16;

/* ------------------------------------------------------------- settings */
unsigned char cfg_mode = MODE_AUTO;
char cfg_host[64];
unsigned short cfg_port = 8080;
char cfg_path[256];
char log_path[256];
unsigned char cfg_found;        /* TELETEKS.CFG existed at start-up */
_transfer char cfg_log = 1;     /* write TELETEKS.LOG (checkbox in Setup) */
unsigned char log_started;
unsigned char https_failed;     /* Auto mode: HTTPS already failed this session */

/* ------------------------------------------------------------- buffers */
_transfer char rxbuf[1024];
_transfer char req[200];
char page[8] = "100";
char ip[4];

/* ======================================================= main window ==== */
_transfer char icon[19] = {0x02, 0x08, 0x08, 0xFF, 0xFF, 0xF8, 0xF1, 0xF8, 0xF1,
                           0x8F, 0x1F, 0x8F, 0x1F, 0x8F, 0x1F, 0x8F, 0x1F, 0xFF, 0xFF};
/* NOTE: no string initialisers on _data/_transfer arrays - SCC gives them
   zero size there, so they overlap the next variable. Filled in main(). */
_transfer char title[40];
_transfer char modetext[8];

_data char pagebuf[8];
_transfer Ctrl_Input cd_page = {pagebuf, 0, 3, 0, 3, 5, 0, 0, 0};
_transfer Ctrl_Text  cd_mode = {modetext, (COLOR_BLACK << 2) | COLOR_ORANGE, ALIGN_CENTER};

/* control IDs equal their index in the group */
#define ID_PREV   1
#define ID_PAGE   2
#define ID_GO     3
#define ID_NEXT   4
#define ID_SUBP   5
#define ID_SUBN   6
#define ID_RELOAD 7
#define ID_SETUP  8
#define ID_MODE   9
#define ID_TOP    10
#define ID_BOT    11
_transfer Ctrl c_bg     = {0,  C_AREA,   -1, COLOR_ORANGE, 0, 0, 10000, 10000};
/* left-hand control column, 36 px wide */
_transfer Ctrl c_prev   = {1,  C_BUTTON, -1, (unsigned short)"<",      1,  29, 17, 12};
_transfer Ctrl c_page   = {2,  C_INPUT,  -1, (unsigned short)&cd_page, 1,   1, 36, 12};
_transfer Ctrl c_go     = {3,  C_BUTTON, -1, (unsigned short)"Go",     1,  15, 36, 12};
_transfer Ctrl c_next   = {4,  C_BUTTON, -1, (unsigned short)">",      20, 29, 17, 12};
_transfer Ctrl c_subp   = {5,  C_BUTTON, -1, (unsigned short)"Sub<",   1,  45, 36, 12};
_transfer Ctrl c_subn   = {6,  C_BUTTON, -1, (unsigned short)"Sub>",   1,  59, 36, 12};
_transfer Ctrl c_reload = {7,  C_BUTTON, -1, (unsigned short)"Reload", 1,  75, 36, 12};
_transfer Ctrl c_setup  = {8,  C_BUTTON, -1, (unsigned short)"Setup",  1,  89, 36, 12};
_transfer Ctrl c_mode   = {9,  C_TEXT,   -1, (unsigned short)&cd_mode, 1, 106, 36, 8};
_transfer Ctrl c_top    = {10, C_IMAGE_EXT, -1, (unsigned short)cv_top, CV_X, CV_Y,         CV_W, CV_HT};
_transfer Ctrl c_bot    = {11, C_IMAGE_EXT, -1, (unsigned short)cv_bot, CV_X, CV_Y + CV_HT, CV_W, CV_HB};

_transfer Ctrl_Group cg_main = {12, 0, &c_bg, 0, 0, ID_GO + 1, 0};

_transfer Window win_main = {
    WIN_NORMAL,
    WIN_CLOSE | WIN_TITLE | WIN_ICON | WIN_RESIZABLE,
    0,
    8, 1,                         /* x, y */
    CV_X + CV_W, CV_HT + CV_HB,   /* w, h (reduced at start-up on small screens) */
    0, 0,
    CV_X + CV_W, CV_HT + CV_HB,   /* full content size */
    100, 60,                      /* minimum size */
    CV_X + CV_W, CV_HT + CV_HB,   /* maximum size */
    icon, title, 0, 0, &cg_main};

/* ======================================================= setup window === */
_transfer char sel_mode = MODE_AUTO;
_transfer char radiocoord[4] = {-1, -1, -1, -1};
_transfer Ctrl_Radio cd_r1 = {&sel_mode, "Auto: HTTPS, else relay", (COLOR_BLACK << 2) | COLOR_ORANGE, MODE_AUTO,  radiocoord};
_transfer Ctrl_Radio cd_r2 = {&sel_mode, "Direct HTTPS only",       (COLOR_BLACK << 2) | COLOR_ORANGE, MODE_HTTPS, radiocoord};
_transfer Ctrl_Radio cd_r3 = {&sel_mode, "Relay only (plain HTTP)", (COLOR_BLACK << 2) | COLOR_ORANGE, MODE_RELAY, radiocoord};
_transfer Ctrl_Text  cd_t1 = {"Connection:",                  (COLOR_BLACK << 2) | COLOR_ORANGE, ALIGN_LEFT};
_transfer Ctrl_Text  cd_t2 = {"Relay IP or name:",            (COLOR_BLACK << 2) | COLOR_ORANGE, ALIGN_LEFT};
_transfer Ctrl_Text  cd_t3 = {"Port:",                        (COLOR_BLACK << 2) | COLOR_ORANGE, ALIGN_LEFT};
_transfer Ctrl_Text  cd_t4 = {"Relay = ttrelay.py on a PC in", (COLOR_RED << 2) | COLOR_ORANGE, ALIGN_LEFT};
_transfer Ctrl_Text  cd_t5 = {"your network (e.g. 192.168.1.20)", (COLOR_RED << 2) | COLOR_ORANGE, ALIGN_LEFT};
_data char hostbuf[64];
_data char portbuf[6];
_transfer Ctrl_Input cd_host = {hostbuf, 0, 0, 0, 0, 63, 0, 0, 0};
_transfer Ctrl_Input cd_port = {portbuf, 0, 0, 0, 0, 5, 0, 0, 0};
_transfer Ctrl_Check cd_log  = {&cfg_log, "Write log file TELETEKS.LOG", (COLOR_BLACK << 2) | COLOR_ORANGE};

#define SID_LOG    11
#define SID_OK     12
#define SID_CANCEL 13
_transfer Ctrl s_bg     = {0,  C_AREA,   -1, COLOR_ORANGE, 0, 0, 10000, 10000};
_transfer Ctrl s_t1     = {1,  C_TEXT,   -1, (unsigned short)&cd_t1, 6, 5, 150, 8};
_transfer Ctrl s_r1     = {2,  C_RADIO,  -1, (unsigned short)&cd_r1, 12, 16, 150, 8};
_transfer Ctrl s_r2     = {3,  C_RADIO,  -1, (unsigned short)&cd_r2, 12, 26, 150, 8};
_transfer Ctrl s_r3     = {4,  C_RADIO,  -1, (unsigned short)&cd_r3, 12, 36, 150, 8};
_transfer Ctrl s_t2     = {5,  C_TEXT,   -1, (unsigned short)&cd_t2, 6, 51, 150, 8};
_transfer Ctrl s_host   = {6,  C_INPUT,  -1, (unsigned short)&cd_host, 6, 61, 120, 12};
_transfer Ctrl s_t3     = {7,  C_TEXT,   -1, (unsigned short)&cd_t3, 132, 51, 40, 8};
_transfer Ctrl s_port   = {8,  C_INPUT,  -1, (unsigned short)&cd_port, 132, 61, 42, 12};
_transfer Ctrl s_t4     = {9,  C_TEXT,   -1, (unsigned short)&cd_t4, 6, 78, 170, 8};
_transfer Ctrl s_t5     = {10, C_TEXT,   -1, (unsigned short)&cd_t5, 6, 87, 170, 8};
_transfer Ctrl s_log    = {11, C_CHECK,  -1, (unsigned short)&cd_log, 6, 100, 170, 8};
_transfer Ctrl s_ok     = {12, C_BUTTON, -1, (unsigned short)"OK",     70, 114, 48, 12};
_transfer Ctrl s_cancel = {13, C_BUTTON, -1, (unsigned short)"Cancel", 126, 114, 48, 12};
_transfer Ctrl_Group cg_setup = {14, 0, &s_bg, 0, 0, SID_OK + 1, SID_CANCEL + 1};

_transfer Window win_setup = {
    WIN_NORMAL,
    WIN_CLOSE | WIN_TITLE | WIN_ICON,
    0,
    30, 30,
    180, 131,
    0, 0,
    180, 131, 180, 131, 180, 131,
    icon, "Teletekst setup", 0, 0, &cg_setup};

unsigned char wid_main, wid_setup = 255;
unsigned char net_ok;            /* network daemon found */
static void setup_open(void);

/* ================================================================ config */
static void cfg_load(void) {
    unsigned char fd;
    unsigned short n;
    char* p;
    char* v;
    Dir_PathAdd(0, "TELETEKS.CFG", cfg_path);
    Dir_PathAdd(0, "TELETEKS.LOG", log_path);
    cfg_host[0] = 0;
    fd = File_Open(_symbank, cfg_path);
    if (fd > 7) return;
    cfg_found = 1;
    n = File_Read(fd, _symbank, rxbuf, sizeof(rxbuf) - 1);
    File_Close(fd);
    rxbuf[n] = 0;
    p = rxbuf;
    while (*p) {
        v = p;
        while (*p && *p != '\r' && *p != '\n') ++p;
        if (*p) *p++ = 0;
        if (!strncmp(v, "mode=", 5)) {
            cfg_mode = v[5] - '0';
            if (cfg_mode < MODE_AUTO || cfg_mode > MODE_RELAY) cfg_mode = MODE_AUTO;
        } else if (!strncmp(v, "relay=", 6)) {
            strncpy(cfg_host, v + 6, 63);
            cfg_host[63] = 0;
        } else if (!strncmp(v, "port=", 5)) {
            cfg_port = (unsigned short)atoi(v + 5);
            if (!cfg_port) cfg_port = 8080;
        } else if (!strncmp(v, "log=", 4)) {
            cfg_log = (v[4] == '1');
        }
    }
}

static void cfg_save(void) {
    unsigned char fd;
    fd = File_New(_symbank, cfg_path, 0);
    if (fd > 7) return;
    strcpy(rxbuf, "mode=");
    rxbuf[5] = '0' + cfg_mode;
    rxbuf[6] = 0;
    strcat(rxbuf, "\r\nrelay=");
    strcat(rxbuf, cfg_host);
    strcat(rxbuf, "\r\nport=");
    uitoa(cfg_port, rxbuf + strlen(rxbuf), 10);
    strcat(rxbuf, cfg_log ? "\r\nlog=1\r\n" : "\r\nlog=0\r\n");
    File_Write(fd, _symbank, rxbuf, strlen(rxbuf));
    File_Close(fd);
}

/* ================================================================ log */
/* Each line is written and the file closed at once, so after a freeze the
   last line in TELETEKS.LOG shows how far the program got. */
_transfer char logbuf[96];

static void log_line(void) {
    unsigned char fd;
    if (!cfg_log) return;
    strcat(logbuf, "\r\n");
    if (!log_started) {
        fd = File_New(_symbank, log_path, 0);
        log_started = 1;
    } else {
        fd = File_Open(_symbank, log_path);
        if (fd <= 7) File_Seek(fd, 0, SEEK_END);
    }
    if (fd > 7) return;
    File_Write(fd, _symbank, logbuf, strlen(logbuf));
    File_Close(fd);
}

static void logs(char* a, char* b) {
    if (!cfg_log) return;
    strncpy(logbuf, a, 40); logbuf[40] = 0;
    if (b) { strncat(logbuf, b, 40); }
    log_line();
}

static void logn(char* a, int v) {
    if (!cfg_log) return;
    strncpy(logbuf, a, 60); logbuf[60] = 0;
    itoa(v, logbuf + strlen(logbuf), 10);
    log_line();
}

/* ================================================================ UI bits */
static void set_input(Ctrl_Input* ci, char* buf, char* text, unsigned char max) {
    strncpy(buf, text, max);
    buf[max] = 0;
    ci->len = strlen(buf);
    ci->cursor = ci->len;
    ci->scroll = 0;
    ci->selection = 0;
}

static void set_title(char* s1, char* s2) {
    strcpy(title, "Teletekst ");
    strcat(title, s1);
    if (s2) strcat(title, s2);
    Win_Redraw_Title(wid_main);
}

static void show_mode(void) {
    if (cfg_mode == MODE_RELAY || (cfg_mode == MODE_AUTO && https_failed)) strcpy(modetext, "Relay");
    else strcpy(modetext, "HTTPS");
    Win_Redraw(wid_main, ID_MODE, 0);
}

static void redraw_page(void) {
    if (colours16)
        tt_render16(cv_top + 24, cv_bot + 24, cv_top[0], pal16);
    else
        tt_render4(cv_top + 24, cv_bot + 24, cv_top[0], pal4);
    Win_Redraw(wid_main, -2, ID_TOP);
}

/* show a message on the teletext screen (no pop-up window) */
static void show_msg(char* l1, char* l2, char* l3) {
    tt_reset();
    tt_text(0, 0, " TELETEKS " VERSION "  NOS Teletekst for SymbOS", 7, 4);
    if (l1) tt_text(9, 1, l1, 3, 0);
    if (l2) tt_text(11, 1, l2, 7, 0);
    if (l3) tt_text(13, 1, l3, 7, 0);
    tt_text(24, 0, " Setup: connection   Reload: try again  ", 0, 6);
    redraw_page();
    logs("msg: ", l1);
}

static char* net_errtext(void) {
    switch (_neterr) {
        case ERR_NOFUNC:   return "Network hardware has no HTTPS (TLS).";
        case ERR_TIMEOUT:  return "Connection timed out.";
        case ERR_CONNECT:  return "Connection refused / not possible.";
        case ERR_BADDOMAIN:return "Server name could not be resolved.";
        case ERR_OFFLINE:  return "No network daemon running.";
        case ERR_NOIP:     return "Network has no IP configuration.";
        case ERR_NOHW:     return "No network hardware found.";
        case ERR_NOSOCKET: return "No free network socket.";
        case ERR_RESPONSE: return "No valid answer (HTTPS not supported?).";
    }
    return "Network error.";
}

/* ================================================================ network */
static signed char receive_all(unsigned char sock) {
    NetStat st;
    TCP_Trans tr;
    for (;;) {
        if (Net_Wait(NET_TCPEVT))           /* sets _neterr (e.g. timeout) */
            goto fail;
        if ((unsigned char)_netmsg[3] != sock)
            continue;                       /* stale event of an old socket */
        /* drain everything that is buffered right now */
        for (;;) {
            if (TCP_Status(sock, &st))
                goto fail;
            if (!st.bytesrec)
                break;
            if (TCP_Receive(sock, _symbank, rxbuf, sizeof(rxbuf), &tr))
                goto fail;
            if (!tr.transferred)
                break;
            tt_feed(rxbuf, tr.transferred);
        }
        if (st.status == TCP_CLOSING || st.status == TCP_CLOSED) {
            TCP_Close(sock);
            return 0;
        }
        if (tt_complete()) {                /* whole body received: don't wait */
            TCP_Disconnect(sock);           /* for the server to hang up      */
            return 0;
        }
    }
fail:
    TCP_Disconnect(sock);
    return -1;
}

/* returns 0 on success, -1 on network error (see _neterr) */
static signed char request(unsigned char relay) {
    signed char sock;
    char* host = relay ? cfg_host : NOS_HOST;
    char num[16];
    logs(relay ? "dns relay " : "dns ", host);
    if (DNS_Resolve(_symbank, host, ip)) {
        logn("  dns failed, err ", _neterr);
        return -1;
    }
    logs("  ip ", iptoa(ip, num));
    if (relay) {
        logn("open tcp port ", cfg_port);
        sock = TCP_OpenClient(ip, -1, cfg_port);
    } else {
        logs("open tls 443", 0);
        sock = SSL_OpenClient(ip, -1, 443, _symbank, NOS_HOST, 0);
    }
    if (sock < 0) {
        logn("  open failed, err ", _neterr);
        return -1;
    }
    logn("  socket ", sock);
    /* same request as MSXTT 0.9.229 */
    strcpy(req, "GET /json/");
    strcat(req, page);
    strcat(req, " HTTP/1.0\r\nHost: ");
    strcat(req, NOS_HOST);
    strcat(req, "\r\nConnection: close\r\n\r\n");
    if (TCP_Send((unsigned char)sock, _symbank, req, strlen(req))) {
        logn("  send failed, err ", _neterr);
        TCP_Disconnect((unsigned char)sock);
        return -1;
    }
    logs("  sent, receiving", 0);
    tt_reset();
    if (receive_all((unsigned char)sock)) {
        logn("  receive failed, err ", _neterr);
        logn("  body bytes ", tt_body);
        return -1;
    }
    logn("  http ", tt_http);
    logn("  body bytes ", tt_body);
    logn("  content found ", tt_gotjson);
    return 0;
}

static void load_page(char* p) {
    signed char r = -1;
    unsigned char use_relay;
    char num[8];

    if (!p || !p[0]) return;
    strncpy(page, p, 7);
    page[7] = 0;
    logs("load ", page);
    set_input(&cd_page, pagebuf, page, 5);
    Win_Redraw(wid_main, ID_PAGE, 0);
    set_title(page, " - loading...");

    use_relay = (cfg_mode == MODE_RELAY) || (cfg_mode == MODE_AUTO && https_failed);
    if (!use_relay) {
        r = request(0);
        if (!r && !tt_http) {           /* no HTTP answer at all: older drivers */
            r = -1;                     /* may not report missing TLS as error */
            _neterr = ERR_RESPONSE;
        }
        if (r && cfg_mode == MODE_AUTO) {
            https_failed = 1;           /* don't try HTTPS again this session */
            show_mode();
            if (!cfg_host[0]) {
                set_title(page, " - HTTPS failed");
                show_msg(net_errtext(),
                         "Run ttrelay.py on a PC in your network",
                         "and enter its IP address in Setup.");
                setup_open();
                return;
            }
            use_relay = 1;
        }
    }
    if (use_relay) {
        if (!cfg_host[0]) {
            set_title(page, " - no relay set");
            show_msg("No relay address configured.",
                     "Run ttrelay.py on a PC in your network",
                     "and enter its IP address in Setup.");
            setup_open();
            return;
        }
        set_title(page, " - via relay...");
        r = request(1);
    }
    if (r) {
        set_title(page, " - error");
        show_msg(net_errtext(),
                 use_relay ? "Is ttrelay.py running on the relay PC?" : "HTTPS connection failed.",
                 use_relay ? "Check relay IP and port in Setup." : "Try Relay mode in Setup.");
        return;
    }
    if (tt_http != 200 || !tt_gotjson) {
        set_title(page, " - not available");
        strcpy(num, "HTTP ");
        itoa(tt_http, num + 5, 10);
        if (!tt_gotjson) {
            show_msg("Page not available.", tt_http ? num : "No valid response.", 0);
            return;
        }
    }
    tt_finish();
    logs("render", 0);
    redraw_page();
    set_title(page, 0);
    logs("done", 0);
}

/* ================================================================ setup */
static void setup_open(void) {
    char num[8];
    if (wid_setup != 255) return;
    logs("setup open", 0);
    sel_mode = cfg_mode;
    radiocoord[0] = radiocoord[1] = radiocoord[2] = radiocoord[3] = -1;
    set_input(&cd_host, hostbuf, cfg_host, 63);
    uitoa(cfg_port, num, 10);
    set_input(&cd_port, portbuf, num, 5);
    win_setup.flags |= WIN_MODAL;
    wid_setup = Win_Open(_symbank, &win_setup);
    win_main.modal = wid_setup + 1;
}

static void setup_close(unsigned char save) {
    unsigned short port;
    if (save) {
        cfg_mode = sel_mode;
        strncpy(cfg_host, hostbuf, 63);
        cfg_host[63] = 0;
        port = (unsigned short)atoi(portbuf);
        cfg_port = port ? port : 8080;
        https_failed = 0;
        cfg_save();
        cfg_found = 1;
        logn("setup saved, mode ", cfg_mode);
        logs("  relay ", cfg_host);
        logn("  port ", cfg_port);
    } else {
        logs("setup cancelled", 0);
    }
    win_main.modal = 0;
    win_setup.flags &= ~WIN_MODAL;
    Win_Close(wid_setup);
    wid_setup = 255;
    show_mode();
    if (save && net_ok) load_page(page);
}

/* ================================================================ input */
static unsigned char typed;     /* digits typed directly on the page */

static void key(unsigned char k) {
    if (k >= '0' && k <= '9') {
        if (typed == 0 || typed >= 3) { pagebuf[0] = 0; typed = 0; }
        pagebuf[typed++] = k;
        pagebuf[typed] = 0;
        set_input(&cd_page, pagebuf, pagebuf, 5);
        Win_Redraw(wid_main, ID_PAGE, 0);
        if (typed == 3) load_page(pagebuf);
        return;
    }
    typed = 0;
    switch (k) {
        case KEY_LEFT:  load_page(tt_prev); break;
        case KEY_RIGHT: load_page(tt_next); break;
        case KEY_UP:    load_page(tt_prevsub); break;
        case KEY_DOWN:  load_page(tt_nextsub); break;
        case KEY_F1: case KEY_F2: case KEY_F3: case KEY_F4:
            if (k - KEY_F1 < tt_nfast) load_page(tt_fpage[k - KEY_F1]);
            break;
        case 'r': case 'R': load_page(page); break;
    }
}

static void click_canvas(int x, int y) {
    char* l;
    x -= CV_X;
    if (x < 0 || y < CV_Y) return;
    l = tt_link_at((unsigned char)(x / 6), (unsigned char)((y - CV_Y) / 7));
    if (l) load_page(l);
}

/* ================================================================ main */
int main(int argc, char* argv[]) {
    unsigned char act;

    strcpy(title, "NOS Teletekst");
    strcpy(pagebuf, "100");
    cfg_load();
    colours16 = (Screen_Colors() == 16);
    Gfx_Init(cv_top, CV_W, CV_HT);
    Gfx_Init(cv_bot, CV_W, CV_HB);
    Gfx_Clear(cv_top, COLOR_BLACK);
    Gfx_Clear(cv_bot, COLOR_BLACK);
    if (!colours16) {               /* 4-colour mode: black = 1 */
        Gfx_Clear(cv_top, 1);
        Gfx_Clear(cv_bot, 1);
    }

    /* small screens (e.g. CPC 200 lines): shrink the window, it scrolls */
    {
        unsigned short sh = Screen_Height();
        /* title bar + borders + taskbar take about 30 lines */
        if (sh && sh < CV_HT + CV_HB + 30) win_main.h = sh - 30;
    }
    logs("TELETEKS " VERSION, 0);
    logn("colours ", Screen_Colors());
    logn("screen height ", Screen_Height());
    logn("config found ", cfg_found);
    logn("mode ", cfg_mode);
    logs("relay ", cfg_host);
    wid_main = Win_Open(_symbank, &win_main);
    logn("window ", wid_main);
    show_mode();

    if (Net_Init()) {
        logn("net init failed, err ", _neterr);
        set_title("- no network", 0);
        show_msg("No network daemon found.", "Start the SymbOS network daemon", "first, then start Teletekst again.");
    } else {
        net_ok = 1;
        logn("net daemon pid ", _netpid);
        if (!cfg_found) {
            /* first start: choose the connection before any network access */
            show_msg("Welcome! Choose the connection in Setup.",
                     "HTTPS needs TLS-capable network hardware;",
                     "otherwise use the relay (ttrelay.py).");
            setup_open();
        } else {
            load_page("100");
        }
    }

    for (;;) {
        _symmsg[0] = 0;
        Msg_Sleep(_sympid, -1, _symmsg);
        if (_symmsg[0] != MSR_DSK_WCLICK)
            continue;
        act = _symmsg[2];

        if (wid_setup != 255 && _symmsg[1] == wid_setup) {
            if (act == DSK_ACT_CLOSE) setup_close(0);
            else if (act == DSK_ACT_CONTENT) {
                if (_symmsg[8] == SID_OK) setup_close(1);
                else if (_symmsg[8] == SID_CANCEL) setup_close(0);
            }
            continue;
        }
        if (_symmsg[1] != wid_main)
            continue;

        if (act == DSK_ACT_CLOSE) {
            logs("exit", 0);
            exit(0);
        }
        if (act == DSK_ACT_KEY) {
            key(_symmsg[4]);
            continue;
        }
        if (act != DSK_ACT_CONTENT)
            continue;
        if (_symmsg[3] == DSK_SUB_KEY) {     /* key while a control has focus */
            if (_symmsg[8] == ID_PAGE) {
                typed = 0;                  /* Return clicks "Go" (retctrl) */
            } else {
                key(_symmsg[4]);
            }
            continue;
        }
        switch (_symmsg[8]) {
            case ID_PREV:   typed = 0; load_page(tt_prev); break;
            case ID_NEXT:   typed = 0; load_page(tt_next); break;
            case ID_GO:     typed = 0; load_page(pagebuf); break;
            case ID_SUBP:   load_page(tt_prevsub); break;
            case ID_SUBN:   load_page(tt_nextsub); break;
            case ID_RELOAD: load_page(page); break;
            case ID_SETUP:  setup_open(); break;
            case ID_TOP: case ID_BOT:
                click_canvas(*(int*)&_symmsg[4], *(int*)&_symmsg[6]);
                break;
        }
    }
    return 0;
}
