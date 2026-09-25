#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "contacts.h"

#define MAX_CONTACTS   1500
#define GROW_STEP      100
#define MIN_FREE_HEAP  (40 * 1024)   /* leave room for Bluetooth and audio */
#define NAME_LEN       24
#define NUM_DIGITS     10      /* national number; drops +91 / leading 0 */
#define MIN_MATCH      7
#define MAX_TELS       4       /* numbers kept per vCard */
#define LINE_LEN       160

typedef struct {
    char name[NAME_LEN];
    char num[NUM_DIGITS + 1];
} contact_t;

static contact_t *s_tab;
static int s_cap;
static volatile int s_count;
static volatile bool s_full;

/* Parser state, carried across chunks. */
static char s_line[LINE_LEN];
static int s_line_len;
static char s_fn[NAME_LEN], s_n[NAME_LEN];
static char s_tels[MAX_TELS][NUM_DIGITS + 1];
static int s_tel_count;

void contacts_reset(void)
{
    s_count = 0;
    s_full = false;
    s_line_len = 0;
}

int contacts_count(void)
{
    return s_count;
}

bool contacts_full(void)
{
    return s_full;
}

/* Keeps the last NUM_DIGITS digits of a phone number. */
static void last_digits(const char *s, char *out)
{
    char all[32];
    int n = 0;
    for (; *s && n < (int)sizeof(all) - 1; s++) {
        if (isdigit((unsigned char)*s)) all[n++] = *s;
    }
    int start = n > NUM_DIGITS ? n - NUM_DIGITS : 0;
    memcpy(out, all + start, n - start);
    out[n - start] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = toupper((unsigned char)c);
    return (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
}

/* Copies a property value, decoding =XX escapes when the vCard says
 * ENCODING=QUOTED-PRINTABLE (vCard 2.1 does this for non-ASCII names). */
static void copy_value(const char *params, const char *val, char *out, size_t size)
{
    bool qp = strstr(params, "QUOTED-PRINTABLE") != NULL;
    size_t o = 0;
    for (const char *p = val; *p && o < size - 1; p++) {
        int hi, lo;
        if (qp && p[0] == '=' && (hi = hexval(p[1])) >= 0 && (lo = hexval(p[2])) >= 0) {
            out[o++] = (char)(hi << 4 | lo);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

static void add_contact(const char *name, const char *num)
{
    if (strlen(num) < MIN_MATCH) {
        return;
    }
    if (s_count >= s_cap) {
        /* PBAP and HFP callbacks both run on the Bluedroid BTC task, so a
         * lookup can't be reading the table while it moves */
        contact_t *t = NULL;
        if (s_cap < MAX_CONTACTS &&
            heap_caps_get_free_size(MALLOC_CAP_8BIT) > MIN_FREE_HEAP + GROW_STEP * sizeof(contact_t)) {
            t = realloc(s_tab, (s_cap + GROW_STEP) * sizeof(contact_t));
        }
        if (!t) {
            s_full = true;
            return;
        }
        s_tab = t;
        s_cap += GROW_STEP;
    }
    contact_t *c = &s_tab[s_count];
    strlcpy(c->name, name, sizeof(c->name));
    strlcpy(c->num, num, sizeof(c->num));
    s_count++;   /* publish only once the entry is complete */
}

static void process_line(char *line)
{
    char *colon = strchr(line, ':');
    if (!colon) {
        return;
    }
    *colon = '\0';
    const char *key = line, *val = colon + 1;

    if (!strcasecmp(key, "BEGIN") && !strcasecmp(val, "VCARD")) {
        s_fn[0] = s_n[0] = '\0';
        s_tel_count = 0;
    } else if (!strncasecmp(key, "FN", 2) && (key[2] == '\0' || key[2] == ';')) {
        copy_value(key, val, s_fn, sizeof(s_fn));
    } else if (!strncasecmp(key, "N", 1) && (key[1] == '\0' || key[1] == ';')) {
        /* "Family;Given;..." -> "Given Family", used when there is no FN */
        char raw[NAME_LEN * 2];
        copy_value(key, val, raw, sizeof(raw));
        char *given = strchr(raw, ';');
        if (given) {
            *given++ = '\0';
            char *end = strchr(given, ';');
            if (end) *end = '\0';
        }
        char full[NAME_LEN * 4];
        snprintf(full, sizeof(full), "%s%s%s", given ? given : "", given && *given && *raw ? " " : "", raw);
        strlcpy(s_n, full, sizeof(s_n));   /* long names are cut to fit */
    } else if (!strncasecmp(key, "TEL", 3) && s_tel_count < MAX_TELS) {
        last_digits(val, s_tels[s_tel_count]);
        if (s_tels[s_tel_count][0]) s_tel_count++;
    } else if (!strcasecmp(key, "END") && !strcasecmp(val, "VCARD")) {
        const char *name = s_fn[0] ? s_fn : s_n;
        for (int i = 0; name[0] && i < s_tel_count; i++) {
            add_contact(name, s_tels[i]);
        }
    }
}

void contacts_feed(const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            if (s_line_len && s_line[s_line_len - 1] == '\r') s_line[s_line_len - 1] = '\0';
            process_line(s_line);
            s_line_len = 0;
        } else if (s_line_len < LINE_LEN - 1) {
            s_line[s_line_len++] = c;   /* over-long lines (photos etc.) are cut */
        }
    }
}

bool contacts_lookup(const char *number, char *name, size_t name_size)
{
    char want[NUM_DIGITS + 1];
    last_digits(number ? number : "", want);
    size_t wl = strlen(want);
    if (wl < MIN_MATCH) {
        return false;
    }
    int n = s_count;
    for (int i = 0; i < n; i++) {
        size_t cl = strlen(s_tab[i].num);
        size_t m = cl < wl ? cl : wl;
        if (m >= MIN_MATCH && !strcmp(s_tab[i].num + cl - m, want + wl - m)) {
            strlcpy(name, s_tab[i].name, name_size);
            return true;
        }
    }
    return false;
}
