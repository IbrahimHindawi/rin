/* asm.c: `rin asm`, the rasm assembler.

   rasm is rin's declaration syntax with x64 instructions for bodies. The design
   is in rasm/docs; the short version, for reading this file:

     - The assembler generates nothing the source does not say. Frames, shadow
       space, saved registers and argument moves are the programmer's.
     - Code goes to Intel-syntax assembly for clang with a `.cv_loc` before each
       statement, so Visual Studio steps through the .rasm source.
     - Data and types go to a C file, so the debugger shows every global by type
       and field name. rin's parser reads the declarations; this file writes the
       C itself, because rin's checker enforces rin's pointer kinds and rasm has
       one pointer type.
     - Every struct offset the code uses is checked against C with a
       _Static_assert in the data file.

   Included by main.c so it can use rin's lexer, parser and import expansion.
   Nothing in rin's own pipeline calls into this file. */

#include <stdarg.h>

/* ---------------------------------------------------------------------------
   Small utilities. The assembler runs once and exits, so its own buffers are
   plain heap blocks; rin's arena is used where rin's parser needs one.
   ------------------------------------------------------------------------ */

static memops_arena *asm_arena;
static i32 asm_errors;

typedef struct AsmSb {
    char *d;
    i64 n;
    i64 cap;
} AsmSb;

static void sb_reserve(AsmSb *s, i64 extra) {
    if (s->n + extra + 1 <= s->cap) return;
    i64 cap = s->cap ? s->cap : 256;
    while (cap < s->n + extra + 1) cap *= 2;
    s->d = (char *)realloc(s->d, (size_t)cap);
    s->cap = cap;
}

static void sb_add(AsmSb *s, const char *str) {
    i64 len = (i64)strlen(str);
    sb_reserve(s, len);
    memcpy(s->d + s->n, str, (size_t)len);
    s->n += len;
    s->d[s->n] = 0;
}

static void sb_addn(AsmSb *s, const char *str, i64 len) {
    sb_reserve(s, len);
    memcpy(s->d + s->n, str, (size_t)len);
    s->n += len;
    s->d[s->n] = 0;
}

static void sb_printf(AsmSb *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[2048];
    i32 len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (len < (i32)sizeof(tmp)) {
        sb_addn(s, tmp, len);
        return;
    }
    char *big = (char *)malloc((size_t)len + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)len + 1, fmt, ap);
    va_end(ap);
    sb_addn(s, big, len);
    free(big);
}

static const char *sb_cstr(AsmSb *s) {
    if (!s->d) {
        sb_reserve(s, 0);
        s->d[0] = 0;
    }
    return s->d;
}

static char *asm_strdup(const char *s) {
    size_t n = strlen(s);
    char *r = (char *)malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

static char *asm_strndup(const char *s, i64 n) {
    char *r = (char *)malloc((size_t)n + 1);
    memcpy(r, s, (size_t)n);
    r[n] = 0;
    return r;
}

static char *asm_s8(string8 s) {
    if (!s.data) return asm_strdup("");
    return asm_strndup((const char *)s.data, (i64)s.length);
}

static bool asm_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static bool asm_ends_with(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* A growable array of pointers. */
typedef struct AsmList {
    void **d;
    i32 n;
    i32 cap;
} AsmList;

static void list_push(AsmList *l, void *p) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->d = (void **)realloc(l->d, sizeof(void *) * (size_t)l->cap);
    }
    l->d[l->n++] = p;
}

/* A string-keyed hash map. */
typedef struct AsmMapEntry {
    char *key;
    void *value;
} AsmMapEntry;

typedef struct AsmMap {
    AsmMapEntry *slots;
    i32 cap;
    i32 count;
} AsmMap;

static u64 asm_hash(const char *s) {
    u64 h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (u8)*s;
        h *= 1099511628211ull;
    }
    return h;
}

static void *map_get(AsmMap *m, const char *key) {
    if (!m->cap) return null;
    u64 i = asm_hash(key) & (u64)(m->cap - 1);
    while (m->slots[i].key) {
        if (asm_streq(m->slots[i].key, key)) return m->slots[i].value;
        i = (i + 1) & (u64)(m->cap - 1);
    }
    return null;
}

static void map_put(AsmMap *m, const char *key, void *value);

static void map_grow(AsmMap *m) {
    AsmMapEntry *old = m->slots;
    i32 old_cap = m->cap;
    m->cap = m->cap ? m->cap * 2 : 64;
    m->slots = (AsmMapEntry *)calloc((size_t)m->cap, sizeof(AsmMapEntry));
    m->count = 0;
    for (i32 i = 0; i < old_cap; i++) {
        if (old[i].key) map_put(m, old[i].key, old[i].value);
    }
    free(old);
}

static void map_put(AsmMap *m, const char *key, void *value) {
    if ((m->count + 1) * 2 > m->cap) map_grow(m);
    u64 i = asm_hash(key) & (u64)(m->cap - 1);
    while (m->slots[i].key) {
        if (asm_streq(m->slots[i].key, key)) {
            m->slots[i].value = value;
            return;
        }
        i = (i + 1) & (u64)(m->cap - 1);
    }
    m->slots[i].key = asm_strdup(key);
    m->slots[i].value = value;
    m->count++;
}

/* ---------------------------------------------------------------------------
   Source files and diagnostics.
   ------------------------------------------------------------------------ */

typedef struct AsmFile {
    char *path;      // canonical, as given to the debugger
    char *src;
    char **lines;    // the source split into lines, without their newlines
    i32 nlines;
    i32 id;          // 1-based .cv_file number
    AsmSb rin_text;  // the declarations as rin text, same line numbers as the source
} AsmFile;

static AsmList asm_files; // AsmFile*

static void asm_error_at(i32 file, i32 line, i32 col, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    const char *path = (file > 0 && file <= asm_files.n) ? ((AsmFile *)asm_files.d[file - 1])->path : "<rasm>";
    printf("%s:%d:%d: rasm error: %s\n", path, line, col, msg);
    asm_errors++;
}

/* ---------------------------------------------------------------------------
   The rasm lexer. `;` starts a comment, a newline is a token, `#` at the start
   of a line is a directive that runs to its end.
   ------------------------------------------------------------------------ */

typedef enum AsmTokKind {
    AT_EOF = 0,
    AT_NL,
    AT_IDENT,
    AT_INT,
    AT_FLOAT,
    AT_STR,
    AT_CHAR,
    AT_PUNCT,
    AT_DIRECTIVE,
} AsmTokKind;

typedef struct AsmTok {
    AsmTokKind kind;
    char *text;
    i32 file;
    i32 line;
    i32 col;
} AsmTok;

typedef struct AsmToks {
    AsmTok *d;
    i32 n;
    i32 cap;
} AsmToks;

static void toks_push(AsmToks *t, AsmTok tok) {
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->d = (AsmTok *)realloc(t->d, sizeof(AsmTok) * (size_t)t->cap);
    }
    t->d[t->n++] = tok;
}

static bool asm_is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool asm_is_ident_char(char c) { return asm_is_ident_start(c) || (c >= '0' && c <= '9'); }
static bool asm_is_digit(char c) { return c >= '0' && c <= '9'; }

static AsmToks asm_lex(const char *src, i32 file) {
    AsmToks out = {0};
    i32 line = 1;
    const char *line_start = src;
    const char *p = src;
    bool at_line_start = true;
    while (*p) {
        char c = *p;
        i32 col = (i32)(p - line_start) + 1;
        if (c == '\n') {
            toks_push(&out, (AsmTok){AT_NL, asm_strdup("\\n"), file, line, col});
            p++;
            line++;
            line_start = p;
            at_line_start = true;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            p++;
            continue;
        }
        if (c == ';') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (c == '#' && at_line_start) {
            const char *s = p;
            while (*p && *p != '\n') p++;
            const char *e = p;
            while (e > s && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
            toks_push(&out, (AsmTok){AT_DIRECTIVE, asm_strndup(s, e - s), file, line, col});
            continue;
        }
        at_line_start = false;
        if (c == '"' || c == '\'') {
            const char *s = p;
            char q = c;
            p++;
            while (*p && *p != q && *p != '\n') {
                if (*p == '\\' && p[1]) p++;
                p++;
            }
            if (*p != q) {
                asm_error_at(file, line, col, "unterminated %s literal", q == '"' ? "string" : "character");
            } else {
                p++;
            }
            toks_push(&out, (AsmTok){q == '"' ? AT_STR : AT_CHAR, asm_strndup(s, p - s), file, line, col});
            continue;
        }
        if (asm_is_digit(c)) {
            const char *s = p;
            bool is_float = false;
            if (c == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'b' || p[1] == 'B')) {
                p += 2;
                while (asm_is_ident_char(*p)) p++;
            } else {
                while (asm_is_digit(*p) || *p == '_') p++;
                if (*p == '.' && asm_is_digit(p[1])) {
                    is_float = true;
                    p++;
                    while (asm_is_digit(*p)) p++;
                }
                if ((*p == 'e' || *p == 'E') && (asm_is_digit(p[1]) || ((p[1] == '+' || p[1] == '-') && asm_is_digit(p[2])))) {
                    is_float = true;
                    p += 2;
                    while (asm_is_digit(*p)) p++;
                }
                while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L' || *p == 'f' || *p == 'F') p++;
            }
            toks_push(&out, (AsmTok){is_float ? AT_FLOAT : AT_INT, asm_strndup(s, p - s), file, line, col});
            continue;
        }
        if (asm_is_ident_start(c)) {
            const char *s = p;
            while (asm_is_ident_char(*p)) p++;
            toks_push(&out, (AsmTok){AT_IDENT, asm_strndup(s, p - s), file, line, col});
            continue;
        }
        static const char *two[] = {"->", "<<", ">>", "==", "!=", "<=", ">=", "&&", "||", "..", null};
        bool matched = false;
        for (i32 i = 0; two[i]; i++) {
            if (p[0] == two[i][0] && p[1] == two[i][1]) {
                toks_push(&out, (AsmTok){AT_PUNCT, asm_strndup(p, 2), file, line, col});
                p += 2;
                matched = true;
                break;
            }
        }
        if (matched) continue;
        toks_push(&out, (AsmTok){AT_PUNCT, asm_strndup(p, 1), file, line, col});
        p++;
    }
    toks_push(&out, (AsmTok){AT_NL, asm_strdup("\\n"), file, line, 1});
    toks_push(&out, (AsmTok){AT_EOF, asm_strdup(""), file, line, 1});
    return out;
}

static bool tok_is(AsmTok *t, const char *text) {
    return (t->kind == AT_PUNCT || t->kind == AT_IDENT) && asm_streq(t->text, text);
}

/* ---------------------------------------------------------------------------
   Program pieces collected from the .rasm files.
   ------------------------------------------------------------------------ */

typedef struct AsmStmt {
    AsmToks toks;    // no newline tokens
    i32 file;
    i32 line;        // the line the debugger shows for this statement
    bool is_label;
} AsmStmt;

typedef struct AsmProc {
    char *name;
    i32 file;
    i32 line;
    bool is_static;
    AsmList stmts;   // AsmStmt*
    i32 func_id;
} AsmProc;

typedef struct AsmMacro {
    char *name;
    AsmList params;  // char*
    AsmList stmts;   // AsmStmt*
    i32 file;
    i32 line;
} AsmMacro;

typedef struct AsmDefine {
    char *name;
    char *text;      // the replacement text, as written
    bool from_rasm;  // written in a .rasm file, so the data file must define it
    bool evaluating;
    i32 file;        // where, when from_rasm
    i32 line;
} AsmDefine;

static AsmList asm_procs;          // AsmProc*
static AsmMap asm_proc_map;        // name -> AsmProc*
static AsmMap asm_macro_map;       // name -> AsmList* of AsmMacro* (overloads)
static AsmMap asm_define_map;      // name -> AsmDefine*
static AsmList asm_define_list;    // AsmDefine*, in order
static AsmMap asm_loaded;          // canonical path -> AsmFile*

/* rin's view of the declarations. */
static AsmMap asm_struct_map;      // name -> StructDecl*
static AsmMap asm_enum_map;        // name -> EnumDecl*
static AsmMap asm_alias_map;       // name -> AliasDecl*
static AsmMap asm_global_map;      // name -> Stmt*
static AsmMap asm_rinproc_map;     // name -> ProcDecl*
static AsmList asm_struct_list, asm_enum_list, asm_alias_list, asm_global_list;
static AsmList asm_c_includes;     // char*, cinclude paths
static AsmList asm_pp_lines;       // char*, other '#' lines from .rin files

/* ---------------------------------------------------------------------------
   Loading: split each .rasm file into its procs and macros (for the assembler)
   and its declarations (as rin text, for rin's parser).
   ------------------------------------------------------------------------ */

static char *asm_clean_line(const char *line) {
    /* The line without its `;` comment and trailing blanks. A `#` line is C's
       and is kept whole. */
    const char *p = line;
    bool in_str = false;
    char q = 0;
    const char *end = line + strlen(line);
    const char *first = line + strspn(line, " \t");
    if (*first == '#') {
        end = line + strlen(line);
    } else {
        for (; *p; p++) {
            if (in_str) {
                if (*p == '\\' && p[1]) {
                    p++;
                    continue;
                }
                if (*p == q) in_str = false;
                continue;
            }
            if (*p == '"' || *p == '\'') {
                in_str = true;
                q = *p;
                continue;
            }
            if (*p == ';') {
                end = p;
                break;
            }
        }
    }
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    return asm_strndup(line, end - line);
}

static char *asm_dir_of(const char *path) {
    const char *slash = null;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    if (!slash) return asm_strdup("");
    return asm_strndup(path, slash - path + 1);
}

static char *asm_resolve_relative(const char *from_file, const char *rel) {
    if ((rel[0] && rel[1] == ':') || rel[0] == '/' || rel[0] == '\\') {
        return asm_strdup(canonicalize_path(asm_arena, string8_from_cstr(asm_arena, rel)));
    }
    char *dir = asm_dir_of(from_file);
    AsmSb s = {0};
    sb_add(&s, dir);
    sb_add(&s, rel);
    char *out = asm_strdup(canonicalize_path(asm_arena, string8_from_cstr(asm_arena, sb_cstr(&s))));
    free(s.d);
    free(dir);
    return out;
}

