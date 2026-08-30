/*
 * Ugaris Gains Tracker
 *
 * Prints your exact experience and gold changes to the chat window,
 * announces item drops appearing on the ground around you, and shows an
 * optional session overlay with exp/hour.
 *
 * Commands:
 *   #track           - Show status and help
 *   #track exp       - Toggle experience gain/loss lines
 *   #track gold      - Toggle gold change lines
 *   #track drops     - Toggle ground item drop announcements
 *   #track overlay   - Toggle the session overlay
 *   #track reset     - Restart the session counters
 *
 * Settings persist in <client config dir>/tracker_mod.cfg.
 *
 * This mod is client-side only: it watches the values the server already
 * sends (experience, gold, the visible map). Ground items have no names
 * on the client, so drops are announced by position.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "amod/amod.h"

#define TRACKER_VERSION "1.1.0"

/* Chat color escape: 0xB0 "c" <palette index> (octal escape keeps the
 * following digits out of the hex escape). Palette: 2 = light green,
 * 3 = light red, 5 = orange, 6 = yellow, 15 = lime. */
#define COL_GAIN  "\260c15"
#define COL_LOSS  "\260c3"
#define COL_GOLD  "\260c6"
#define COL_DROP  "\260c5"
#define COL_INFO  "\260c2"

/* ------------------------------------------------------------------ state */

static int s_ingame;         /* amod_gamestart() seen */
static unsigned int s_ticks; /* our own tick counter (24/s while in game) */

/* settings (all default on except the overlay) */
static int s_show_exp = 1;
static int s_show_gold = 1;
static int s_show_drops = 1;
static int s_show_overlay = 0;

/* overlay position: drag offset + the rect drawn last frame */
static int s_off_x, s_off_y;
static int s_ov_x0, s_ov_y0, s_ov_x1, s_ov_y1;
static int s_drag, s_drag_mx, s_drag_my;

/* change detection */
static uint32_t s_last_exp, s_last_gold;
static unsigned int s_baseline_until; /* ignore deltas until this tick */

/* session counters */
static unsigned int s_session_start;
static long long s_session_exp, s_session_gold;
static unsigned int s_session_drops;

/* Ground item tracking by world position. Areas are at most 256x256 tiles.
 * state: 0 = never seen since area change, 1 = seen empty, 2 = seen item.
 * seen[] holds our tick counter of the last time the tile was visible, so
 * an item is only announced when the tile was continuously observed. */
static unsigned char s_tile_state[256 * 256];
static unsigned int s_tile_seen[256 * 256];

/* ------------------------------------------------------------------ utils */

static const char *fmt_thousands(long long v)
{
    static char buf[4][32];
    static int slot;
    char raw[24], *out;
    int len, i, o = 0;

    slot = (slot + 1) & 3;
    out = buf[slot];
    if (v < 0) { out[o++] = '-'; v = -v; }
    len = snprintf(raw, sizeof(raw), "%lld", v);
    for (i = 0; i < len; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = raw[i];
    }
    out[o] = 0;
    return out;
}

/* gold is counted in silver; 100 silver = 1 gold */
static const char *fmt_money(long long silver)
{
    static char buf[2][40];
    static int slot;
    char *out;

    slot ^= 1;
    out = buf[slot];
    if (silver >= 100 || silver <= -100) {
        snprintf(out, 40, "%s.%02dG", fmt_thousands(silver / 100),
                 (int)(silver < 0 ? -silver % 100 : silver % 100));
    } else {
        snprintf(out, 40, "%lldS", silver);
    }
    return out;
}

static void reset_session(void)
{
    s_session_start = s_ticks;
    s_session_exp = 0;
    s_session_gold = 0;
    s_session_drops = 0;
}

static void reset_tiles(void)
{
    memset(s_tile_state, 0, sizeof(s_tile_state));
}

/* ----------------------------------------------------------------- config */

