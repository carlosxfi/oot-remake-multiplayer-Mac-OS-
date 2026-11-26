/* SPDX-License-Identifier: GPL-3.0-or-later
   The mods list between the co-op module's Mods tab and Hookshot's mod loader (oot-remake-multiplayer, docs/mods.md).

   Hookshot indexes its mods folder once, at the start, before the module is loaded, and keeps the game's files open
   while it runs. So the tab never renames or moves a mod: it writes one small file into the mods folder, and Hookshot's
   host/mods.c (our patch 0011) reads it at the next start:

     <mods>/coop-mods.list      written by the module: the order and the switches
         coop-mods 1
         on <name>              first row = wins a file two mods have (it is indexed last)
         off <name>             left out
         default off            (optional, any line after the first) a mod the list does not name is off too
       <name> is the mod's name as Hookshot logs it: its path under the mods folder with '/'. A mod the list does not
       name is on and keeps Hookshot's own order (by name) under the named ones; with "default off" it is left out,
       as an "off" row would (co-op's Original game, coop_original.h: a mod put into the folder after the switch went on
       stays out too, player report 266). Lines it does not know are skipped (an older reader skips "default off").

     <mods>/coop-mods.applied   written by Hookshot at each start: what this run indexed
         coop-mods-applied 1 <process id>
         on <files> <name>      in the order indexed (the last one wins)
         off <name>

   The same file is in Hookshot's host/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/mods_test.cpp checks it). Plain C, header only, no Hookshot or co-op include. */
#ifndef HOS_COOP_MODS_H
#define HOS_COOP_MODS_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HOS_COOP_MODS_VERSION 1
#define HOS_COOP_MODS_LIST_FILE "coop-mods.list"
#define HOS_COOP_MODS_DEFAULT_OFF "default off"   /* the line that leaves the mods the list does not name out */
#define HOS_COOP_MODS_APPLIED_FILE "coop-mods.applied"
#define HOS_COOP_MODS_MAX 256    /* rows read; more are skipped */
#define HOS_COOP_MODS_NAME 260   /* a name with its 0 (MAX_PATH) */
/* The most a list file can hold that is read: a caller's buffer for it. */
#define HOS_COOP_MODS_TEXT_MAX (HOS_COOP_MODS_MAX * (HOS_COOP_MODS_NAME + 8) + 64)

typedef struct HosCoopModsRow {
    char name[HOS_COOP_MODS_NAME];
    int off;
} HosCoopModsRow;

typedef struct HosCoopModsList {
    int count;
    int default_off;   /* 1: the list has a "default off" line: a mod it does not name is off (hos_coop_mods_rank) */
    HosCoopModsRow rows[HOS_COOP_MODS_MAX];
} HosCoopModsList;

/* Names are compared as Hookshot sorts them: ASCII letters without case; '\' counts as '/'. */
static inline int hos_coop_mods_fold(int c) {
    if (c == '\\') return '/';
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}
static inline int hos_coop_mods_same(const char *a, const char *b) {
    for (;; a++, b++) {
        if (hos_coop_mods_fold((unsigned char)*a) != hos_coop_mods_fold((unsigned char)*b)) return 0;
        if (!*a) return 1;
    }
}