static char *asm_unquote(const char *lit) {
    size_t n = strlen(lit);
    if (n >= 2 && lit[0] == '"' && lit[n - 1] == '"') return asm_strndup(lit + 1, (i64)n - 2);
    return asm_strdup(lit);
}

static void asm_add_define_line(const char *directive, bool from_rasm, i32 file, i32 line) {
    /* `#define NAME value`. Function-like macros and other directives belong to C. */
    const char *p = directive;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '#') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "define", 6) != 0 || asm_is_ident_char(p[6])) {
        if (!from_rasm) list_push(&asm_pp_lines, asm_strdup(directive));
        else asm_error_at(file, line, 1, "only object-like #define lines are allowed in a .rasm file");
        return;
    }
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    const char *ns = p;
    while (asm_is_ident_char(*p)) p++;
    if (p == ns) return;
    char *name = asm_strndup(ns, p - ns);
    if (*p == '(') {
        if (from_rasm) asm_error_at(file, line, 1, "function-like #define %s is C's; rasm has no use for it", name);
        else list_push(&asm_pp_lines, asm_strdup(directive));
        free(name);
        return;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (from_rasm && strchr(p, ';')) {
        asm_error_at(file, line, 1, "a #define line takes no ';' comment: C would read it as part of the value");
    }
    if (map_get(&asm_define_map, name)) {
        free(name);
        return;
    }
    AsmDefine *d = (AsmDefine *)calloc(1, sizeof(AsmDefine));
    d->name = name;
    d->text = asm_strdup(p);
    d->from_rasm = from_rasm;
    d->file = file;
    d->line = line;
    map_put(&asm_define_map, name, d);
    list_push(&asm_define_list, d);
}

/* One statement: tokens up to a newline outside ( ) and [ ]. */
static AsmStmt *asm_take_stmt(AsmToks *t, i32 *pos) {
    while (t->d[*pos].kind == AT_NL) (*pos)++;
    if (t->d[*pos].kind == AT_EOF) return null;
    AsmStmt *s = (AsmStmt *)calloc(1, sizeof(AsmStmt));
    s->file = t->d[*pos].file;
    s->line = t->d[*pos].line;
    i32 depth = 0;
    while (t->d[*pos].kind != AT_EOF) {
        AsmTok *k = &t->d[*pos];
        if (k->kind == AT_NL && depth == 0) break;
        if (k->kind == AT_NL) {
            (*pos)++;
            continue;
        }
        if (tok_is(k, "(") || tok_is(k, "[")) depth++;
        if (tok_is(k, ")") || tok_is(k, "]")) depth--;
        toks_push(&s->toks, *k);
        (*pos)++;
        /* A label ends its statement, so `again: dec rcx` is two. */
        if (depth == 0 && s->toks.n == 2 && s->toks.d[0].kind == AT_IDENT && tok_is(&s->toks.d[1], ":") &&
            t->d[*pos].kind != AT_NL) {
            break;
        }
    }
    if (s->toks.n == 2 && s->toks.d[0].kind == AT_IDENT && tok_is(&s->toks.d[1], ":")) s->is_label = true;
    return s;
}

/* The statements of a `{ ... }` body that starts after token *pos (the `{`). */
static void asm_take_body(AsmToks *t, i32 *pos, AsmList *out, i32 *end_line) {
    while (true) {
        while (t->d[*pos].kind == AT_NL) (*pos)++;
        AsmTok *k = &t->d[*pos];
        if (k->kind == AT_EOF) {
            asm_error_at(k->file, k->line, k->col, "missing '}' at the end of a body");
            *end_line = k->line;
            return;
        }
        if (tok_is(k, "}")) {
            *end_line = k->line;
            (*pos)++;
            return;
        }
        AsmStmt *s = asm_take_stmt(t, pos);
        if (!s) return;
        if (s->toks.n > 0 && tok_is(&s->toks.d[s->toks.n - 1], "}")) {
            /* `ret }` on one line: split off the brace. */
            s->toks.n--;
            (*pos)--;
            while (t->d[*pos].kind != AT_PUNCT || !asm_streq(t->d[*pos].text, "}")) (*pos)--;
            if (s->toks.n > 0) list_push(out, s);
            continue;
        }
        list_push(out, s);
    }
}

static void asm_load_file(const char *path, i32 from_file, i32 from_line);

static void asm_scan_file(AsmFile *f, AsmToks *t) {
    /* Per line: blank it, replace it, or add a ';' after it in the rin text. */
    char **out = (char **)calloc((size_t)f->nlines + 2, sizeof(char *));
    bool *semi = (bool *)calloc((size_t)f->nlines + 2, sizeof(bool));
    bool *blank = (bool *)calloc((size_t)f->nlines + 2, sizeof(bool));

    i32 pos = 0;
    while (true) {
        while (t->d[pos].kind == AT_NL) pos++;
        AsmTok *k = &t->d[pos];
        if (k->kind == AT_EOF) break;

        if (k->kind == AT_DIRECTIVE) {
            asm_add_define_line(k->text, true, f->id, k->line);
            pos++;
            continue;
        }

        if (tok_is(k, "import")) {
            AsmTok *lit = &t->d[pos + 1];
            if (lit->kind != AT_STR) {
                asm_error_at(f->id, k->line, k->col, "expected a path string after import");
                pos++;
                continue;
            }
            char *rel = asm_unquote(lit->text);
            if (asm_ends_with(rel, ".rasm")) {
                char *full = asm_resolve_relative(f->path, rel);
                asm_load_file(full, f->id, k->line);
                blank[k->line] = true;
            }
            /* A .rin import stays in the rin text, where rin's parser resolves it. */
            free(rel);
            pos += 2;
            continue;
        }

        if (k->kind == AT_IDENT && tok_is(&t->d[pos + 1], ":")) {
            i32 start_line = k->line;
            char *name = k->text;
            AsmTok *kind = &t->d[pos + 2];

            if (tok_is(kind, "macro")) {
                AsmMacro *m = (AsmMacro *)calloc(1, sizeof(AsmMacro));
                m->name = name;
                m->file = f->id;
                m->line = k->line;
                pos += 3;
                if (!tok_is(&t->d[pos], "(")) {
                    asm_error_at(f->id, k->line, k->col, "expected '(' after macro");
                } else {
                    pos++;
                    while (!tok_is(&t->d[pos], ")") && t->d[pos].kind != AT_EOF) {
                        if (t->d[pos].kind == AT_IDENT) list_push(&m->params, t->d[pos].text);
                        pos++;
                    }
                    pos++;
                }
                if (!tok_is(&t->d[pos], "=") || !tok_is(&t->d[pos + 1], "{")) {
                    asm_error_at(f->id, k->line, k->col, "expected '= {' after the macro's parameters");
                    pos++;
                    continue;
                }
                pos += 2;
                i32 end_line = start_line;
                asm_take_body(t, &pos, &m->stmts, &end_line);
                for (i32 l = start_line; l <= end_line; l++) blank[l] = true;
                AsmList *overloads = (AsmList *)map_get(&asm_macro_map, name);
                if (!overloads) {
                    overloads = (AsmList *)calloc(1, sizeof(AsmList));
                    map_put(&asm_macro_map, name, overloads);
                }
                for (i32 i = 0; i < overloads->n; i++) {
                    if (((AsmMacro *)overloads->d[i])->params.n == m->params.n) {
                        asm_error_at(f->id, k->line, k->col, "macro %s already has a definition with %d parameters", name, m->params.n);
                    }
                }
                list_push(overloads, m);
                continue;
            }

            if (tok_is(kind, "proc")) {
                /* Find the `=` that ends the header, outside parentheses. */
                i32 j = pos + 3;
                i32 depth = 0;
                bool is_static = false;
                if (tok_is(&t->d[j], "[")) {
                    while (!tok_is(&t->d[j], "]") && t->d[j].kind != AT_EOF) {
                        if (tok_is(&t->d[j], "static")) is_static = true;
                        j++;
                    }
                    j++;
                }
                i32 sig_start = j;
                while (t->d[j].kind != AT_EOF) {
                    if (tok_is(&t->d[j], "(")) depth++;
                    if (tok_is(&t->d[j], ")")) depth--;
                    if (depth == 0 && tok_is(&t->d[j], "=")) break;
                    j++;
                }
                i32 sig_end = j;
                i32 after = j + 1;
                while (t->d[after].kind == AT_NL) after++;
                if (!tok_is(&t->d[after], "{")) {
                    asm_error_at(f->id, k->line, k->col, "expected '{' after proc %s's '='", name);
                    pos = after;
                    continue;
                }
                i32 body_first = after + 1;
                i32 peek = body_first;
                while (t->d[peek].kind == AT_NL) peek++;
                if (tok_is(&t->d[peek], "}")) {
                    /* `= {}`: a declaration of a proc defined elsewhere. rin text as is. */
                    pos = peek + 1;
                    continue;
                }
                AsmProc *pr = (AsmProc *)calloc(1, sizeof(AsmProc));
                pr->name = name;
                pr->file = f->id;
                pr->line = k->line;
                pr->is_static = is_static;
                pos = body_first;
                i32 end_line = start_line;
                asm_take_body(t, &pos, &pr->stmts, &end_line);
                if (map_get(&asm_proc_map, name)) {
                    asm_error_at(f->id, k->line, k->col, "proc %s is defined twice", name);
                }
                map_put(&asm_proc_map, name, pr);
                list_push(&asm_procs, pr);
                /* The rin text keeps the signature, as an external declaration. */
                AsmSb hdr = {0};
                sb_printf(&hdr, "%s: proc[external]", name);
                bool has_ret = false;
                for (i32 x = sig_start; x < sig_end; x++) {
                    if (t->d[x].kind == AT_NL) continue;
                    if (tok_is(&t->d[x], "->")) has_ret = true;
                    sb_add(&hdr, " ");
                    sb_add(&hdr, t->d[x].text);
                }
                /* rin wants a return type; a rasm proc may leave it out. */
                if (!has_ret) sb_add(&hdr, " -> void");
                sb_add(&hdr, " = {}");
                for (i32 l = start_line; l <= end_line; l++) blank[l] = true;
                out[start_line] = asm_strdup(sb_cstr(&hdr));
                blank[start_line] = false;
                free(hdr.d);
                continue;
            }

            /* A declaration for rin: a struct, union or enum body, or a value. */
            bool body_kind = tok_is(kind, "struct") || tok_is(kind, "union") || tok_is(kind, "enum");
            bool is_enum = tok_is(kind, "enum");
            i32 j = pos + 2;
            i32 depth = 0;
            i32 last_line = k->line;
            bool seen_body = false;
            while (t->d[j].kind != AT_EOF) {
                AsmTok *x = &t->d[j];
                if (x->kind == AT_NL) {
                    if (depth == 0 && (!body_kind || seen_body)) break;
                    if (body_kind && !is_enum && depth >= 1) {
                        AsmTok *prev = &t->d[j - 1];
                        if (prev->kind != AT_NL && !tok_is(prev, "{") && !tok_is(prev, ";")) semi[prev->line] = true;
                    }
                    j++;
                    continue;
                }
                if (tok_is(x, "(") || tok_is(x, "[") || tok_is(x, "{")) depth++;
                if (tok_is(x, ")") || tok_is(x, "]") || tok_is(x, "}")) {
                    depth--;
                    if (body_kind && depth == 0 && tok_is(x, "}")) seen_body = true;
                }
                last_line = x->line;
                j++;
            }
            if (!body_kind) semi[last_line] = true;
            pos = j;
            continue;
        }

        asm_error_at(f->id, k->line, k->col, "expected a declaration, a proc, a macro, an import or a #define, found '%s'", k->text);
        while (t->d[pos].kind != AT_NL && t->d[pos].kind != AT_EOF) pos++;
    }

    for (i32 l = 1; l <= f->nlines; l++) {
        if (out[l]) {
            sb_add(&f->rin_text, out[l]);
        } else if (!blank[l]) {
            char *clean = asm_clean_line(f->lines[l - 1]);
            sb_add(&f->rin_text, clean);
            if (semi[l] && clean[0] && clean[strlen(clean) - 1] != ';') sb_add(&f->rin_text, ";");
            free(clean);
        }
        sb_add(&f->rin_text, "\n");
    }
    free(out);
    free(semi);
    free(blank);
}

static void asm_load_file(const char *path, i32 from_file, i32 from_line) {
    if (map_get(&asm_loaded, path)) return;
    /* --stdin-path: the editor's unsaved text for this one file */
    string8 src = read_rin_source_for_path(asm_arena, path);
    if (!src.data) {
        asm_error_at(from_file, from_line, 1, "cannot read %s", path);
        return;
    }
    AsmFile *f = (AsmFile *)calloc(1, sizeof(AsmFile));
    f->path = asm_strdup(path);
    f->src = asm_s8(src);
    list_push(&asm_files, f);
    f->id = asm_files.n;
    map_put(&asm_loaded, path, f);

    /* Lines, for the rin text. */
    i32 count = 1;
    for (char *p = f->src; *p; p++) if (*p == '\n') count++;
    f->lines = (char **)calloc((size_t)count + 1, sizeof(char *));
    char *s = f->src;
    for (char *p = f->src;; p++) {
        if (*p == '\n' || *p == 0) {
            f->lines[f->nlines++] = asm_strndup(s, p - s);
            if (*p == 0) break;
            s = p + 1;
        }
    }
    AsmToks t = asm_lex(f->src, f->id);
    asm_scan_file(f, &t);
}

/* ---------------------------------------------------------------------------
   rin's parser reads the declarations.
   ------------------------------------------------------------------------ */

