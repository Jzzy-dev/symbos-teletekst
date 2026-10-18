/* ttcore.h - interface of ttcore.c (NOS Teletekst page model / parser / renderer) */
#define TT_COLS   40
#define TT_ROWS   25
#define TT_CELLS  1000
#define TT_MAXLNK 48

extern unsigned char tt_ch[], tt_at[], tt_lk[];
extern char tt_links[TT_MAXLNK][8];
extern unsigned char tt_nlinks;
extern char tt_prev[8], tt_next[8], tt_prevsub[8], tt_nextsub[8];
extern char tt_ftitle[4][14];
extern char tt_fpage[4][8];
extern unsigned char tt_nfast;
extern int tt_http;
extern unsigned char tt_gotjson;

void tt_reset(void);
void tt_feed(char* buf, unsigned short len);
void tt_finish(void);
unsigned char tt_complete(void);
void tt_text(unsigned char row, unsigned char col, char* t, unsigned char fg, unsigned char bg);
extern unsigned short tt_clen, tt_body;
char* tt_link_at(unsigned char col, unsigned char row);
void tt_render16(unsigned char* top, unsigned char* bot, unsigned char bw, unsigned char* pal);
void tt_render4(unsigned char* top, unsigned char* bot, unsigned char bw, unsigned char* pal);
