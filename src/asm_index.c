/* ---------------------------------------------------------------------------
   `rin asm --index out.json`: what the assembler knows about a program, for
   the editor. Included by asm.c, after everything it reads.

   One JSON object: the files, every proc, label, macro, #define, global,
   struct (with its layout), enum and alias, each with where it was written.
   Errors are printed as usual, before it; an index is written even when there
   are some, with whatever was read.
   ------------------------------------------------------------------------ */

static void ix_str(AsmSb *o, const char *s) {
    sb_add(o, "\"");
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        if (*p == '"') sb_add(o, "\\\"");
        else if (*p == '\\') sb_add(o, "\\\\");
        else if (*p == '\n') sb_add(o, "\\n");
        else if (*p == '\r') sb_add(o, "\\r");
        else if (*p == '\t') sb_add(o, "\\t");
        else if (*p < 0x20) sb_printf(o, "\\u%04x", *p);
        else sb_addn(o, (const char *)p, 1);
    }
    sb_add(o, "\"");
}

static void ix_s8(AsmSb *o, string8 s) {
    char *c = asm_s8(s);
    ix_str(o, c);
    free(c);
}

static void ix_type(AsmSb *o, TypeExpr *t) {
    if (!t) {
        ix_str(o, "");
        return;
    }
    ix_s8(o, symbol_type_i_string(asm_arena, t));
}

/* A .rin declaration's file: its source path, or the index of the .rasm. */
static void ix_where(AsmSb *o, const char *path, i32 line, i32 col) {
    sb_add(o, "\"path\":");
    ix_str(o, path ? path : "");
    sb_printf(o, ",\"line\":%d,\"col\":%d", line, col);
}

static void ix_rasm_where(AsmSb *o, i32 file, i32 line) {
    const char *path = (file > 0 && file <= asm_files.n) ? ((AsmFile *)asm_files.d[file - 1])->path : "";
    ix_where(o, path, line, 1);
}

static void ix_size(AsmSb *o, TypeExpr *t) {
    AsmSA sa = t ? asm_type_sa(t) : (AsmSA){0};
    if (sa.ok) sb_printf(o, ",\"size\":%lld", (long long)sa.size);
}

/* The fields of a layout, anonymous members flattened at their offsets. */
static void ix_fields(AsmSb *o, StructDecl *sd, i64 base, bool *first) {
    AsmStructLayout *l = asm_struct_layout(sd);
    if (!l || !l->ok) return;
    for (i32 i = 0; i < l->nfields; i++) {
        AsmFieldLayout *f = &l->fields[i];
        if (f->anon) {
            ix_fields(o, f->anon, base + f->offset, first);
            continue;
        }
        if (sd->is_external && f->name && asm_is_anon_name(f->name) && asm_type_struct(f->type)) {
            ix_fields(o, asm_type_struct(f->type), base + f->offset, first);
            continue;
        }
        if (!*first) sb_add(o, ",");
        *first = false;
        sb_add(o, "{\"name\":");
        ix_str(o, f->name);
        sb_add(o, ",\"type\":");
        ix_type(o, f->type);
        sb_printf(o, ",\"offset\":%lld", (long long)(base + f->offset));
        ix_size(o, f->type);
        sb_add(o, "}");
    }
}

/* The source line of a proc's header, trimmed: its signature as written. */
static const char *ix_line_text(i32 file, i32 line) {
    if (file <= 0 || file > asm_files.n) return "";
    AsmFile *f = (AsmFile *)asm_files.d[file - 1];
    if (line <= 0 || line > f->nlines) return "";
    return f->lines[line - 1];
}