static void asm_collect_program(Program *prog) {
    for (i32 i = 0; i < prog->structs.length; i++) {
        StructDecl *sd = (StructDecl *)prog->structs.data[i];
        char *name = asm_s8(sd->name);
        if (!map_get(&asm_struct_map, name)) {
            map_put(&asm_struct_map, name, sd);
            list_push(&asm_struct_list, sd);
        }
        free(name);
    }
    for (i32 i = 0; i < prog->enums.length; i++) {
        EnumDecl *ed = (EnumDecl *)prog->enums.data[i];
        char *name = asm_s8(ed->name);
        if (!map_get(&asm_enum_map, name)) {
            map_put(&asm_enum_map, name, ed);
            list_push(&asm_enum_list, ed);
        }
        free(name);
    }
    for (i32 i = 0; i < prog->aliases.length; i++) {
        AliasDecl *ad = (AliasDecl *)prog->aliases.data[i];
        char *name = asm_s8(ad->name);
        if (!map_get(&asm_alias_map, name)) {
            map_put(&asm_alias_map, name, ad);
            list_push(&asm_alias_list, ad);
        }
        free(name);
    }
    for (i32 i = 0; i < prog->globals.length; i++) {
        Stmt *g = (Stmt *)prog->globals.data[i];
        char *name = asm_s8(g->name);
        if (!map_get(&asm_global_map, name)) {
            map_put(&asm_global_map, name, g);
            list_push(&asm_global_list, g);
        }
        free(name);
    }
    for (i32 i = 0; i < prog->procs.length; i++) {
        ProcDecl *pd = (ProcDecl *)prog->procs.data[i];
        char *name = asm_s8(pd->name);
        if (!map_get(&asm_rinproc_map, name)) map_put(&asm_rinproc_map, name, pd);
        free(name);
    }
    for (i32 i = 0; i < prog->c_imports.length; i++) {
        char *inc = asm_unquote(asm_s8(prog->c_imports.data[i]));
        bool seen = false;
        for (i32 k = 0; k < asm_c_includes.n; k++) {
            if (asm_streq((char *)asm_c_includes.d[k], inc)) seen = true;
        }
        if (!seen) list_push(&asm_c_includes, inc);
    }
    for (i32 i = 0; i < prog->preprocessor_lines.length; i++) {
        char *line = asm_s8(prog->preprocessor_lines.data[i]);
        asm_add_define_line(line, false, 0, 0);
        free(line);
    }
}

static bool asm_parse_declarations(void) {
    Vec_string8 visited = Vec_string8_reserve(asm_arena, 8);
    for (i32 i = 0; i < asm_files.n; i++) {
        AsmFile *f = (AsmFile *)asm_files.d[i];
        g_source_path = f->path;
        string8 text = string8_from_cstr(asm_arena, sb_cstr(&f->rin_text));
        Vec_Token tokens = {0};
        Vec_string8 directives = {0};
        lex_tokens(asm_arena, text, &tokens, &directives);
        Parser parser = {0};
        parser.arena = asm_arena;
        parser.source = text;
        parser.tokens = tokens;
        parser.index = 0;
        Program prog = parse_program(&parser);
        prog.preprocessor_lines = directives;
        Vec_string8 stack = Vec_string8_reserve(asm_arena, 8);
        Vec_string8_append(asm_arena, &stack, string8_from_cstr(asm_arena, f->path));
        prog = expand_rin_imports(asm_arena, &prog, &visited, &stack);
        if (g_error_count > 0) return false;
        asm_collect_program(&prog);
    }
    return true;
}

/* ---------------------------------------------------------------------------
   Constant expressions. Integers only: everything an immediate, an array
   count, an index or an enum value can be.
   ------------------------------------------------------------------------ */

typedef struct AsmEval {
    AsmTok *t;
    i32 n;
    i32 pos;
    bool ok;
    i32 file;
    i32 line;
} AsmEval;

static bool asm_parse_int_text(const char *text, i64 *out) {
    const char *p = text;
    u64 v = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while (*p) {
            char c = *p;
            i32 d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c == '_') { p++; continue; }
            else break;
            v = v * 16 + (u64)d;
            p++;
        }
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        while (*p == '0' || *p == '1' || *p == '_') {
            if (*p != '_') v = v * 2 + (u64)(*p - '0');
            p++;
        }
    } else {
        if (!asm_is_digit(*p)) return false;
        while (asm_is_digit(*p) || *p == '_') {
            if (*p != '_') v = v * 10 + (u64)(*p - '0');
            p++;
        }
    }
    while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') p++;
    if (*p) return false;
    *out = (i64)v;
    return true;
}

static i64 asm_char_value(const char *lit) {
    const char *p = lit + 1;
    if (*p == '\\') {
        p++;
        switch (*p) {
            case 'n': return '\n';
            case 't': return '\t';
            case 'r': return '\r';
            case '0': return 0;
            case '\\': return '\\';
            case '\'': return '\'';
            case '"': return '"';
            default: return (u8)*p;
        }
    }
    return (u8)*p;
}

typedef struct AsmSA {
    i64 size;
    i64 align;
    bool ok;
} AsmSA;

static bool asm_primitive_sa(const char *n, AsmSA *out);
static i64 asm_eval_expr(AsmEval *e);
static EnumDecl *asm_enum_named(const char *name);
static bool asm_eval_text(const char *text, i64 *out, i32 file, i32 line);
static bool asm_sizeof_type_toks(AsmEval *e, i64 *out);
static bool asm_offsetof_toks(AsmEval *e, i64 *out);
static bool asm_enum_member_value(const char *enum_name, const char *member, i64 *out);

static AsmTok *ev_peek(AsmEval *e) {
    static AsmTok eof = {AT_EOF, (char *)"", 0, 0, 0};
    return e->pos < e->n ? &e->t[e->pos] : &eof;
}

static i64 asm_eval_primary(AsmEval *e) {
    AsmTok *k = ev_peek(e);
    if (k->kind == AT_EOF) {
        e->ok = false;
        return 0;
    }
    if (tok_is(k, "(")) {
        /* A C cast, as header constants are written: `( DWORD ) - 11`. The
           type is a known name, maybe with `*`s; the value is what follows. */
        i32 j = e->pos + 1;
        if (j < e->n && e->t[j].kind == AT_IDENT) {
            const char *tn = e->t[j].text;
            AsmSA prim;
            bool is_type = asm_primitive_sa(tn, &prim) || map_get(&asm_alias_map, tn) ||
                           map_get(&asm_struct_map, tn) || map_get(&asm_enum_map, tn) || asm_streq(tn, "void");
            if (is_type) {
                j++;
                while (j < e->n && tok_is(&e->t[j], "*")) j++;
                if (j < e->n && tok_is(&e->t[j], ")")) {
                    e->pos = j + 1;
                    return asm_eval_primary(e);
                }
            }
        }
        e->pos++;
        i64 v = asm_eval_expr(e);
        if (!tok_is(ev_peek(e), ")")) e->ok = false;
        else e->pos++;
        return v;
    }
    if (tok_is(k, "-")) { e->pos++; return -asm_eval_primary(e); }
    if (tok_is(k, "+")) { e->pos++; return asm_eval_primary(e); }
    if (tok_is(k, "~")) { e->pos++; return ~asm_eval_primary(e); }
    if (tok_is(k, "!")) { e->pos++; return !asm_eval_primary(e); }
    if (k->kind == AT_INT) {
        i64 v = 0;
        if (!asm_parse_int_text(k->text, &v)) e->ok = false;
        e->pos++;
        return v;
    }
    if (k->kind == AT_CHAR) {
        /* One character, maybe escaped. MASM reads 'ab' as two bytes; C
           compilers differ; taking the first silently would be wrong. */
        const char *body = k->text + 1;
        size_t len = strlen(body);                    /* the closing quote included */
        size_t want = body[0] == '\\' ? 3 : 2;        /* 'a' or '\n' */
        if (len != want) {
            asm_error_at(e->file, e->line, k->col, "%s: a character literal holds one character; write several as a number", k->text);
            e->ok = false;
        }
        e->pos++;
        return asm_char_value(k->text);
    }
    if (k->kind == AT_IDENT) {
        if ((asm_streq(k->text, "sizeof") || asm_streq(k->text, "alignof")) && tok_is(&e->t[e->pos + 1 < e->n ? e->pos + 1 : e->pos], "(")) {
            e->pos += 2;
            i64 v = 0;
            if (!asm_sizeof_type_toks(e, &v)) e->ok = false;
            if (!tok_is(ev_peek(e), ")")) e->ok = false;
            else e->pos++;
            return v;
        }
        if (asm_streq(k->text, "offsetof") && e->pos + 1 < e->n && tok_is(&e->t[e->pos + 1], "(")) {
            e->pos += 2;
            i64 v = 0;
            if (!asm_offsetof_toks(e, &v)) e->ok = false;
            if (!tok_is(ev_peek(e), ")")) e->ok = false;
            else e->pos++;
            return v;
        }
        if (asm_streq(k->text, "cast") && e->pos + 1 < e->n && tok_is(&e->t[e->pos + 1], "(")) {
            /* An integer cast in a constant: the value, the type ignored. */
            e->pos += 2;
            i64 v = asm_eval_expr(e);
            while (e->pos < e->n && !tok_is(ev_peek(e), ")")) e->pos++;
            if (tok_is(ev_peek(e), ")")) e->pos++;
            return v;
        }
        if (asm_streq(k->text, "true")) { e->pos++; return 1; }
        if (asm_streq(k->text, "false")) { e->pos++; return 0; }
        /* Enum.Member */
        if (e->pos + 2 < e->n && tok_is(&e->t[e->pos + 1], ".") && e->t[e->pos + 2].kind == AT_IDENT &&
            asm_enum_named(k->text)) {
            i64 v = 0;
            if (!asm_enum_member_value(k->text, e->t[e->pos + 2].text, &v)) e->ok = false;
            e->pos += 3;
            return v;
        }
        AsmDefine *d = (AsmDefine *)map_get(&asm_define_map, k->text);
        if (d) {
            e->pos++;
            if (d->evaluating) {
                e->ok = false;
                return 0;
            }
            d->evaluating = true;
            i64 v = 0;
            bool ok = asm_eval_text(d->text, &v, e->file, e->line);
            d->evaluating = false;
            if (!ok) e->ok = false;
            return v;
        }
    }
    e->ok = false;
    return 0;
}

static i32 asm_binop_prec(AsmTok *k) {
    if (k->kind != AT_PUNCT) return -1;
    const char *s = k->text;
    if (asm_streq(s, "||")) return 1;
    if (asm_streq(s, "&&")) return 2;
    if (asm_streq(s, "|")) return 3;
    if (asm_streq(s, "^")) return 4;
    if (asm_streq(s, "&")) return 5;
    if (asm_streq(s, "==") || asm_streq(s, "!=")) return 6;
    if (asm_streq(s, "<") || asm_streq(s, ">") || asm_streq(s, "<=") || asm_streq(s, ">=")) return 7;
    if (asm_streq(s, "<<") || asm_streq(s, ">>")) return 8;
    if (asm_streq(s, "+") || asm_streq(s, "-")) return 9;
    if (asm_streq(s, "*") || asm_streq(s, "/") || asm_streq(s, "%")) return 10;
    return -1;
}

static i64 asm_eval_binary(AsmEval *e, i32 min_prec) {
    i64 left = asm_eval_primary(e);
    while (e->ok) {
        AsmTok *op = ev_peek(e);
        i32 prec = asm_binop_prec(op);
        if (prec < min_prec || prec < 0) break;
        e->pos++;
        i64 right = asm_eval_binary(e, prec + 1);
        const char *s = op->text;
        if (asm_streq(s, "+")) left = left + right;
        else if (asm_streq(s, "-")) left = left - right;
        else if (asm_streq(s, "*")) left = left * right;
        else if (asm_streq(s, "/")) left = right ? left / right : 0;
        else if (asm_streq(s, "%")) left = right ? left % right : 0;
        else if (asm_streq(s, "<<")) left = (i64)((u64)left << right);
        else if (asm_streq(s, ">>")) left = left >> right;
        else if (asm_streq(s, "&")) left = left & right;
        else if (asm_streq(s, "|")) left = left | right;
        else if (asm_streq(s, "^")) left = left ^ right;
        else if (asm_streq(s, "&&")) left = left && right;
        else if (asm_streq(s, "||")) left = left || right;
        else if (asm_streq(s, "==")) left = left == right;
        else if (asm_streq(s, "!=")) left = left != right;
        else if (asm_streq(s, "<")) left = left < right;
        else if (asm_streq(s, ">")) left = left > right;
        else if (asm_streq(s, "<=")) left = left <= right;
        else if (asm_streq(s, ">=")) left = left >= right;
    }
    return left;
}

static i64 asm_eval_expr(AsmEval *e) { return asm_eval_binary(e, 1); }

/* Whole token range as one constant. */
static bool asm_eval_toks(AsmTok *t, i32 n, i64 *out, i32 file, i32 line) {
    AsmEval e = {t, n, 0, true, file, line};
    i64 v = asm_eval_expr(&e);
    if (!e.ok || e.pos != n) return false;
    *out = v;
    return true;
}

static bool asm_eval_text(const char *text, i64 *out, i32 file, i32 line) {
    AsmToks t = asm_lex(text, file);
    /* drop the trailing newline and EOF */
    i32 n = 0;
    while (n < t.n && t.d[n].kind != AT_NL && t.d[n].kind != AT_EOF) n++;
    return asm_eval_toks(t.d, n, out, file, line);
}

/* Constant value of a rin expression: enum values and array counts. */
static bool asm_eval_rin(Expr *x, EnumDecl *scope_enum, i64 *out);

static bool asm_enum_item_value(EnumDecl *ed, i32 index, i64 *out) {
    i64 v = 0;
    for (i32 i = 0; i <= index; i++) {
        EnumItem *it = (EnumItem *)ed->items.data[i];
        if (it->value_expr) {
            if (!asm_eval_rin(it->value_expr, ed, &v)) return false;
        } else if (it->value.length > 0) {
            char *txt = asm_s8(it->value);
            bool ok = asm_eval_text(txt, &v, 0, it->line);
            if (!ok) {
                /* another member of the same enum */
                bool found = false;
                for (i32 k = 0; k < ed->items.length; k++) {
                    EnumItem *o = (EnumItem *)ed->items.data[k];
                    char *on = asm_s8(o->name);
                    if (asm_streq(on, txt) && k < i) {
                        found = asm_enum_item_value(ed, k, &v);
                    }
                    free(on);
                }
                if (!found) {
                    free(txt);
                    return false;
                }
            }
            free(txt);
        } else if (i > 0) {
            v = v + 1;
        } else {
            v = 0;
        }
    }
    *out = v;
    return true;
}

static bool asm_enum_member_value(const char *enum_name, const char *member, i64 *out) {
    EnumDecl *ed = asm_enum_named(enum_name);
    if (!ed) return false;
    for (i32 i = 0; i < ed->items.length; i++) {
        EnumItem *it = (EnumItem *)ed->items.data[i];
        char *name = asm_s8(it->name);
        bool hit = asm_streq(name, member);
        free(name);
        if (hit) return asm_enum_item_value(ed, i, out);
    }
    return false;
}