/* Reads a list file's text (n bytes; no 0 needed). 1 when it is a list of this version; else 0 and an empty list. */
static inline int hos_coop_mods_parse(const char *text, size_t n, HosCoopModsList *out) {
    size_t at = 0;
    int line_no = 0;
    out->count = 0;
    out->default_off = 0;
    while (at < n) {
        size_t end = at, len;
        const char *line = text + at;
        while (end < n && text[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (len && line[len - 1] == '\r') len--;
        if (line_no++ == 0) {
            if (len != 11 || memcmp(line, "coop-mods 1", 11)) return 0;
            continue;
        }
        {
            int off;
            int i, known = 0;
            if (len == sizeof HOS_COOP_MODS_DEFAULT_OFF - 1 && !memcmp(line, HOS_COOP_MODS_DEFAULT_OFF, len)) {
                out->default_off = 1;
                continue;
            }
            if (len > 3 && !memcmp(line, "on ", 3)) off = 0, line += 3, len -= 3;
            else if (len > 4 && !memcmp(line, "off ", 4)) off = 1, line += 4, len -= 4;
            else continue;
            if (len >= HOS_COOP_MODS_NAME || out->count >= HOS_COOP_MODS_MAX) continue;
            memcpy(out->rows[out->count].name, line, len);
            out->rows[out->count].name[len] = 0;
            for (i = 0; i < out->count && !known; i++)   /* (a name twice: its first row counts) */
                known = hos_coop_mods_same(out->rows[i].name, out->rows[out->count].name);
            if (known) continue;
            out->rows[out->count++].off = off;
        }
    }
    return line_no > 0;
}

/* A mod's place: -1 = off (an "off" row, or not in a "default off" list); 0 = not in the list (Hookshot's own order,
   under the listed ones); else the higher wins (the list's first row has the highest). Mods are indexed by rising rank,
   then by name. */
static inline int hos_coop_mods_rank(const HosCoopModsList *list, const char *name) {
    int i;
    for (i = 0; i < list->count; i++)
        if (hos_coop_mods_same(list->rows[i].name, name)) return list->rows[i].off ? -1 : list->count - i;
    return list->default_off ? -1 : 0;
}

/* The applied file's lines (each with its '\n'); what snprintf returns. */
static inline int hos_coop_mods_applied_head(char *out, size_t cap, unsigned long pid) {
    return snprintf(out, cap, "coop-mods-applied %d %lu\n", HOS_COOP_MODS_VERSION, pid);
}
static inline int hos_coop_mods_applied_row(char *out, size_t cap, const char *name, int files) {   /* files < 0: off */
    return files < 0 ? snprintf(out, cap, "off %s\n", name) : snprintf(out, cap, "on %d %s\n", files, name);
}

/* Co-op's own packs (the puppet pack, the HUD pack): the kit keeps them on and builds the game with them; never listed.
   A name whose last part starts with coop-puppet, coop_hud or coop-hud. */
static inline int hos_coop_mods_own(const char *name) {
    static const char *const own[] = {"coop-puppet", "coop_hud", "coop-hud"};
    const char *leaf = name, *p;
    size_t i, k;
    for (p = name; *p; p++)
        if (*p == '/' || *p == '\\') leaf = p + 1;
    for (i = 0; i < sizeof own / sizeof own[0]; i++) {
        for (k = 0; own[i][k] && hos_coop_mods_fold((unsigned char)leaf[k]) == own[i][k]; k++) {}
        if (!own[i][k]) return 1;
    }
    return 0;
}

/* A mod's code against the game's build (player report: a code mod switched in the Mods tab, then the game started from
   longshot.exe, which does not build). The game is compiled ahead of time, and a mod's ExeFS patches go into it only when
   it is built (the kit's Start Squadala builds again when they changed). The build lists what it took in
   <game>/exefs_patches.txt (nx2/exefs_patches.py): "<mod>\t<file>\t<hash>\t<kind>[\t<note>]" a line, '#' a comment,
   <kind> code or data (applied), other, empty or bad (not applied). The build lists only the mods that were on, and writes
   no list when no mod had a patch file (nx2/make_game.py): no list reads as an empty one (n = 0). */
#define HOS_COOP_MODS_BUILT_NONE 0   /* the list names none of its files: the game was built while it was off (or gone) */
#define HOS_COOP_MODS_BUILT_CODE 1   /* code or data of it applied: its code is in the game */
#define HOS_COOP_MODS_BUILT_NAMED 2  /* named, nothing applied (another game version's, empty): its code changes nothing */
#define HOS_COOP_MODS_BUILT_CHANGED 3   /* code of it applied, but a patch file it has now is not as built (put in or
                                           changed since: hos_coop_mods_built_changed): the game runs its old code */
static inline int hos_coop_mods_built(const char *list, size_t n, const char *name) {
    size_t at = 0;
    int named = 0;
    while (at < n) {
        size_t end = at, len, f = 0, field = 0;
        const char *line = list + at;
        while (end < n && list[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (!len || line[0] == '#') continue;
        while (f < len && line[f] != '\t') f++;
        if (f == len || strlen(name) != f) continue;
        {
            size_t k;
            int same = 1;
            for (k = 0; k < f && same; k++)
                same = hos_coop_mods_fold((unsigned char)line[k]) == hos_coop_mods_fold((unsigned char)name[k]);
            if (!same) continue;
        }
        named = 1;
        for (f = 0; f < len && field < 3; f++)   /* (the fourth field: the kind) */
            if (line[f] == '\t') field++;
        if (field == 3 && f + 4 <= len && (!memcmp(line + f, "code", 4) || !memcmp(line + f, "data", 4)))
            return HOS_COOP_MODS_BUILT_CODE;
    }
    return named ? HOS_COOP_MODS_BUILT_NAMED : HOS_COOP_MODS_BUILT_NONE;
}

/* FNV-1a 32 of a file's bytes, as the build and host/mods.c exefs_status hash a patch file: start with
   HOS_COOP_MODS_FNV32_START, feed each block. */
#define HOS_COOP_MODS_FNV32_START 0x811C9DC5u
static inline unsigned long hos_coop_mods_fnv32(unsigned long h, const void *data, size_t n) {
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < n; i++) h = ((h ^ p[i]) * 0x01000193u) & 0xFFFFFFFFu;
    return h;
}

/* Whether the list has this patch file of this mod as it is now: its file name and its hash (exefs_status's test). */
static inline int hos_coop_mods_built_file(const char *list, size_t n, const char *name, const char *file, unsigned long hash) {
    char want[24];
    size_t at = 0, name_n = strlen(name), file_n = strlen(file), want_n;
    want_n = (size_t)snprintf(want, sizeof want, "\t%08lx\t", hash & 0xFFFFFFFFu);
    while (at < n) {
        size_t end = at, len, k;
        const char *line = list + at;
        int same = 1;
        while (end < n && list[end] != '\n') end++;
        len = end - at;
        at = end + 1;
        if (len < name_n + 1 + file_n + want_n || line[name_n] != '\t') continue;
        for (k = 0; k < name_n && same; k++)
            same = hos_coop_mods_fold((unsigned char)line[k]) == hos_coop_mods_fold((unsigned char)name[k]);
        for (k = 0; k < file_n && same; k++)
            same = hos_coop_mods_fold((unsigned char)line[name_n + 1 + k]) == hos_coop_mods_fold((unsigned char)file[k]);
        if (same && !memcmp(line + name_n + 1 + file_n, want, want_n)) return 1;
    }
    return 0;
}

/* A mod's build state with its patch files now: CODE becomes CHANGED when `waiting` of them (files the list does not
   have as they are: hos_coop_mods_built_file) are not in the game. */
static inline int hos_coop_mods_built_changed(int built, int waiting) {
    return built == HOS_COOP_MODS_BUILT_CODE && waiting > 0 ? HOS_COOP_MODS_BUILT_CHANGED : built;
}

/* Whether a mod's files are used this start: as the game was built for it, not as switched since. A mod whose code is in
   the game keeps its files on (its code reads them); a mod with ExeFS patch files the build did not take keeps its files
   off (its files without its code: Character Select's picks do nothing, and its tables lack what the kit's start merges
   in). The next game build (Start Squadala) takes the switch, code and files together. A mod whose code changed since the
   build (CHANGED: a new release put in by hand) runs its old code: its old files are gone, and its code without files
   is worse than with newer ones, so its files stay on, and the start and the Mods tab say a build is needed. Co-op's
   own packs: as switched. on: the switch; built: hos_coop_mods_built, then hos_coop_mods_built_changed; patch_files:
   its .ips and .pchtxt files. */
static inline int hos_coop_mods_as_built(int on, int built, int patch_files, const char *name) {
    if (hos_coop_mods_own(name)) return on;
    if (built == HOS_COOP_MODS_BUILT_CODE || built == HOS_COOP_MODS_BUILT_CHANGED) return 1;
    if (built == HOS_COOP_MODS_BUILT_NONE && patch_files > 0) return 0;
    return on;
}

/* ---- The kit's merge at a start the kit did not make (player report 288) ----
   The kit's start (Start Squadala: play.ps1, start.sh) runs tools/puppet_pack/merge_tables.py before the game, so the
   files two mods share (size tables, actor tables, the menu archives) are one file with every mod's part. A game
   started any other way read the mods as they lay: the Insert menu's Restart now after a randomizer seed was made
   (host/hos_relaunch.c starts the game itself), longshot.exe or a shortcut to it, Nucleus Co-op. The seed's menu
   archive then won without the Character Select mod's layouts, and that mod's Characters page crashed the game on a
   layout that was not there. So host/mods.c runs the same merge itself, before it indexes the mods, when the game
   was started again by Restart now (the restarted game is given HOS_COOP_MODS_RESTART_ENV) or without the kit's
   marker. The merge is stamped: with nothing changed it writes nothing.
   The switch HOS_COOP_MODS_START_MERGE_ENV: 1 on, 0 off, not set: HOS_COOP_MODS_START_MERGE_DEFAULT. */
#define HOS_COOP_MODS_KIT_START_ENV "OOT_COOP_KIT_START"
#define HOS_COOP_MODS_RESTART_ENV "OOT_COOP_RESTART"
#define HOS_COOP_MODS_START_MERGE_ENV "OOT_COOP_START_MERGE"
#define HOS_COOP_MODS_START_MERGE_DEFAULT 0      /* (off until a run of two games with pictures proved it) */
#define HOS_COOP_MODS_START_MERGE_MS 180000UL    /* the merge's time limit: then it is ended and the game starts */
#define HOS_COOP_MODS_START_MERGE_OUT "coop-start-merge.txt"   /* beside the game: what the merge said (then logged) */
#define HOS_COOP_MODS_KIT_UP 5                   /* how many folders above the game's the kit is looked for */
#ifdef _WIN32
#define HOS_COOP_MODS_SEP '\\'
#else
#define HOS_COOP_MODS_SEP '/'
#endif

static inline int hos_coop_mods_env_on(const char *v) { return v && v[0] && !(v[0] == '0' && !v[1]); }

/* Whether host/mods.c runs the merge at this start, and why. sw, kit_start, restart: the three variables' values (NULL:
   not set); original_game: the list says "default off" (co-op's Original game: the kit merges nothing for it either). */
#define HOS_COOP_MODS_MERGE_NO 0
#define HOS_COOP_MODS_MERGE_RESTART 1   /* started again by the Insert menu's Restart now */
#define HOS_COOP_MODS_MERGE_DIRECT 2    /* started without the kit's start file */
static inline int hos_coop_mods_start_merge(const char *sw, const char *kit_start, const char *restart, int original_game) {
    const int on = (sw && sw[0]) ? hos_coop_mods_env_on(sw) : HOS_COOP_MODS_START_MERGE_DEFAULT;
    if (!on || original_game) return HOS_COOP_MODS_MERGE_NO;
    if (hos_coop_mods_env_on(restart)) return HOS_COOP_MODS_MERGE_RESTART;
    return hos_coop_mods_env_on(kit_start) ? HOS_COOP_MODS_MERGE_NO : HOS_COOP_MODS_MERGE_DIRECT;
}

/* dir (with or without a last separator) `up` folders up, without a last separator: 1, or 0 when there is no folder
   that far up or it does not fit. */
static inline int hos_coop_mods_up(const char *dir, int up, char *out, size_t cap) {
    size_t n = strlen(dir);
    if (!n || n >= cap) return 0;
    memcpy(out, dir, n + 1);
    while (n > 1 && (out[n - 1] == '/' || out[n - 1] == '\\')) out[--n] = 0;
    for (; up > 0; up--) {
        size_t k = n;
        while (k > 0 && out[k - 1] != '/' && out[k - 1] != '\\') k--;
        if (k <= 1) return 0;   /* (no folder above, or only the root) */
        n = k - 1;
        out[n] = 0;
    }
    return 1;
}

/* The merge's files in a kit, by the kit's own layout only: the game runs nothing a file in the mods folder (which a
   downloaded mod could bring) or a variable names. Windows: <kit>\tools\puppet_pack\merge_tables.py and the kit's own
   Python, <kit>\hookshot\tools\python\python.exe. Linux (the kit is its data folder): <kit>/tools/puppet_pack/
   merge_tables.py, the builder's Python <kit>/builder/image/usr/python/bin/python3, else the system's
   /usr/bin/python3 (as start.sh, by its fixed path). 1, or 0 when there is no such file here or it does not fit. */
#define HOS_COOP_MODS_KIT_TOOL 0
#define HOS_COOP_MODS_KIT_PYTHON 1
#define HOS_COOP_MODS_KIT_PYTHON_SYSTEM 2
static inline int hos_coop_mods_kit_file(const char *kit, int which, char *out, size_t cap) {
    static const char *const tool[] = {"tools", "puppet_pack", "merge_tables.py", NULL};
#ifdef _WIN32
    static const char *const python[] = {"hookshot", "tools", "python", "python.exe", NULL};
#else
    static const char *const python[] = {"builder", "image", "usr", "python", "bin", "python3", NULL};
#endif
    const char *const *parts = which == HOS_COOP_MODS_KIT_TOOL ? tool : python;
    size_t n = strlen(kit), i;
    if (which == HOS_COOP_MODS_KIT_PYTHON_SYSTEM) {
#ifdef _WIN32
        return 0;
#else
        return snprintf(out, cap, "/usr/bin/python3") < (int)cap;
#endif
    }
    if (which != HOS_COOP_MODS_KIT_TOOL && which != HOS_COOP_MODS_KIT_PYTHON) return 0;
    if (!n || n >= cap) return 0;
    memcpy(out, kit, n + 1);
    for (i = 0; parts[i]; i++) {
        const size_t len = strlen(parts[i]);
        if (n + 1 + len >= cap) return 0;
        out[n++] = HOS_COOP_MODS_SEP;
        memcpy(out + n, parts[i], len + 1);
        n += len;
    }
    return 1;
}

#endif