static void asm_write_index(const char *out_path) {
    AsmSb o = {0};
    sb_add(&o, "{\"files\":[");
    for (i32 i = 0; i < asm_files.n; i++) {
        if (i) sb_add(&o, ",");
        ix_str(&o, ((AsmFile *)asm_files.d[i])->path);
    }

    sb_add(&o, "],\n\"procs\":[");
    for (i32 i = 0; i < asm_procs.n; i++) {
        AsmProc *p = (AsmProc *)asm_procs.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_str(&o, p->name);
        sb_add(&o, ",");
        ix_rasm_where(&o, p->file, p->line);
        char *sig = asm_clean_line(ix_line_text(p->file, p->line));
        char *eq = strrchr(sig, '=');
        if (eq) *eq = 0;
        sb_add(&o, ",\"sig\":");
        ix_str(&o, sig);
        free(sig);
        /* its labels, as written in the proc (not those macros bring) */
        sb_add(&o, ",\"labels\":[");
        bool first = true;
        for (i32 s = 0; s < p->stmts.n; s++) {
            AsmStmt *st = (AsmStmt *)p->stmts.d[s];
            if (!st->is_label) continue;
            if (!first) sb_add(&o, ",");
            first = false;
            sb_add(&o, "{\"name\":");
            ix_str(&o, st->toks.d[0].text);
            sb_printf(&o, ",\"line\":%d,\"col\":%d}", st->line, st->toks.d[0].col);
        }
        sb_printf(&o, "],\"end\":%d}", p->stmts.n ? ((AsmStmt *)p->stmts.d[p->stmts.n - 1])->line : p->line);
    }

    sb_add(&o, "],\n\"macros\":[");
    bool first = true;
    for (i32 b = 0; b < asm_macro_map.cap; b++) {
        if (!asm_macro_map.slots[b].key) continue;
        AsmList *overloads = (AsmList *)asm_macro_map.slots[b].value;
        for (i32 i = 0; i < overloads->n; i++) {
            AsmMacro *m = (AsmMacro *)overloads->d[i];
            if (!first) sb_add(&o, ",\n");
            first = false;
            sb_add(&o, "{\"name\":");
            ix_str(&o, m->name);
            sb_add(&o, ",");
            ix_rasm_where(&o, m->file, m->line);
            sb_add(&o, ",\"params\":[");
            for (i32 k = 0; k < m->params.n; k++) {
                if (k) sb_add(&o, ",");
                ix_str(&o, (const char *)m->params.d[k]);
            }
            sb_add(&o, "],\"body\":[");
            for (i32 s = 0; s < m->stmts.n; s++) {
                AsmStmt *st = (AsmStmt *)m->stmts.d[s];
                if (s) sb_add(&o, ",");
                char *text = asm_clean_line(ix_line_text(st->file, st->line));
                ix_str(&o, text);
                free(text);
            }
            sb_add(&o, "]}");
        }
    }

    sb_add(&o, "],\n\"defines\":[");
    for (i32 i = 0; i < asm_define_list.n; i++) {
        AsmDefine *d = (AsmDefine *)asm_define_list.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_str(&o, d->name);
        sb_add(&o, ",\"text\":");
        ix_str(&o, d->text);
        i64 v = 0;
        if (!d->evaluating) {
            d->evaluating = true;
            if (asm_eval_text(d->text, &v, d->file, d->line)) sb_printf(&o, ",\"value\":%lld", (long long)v);
            d->evaluating = false;
        }
        if (d->from_rasm) {
            sb_add(&o, ",");
            ix_rasm_where(&o, d->file, d->line);
        }
        sb_add(&o, "}");
    }

    sb_add(&o, "],\n\"globals\":[");
    for (i32 i = 0; i < asm_global_list.n; i++) {
        Stmt *g = (Stmt *)asm_global_list.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_s8(&o, g->name);
        sb_add(&o, ",\"type\":");
        ix_type(&o, g->type);
        ix_size(&o, g->type);
        sb_add(&o, ",");
        ix_where(&o, g->source_path, g->line, g->col);
        sb_printf(&o, ",\"external\":%s}", g->is_external || !g->expr ? "true" : "false");
    }

    sb_add(&o, "],\n\"structs\":[");
    for (i32 i = 0; i < asm_struct_list.n; i++) {
        StructDecl *sd = (StructDecl *)asm_struct_list.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_s8(&o, sd->name);
        sb_add(&o, ",");
        ix_where(&o, sd->source_path, sd->line, sd->col);
        sb_printf(&o, ",\"union\":%s", sd->is_union ? "true" : "false");
        AsmStructLayout *l = asm_struct_layout(sd);
        if (l && l->ok) sb_printf(&o, ",\"size\":%lld,\"align\":%lld", (long long)l->size, (long long)l->align);
        sb_add(&o, ",\"fields\":[");
        bool ff = true;
        ix_fields(&o, sd, 0, &ff);
        sb_add(&o, "]}");
    }

    sb_add(&o, "],\n\"enums\":[");
    for (i32 i = 0; i < asm_enum_list.n; i++) {
        EnumDecl *ed = (EnumDecl *)asm_enum_list.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_s8(&o, ed->name);
        sb_add(&o, ",");
        ix_where(&o, ed->source_path, ed->line, ed->col);
        sb_add(&o, ",\"members\":[");
        for (i32 k = 0; k < ed->items.length; k++) {
            EnumItem *it = (EnumItem *)ed->items.data[k];
            if (k) sb_add(&o, ",");
            sb_add(&o, "{\"name\":");
            ix_s8(&o, it->name);
            i64 v = 0;
            if (asm_enum_item_value(ed, k, &v)) sb_printf(&o, ",\"value\":%lld", (long long)v);
            sb_printf(&o, ",\"line\":%d}", it->line);
        }
        sb_add(&o, "]}");
    }

    sb_add(&o, "],\n\"aliases\":[");
    for (i32 i = 0; i < asm_alias_list.n; i++) {
        AliasDecl *ad = (AliasDecl *)asm_alias_list.d[i];
        if (i) sb_add(&o, ",\n");
        sb_add(&o, "{\"name\":");
        ix_s8(&o, ad->name);
        sb_add(&o, ",\"type\":");
        ix_type(&o, ad->type);
        ix_size(&o, ad->type);
        sb_add(&o, ",");
        ix_where(&o, ad->source_path, ad->line, ad->col);
        sb_add(&o, "}");
    }

    /* Procs declared in .rin files (the Windows API) and rasm procs' rin
       signatures: the parameter names and types a call passes. */
    sb_add(&o, "],\n\"functions\":[");
    first = true;
    for (i32 b = 0; b < asm_rinproc_map.cap; b++) {
        if (!asm_rinproc_map.slots[b].key) continue;
        ProcDecl *pd = (ProcDecl *)asm_rinproc_map.slots[b].value;
        if (!first) sb_add(&o, ",\n");
        first = false;
        sb_add(&o, "{\"name\":");
        ix_s8(&o, pd->name);
        sb_add(&o, ",");
        ix_where(&o, pd->source_path, pd->line, pd->col);
        sb_add(&o, ",\"params\":[");
        for (i32 k = 0; k < pd->params.length; k++) {
            Param *pa = (Param *)pd->params.data[k];
            if (k) sb_add(&o, ",");
            sb_add(&o, "{\"name\":");
            ix_s8(&o, pa->name);
            sb_add(&o, ",\"type\":");
            ix_type(&o, pa->type);
            sb_add(&o, "}");
        }
        sb_add(&o, "],\"ret\":");
        ix_type(&o, pd->ret_type);
        sb_printf(&o, ",\"variadic\":%s}", pd->is_variadic ? "true" : "false");
    }
    sb_add(&o, "]}\n");
    asm_write_file(out_path, sb_cstr(&o));
    free(o.d);
}