static bool asm_eval_rin(Expr *x, EnumDecl *scope_enum, i64 *out) {
    if (!x) return false;
    switch (x->kind) {
        case Expr_Number: {
            char *t = asm_s8(x->number);
            bool ok = asm_parse_int_text(t, out);
            free(t);
            return ok;
        }
        case Expr_Char: {
            char *t = asm_s8(x->string_lit);
            *out = asm_char_value(t);
            free(t);
            return true;
        }
        case Expr_Name: {
            char *t = asm_s8(x->name);
            bool ok = false;
            if (scope_enum) {
                for (i32 i = 0; i < scope_enum->items.length; i++) {
                    EnumItem *it = (EnumItem *)scope_enum->items.data[i];
                    char *n = asm_s8(it->name);
                    if (asm_streq(n, t)) ok = asm_enum_item_value(scope_enum, i, out);
                    free(n);
                    if (ok) break;
                }
            }
            if (!ok) ok = asm_eval_text(t, out, 0, x->line);
            free(t);
            return ok;
        }
        case Expr_Field: {
            if (x->base && x->base->kind == Expr_Name) {
                char *en = asm_s8(x->base->name);
                char *mn = asm_s8(x->name);
                bool ok = asm_enum_member_value(en, mn, out);
                free(en);
                free(mn);
                return ok;
            }
            return false;
        }
        case Expr_Unary: {
            i64 v = 0;
            if (!asm_eval_rin(x->inner ? x->inner : x->right, scope_enum, &v)) return false;
            if (x->op == Token_Minus) *out = -v;
            else if (x->op == Token_Tilde) *out = ~v;
            else if (x->op == Token_Bang) *out = !v;
            else *out = v;
            return true;
        }
        case Expr_Binary: {
            i64 a = 0, b = 0;
            if (!asm_eval_rin(x->left, scope_enum, &a) || !asm_eval_rin(x->right, scope_enum, &b)) return false;
            switch (x->op) {
                case Token_Plus: *out = a + b; break;
                case Token_Minus: *out = a - b; break;
                case Token_Star: *out = a * b; break;
                case Token_Slash: *out = b ? a / b : 0; break;
                case Token_Percent: *out = b ? a % b : 0; break;
                case Token_Keyword_Shl: *out = (i64)((u64)a << b); break;
                case Token_Keyword_Shr: *out = a >> b; break;
                case Token_Ampersand: *out = a & b; break;
                case Token_Pipe: *out = a | b; break;
                case Token_Caret: *out = a ^ b; break;
                default: return false;
            }
            return true;
        }
        case Expr_Cast:
            return asm_eval_rin(x->inner, scope_enum, out);
        default:
            return false;
    }
}

/* ---------------------------------------------------------------------------
   Layout, by C's rules for x64.
   ------------------------------------------------------------------------ */

typedef struct AsmFieldLayout {
    char *name;      // empty for an anonymous member
    i64 offset;
    TypeExpr *type;  // null for an anonymous member
    StructDecl *anon;
} AsmFieldLayout;

typedef struct AsmStructLayout {
    i64 size;
    i64 align;
    bool ok;
    bool busy;
    AsmFieldLayout *fields;
    i32 nfields;
} AsmStructLayout;

static AsmMap asm_layout_cache; // StructDecl pointer as text -> AsmStructLayout*


static AsmSA asm_type_sa(TypeExpr *t);
static AsmStructLayout *asm_struct_layout(StructDecl *sd);

static bool asm_primitive_sa(const char *n, AsmSA *out) {
    static const struct { const char *name; i64 size; } prims[] = {
        {"u8", 1}, {"i8", 1}, {"b8", 1}, {"c8", 1}, {"char", 1}, {"bool", 1},
        {"u16", 2}, {"i16", 2}, {"b16", 2}, {"short", 2},
        {"u32", 4}, {"i32", 4}, {"b32", 4}, {"f32", 4}, {"int", 4}, {"float", 4},
        /* rin's C-width names, as rinbind writes them; long is 4 bytes on Windows */
        {"long", 4}, {"ulong", 4},
        {"u64", 8}, {"i64", 8}, {"f64", 8}, {"usize", 8}, {"isize", 8}, {"ptrdiff", 8},
        {"b64", 8}, {"intptr", 8}, {"uintptr", 8}, {"intmax", 8}, {"uintmax", 8}, {"double", 8},
        {null, 0}};
    for (i32 i = 0; prims[i].name; i++) {
        if (asm_streq(prims[i].name, n)) {
            out->size = prims[i].size;
            out->align = prims[i].size;
            out->ok = true;
            return true;
        }
    }
    return false;
}

/* Follow aliases to the type that has a shape. */
static TypeExpr *asm_resolve_type(TypeExpr *t) {
    for (i32 guard = 0; t && t->kind == Type_Name && guard < 64; guard++) {
        char *n = asm_s8(t->name);
        AliasDecl *ad = (AliasDecl *)map_get(&asm_alias_map, n);
        free(n);
        if (!ad || !ad->type) break;
        t = ad->type;
    }
    return t;
}

/* An enum by name, through aliases: D3D11_PRIMITIVE_TOPOLOGY is
   D3D_PRIMITIVE_TOPOLOGY in the headers. */
static EnumDecl *asm_enum_named(const char *name) {
    EnumDecl *ed = (EnumDecl *)map_get(&asm_enum_map, name);
    if (ed) return ed;
    AliasDecl *ad = (AliasDecl *)map_get(&asm_alias_map, name);
    for (i32 guard = 0; ad && ad->type && ad->type->kind == Type_Name && guard < 16; guard++) {
        char *n = asm_s8(ad->type->name);
        ed = (EnumDecl *)map_get(&asm_enum_map, n);
        AliasDecl *next = (AliasDecl *)map_get(&asm_alias_map, n);
        free(n);
        if (ed) return ed;
        ad = next;
    }
    return null;
}

static StructDecl *asm_type_struct(TypeExpr *t) {
    t = asm_resolve_type(t);
    if (!t || t->kind != Type_Name) return null;
    char *n = asm_s8(t->name);
    StructDecl *sd = (StructDecl *)map_get(&asm_struct_map, n);
    free(n);
    return sd;
}

static bool asm_array_count(TypeExpr *t, i64 *out) {
    char *txt = asm_s8(t->array_count);
    bool ok = asm_eval_text(txt, out, 0, t->line);
    free(txt);
    return ok;
}

static AsmSA asm_type_sa(TypeExpr *t) {
    AsmSA r = {0, 1, false};
    t = asm_resolve_type(t);
    if (!t) return r;
    switch (t->kind) {
        case Type_Ptr:
        case Type_Proc:
            r.size = 8;
            r.align = 8;
            r.ok = true;
            return r;
        case Type_Array: {
            AsmSA e = asm_type_sa(t->elem);
            i64 count = 0;
            if (!e.ok) return r;
            if (t->array_count.length == 0) {
                /* `[]T`: a slice, pointer and length. */
                r.size = 16;
                r.align = 8;
                r.ok = true;
                return r;
            }
            if (!asm_array_count(t, &count)) return r;
            r.size = e.size * count;
            r.align = e.align;
            r.ok = true;
            return r;
        }
        case Type_Name: {
            char *n = asm_s8(t->name);
            if (asm_primitive_sa(n, &r)) {
                free(n);
                return r;
            }
            StructDecl *sd = (StructDecl *)map_get(&asm_struct_map, n);
            if (sd) {
                AsmStructLayout *l = asm_struct_layout(sd);
                free(n);
                if (l && l->ok) {
                    r.size = l->size;
                    r.align = l->align;
                    r.ok = true;
                }
                return r;
            }
            EnumDecl *ed = (EnumDecl *)map_get(&asm_enum_map, n);
            free(n);
            if (ed) {
                if (ed->underlying.length) {
                    char *u = asm_s8(ed->underlying);
                    asm_primitive_sa(u, &r);
                    free(u);
                    if (r.ok) return r;
                }
                r.size = 4;
                r.align = 4;
                r.ok = true;
            }
            return r;
        }
        default:
            return r;
    }
}

static AsmStructLayout *asm_struct_layout(StructDecl *sd) {
    char key[32];
    snprintf(key, sizeof(key), "%p", (void *)sd);
    AsmStructLayout *l = (AsmStructLayout *)map_get(&asm_layout_cache, key);
    if (l) return l->busy ? null : l;
    l = (AsmStructLayout *)calloc(1, sizeof(AsmStructLayout));
    l->busy = true;
    map_put(&asm_layout_cache, key, l);
    l->fields = (AsmFieldLayout *)calloc((size_t)sd->fields.length + 1, sizeof(AsmFieldLayout));
    i64 offset = 0, size = 0, align = 1;
    bool ok = !sd->is_generic && sd->fields.length > 0;
    i64 bit_unit_offset = -1, bit_unit_size = 0, bits_used = 0;
    for (i32 i = 0; i < sd->fields.length && ok; i++) {
        Field *f = (Field *)sd->fields.data[i];
        AsmSA sa = {0, 1, false};
        if (f->anon) {
            AsmStructLayout *al = asm_struct_layout(f->anon);
            if (al && al->ok) {
                sa.size = al->size;
                sa.align = al->align;
                sa.ok = true;
            }
        } else {
            sa = asm_type_sa(f->type);
        }
        if (!sa.ok) {
            ok = false;
            break;
        }
        if (f->align.length) {
            char *a = asm_s8(f->align);
            i64 av = 0;
            if (asm_eval_text(a, &av, 0, f->line) && av > sa.align) sa.align = av;
            free(a);
        }
        if (sd->packed) sa.align = 1;
        if (f->bit_width.length) {
            /* MSVC: bitfields of one type share a unit of that type's size. */
            char *bw = asm_s8(f->bit_width);
            i64 bits = 0;
            asm_eval_text(bw, &bits, 0, f->line);
            free(bw);
            i64 at = sd->is_union ? 0 : offset;
            if (bit_unit_offset < 0 || bit_unit_size != sa.size || bits_used + bits > sa.size * 8 || sd->is_union) {
                if (!sd->is_union) {
                    at = (offset + sa.align - 1) / sa.align * sa.align;
                    offset = at + sa.size;
                }
                bit_unit_offset = at;
                bit_unit_size = sa.size;
                bits_used = 0;
            }
            bits_used += bits;
            l->fields[l->nfields++] = (AsmFieldLayout){asm_s8(f->name), bit_unit_offset, f->type, null};
            if (sa.align > align) align = sa.align;
            if (sd->is_union && sa.size > size) size = sa.size;
            continue;
        }
        bit_unit_offset = -1;
        i64 at = 0;
        if (!sd->is_union) {
            at = (offset + sa.align - 1) / sa.align * sa.align;
            offset = at + sa.size;
        } else if (sa.size > size) {
            size = sa.size;
        }
        l->fields[l->nfields++] = (AsmFieldLayout){f->anon ? asm_strdup("") : asm_s8(f->name), at, f->anon ? null : f->type, f->anon};
        if (sa.align > align) align = sa.align;
    }
    if (sd->align.length) {
        char *a = asm_s8(sd->align);
        i64 av = 0;
        if (asm_eval_text(a, &av, 0, sd->line) && av > align) align = av;
        free(a);
    }
    if (!sd->is_union) size = offset;
    size = (size + align - 1) / align * align;
    l->size = size;
    l->align = align;
    l->ok = ok;
    l->busy = false;
    return l;
}

/* A field by name, looking through anonymous members. */
/* rinbind writes C's anonymous struct and union members, in a header's
   (external) struct, as fields named `_anon0`, `_anon1`, ... of a named type. C has no such names, so they are
   looked through, as C does: `desc.Texture2DArray.MipSlice`. */
static bool asm_is_anon_name(const char *name) {
    if (strncmp(name, "_anon", 5) != 0 || !name[5]) return false;
    for (const char *p = name + 5; *p; p++) if (!asm_is_digit(*p)) return false;
    return true;
}