static void config_path(char *out, size_t n)
{
    const char *dir = client_config_dir();
    snprintf(out, n, "%stracker_mod.cfg", dir && *dir ? dir : "");
}

static void save_config(void)
{
    char path[512];
    FILE *f;

    config_path(path, sizeof(path));
    f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "exp=%d\ngold=%d\ndrops=%d\noverlay=%d\noffx=%d\noffy=%d\n",
            s_show_exp, s_show_gold, s_show_drops, s_show_overlay, s_off_x, s_off_y);
    fclose(f);
}

static void load_config(void)
{
    char path[512], line[64];
    FILE *f;

    config_path(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        int v = atoi(strchr(line, '=') ? strchr(line, '=') + 1 : "0");
        if (!strncmp(line, "exp=", 4)) s_show_exp = v;
        else if (!strncmp(line, "gold=", 5)) s_show_gold = v;
        else if (!strncmp(line, "drops=", 6)) s_show_drops = v;
        else if (!strncmp(line, "overlay=", 8)) s_show_overlay = v;
        else if (!strncmp(line, "offx=", 5)) s_off_x = v;
        else if (!strncmp(line, "offy=", 5)) s_off_y = v;
    }
    fclose(f);
}

/* ------------------------------------------------------------ exp / gold */

static void check_exp(void)
{
    long long d = (long long)experience - (long long)s_last_exp;

    if (!d) return;
    s_last_exp = experience;
    if (s_ticks < s_baseline_until) return; /* login sync, not a gain */

    s_session_exp += d;
    if (!s_show_exp) return;

    if (d > 0) {
        int lvl = exp2level((int)experience);
        long long next = (long long)level2exp(lvl + 1) - (long long)experience;
        addline(COL_GAIN "+%s exp \260c1(%s to level %d)",
                fmt_thousands(d), fmt_thousands(next), lvl + 1);
    } else {
        addline(COL_LOSS "%s exp", fmt_thousands(d));
    }
}

static void check_gold(void)
{
    long long d = (long long)gold - (long long)s_last_gold;

    if (!d) return;
    s_last_gold = gold;
    if (s_ticks < s_baseline_until) return;

    s_session_gold += d;
    if (!s_show_gold) return;

    addline(COL_GOLD "%s%s", d > 0 ? "+" : "", fmt_money(d));
}

/* ------------------------------------------------------------------ drops */

static void check_drops(void)
{
    unsigned int x, y;
    int announced = 0, extra = 0;

    for (y = 0; y < MAPDY; y++) {
        for (x = 0; x < MAPDX; x++) {
            map_index_t mn = mapmn(x, y);
            unsigned int wx, wy, w;
            int has_item;

            if (mn >= (map_index_t)MAXMN) continue;
            if (!(map[mn].flags & CMF_VISIBLE) || !map[mn].rlight) continue;

            wx = (unsigned int)(originx - DIST + x) & 255;
            wy = (unsigned int)(originy - DIST + y) & 255;
            w = wx + wy * 256;

            has_item = (map[mn].isprite && (map[mn].flags & CMF_TAKE));

            if (s_tile_state[w] == 1 && has_item && s_tile_seen[w] == s_ticks - 1 &&
                s_ticks >= s_baseline_until) {
                s_session_drops++;
                if (s_show_drops) {
                    int dx = (int)wx - (int)originx;
                    int dy = (int)wy - (int)originy;
                    int dist = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
                    if (announced < 3) {
                        if (dist == 0)
                            addline(COL_DROP "Something dropped at your feet");
                        else
                            addline(COL_DROP "Something dropped at %u,%u \260c1(%d tile%s away)",
                                    wx, wy, dist, dist == 1 ? "" : "s");
                        announced++;
                    } else {
                        extra++;
                    }
                }
            }

            s_tile_state[w] = has_item ? 2 : 1;
            s_tile_seen[w] = s_ticks;
        }
    }
    if (extra) addline(COL_DROP "...and %d more item%s", extra, extra == 1 ? "" : "s");
}