static bool asm_find_field(StructDecl *sd, const char *name, i64 *offset, TypeExpr **type) {
    AsmStructLayout *l = asm_struct_layout(sd);
    if (!l || !l->ok) return false;
    for (i32 i = 0; i < l->nfields; i++) {
        AsmFieldLayout *f = &l->fields[i];
        StructDecl *through = f->anon;
        if (!through && sd->is_external && f->name && asm_is_anon_name(f->name) && !asm_streq(f->name, name)) through = asm_type_struct(f->type);
        if (through) {
            i64 inner = 0;
            if (asm_find_field(through, name, &inner, type)) {
                *offset = f->offset + inner;
                return true;
            }
        } else if (asm_streq(f->name, name)) {
            *offset = f->offset;
            *type = f->type;
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
   Layout assertions for the data file: every offset and size the code used.
   ------------------------------------------------------------------------ */

static AsmMap asm_assert_seen;
static AsmSb asm_asserts;

static void asm_note_offset(const char *struct_name, const char *path, i64 offset) {
    AsmSb key = {0};
    sb_printf(&key, "o:%s:%s", struct_name, path);
    if (!map_get(&asm_assert_seen, sb_cstr(&key))) {
        map_put(&asm_assert_seen, sb_cstr(&key), (void *)1);
        sb_printf(&asm_asserts, "_Static_assert(offsetof(%s, %s) == %lld, \"rasm layout: %s.%s\");\n",
                  struct_name, path, (long long)offset, struct_name, path);
    }
    free(key.d);
}

static void asm_note_size(const char *what, i64 size, bool is_type) {
    AsmSb key = {0};
    sb_printf(&key, "s:%s", what);
    if (!map_get(&asm_assert_seen, sb_cstr(&key))) {
        map_put(&asm_assert_seen, sb_cstr(&key), (void *)1);
        sb_printf(&asm_asserts, "_Static_assert(sizeof(%s) == %lld, \"rasm layout: sizeof(%s)\");\n",
                  what, (long long)size, what);
    }
    (void)is_type;
    free(key.d);
}

/* A field path on a type: `.f.g[2]`, starting at token *pos. Accumulates the
   offset and the final type; records the assertions. */
static bool asm_walk_path(AsmTok *t, i32 n, i32 *pos, TypeExpr **type, i64 *offset, AsmSb *path_text, i32 file, i32 line) {
    char *root_struct = null;
    i64 root_base = 0;
    AsmSb rel = {0};
    while (*pos < n) {
        AsmTok *k = &t[*pos];
        if (tok_is(k, ".") && *pos + 1 < n && t[*pos + 1].kind == AT_IDENT) {
            StructDecl *sd = asm_type_struct(*type);
            const char *fname = t[*pos + 1].text;
            if (!sd) {
                asm_error_at(file, line, k->col, "'.%s': not a struct", fname);
                free(rel.d);
                return false;
            }
            i64 fo = 0;
            TypeExpr *ft = null;
            if (!asm_find_field(sd, fname, &fo, &ft)) {
                char *sn = asm_s8(sd->name);
                asm_error_at(file, line, k->col, "%s has no field '%s' (or its layout is unknown)", sn, fname);
                free(sn);
                free(rel.d);
                return false;
            }
            if (!root_struct) {
                TypeExpr *written = *type;
                root_struct = (written && written->kind == Type_Name) ? asm_s8(written->name) : asm_s8(sd->name);
                root_base = *offset;
            }
            /* `_anonN`, written out, is not a name C knows: the assert skips it */
            if (!(sd->is_external && asm_is_anon_name(fname))) {
                if (rel.n) sb_add(&rel, ".");
                sb_add(&rel, fname);
                if (!sd->is_generic) asm_note_offset(root_struct, sb_cstr(&rel), *offset + fo - root_base);
            }
            *offset += fo;
            *type = ft;
            if (path_text) {
                sb_add(path_text, ".");
                sb_add(path_text, fname);
            }
            *pos += 2;
            continue;
        }
        if (tok_is(k, "[")) {
            TypeExpr *rt = asm_resolve_type(*type);
            if (!rt || rt->kind != Type_Array) break;
            i32 depth = 0, j = *pos;
            for (; j < n; j++) {
                if (tok_is(&t[j], "[")) depth++;
                if (tok_is(&t[j], "]") && --depth == 0) break;
            }
            i64 idx = 0;
            if (!asm_eval_toks(t + *pos + 1, j - *pos - 1, &idx, file, line)) {
                asm_error_at(file, line, k->col, "an index on a global must be a constant; use lea and a register to index at run time");
                free(rel.d);
                return false;
            }
            AsmSA es = asm_type_sa(rt->elem);
            if (!es.ok) {
                asm_error_at(file, line, k->col, "element size unknown");
                free(rel.d);
                return false;
            }
            *offset += idx * es.size;
            *type = rt->elem;
            if (rel.n) {
                char b[32];
                snprintf(b, sizeof(b), "[%lld]", (long long)idx);
                sb_add(&rel, b);
            }
            if (path_text) {
                char b[32];
                snprintf(b, sizeof(b), "[%lld]", (long long)idx);
                sb_add(path_text, b);
            }
            *pos = j + 1;
            continue;
        }
        break;
    }
    free(rel.d);
    free(root_struct);
    return true;
}

/* offsetof(T.f.g) */
static bool asm_offsetof_toks(AsmEval *e, i64 *out) {
    AsmTok *k = ev_peek(e);
    if (k->kind != AT_IDENT) return false;
    TypeExpr *t = type_new(asm_arena, Type_Name);
    t->name = string8_from_cstr(asm_arena, k->text);
    e->pos++;
    i64 off = 0;
    i32 end = e->pos;
    while (end < e->n && !tok_is(&e->t[end], ")")) end++;
    if (!asm_walk_path(e->t, end, &e->pos, &t, &off, null, e->file, e->line)) return false;
    *out = off;
    return true;
}

/* sizeof(T) or sizeof(global) */
static bool asm_sizeof_type_toks(AsmEval *e, i64 *out) {
    AsmTok *k = ev_peek(e);
    if (k->kind != AT_IDENT) return false;
    Stmt *g = (Stmt *)map_get(&asm_global_map, k->text);
    TypeExpr *t = null;
    if (g) {
        t = g->type;
    } else {
        t = type_new(asm_arena, Type_Name);
        t->name = string8_from_cstr(asm_arena, k->text);
    }
    e->pos++;
    i64 off = 0;
    i32 end = e->pos;
    while (end < e->n && !tok_is(&e->t[end], ")")) end++;
    if (!asm_walk_path(e->t, end, &e->pos, &t, &off, null, e->file, e->line)) return false;
    AsmSA sa = asm_type_sa(t);
    if (!sa.ok) return false;
    if (g && e->pos == end) asm_note_size(k->text, sa.size, false);
    else if (e->pos == end && !g && asm_type_struct(t)) asm_note_size(k->text, sa.size, true);
    *out = sa.size;
    return true;
}

/* ---------------------------------------------------------------------------
   Instructions.
   ------------------------------------------------------------------------ */

typedef enum AsmRegClass {
    ASMREG_NONE = 0,
    ASMREG_GP,
    ASMREG_VEC,
    ASMREG_OTHER,
} AsmRegClass;

static i32 asm_register_size(const char *n, AsmRegClass *cls) {
    static const char *r64[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", null};
    static const char *r32[] = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp", "r8d", "r9d", "r10d", "r11d", "r12d", "r13d", "r14d", "r15d", null};
    static const char *r16[] = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w", "r14w", "r15w", null};
    static const char *r8[] = {"al", "bl", "cl", "dl", "sil", "dil", "bpl", "spl", "ah", "bh", "ch", "dh", "r8b", "r9b", "r10b", "r11b", "r12b", "r13b", "r14b", "r15b", null};
    if (cls) *cls = ASMREG_GP;
    for (i32 i = 0; r64[i]; i++) if (asm_streq(n, r64[i])) return 8;
    for (i32 i = 0; r32[i]; i++) if (asm_streq(n, r32[i])) return 4;
    for (i32 i = 0; r16[i]; i++) if (asm_streq(n, r16[i])) return 2;
    for (i32 i = 0; r8[i]; i++) if (asm_streq(n, r8[i])) return 1;
    if (cls) *cls = ASMREG_VEC;
    if (strncmp(n, "xmm", 3) == 0 && asm_is_digit(n[3])) return 16;
    if (strncmp(n, "ymm", 3) == 0 && asm_is_digit(n[3])) return 32;
    if (strncmp(n, "zmm", 3) == 0 && asm_is_digit(n[3])) return 64;
    if (cls) *cls = ASMREG_OTHER;
    if (asm_streq(n, "rip")) return 8;
    if (n[0] == 'k' && asm_is_digit(n[1]) && !n[2]) return 8;
    if (cls) *cls = ASMREG_NONE;
    return 0;
}

static const char *asm_size_word(i64 size) {
    switch (size) {
        case 1: return "byte";
        case 2: return "word";
        case 4: return "dword";
        case 8: return "qword";
        case 10: return "tbyte";
        case 16: return "xmmword";
        case 32: return "ymmword";
        case 64: return "zmmword";
        default: return null;
    }
}

static bool asm_is_size_word(const char *s) {
    return asm_streq(s, "byte") || asm_streq(s, "word") || asm_streq(s, "dword") || asm_streq(s, "qword") ||
           asm_streq(s, "tbyte") || asm_streq(s, "xmmword") || asm_streq(s, "ymmword") || asm_streq(s, "zmmword") ||
           asm_streq(s, "oword");
}

/* The constant pool. */
static AsmSb asm_rdata;
static i32 asm_pool_count;
static AsmMap asm_pool_map;

static char *asm_pool_string(const char *lit) {
    AsmSb key = {0};
    sb_printf(&key, "s:%s", lit);
    char *label = (char *)map_get(&asm_pool_map, sb_cstr(&key));
    if (!label) {
        AsmSb l = {0};
        sb_printf(&l, ".Lrasm_str_%d", asm_pool_count++);
        label = asm_strdup(sb_cstr(&l));
        free(l.d);
        map_put(&asm_pool_map, sb_cstr(&key), label);
        sb_printf(&asm_rdata, "%s:\n    .asciz %s\n", label, lit);
    }
    free(key.d);
    return label;
}

static char *asm_pool_float(const char *text, bool negative, bool f64) {
    double v = strtod(text, null);
    if (negative) v = -v;
    AsmSb key = {0};
    sb_printf(&key, "%s:%s%s", f64 ? "d" : "f", negative ? "-" : "", text);
    char *label = (char *)map_get(&asm_pool_map, sb_cstr(&key));
    if (!label) {
        AsmSb l = {0};
        sb_printf(&l, ".Lrasm_%s_%d", f64 ? "f64" : "f32", asm_pool_count++);
        label = asm_strdup(sb_cstr(&l));
        free(l.d);
        map_put(&asm_pool_map, sb_cstr(&key), label);
        if (f64) {
            u64 bits;
            memcpy(&bits, &v, 8);
            sb_printf(&asm_rdata, "    .p2align 3\n%s:\n    .quad 0x%016llx    # %s%s\n", label, (unsigned long long)bits, negative ? "-" : "", text);
        } else {
            float fv = (float)v;
            u32 bits;
            memcpy(&bits, &fv, 4);
            sb_printf(&asm_rdata, "    .p2align 2\n%s:\n    .long 0x%08x    # %s%s\n", label, bits, negative ? "-" : "", text);
        }
    }
    free(key.d);
    return label;
}

typedef enum AsmOpKind {
    OP_REG,
    OP_MEM,
    OP_IMM,
    OP_ADDR,   // a link-time address constant
    OP_CODE,   // a code label or proc symbol
} AsmOpKind;

typedef struct AsmOperand {
    AsmOpKind kind;
    AsmSb text;
    i32 size;      // bytes, 0 when unknown
    AsmSb note;    // a field path, for the comment
} AsmOperand;

typedef struct AsmCtx {
    AsmProc *proc;
    AsmMap labels;   // label name -> 1
    const char *mnemonic;
    i32 file;
    i32 line;
} AsmCtx;

static void asm_label_name(AsmSb *out, AsmProc *p, const char *label) {
    sb_printf(out, ".Lrasm_%s_%s", p->name, label);
}

static bool asm_is_float_mnemonic_f64(const char *m) {
    size_t n = strlen(m);
    return n >= 2 && (asm_streq(m + n - 2, "sd") || asm_streq(m + n - 2, "pd"));
}

/* The inside of [ ... ]: registers stay, constants become numbers. */
static bool asm_rewrite_bracket(AsmCtx *c, AsmTok *t, i32 n, AsmSb *out) {
    i32 i = 0;
    while (i < n) {
        AsmTok *k = &t[i];
        if (k->kind == AT_IDENT && asm_register_size(k->text, null)) {
            sb_add(out, k->text);
            i++;
            continue;
        }
        if (k->kind == AT_PUNCT && (tok_is(k, "+") || tok_is(k, "-") || tok_is(k, "*"))) {
            sb_printf(out, " %s ", k->text);
            i++;
            continue;
        }
        /* A constant run: everything up to the next + - or register at depth 0. */
        i32 j = i;
        i32 depth = 0;
        while (j < n) {
            AsmTok *x = &t[j];
            if (tok_is(x, "(")) depth++;
            if (tok_is(x, ")")) depth--;
            if (depth == 0 && j > i && (tok_is(x, "+") || tok_is(x, "-") || tok_is(x, "*"))) break;
            if (depth == 0 && x->kind == AT_IDENT && asm_register_size(x->text, null)) break;
            j++;
        }
        i64 v = 0;
        if (!asm_eval_toks(t + i, j - i, &v, c->file, c->line)) {
            if (k->kind == AT_IDENT && map_get(&asm_global_map, k->text)) {
                asm_error_at(c->file, c->line, k->col, "'%s' inside [ ]: a global is addressed by name, as '%s', or lea rsi, %s first and index [rsi + ...]", k->text, k->text, k->text);
            } else {
                asm_error_at(c->file, c->line, k->col, "'%s' is not a register or a constant", k->text);
            }
            return false;
        }
        sb_printf(out, "%lld", (long long)v);
        i = j;
    }
    return true;
}

static bool asm_operand(AsmCtx *c, AsmTok *t, i32 n, AsmOperand *op) {
    memset(op, 0, sizeof(*op));
    if (n == 0) {
        asm_error_at(c->file, c->line, 1, "empty operand");
        return false;
    }
    i32 col = t[0].col;
    const char *explicit_size = null;
    i32 i = 0;
    if (t[0].kind == AT_IDENT && asm_is_size_word(t[0].text) && n > 1 && tok_is(&t[1], "ptr")) {
        explicit_size = t[0].text;
        i = 2;
    }

    /* [ ... ] with an optional .T.f path after it */
    if (tok_is(&t[i], "[")) {
        i32 depth = 0, j = i;
        for (; j < n; j++) {
            if (tok_is(&t[j], "[")) depth++;
            if (tok_is(&t[j], "]") && --depth == 0) break;
        }
        if (j >= n) {
            asm_error_at(c->file, c->line, col, "missing ']'");
            return false;
        }
        AsmSb inner = {0};
        if (!asm_rewrite_bracket(c, t + i + 1, j - i - 1, &inner)) {
            free(inner.d);
            return false;
        }
        i32 pos = j + 1;
        i64 off = 0;
        i32 size = 0;
        if (pos < n && tok_is(&t[pos], ".") && pos + 1 < n && t[pos + 1].kind == AT_IDENT) {
            TypeExpr *ty = type_new(asm_arena, Type_Name);
            ty->name = string8_from_cstr(asm_arena, t[pos + 1].text);
            sb_add(&op->note, t[pos + 1].text);
            pos += 2;
            if (!asm_type_struct(ty)) {
                asm_error_at(c->file, c->line, t[pos - 1].col, "'%s' is not a struct", t[pos - 1].text);
                free(inner.d);
                return false;
            }
            if (!asm_walk_path(t, n, &pos, &ty, &off, &op->note, c->file, c->line)) {
                free(inner.d);
                return false;
            }
            AsmSA sa = asm_type_sa(ty);
            if (sa.ok) size = (i32)sa.size;
        }
        if (pos != n) {
            asm_error_at(c->file, c->line, t[pos].col, "unexpected '%s' after a memory operand", t[pos].text);
            free(inner.d);
            return false;
        }
        op->kind = OP_MEM;
        const char *sw = explicit_size ? explicit_size : asm_size_word(size);
        if (sw) sb_printf(&op->text, "%s ptr ", sw);
        sb_add(&op->text, "[");
        sb_add(&op->text, sb_cstr(&inner));
        if (off) sb_printf(&op->text, " + %lld", (long long)off);
        sb_add(&op->text, "]");
        op->size = explicit_size ? 0 : size;
        free(inner.d);
        return true;
    }

    AsmTok *k = &t[i];

    /* a string literal: the address of a pooled string */
    if (k->kind == AT_STR && i + 1 == n) {
        char *label = asm_pool_string(k->text);
        op->kind = OP_ADDR;
        sb_printf(&op->text, "offset %s", label);
        return true;
    }

    /* a float literal: a pooled constant in memory */
    if (k->kind == AT_FLOAT || (tok_is(k, "-") && i + 1 < n && t[i + 1].kind == AT_FLOAT)) {
        bool neg = tok_is(k, "-");
        AsmTok *fk = neg ? &t[i + 1] : k;
        if ((neg ? i + 2 : i + 1) != n) {
            asm_error_at(c->file, c->line, col, "a float literal is a constant in memory; it takes no arithmetic");
            return false;
        }
        bool f64 = asm_is_float_mnemonic_f64(c->mnemonic);
        char *label = asm_pool_float(fk->text, neg, f64);
        op->kind = OP_MEM;
        sb_printf(&op->text, "%s ptr [rip + %s]", explicit_size ? explicit_size : (f64 ? "qword" : "dword"), label);
        op->size = f64 ? 8 : 4;
        return true;
    }

    if (k->kind == AT_IDENT) {
        AsmRegClass cls;
        i32 rs = asm_register_size(k->text, &cls);
        if (rs && i + 1 == n && !explicit_size) {
            op->kind = OP_REG;
            sb_add(&op->text, k->text);
            op->size = rs;
            return true;
        }
        /* x87 st(n) */
        if (asm_streq(k->text, "st") && !explicit_size) {
            op->kind = OP_REG;
            for (i32 x = i; x < n; x++) sb_add(&op->text, t[x].text);
            return true;
        }
        if (i + 1 == n && map_get(&c->labels, k->text) && !explicit_size) {
            op->kind = OP_CODE;
            asm_label_name(&op->text, c->proc, k->text);
            return true;
        }
        Stmt *g = (Stmt *)map_get(&asm_global_map, k->text);
        if (g) {
            TypeExpr *ty = g->type;
            i64 off = 0;
            i32 pos = i + 1;
            sb_add(&op->note, k->text);
            if (!asm_walk_path(t, n, &pos, &ty, &off, &op->note, c->file, c->line)) return false;
            bool addr = false;
            if (pos < n && tok_is(&t[pos], "&")) {
                addr = true;
                pos++;
            }
            if (pos != n) {
                asm_error_at(c->file, c->line, t[pos].col, "unexpected '%s' after %s", t[pos].text, k->text);
                return false;
            }
            if (addr) {
                if (explicit_size) {
                    asm_error_at(c->file, c->line, col, "an address takes no size");
                    return false;
                }
                op->kind = OP_ADDR;
                sb_printf(&op->text, "offset \"%s\"", k->text);
                if (off) sb_printf(&op->text, " + %lld", (long long)off);
                op->note.n = 0;
                return true;
            }
            AsmSA sa = asm_type_sa(ty);
            op->kind = OP_MEM;
            const char *sw = explicit_size ? explicit_size : (sa.ok ? asm_size_word(sa.size) : null);
            if (sw) sb_printf(&op->text, "%s ptr ", sw);
            sb_printf(&op->text, "[rip + \"%s\"", k->text);
            if (off) sb_printf(&op->text, " + %lld", (long long)off);
            sb_add(&op->text, "]");
            op->size = (sa.ok && !explicit_size) ? (i32)sa.size : 0;
            if (sa.ok && op->note.n && strchr(sb_cstr(&op->note), '.') == null && strchr(sb_cstr(&op->note), '[') == null) {
                op->note.n = 0;
                if (op->note.d) op->note.d[0] = 0;
            }
            return true;
        }
        if (!explicit_size) {
            bool is_proc = map_get(&asm_proc_map, k->text) || map_get(&asm_rinproc_map, k->text);
            if (is_proc) {
                if (i + 1 == n) {
                    op->kind = OP_CODE;
                    sb_printf(&op->text, "\"%s\"", k->text);
                    return true;
                }
                if (i + 2 == n && tok_is(&t[i + 1], "&")) {
                    op->kind = OP_ADDR;
                    sb_printf(&op->text, "offset \"%s\"", k->text);
                    return true;
                }
            }
        }
    }

    /* a constant expression */
    i64 v = 0;
    if (!explicit_size && asm_eval_toks(t + i, n - i, &v, c->file, c->line)) {
        op->kind = OP_IMM;
        sb_printf(&op->text, "%lld", (long long)v);
        return true;
    }

    /* a lone unknown name: an external code symbol, such as a Windows API */
    if (!explicit_size && k->kind == AT_IDENT && i + 1 == n && !map_get(&asm_define_map, k->text)) {
        op->kind = OP_CODE;
        sb_printf(&op->text, "\"%s\"", k->text);
        return true;
    }

    asm_error_at(c->file, c->line, col, "cannot read this operand (starting at '%s')", k->text);
    return false;
}

static bool asm_is_jump_mnemonic(const char *m) {
    return m[0] == 'j' || asm_streq(m, "call") || asm_streq(m, "loop") || asm_streq(m, "loope") ||
           asm_streq(m, "loopne") || asm_streq(m, "jrcxz");
}

static bool asm_is_prefix(const char *m) {
    return asm_streq(m, "rep") || asm_streq(m, "repe") || asm_streq(m, "repz") || asm_streq(m, "repne") ||
           asm_streq(m, "repnz") || asm_streq(m, "lock");
}

static void asm_emit_instruction(AsmCtx *c, AsmStmt *s, AsmSb *out) {
    AsmTok *t = s->toks.d;
    i32 n = s->toks.n;
    i32 i = 0;
    AsmSb mnem = {0};
    while (i < n && t[i].kind == AT_IDENT && asm_is_prefix(t[i].text)) {
        sb_add(&mnem, t[i].text);
        sb_add(&mnem, " ");
        i++;
    }
    if (i >= n || t[i].kind != AT_IDENT) {
        asm_error_at(s->file, s->line, t[0].col, "expected an instruction");
        free(mnem.d);
        return;
    }
    const char *m = t[i].text;
    c->mnemonic = m;
    c->file = s->file;
    c->line = s->line;
    i++;

    AsmOperand ops[4];
    i32 nops = 0;
    while (i < n && nops < 4) {
        i32 j = i, depth = 0;
        while (j < n) {
            if (tok_is(&t[j], "(") || tok_is(&t[j], "[")) depth++;
            if (tok_is(&t[j], ")") || tok_is(&t[j], "]")) depth--;
            if (depth == 0 && tok_is(&t[j], ",")) break;
            j++;
        }
        if (!asm_operand(c, t + i, j - i, &ops[nops])) {
            for (i32 x = 0; x < nops; x++) {
                free(ops[x].text.d);
                free(ops[x].note.d);
            }
            free(mnem.d);
            return;
        }
        nops++;
        i = j + 1;
    }

    /* An address constant is a 64-bit immediate: only `mov r64, x&`, which
       GAS spells movabs. Anything else would be a 32-bit absolute address. */
    const char *emit_m = m;
    for (i32 x = 0; x < nops; x++) {
        if (ops[x].kind != OP_ADDR) continue;
        if (asm_streq(m, "mov") && nops == 2 && x == 1 && ops[0].kind == OP_REG && ops[0].size == 8) {
            emit_m = "movabs";
        } else {
            asm_error_at(s->file, s->line, t[0].col,
                         "an address constant (x&, a string) can only be moved into a 64-bit register: mov r64, x&");
        }
    }
    /* Two operands of known, different sizes: say so at the .rasm line, before
       the assembler says it at the .s line. movzx, movsx, movsxd and the
       conversions take mixed sizes on purpose. */
    if (nops == 2 && ops[0].size && ops[1].size && ops[0].size != ops[1].size &&
        ops[0].kind != OP_IMM && ops[1].kind != OP_IMM &&
        (asm_streq(m, "mov") || asm_streq(m, "add") || asm_streq(m, "sub") || asm_streq(m, "cmp") ||
         asm_streq(m, "and") || asm_streq(m, "or") || asm_streq(m, "xor") || asm_streq(m, "test") ||
         asm_streq(m, "adc") || asm_streq(m, "sbb") || asm_streq(m, "xchg"))) {
        asm_error_at(s->file, s->line, t[0].col,
                     "%s: the operands are %d and %d bytes (%s, %s); use a register of the same size, or movzx/movsxd",
                     m, ops[0].size, ops[1].size, sb_cstr(&ops[0].text), sb_cstr(&ops[1].text));
    }
    /* lea computes an address; the operand's size means nothing to it. */
    if (asm_streq(m, "lea") && nops == 2 && ops[1].kind == OP_MEM) {
        char *txt = asm_strdup(sb_cstr(&ops[1].text));
        char *br = strchr(txt, '[');
        ops[1].text.n = 0;
        sb_add(&ops[1].text, br ? br : txt);
        free(txt);
    }
    /* A code symbol used anywhere but a jump or call is its address in memory,
       which is not what anyone means. */
    for (i32 x = 0; x < nops; x++) {
        if (ops[x].kind == OP_CODE && !asm_is_jump_mnemonic(m)) {
            asm_error_at(s->file, s->line, t[0].col, "%s is code: take its address with %s& (mov r64) or lea", sb_cstr(&ops[x].text), sb_cstr(&ops[x].text));
        }
    }

    sb_printf(out, "    %s%s", sb_cstr(&mnem), emit_m);
    for (i32 x = 0; x < nops; x++) {
        sb_add(out, x == 0 ? " " : ", ");
        sb_add(out, sb_cstr(&ops[x].text));
    }
    bool noted = false;
    for (i32 x = 0; x < nops; x++) {
        if (ops[x].note.n) {
            sb_add(out, noted ? ", " : "    # ");
            sb_add(out, sb_cstr(&ops[x].note));
            noted = true;
        }
    }
    sb_add(out, "\n");
    for (i32 x = 0; x < nops; x++) {
        free(ops[x].text.d);
        free(ops[x].note.d);
    }
    free(mnem.d);
}

/* ---------------------------------------------------------------------------
   Macro expansion.
   ------------------------------------------------------------------------ */

static i32 asm_expansion_counter;

static void asm_expand(AsmStmt *s, i32 depth, i32 line_file, i32 line, AsmList *out);

static void asm_expand_macro(AsmStmt *s, AsmList *overloads, i32 depth, i32 line_file, i32 line, AsmList *out) {
    AsmTok *t = s->toks.d;
    i32 n = s->toks.n;
    /* Arguments: split at commas outside ( ) [ ] { }. */
    AsmList args = {0}; // AsmToks*
    i32 i = 2;
    if (!(n >= 3 && tok_is(&t[n - 1], ")"))) {
        asm_error_at(s->file, s->line, t[0].col, "a macro call is name(arguments)");
        return;
    }
    i32 end = n - 1;
    if (i < end) {
        while (true) {
            AsmToks *a = (AsmToks *)calloc(1, sizeof(AsmToks));
            i32 d = 0;
            while (i < end) {
                if (tok_is(&t[i], "(") || tok_is(&t[i], "[") || tok_is(&t[i], "{")) d++;
                if (tok_is(&t[i], ")") || tok_is(&t[i], "]") || tok_is(&t[i], "}")) d--;
                if (d == 0 && tok_is(&t[i], ",")) break;
                toks_push(a, t[i]);
                i++;
            }
            list_push(&args, a);
            if (i >= end) break;
            i++; // the comma
        }
    }
    AsmMacro *m = null;
    for (i32 k = 0; k < overloads->n; k++) {
        AsmMacro *cand = (AsmMacro *)overloads->d[k];
        if (cand->params.n == args.n) m = cand;
    }
    if (!m) {
        asm_error_at(s->file, s->line, t[0].col, "macro %s has no definition taking %d arguments", t[0].text, args.n);
        return;
    }
    i32 expansion = ++asm_expansion_counter;
    /* Labels defined in the body are renamed per expansion. */
    AsmMap body_labels = {0};
    for (i32 k = 0; k < m->stmts.n; k++) {
        AsmStmt *b = (AsmStmt *)m->stmts.d[k];
        if (b->is_label) map_put(&body_labels, b->toks.d[0].text, (void *)1);
    }
    for (i32 k = 0; k < m->stmts.n; k++) {
        AsmStmt *b = (AsmStmt *)m->stmts.d[k];
        AsmStmt *x = (AsmStmt *)calloc(1, sizeof(AsmStmt));
        x->file = b->file;
        x->line = b->line;
        x->is_label = b->is_label;
        for (i32 q = 0; q < b->toks.n; q++) {
            AsmTok tk = b->toks.d[q];
            if (tk.kind == AT_IDENT) {
                i32 pi = -1;
                for (i32 p = 0; p < m->params.n; p++) {
                    if (asm_streq((char *)m->params.d[p], tk.text)) pi = p;
                }
                if (pi >= 0) {
                    AsmToks *a = (AsmToks *)args.d[pi];
                    for (i32 r = 0; r < a->n; r++) toks_push(&x->toks, a->d[r]);
                    continue;
                }
                if (map_get(&body_labels, tk.text)) {
                    AsmSb nn = {0};
                    sb_printf(&nn, "%s__m%d", tk.text, expansion);
                    tk.text = asm_strdup(sb_cstr(&nn));
                    free(nn.d);
                }
            }
            toks_push(&x->toks, tk);
        }
        asm_expand(x, depth + 1, line_file, line, out);
    }
}

static void asm_expand(AsmStmt *s, i32 depth, i32 line_file, i32 line, AsmList *out) {
    if (depth > 32) {
        asm_error_at(s->file, s->line, 1, "macros nested more than 32 deep (a macro calling itself?)");
        return;
    }
    if (s->toks.n >= 2 && s->toks.d[0].kind == AT_IDENT && tok_is(&s->toks.d[1], "(") && !s->is_label) {
        AsmList *overloads = (AsmList *)map_get(&asm_macro_map, s->toks.d[0].text);
        if (overloads) {
            asm_expand_macro(s, overloads, depth, line_file, line, out);
            return;
        }
        if (!asm_streq(s->toks.d[0].text, "st")) {
            asm_error_at(s->file, s->line, s->toks.d[0].col, "%s( ... ): no macro by that name", s->toks.d[0].text);
            return;
        }
    }
    s->file = line_file;
    s->line = line;
    list_push(out, s);
}

/* ---------------------------------------------------------------------------
   The .s file.
   ------------------------------------------------------------------------ */

static void asm_cv_path(AsmSb *out, const char *path) {
    for (const char *p = path; *p; p++) {
        if (*p == '\\') sb_add(out, "/");
        else if (*p == '"') sb_add(out, "\\\"");
        else sb_addn(out, p, 1);
    }
}

static void asm_emit_code(AsmSb *s_file) {
    AsmSb text = {0};
    AsmSb debug = {0};
    for (i32 pi = 0; pi < asm_procs.n; pi++) {
        AsmProc *p = (AsmProc *)asm_procs.d[pi];
        p->func_id = pi;
        AsmCtx c = {0};
        c.proc = p;
        /* Expand first, so labels from the proc itself are known. */
        AsmList stmts = {0};
        for (i32 i = 0; i < p->stmts.n; i++) {
            AsmStmt *st = (AsmStmt *)p->stmts.d[i];
            asm_expand(st, 0, st->file, st->line, &stmts);
        }
        for (i32 i = 0; i < stmts.n; i++) {
            AsmStmt *st = (AsmStmt *)stmts.d[i];
            if (st->is_label) {
                if (map_get(&c.labels, st->toks.d[0].text)) {
                    asm_error_at(st->file, st->line, st->toks.d[0].col, "label %s is defined twice in %s", st->toks.d[0].text, p->name);
                }
                map_put(&c.labels, st->toks.d[0].text, (void *)1);
            }
        }
        sb_add(&text, "\n");
        if (!p->is_static) sb_printf(&text, "    .globl \"%s\"\n", p->name);
        sb_printf(&text, "    .p2align 4\n\"%s\":\n    .cv_func_id %d\n", p->name, pi);
        i32 last_file = -1, last_line = -1;
        for (i32 i = 0; i < stmts.n; i++) {
            AsmStmt *st = (AsmStmt *)stmts.d[i];
            if (st->file != last_file || st->line != last_line) {
                sb_printf(&text, "    .cv_loc %d %d %d 0\n", pi, st->file, st->line);
                last_file = st->file;
                last_line = st->line;
            }
            if (st->is_label) {
                asm_label_name(&text, p, st->toks.d[0].text);
                sb_add(&text, ":\n");
                continue;
            }
            asm_emit_instruction(&c, st, &text);
        }
        sb_printf(&text, ".Lrasm_end_%d:\n", pi);

        sb_printf(&debug,
                  "    .short .Lrasm_p%d_end-.Lrasm_p%d_begin\n"
                  ".Lrasm_p%d_begin:\n"
                  "    .short 0x1110                 # S_GPROC32 %s\n"
                  "    .long 0, 0, 0\n"
                  "    .long .Lrasm_end_%d-\"%s\"\n"
                  "    .long 0\n"
                  "    .long .Lrasm_end_%d-\"%s\"\n"
                  "    .long 0\n"
                  "    .secrel32 \"%s\"\n"
                  "    .secidx \"%s\"\n"
                  "    .byte 0\n"
                  "    .asciz \"%s\"\n"
                  "    .p2align 2\n"
                  ".Lrasm_p%d_end:\n"
                  "    .short 2\n"
                  "    .short 6                      # S_END\n",
                  pi, pi, pi, p->name, pi, p->name, pi, p->name, p->name, p->name, p->name, pi);
    }

    sb_add(s_file, "# Generated by `rin asm`. Do not edit: the source is the .rasm.\n");
    sb_add(s_file, "    .intel_syntax noprefix\n");
    for (i32 i = 0; i < asm_files.n; i++) {
        AsmFile *f = (AsmFile *)asm_files.d[i];
        sb_printf(s_file, "    .cv_file %d \"", f->id);
        asm_cv_path(s_file, f->path);
        sb_add(s_file, "\"\n");
    }
    if (asm_rdata.n) {
        sb_add(s_file, "\n    .section .rdata,\"dr\"\n");
        sb_add(s_file, sb_cstr(&asm_rdata));
    }
    sb_add(s_file, "\n    .text\n");
    sb_add(s_file, sb_cstr(&text));
    sb_add(s_file, "\n    .section .debug$S,\"dr\"\n    .p2align 2\n    .long 4\n    .long 241\n");
    sb_add(s_file, "    .long .Lrasm_sym_end-.Lrasm_sym_begin\n.Lrasm_sym_begin:\n");
    sb_add(s_file, sb_cstr(&debug));
    sb_add(s_file, ".Lrasm_sym_end:\n    .p2align 2\n");
    for (i32 pi = 0; pi < asm_procs.n; pi++) {
        AsmProc *p = (AsmProc *)asm_procs.d[pi];
        sb_printf(s_file, "    .cv_linetable %d, \"%s\", .Lrasm_end_%d\n", pi, p->name, pi);
    }
    sb_add(s_file, "    .cv_filechecksums\n    .cv_stringtable\n");
    free(text.d);
    free(debug.d);
}

/* ---------------------------------------------------------------------------
   The C data file.
   ------------------------------------------------------------------------ */

static void asm_c_decl(AsmSb *out, TypeExpr *t, const char *declarator);

static void asm_c_params(AsmSb *out, TypeExpr *t) {
    sb_add(out, "(");
    if (t->args.length == 0) sb_add(out, "void");
    for (i32 i = 0; i < t->args.length; i++) {
        if (i) sb_add(out, ", ");
        asm_c_decl(out, (TypeExpr *)t->args.data[i], "");
    }
    if (t->is_variadic) sb_add(out, ", ...");
    sb_add(out, ")");
}

/* C's declarator syntax, inside out. */
static void asm_c_decl(AsmSb *out, TypeExpr *t, const char *declarator) {
    if (!t) {
        sb_printf(out, "void %s", declarator);
        return;
    }
    switch (t->kind) {
        case Type_Name: {
            char *n = asm_s8(t->name);
            sb_printf(out, "%s%s%s%s", t->is_const ? "const " : "", n, declarator[0] ? " " : "", declarator);
            free(n);
            return;
        }
        case Type_Ptr: {
            AsmSb inner = {0};
            TypeExpr *e = t->elem;
            bool paren = e && (e->kind == Type_Array || e->kind == Type_Proc);
            sb_printf(&inner, "%s*%s%s%s", paren ? "(" : "", t->is_const ? " const " : "", declarator, paren ? ")" : "");
            if (e && e->kind == Type_Name && e->name.length == 4 && memcmp(e->name.data, "void", 4) == 0) {
                sb_printf(out, "%svoid %s", e->is_const ? "const " : "", sb_cstr(&inner));
            } else {
                asm_c_decl(out, e, sb_cstr(&inner));
            }
            free(inner.d);
            return;
        }
        case Type_Array: {
            AsmSb inner = {0};
            i64 count = 0;
            if (t->array_count.length && asm_array_count(t, &count)) {
                sb_printf(&inner, "%s[%lld]", declarator, (long long)count);
            } else if (t->array_count.length) {
                char *c = asm_s8(t->array_count);
                sb_printf(&inner, "%s[%s]", declarator, c);
                free(c);
            } else {
                sb_printf(&inner, "%s[]", declarator);
            }
            TypeExpr *e = t->elem;
            if (t->is_const && e && !e->is_const) {
                sb_add(out, "const ");
            }
            asm_c_decl(out, e, sb_cstr(&inner));
            free(inner.d);
            return;
        }
        case Type_Proc: {
            AsmSb inner = {0};
            sb_add(&inner, declarator);
            asm_c_params(&inner, t);
            asm_c_decl(out, t->ret_type, sb_cstr(&inner));
            free(inner.d);
            return;
        }
        case Type_Generic: {
            char *n = asm_s8(t->name);
            sb_printf(out, "/* generic %s: not supported in rasm data yet */ void *%s", n, declarator);
            free(n);
            return;
        }
    }
}

static const char *asm_c_binop(TokenKind k) {
    switch (k) {
        case Token_Plus: return "+";
        case Token_Minus: return "-";
        case Token_Star: return "*";
        case Token_Slash: return "/";
        case Token_Percent: return "%";
        case Token_Keyword_Shl: return "<<";
        case Token_Keyword_Shr: return ">>";
        case Token_Ampersand: return "&";
        case Token_Pipe: return "|";
        case Token_Caret: return "^";
        case Token_AmpAmp: case Token_Keyword_And: return "&&";
        case Token_PipePipe: case Token_Keyword_Or: return "||";
        case Token_EqualEqual: return "==";
        case Token_BangEqual: return "!=";
        case Token_LAngle: return "<";
        case Token_RAngle: return ">";
        case Token_LessEqual: return "<=";
        case Token_GreaterEqual: return ">=";
        default: return "?";
    }
}

static AsmMap asm_proto_needed; // proc name -> 1

static void asm_c_expr(AsmSb *out, Expr *x) {
    if (!x) {
        sb_add(out, "0");
        return;
    }
    switch (x->kind) {
        case Expr_Number: {
            char *t = asm_s8(x->number);
            for (char *p = t; *p; p++) if (*p != '_') sb_addn(out, p, 1);
            free(t);
            return;
        }
        case Expr_String: {
            char *t = asm_s8(x->string_lit);
            sb_add(out, t);
            free(t);
            return;
        }
        case Expr_Char: {
            char *t = asm_s8(x->string_lit);
            sb_add(out, t);
            free(t);
            return;
        }
        case Expr_Name: {
            char *t = asm_s8(x->name);
            if (asm_streq(t, "null")) sb_add(out, "0");
            else if (asm_streq(t, "true")) sb_add(out, "1");
            else if (asm_streq(t, "false")) sb_add(out, "0");
            else sb_add(out, t);
            free(t);
            return;
        }
        case Expr_Field: {
            if (x->base && x->base->kind == Expr_Name) {
                char *en = asm_s8(x->base->name);
                if (asm_enum_named(en)) {
                    char *mn = asm_s8(x->name);
                    i64 v = 0;
                    if (asm_enum_member_value(en, mn, &v)) sb_printf(out, "%lld", (long long)v);
                    else sb_printf(out, "%s", mn);
                    free(mn);
                    free(en);
                    return;
                }
                free(en);
            }
            asm_c_expr(out, x->base);
            char *mn = asm_s8(x->name);
            sb_printf(out, ".%s", mn);
            free(mn);
            return;
        }
        case Expr_Addr: {
            Expr *in = x->inner;
            if (in && in->kind == Expr_Name) {
                char *n = asm_s8(in->name);
                if (map_get(&asm_proc_map, n) || map_get(&asm_rinproc_map, n)) {
                    if (map_get(&asm_proc_map, n)) map_put(&asm_proto_needed, n, (void *)1);
                    sb_add(out, n);
                    free(n);
                    return;
                }
                free(n);
            }
            sb_add(out, "&(");
            asm_c_expr(out, in);
            sb_add(out, ")");
            return;
        }
        case Expr_Index:
            asm_c_expr(out, x->base);
            sb_add(out, "[");
            asm_c_expr(out, x->index_expr);
            sb_add(out, "]");
            return;
        case Expr_ZeroInit:
            sb_add(out, "{0}");
            return;
        case Expr_InitList: {
            sb_add(out, "{");
            for (i32 i = 0; i < x->args.length; i++) {
                if (i) sb_add(out, ",");
                sb_add(out, " ");
                Expr *d = (Expr *)x->designators.data[i];
                i32 dk = x->designator_kinds.data[i];
                if (d && dk == InitDesignator_Field) {
                    char *n = asm_s8(d->name);
                    sb_printf(out, ".%s = ", n);
                    free(n);
                } else if (d && dk == InitDesignator_Index) {
                    sb_add(out, "[");
                    asm_c_expr(out, d);
                    sb_add(out, "] = ");
                }
                asm_c_expr(out, (Expr *)x->args.data[i]);
            }
            sb_add(out, " }");
            return;
        }
        case Expr_Cast:
            sb_add(out, "((");
            asm_c_decl(out, x->cast_type, "");
            sb_add(out, ")(");
            asm_c_expr(out, x->inner);
            sb_add(out, "))");
            return;
        case Expr_SizeofType:
        case Expr_AlignofType:
            sb_add(out, x->kind == Expr_SizeofType ? "sizeof(" : "_Alignof(");
            if (x->cast_type) asm_c_decl(out, x->cast_type, "");
            else asm_c_expr(out, x->inner);
            sb_add(out, ")");
            return;
        case Expr_Binary:
            sb_add(out, "(");
            asm_c_expr(out, x->left);
            sb_printf(out, " %s ", asm_c_binop(x->op));
            asm_c_expr(out, x->right);
            sb_add(out, ")");
            return;
        case Expr_Unary:
            sb_add(out, x->op == Token_Minus ? "-" : x->op == Token_Tilde ? "~" : x->op == Token_Bang ? "!" : "");
            sb_add(out, "(");
            asm_c_expr(out, x->inner ? x->inner : x->right);
            sb_add(out, ")");
            return;
        case Expr_Ternary:
            sb_add(out, "(");
            asm_c_expr(out, x->left);
            sb_add(out, " ? ");
            asm_c_expr(out, x->right);
            sb_add(out, " : ");
            asm_c_expr(out, x->third);
            sb_add(out, ")");
            return;
        default:
            sb_add(out, "0 /* unsupported initializer */");
            return;
    }
}

static AsmMap asm_struct_emitted;

static void asm_c_struct(AsmSb *out, StructDecl *sd);

static void asm_c_struct_deps(AsmSb *out, TypeExpr *t) {
    t = asm_resolve_type(t);
    if (!t) return;
    if (t->kind == Type_Array) {
        asm_c_struct_deps(out, t->elem);
        return;
    }
    if (t->kind != Type_Name) return;
    char *n = asm_s8(t->name);
    StructDecl *sd = (StructDecl *)map_get(&asm_struct_map, n);
    free(n);
    if (sd && !sd->is_external) asm_c_struct(out, sd);
}

static void asm_c_struct_fields(AsmSb *out, StructDecl *sd, const char *indent) {
    for (i32 i = 0; i < sd->fields.length; i++) {
        Field *f = (Field *)sd->fields.data[i];
        if (f->anon) {
            sb_printf(out, "%s%s {\n", indent, f->anon->is_union ? "union" : "struct");
            AsmSb deeper = {0};
            sb_printf(&deeper, "%s    ", indent);
            asm_c_struct_fields(out, f->anon, sb_cstr(&deeper));
            free(deeper.d);
            sb_printf(out, "%s};\n", indent);
            continue;
        }
        char *name = asm_s8(f->name);
        sb_add(out, indent);
        if (f->align.length) {
            char *a = asm_s8(f->align);
            sb_printf(out, "_Alignas(%s) ", a);
            free(a);
        }
        asm_c_decl(out, f->type, name);
        if (f->bit_width.length) {
            char *b = asm_s8(f->bit_width);
            sb_printf(out, " : %s", b);
            free(b);
        }
        sb_add(out, ";\n");
        free(name);
    }
}

static void asm_c_struct(AsmSb *out, StructDecl *sd) {
    char *name = asm_s8(sd->name);
    if (map_get(&asm_struct_emitted, name) || sd->is_external || sd->is_generic) {
        free(name);
        return;
    }
    map_put(&asm_struct_emitted, name, (void *)1);
    for (i32 i = 0; i < sd->fields.length; i++) {
        Field *f = (Field *)sd->fields.data[i];
        if (f->type) asm_c_struct_deps(out, f->type);
    }
    if (sd->packed) sb_add(out, "#pragma pack(push, 1)\n");
    sb_printf(out, "%s %s {\n", sd->is_union ? "union" : "struct", name);
    asm_c_struct_fields(out, sd, "    ");
    sb_add(out, "}");
    if (sd->align.length) {
        char *a = asm_s8(sd->align);
        sb_printf(out, " __attribute__((aligned(%s)))", a);
        free(a);
    }
    sb_add(out, ";\n");
    if (sd->packed) sb_add(out, "#pragma pack(pop)\n");
    free(name);
}

/* c_name: the data file's own name, for the #line that hands the asserts back
   to it after the globals have been pointed at their .rasm lines. */
static void asm_emit_data(AsmSb *c, const char *c_name) {
    sb_add(c, "/* Generated by `rin asm`: the data and types of a rasm program. Do not edit. */\n");
    sb_add(c, "#include \"core.h\"\n#include <stddef.h>\n");
    for (i32 i = 0; i < asm_c_includes.n; i++) sb_printf(c, "#include \"%s\"\n", (char *)asm_c_includes.d[i]);
    for (i32 i = 0; i < asm_pp_lines.n; i++) {
        const char *line = (const char *)asm_pp_lines.d[i];
        const char *p = line + 1;
        while (*p == ' ') p++;
        if (strncmp(p, "pragma", 6) == 0) sb_printf(c, "%s\n", line);
    }
    sb_add(c, "\n");
    for (i32 i = 0; i < asm_define_list.n; i++) {
        AsmDefine *d = (AsmDefine *)asm_define_list.d[i];
        sb_printf(c, "#ifndef %s\n#define %s %s\n#endif\n", d->name, d->name, d->text);
    }
    sb_add(c, "\n");

    /* Types: forward declarations, then bodies in dependency order. */
    for (i32 i = 0; i < asm_struct_list.n; i++) {
        StructDecl *sd = (StructDecl *)asm_struct_list.d[i];
        if (sd->is_external || sd->is_generic) continue;
        char *n = asm_s8(sd->name);
        sb_printf(c, "typedef %s %s %s;\n", sd->is_union ? "union" : "struct", n, n);
        free(n);
    }
    for (i32 i = 0; i < asm_enum_list.n; i++) {
        EnumDecl *ed = (EnumDecl *)asm_enum_list.d[i];
        if (ed->is_external) continue;
        char *n = asm_s8(ed->name);
        sb_printf(c, "typedef enum %s {", n);
        for (i32 k = 0; k < ed->items.length; k++) {
            EnumItem *it = (EnumItem *)ed->items.data[k];
            char *mn = asm_s8(it->name);
            i64 v = 0;
            asm_enum_item_value(ed, k, &v);
            sb_printf(c, "%s %s_%s = %lld", k ? "," : "", n, mn, (long long)v);
            free(mn);
        }
        sb_printf(c, " } %s;\n", n);
        free(n);
    }
    for (i32 i = 0; i < asm_alias_list.n; i++) {
        AliasDecl *ad = (AliasDecl *)asm_alias_list.d[i];
        if (ad->is_external) continue;
        char *n = asm_s8(ad->name);
        sb_add(c, "typedef ");
        asm_c_decl(c, ad->type, n);
        sb_add(c, ";\n");
        free(n);
    }
    for (i32 i = 0; i < asm_struct_list.n; i++) asm_c_struct(c, (StructDecl *)asm_struct_list.d[i]);
    sb_add(c, "\n");

    /* Globals. Initialisers are emitted first so the prototypes they need are known. */
    AsmSb globals = {0};
    for (i32 i = 0; i < asm_global_list.n; i++) {
        Stmt *g = (Stmt *)asm_global_list.d[i];
        /* A global with no initialiser is one C already has, from a header. */
        if (g->is_external || (!g->expr && !g->is_uninitialized)) continue;
        char *n = asm_s8(g->name);
        /* C's complaints about an initialiser name the .rasm line it came from */
        if (g->source_path && g->line > 0) {
            sb_printf(&globals, "#line %d \"", g->line);
            asm_cv_path(&globals, g->source_path);
            sb_add(&globals, "\"\n");
        }
        if (g->align.length) {
            char *a = asm_s8(g->align);
            sb_printf(&globals, "_Alignas(%s) ", a);
            free(a);
        }
        asm_c_decl(&globals, g->type, n);
        if (g->expr && !g->is_uninitialized) {
            sb_add(&globals, " = ");
            asm_c_expr(&globals, g->expr);
        }
        sb_add(&globals, ";\n");
        free(n);
    }
    for (i32 i = 0; i < asm_procs.n; i++) {
        AsmProc *p = (AsmProc *)asm_procs.d[i];
        if (!map_get(&asm_proto_needed, p->name)) continue;
        ProcDecl *pd = (ProcDecl *)map_get(&asm_rinproc_map, p->name);
        if (!pd) continue;
        AsmSb decl = {0};
        sb_printf(&decl, "%s(", p->name);
        if (pd->params.length == 0) sb_add(&decl, "void");
        for (i32 k = 0; k < pd->params.length; k++) {
            Param *pa = (Param *)pd->params.data[k];
            char *pn = asm_s8(pa->name);
            if (k) sb_add(&decl, ", ");
            asm_c_decl(&decl, pa->type, pn);
            free(pn);
        }
        sb_add(&decl, ")");
        asm_c_decl(c, pd->ret_type, sb_cstr(&decl));
        sb_add(c, ";\n");
        free(decl.d);
    }
    sb_add(c, sb_cstr(&globals));
    free(globals.d);
    i32 lines = 1;
    for (i64 i = 0; i < c->n; i++) if (c->d[i] == '\n') lines++;
    sb_printf(c, "#line %d \"%s\"\n", lines + 1, c_name);
    sb_add(c, "\n/* Every offset and size the code side used, checked against C. */\n");
    sb_add(c, sb_cstr(&asm_asserts));
}

/* ---------------------------------------------------------------------------
   The command.
   ------------------------------------------------------------------------ */

static bool asm_write_file(const char *path, const char *text) {
    FILE *f = null;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        printf("rin asm: error: cannot write %s\n", path);
        return false;
    }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return true;
}

static i32 asm_run(const char *cmd, bool verbose) {
    if (verbose) printf("+ %s\n", cmd);
    AsmSb wrapped = {0};
    sb_printf(&wrapped, "\"%s\"", cmd);
    i32 rc = system(sb_cstr(&wrapped));
    free(wrapped.d);
    return rc;
}

#include "asm_index.c"

static void asm_usage(void) {
    printf("usage: rin asm <file.rasm> [-o out.exe] [-l library]... [--windows] [--no-link] [--verbose]\n"
           "       rin asm <file.rasm> --index out.json [--stdin-path file.rasm]\n"
           "\n"
           "Assembles a rasm program: the code through clang with .cv_loc line records,\n"
           "the data as C, linked with lld into an executable that debugs at .rasm lines.\n"
           "Generated files go to build/rasm/.\n"
           "\n"
           "--index checks the program and writes what it declares (procs, labels,\n"
           "macros, defines, globals, struct layouts, enums) as JSON, for an editor;\n"
           "nothing is built. --stdin-path reads that file's text from stdin instead.\n");
}

static i32 asm_main(i32 argc, char *argv[], memops_arena *arena) {
    asm_arena = arena;
    const char *input = null;
    const char *exe = null;
    bool windows = false, link = true, verbose = false;
    const char *index_path = null;
    const char *stdin_path = null;
    AsmList libs = {0};
    for (i32 i = 2; i < argc; i++) {
        const char *a = argv[i];
        if (asm_streq(a, "-o") && i + 1 < argc) exe = argv[++i];
        else if (asm_streq(a, "-l") && i + 1 < argc) list_push(&libs, argv[++i]);
        else if (strncmp(a, "-l", 2) == 0 && a[2]) list_push(&libs, (void *)(a + 2));
        else if (asm_streq(a, "--windows")) windows = true;
        else if (asm_streq(a, "--no-link")) link = false;
        else if (asm_streq(a, "--verbose")) verbose = true;
        else if (asm_streq(a, "--index") && i + 1 < argc) index_path = argv[++i];
        else if (asm_streq(a, "--stdin-path") && i + 1 < argc) stdin_path = argv[++i];
        else if (asm_streq(a, "-h") || asm_streq(a, "--help")) {
            asm_usage();
            return 0;
        } else if (a[0] == '-') {
            printf("rin asm: error: unknown option %s\n", a);
            return 1;
        } else if (!input) input = a;
        else {
            printf("rin asm: error: unexpected argument %s\n", a);
            return 1;
        }
    }
    if (!input) {
        asm_usage();
        return 1;
    }
    const char *full = canonicalize_path(arena, string8_from_cstr(arena, input));
    if (stdin_path) {
        g_stdin_override_path = canonicalize_path(arena, string8_from_cstr(arena, stdin_path));
        g_stdin_override_source = read_stdin_string8(arena);
    }
    if (!file_exists_cstr(full) && !(stdin_path && cstr_equals(full, g_stdin_override_path))) {
        printf("rin asm: error: cannot read %s\n", input);
        return 1;
    }

    if (index_path) {
        /* Everything up to the .s, for its diagnostics; then the index. */
        asm_load_file(full, 0, 0);
        if (asm_parse_declarations()) {
            AsmSb s_file = {0};
            asm_emit_code(&s_file);
            free(s_file.d);
        }
        asm_write_index(index_path);
        return (asm_errors || g_error_count) ? 1 : 0;
    }

    asm_load_file(full, 0, 0);
    if (asm_errors) return 1;
    if (!asm_parse_declarations()) return 1;

    AsmSb s_file = {0};
    asm_emit_code(&s_file);
    if (asm_errors) {
        printf("rin asm: %d error%s\n", asm_errors, asm_errors == 1 ? "" : "s");
        return 1;
    }
    const char *stem = build_stem_of(arena, input);
    AsmSb c_name = {0};
    sb_printf(&c_name, "%s_data.c", stem);
    AsmSb c_file = {0};
    asm_emit_data(&c_file, sb_cstr(&c_name));

    const char *gen_dir = "build/rasm";
    if (!ensure_directory("build") || !ensure_directory(gen_dir)) {
        printf("rin asm: error: cannot create %s\n", gen_dir);
        return 1;
    }
    const char *s_path = join_path3(arena, gen_dir, stem, ".s");
    AsmSb data_stem = {0};
    sb_printf(&data_stem, "%s_data", stem);
    const char *c_path = join_path3(arena, gen_dir, sb_cstr(&data_stem), ".c");
    const char *s_obj = join_path3(arena, gen_dir, stem, ".obj");
    const char *c_obj = join_path3(arena, gen_dir, sb_cstr(&data_stem), ".obj");
    if (!asm_write_file(s_path, sb_cstr(&s_file)) || !asm_write_file(c_path, sb_cstr(&c_file))) return 1;

    /* The rin root, for core.h: forward slashes and no trailing separator, so
       a quoted -I"..." does not end in an escaped quote. */
    char *root = asm_strdup(g_exe_import_root ? g_exe_import_root : ".");
    for (char *p = root; *p; p++) if (*p == '\\') *p = '/';
    while (strlen(root) > 1 && root[strlen(root) - 1] == '/') root[strlen(root) - 1] = 0;
    AsmSb cmd = {0};
    sb_printf(&cmd, "clang --target=x86_64-pc-windows-msvc -c \"%s\" -o \"%s\"", s_path, s_obj);
    if (asm_run(sb_cstr(&cmd), verbose) != 0) {
        printf("rin asm: error: assembling %s failed\n", s_path);
        return 1;
    }
    cmd.n = 0;
    sb_printf(&cmd, "clang --target=x86_64-pc-windows-msvc -c -g -gcodeview -O0 -Wno-everything -Werror=excess-initializers -I\"%s\" -I\"%s/std\" \"%s\" -o \"%s\"",
              root, root, c_path, c_obj);
    if (asm_run(sb_cstr(&cmd), verbose) != 0) {
        printf("rin asm: error: compiling the data file %s failed\n", c_path);
        return 1;
    }
    if (!link) {
        printf("rin asm: wrote %s and %s\n", s_obj, c_obj);
        return 0;
    }
    if (!exe) exe = join_path3(arena, "build", stem, ".exe");
    cmd.n = 0;
    sb_printf(&cmd, "clang --target=x86_64-pc-windows-msvc -fuse-ld=lld -g \"%s\" \"%s\" -o \"%s\" -Xlinker /subsystem:%s -lkernel32 -luser32",
              s_obj, c_obj, exe, windows ? "windows" : "console");
    for (i32 i = 0; i < libs.n; i++) {
        const char *l = (const char *)libs.d[i];
        if (asm_ends_with(l, ".lib")) sb_printf(&cmd, " -l%.*s", (int)(strlen(l) - 4), l);
        else sb_printf(&cmd, " -l%s", l);
    }
    if (windows) sb_add(&cmd, " -Xlinker /entry:mainCRTStartup");
    if (asm_run(sb_cstr(&cmd), verbose) != 0) {
        printf("rin asm: error: linking %s failed\n", exe);
        return 1;
    }
    printf("rin asm: built %s\n", exe);
    return 0;
}