/* ---------------------------------------------------------------- overlay */

#define OV_BG    IRGB(2, 2, 4)
#define OV_EDGE  IRGB(24, 18, 4)
#define OV_GOLD  IRGB(31, 26, 8)
#define OV_DIM   IRGB(13, 13, 15)
#define C_MONEY  IRGB(31, 28, 6)

/* One colored segment; returns the x after the drawn text. */
static int seg(int x, int y, unsigned short color, const char *text)
{
    render_text(x, y, color, RENDER_TEXT_SMALL, text);
    return x + render_text_length(RENDER_TEXT_SMALL, text);
}

static void draw_overlay(void)
{
    unsigned int secs = (s_ticks - s_session_start) / 24;
    long long per_hour = secs ? s_session_exp * 3600 / secs : 0;
    char t_time[24], t_exp[40], t_rate[40], t_gold[44], t_drops[24];
    int x0 = dotx(DOT_MTL) + 12 + s_off_x;
    int y0 = doty(DOT_MTL) + 26 + s_off_y;
    int x, w;

    /* keep the chip reachable */
    if (x0 < dotx(DOT_MTL) - 40) x0 = dotx(DOT_MTL) - 40;
    if (y0 < doty(DOT_MTL) + 4) y0 = doty(DOT_MTL) + 4;
    if (x0 > dotx(DOT_MBR) - 120) x0 = dotx(DOT_MBR) - 120;
    if (y0 > doty(DOT_MBR) - 20) y0 = doty(DOT_MBR) - 20;

    snprintf(t_time, sizeof(t_time), "%u:%02u:%02u", secs / 3600, (secs / 60) % 60, secs % 60);
    snprintf(t_exp, sizeof(t_exp), "%s%s xp", s_session_exp >= 0 ? "+" : "",
             fmt_thousands(s_session_exp));
    snprintf(t_rate, sizeof(t_rate), " (%s/h)", fmt_thousands(per_hour));
    snprintf(t_gold, sizeof(t_gold), "%s%s", s_session_gold >= 0 ? "+" : "",
             fmt_money(s_session_gold));
    snprintf(t_drops, sizeof(t_drops), "%u drop%s", s_session_drops,
             s_session_drops == 1 ? "" : "s");

    w = render_text_length(RENDER_TEXT_SMALL, t_time) + 14 +
        render_text_length(RENDER_TEXT_SMALL, t_exp) +
        render_text_length(RENDER_TEXT_SMALL, t_rate) + 14 +
        render_text_length(RENDER_TEXT_SMALL, t_gold) + 14 +
        render_text_length(RENDER_TEXT_SMALL, t_drops);

    s_ov_x0 = x0 - 8;
    s_ov_y0 = y0 - 5;
    s_ov_x1 = x0 + w + 8;
    s_ov_y1 = y0 + 15;

    render_rounded_rect_filled_alpha(x0 - 8, y0 - 5, x0 + w + 8, y0 + 15, 6, OV_BG, 205);
    render_rounded_rect_alpha(x0 - 8, y0 - 5, x0 + w + 8, y0 + 15, 6, OV_EDGE, 120);
    render_gradient_rect_h(x0 - 2, y0 - 5, x0 + w + 2, y0 - 4, OV_GOLD, OV_BG, 150);

    x = seg(x0, y0, lightgraycolor, t_time);
    x = seg(x + 5, y0, OV_DIM, "|");
    x = seg(x + 5, y0, s_session_exp >= 0 ? lightgreencolor : lightredcolor, t_exp);
    x = seg(x, y0, graycolor, t_rate);
    x = seg(x + 5, y0, OV_DIM, "|");
    x = seg(x + 5, y0, C_MONEY, t_gold);
    x = seg(x + 5, y0, OV_DIM, "|");
    seg(x + 5, y0, orangecolor, t_drops);
}

/* ------------------------------------------------------------- mod hooks */

DLL_EXPORT char *amod_version(void)
{
    return "Gains Tracker " TRACKER_VERSION;
}

DLL_EXPORT void amod_init(void)
{
    /* Client variables are not safe to touch yet; wait for gamestart. */
}

DLL_EXPORT void amod_exit(void)
{
}

DLL_EXPORT void amod_gamestart(void)
{
    s_ingame = 1;
    s_ticks = 0;
    s_baseline_until = 3 * 24; /* let the login value sync settle */
    s_last_exp = experience;
    s_last_gold = gold;
    reset_tiles();
    reset_session();
    load_config();
}

DLL_EXPORT void amod_areachange(void)
{
    /* The visible map now shows a different area; forget every tile. */
    reset_tiles();
}

DLL_EXPORT void amod_tick(void)
{
    if (!s_ingame) return;
    s_ticks++;

    if (s_ticks == 24)
        addline("Gains Tracker %s loaded. Type #track for options.", TRACKER_VERSION);

    /* Re-baseline instead of announcing during the settling window. */
    if (s_ticks == s_baseline_until) {
        s_last_exp = experience;
        s_last_gold = gold;
        reset_session();
    }

    check_exp();
    check_gold();
    check_drops();
}

DLL_EXPORT void amod_frame(void)
{
    if (!s_ingame || !s_show_overlay) return;
    draw_overlay();
}

/* The overlay chip is draggable: grab it anywhere, drop it anywhere. */

static int inside_overlay(int x, int y)
{
    return s_ingame && s_show_overlay &&
           x >= s_ov_x0 && x <= s_ov_x1 && y >= s_ov_y0 && y <= s_ov_y1;
}

DLL_EXPORT void amod_mouse_move(int x, int y)
{
    if (s_drag) {
        s_off_x += x - s_drag_mx;
        s_off_y += y - s_drag_my;
        s_drag_mx = x;
        s_drag_my = y;
    }
}

DLL_EXPORT int amod_mouse_over(int x, int y)
{
    return inside_overlay(x, y);
}

DLL_EXPORT int amod_mouse_click(int x, int y, int what)
{
    if (what == SDL_MOUM_LDOWN && inside_overlay(x, y)) {
        s_drag = 1;
        s_drag_mx = x;
        s_drag_my = y;
        return 1;
    }
    if (what == SDL_MOUM_LUP && s_drag) {
        s_drag = 0;
        save_config();
        return 1;
    }
    return 0;
}

static int toggle(int *setting, const char *name)
{
    *setting = !*setting;
    addline(COL_INFO "Tracker: %s %s", name, *setting ? "on" : "off");
    save_config();
    return 1;
}

DLL_EXPORT int amod_client_cmd(const char *buf)
{
    if (strncmp(buf, "#track", 6)) return 0;
    buf += 6;
    while (*buf == ' ') buf++;

    if (!*buf || !strcmp(buf, "help")) {
        addline(COL_INFO "Gains Tracker %s - exp:%s gold:%s drops:%s overlay:%s",
                TRACKER_VERSION,
                s_show_exp ? "on" : "off", s_show_gold ? "on" : "off",
                s_show_drops ? "on" : "off", s_show_overlay ? "on" : "off");
        addline("#track exp/gold/drops/overlay toggles a feature, #track reset restarts the session");
        return 1;
    }
    if (!strcmp(buf, "exp")) return toggle(&s_show_exp, "exp lines");
    if (!strcmp(buf, "gold")) return toggle(&s_show_gold, "gold lines");
    if (!strcmp(buf, "drops")) return toggle(&s_show_drops, "drop announcements");
    if (!strcmp(buf, "overlay")) return toggle(&s_show_overlay, "session overlay");
    if (!strcmp(buf, "reset")) {
        reset_session();
        addline(COL_INFO "Tracker: session counters reset");
        return 1;
    }
    addline(COL_INFO "Unknown #track option. Try #track help");
    return 1;
}
