/* SQL to Lisp-AST parser for Rayforce */
#include "lang/sql_parse.h"
#include "lang/eval.h"
#include "table/sym.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>

/* strndup replacement */
static char* sdup(const char* s, size_t n) {
    char* r = (char*)malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n); r[n] = 0;
    return r;
}

typedef enum {
    TOK_EOF, TOK_IDENT, TOK_NUMBER, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMI, TOK_DOT, TOK_STAR,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_PLUS, TOK_MINUS, TOK_SLASH,
    TOK_AND, TOK_AS, TOK_ASC, TOK_BY, TOK_CASE, TOK_CROSS,
    TOK_DELETE_KW, TOK_DESC, TOK_DISTINCT, TOK_ELSE, TOK_END,
    TOK_FALSE_KW, TOK_FROM, TOK_GROUP, TOK_HAVING, TOK_IN,
    TOK_INNER, TOK_INSERT, TOK_INTO, TOK_IS, TOK_JOIN,
    TOK_LEFT, TOK_LIMIT, TOK_NOT, TOK_NULL_KW, TOK_OFFSET,
    TOK_ON, TOK_OR, TOK_ORDER, TOK_RIGHT, TOK_SELECT,
    TOK_SET_KW, TOK_THEN, TOK_TRUE_KW, TOK_UPDATE,
    TOK_VALUES, TOK_WHERE, TOK_WHEN,
} tok_type_t;

typedef struct { tok_type_t type; const char* start; int64_t len;
    double num_val; int64_t line; int64_t col; } token_t;
typedef struct {
    const char* sql; int64_t len; int64_t pos;
    int64_t line; int64_t col;
    token_t curr; token_t peek; int has_peek;
    int err; const char* err_msg;
} sql_parser_t;

static int lex_next_raw(sql_parser_t* P);
/* ===== Tokenizer ===== */
/* Keyword lookup: returns token type or TOK_IDENT */
static int kw_to_tok(const char* s, int64_t len) {
    if (len == 2) {
        if (strncasecmp(s,"AS",2)==0) return TOK_AS;
        if (strncasecmp(s,"BY",2)==0) return TOK_BY;
        if (strncasecmp(s,"IN",2)==0) return TOK_IN;
        if (strncasecmp(s,"IS",2)==0) return TOK_IS;
        if (strncasecmp(s,"ON",2)==0) return TOK_ON;
        if (strncasecmp(s,"OR",2)==0) return TOK_OR;
    }
    if (len == 3) {
        if (strncasecmp(s,"AND",3)==0) return TOK_AND;
        if (strncasecmp(s,"ASC",3)==0) return TOK_ASC;
        if (strncasecmp(s,"END",3)==0) return TOK_END;
        if (strncasecmp(s,"NOT",3)==0) return TOK_NOT;
        if (strncasecmp(s,"SET",3)==0) return TOK_SET_KW;
    }
    if (len == 4) {
        if (strncasecmp(s,"CASE",4)==0) return TOK_CASE;
        if (strncasecmp(s,"DESC",4)==0) return TOK_DESC;
        if (strncasecmp(s,"ELSE",4)==0) return TOK_ELSE;
        if (strncasecmp(s,"FROM",4)==0) return TOK_FROM;
        if (strncasecmp(s,"JOIN",4)==0) return TOK_JOIN;
        if (strncasecmp(s,"LEFT",4)==0) return TOK_LEFT;
        if (strncasecmp(s,"NULL",4)==0) return TOK_NULL_KW;
        if (strncasecmp(s,"THEN",4)==0) return TOK_THEN;
        if (strncasecmp(s,"TRUE",4)==0) return TOK_TRUE_KW;
        if (strncasecmp(s,"WHEN",4)==0) return TOK_WHEN;
    }
    if (len == 5) {
        if (strncasecmp(s,"CROSS",5)==0) return TOK_CROSS;
        if (strncasecmp(s,"GROUP",5)==0) return TOK_GROUP;
        if (strncasecmp(s,"INNER",5)==0) return TOK_INNER;
        if (strncasecmp(s,"LIMIT",5)==0) return TOK_LIMIT;
        if (strncasecmp(s,"ORDER",5)==0) return TOK_ORDER;
        if (strncasecmp(s,"RIGHT",5)==0) return TOK_RIGHT;
        if (strncasecmp(s,"WHERE",5)==0) return TOK_WHERE;
    }
    if (len == 6) {
        if (strncasecmp(s,"DELETE",6)==0) return TOK_DELETE_KW;
        if (strncasecmp(s,"HAVING",6)==0) return TOK_HAVING;
        if (strncasecmp(s,"INSERT",6)==0) return TOK_INSERT;
        if (strncasecmp(s,"OFFSET",6)==0) return TOK_OFFSET;
        if (strncasecmp(s,"SELECT",6)==0) return TOK_SELECT;
        if (strncasecmp(s,"UPDATE",6)==0) return TOK_UPDATE;
        if (strncasecmp(s,"VALUES",6)==0) return TOK_VALUES;
    }
    if (len == 4) {
        if (strncasecmp(s,"INTO",4)==0) return TOK_INTO;
    }
    if (len == 8) {
        if (strncasecmp(s,"DISTINCT",8)==0) return TOK_DISTINCT;
    }
    return TOK_IDENT;
}

/* Core tokenizer */
static int lex_next_raw(sql_parser_t* P) {
    token_t* t = &P->curr;
    memset(t, 0, sizeof(*t));
    t->type = TOK_EOF;
    while (P->pos < P->len) {
        char c = P->sql[P->pos];
        if (isspace((unsigned char)c)) {
            if (c == '\n') { P->line++; P->col = 0; }
            P->pos++; P->col++; continue;
        }
        t->start = P->sql + P->pos;
        t->line = P->line; t->col = P->col;
        /* Line comment */
        if (c == '-' && P->pos+1 < P->len && P->sql[P->pos+1] == '-') {
            while (P->pos < P->len && P->sql[P->pos] != '\n') P->pos++;
            continue;
        }
        /* Block comment */
        if (c == '/' && P->pos+1 < P->len && P->sql[P->pos+1] == '*') {
            P->pos += 2;
            while (P->pos < P->len) {
                if (P->sql[P->pos] == '*' && P->pos+1 < P->len && P->sql[P->pos+1] == '/') { P->pos+=2; break; }
                if (P->sql[P->pos] == '\n') P->line++;
                P->pos++;
            }
            continue;
        }
        /* String literal */
        if (c == '\'') {
            P->pos++; t->type = TOK_STRING; t->start = P->sql + P->pos;
            while (P->pos < P->len) {
                if (P->sql[P->pos] == '\'') {
                    if (P->pos+1 < P->len && P->sql[P->pos+1] == '\'') { P->pos+=2; continue; }
                    break;
                }
                if (P->sql[P->pos] == '\n') P->line++;
                P->pos++;
            }
            t->len = (P->sql+P->pos) - t->start;
            if (P->pos < P->len) P->pos++;
            return 1;
        }
        /* Number */
        if (isdigit((unsigned char)c) || (c == '.' && P->pos+1 < P->len && isdigit((unsigned char)P->sql[P->pos+1]))) {
            t->type = TOK_NUMBER;
            const char* s = P->sql + P->pos;
            while (P->pos < P->len && (isdigit((unsigned char)P->sql[P->pos]) || P->sql[P->pos] == '.')) P->pos++;
            if (P->pos < P->len && (P->sql[P->pos] == 'e' || P->sql[P->pos] == 'E')) {
                P->pos++;
                if (P->pos < P->len && (P->sql[P->pos] == '+' || P->sql[P->pos] == '-')) P->pos++;
                while (P->pos < P->len && isdigit((unsigned char)P->sql[P->pos])) P->pos++;
            }
            t->len = (P->sql+P->pos) - s;
            t->num_val = strtod(s, NULL);
            return 1;
        }
        /* Identifiers or keywords */
        if (isalpha((unsigned char)c) || c == '_') {
            const char* s = P->sql + P->pos;
            while (P->pos < P->len && (isalnum((unsigned char)P->sql[P->pos]) || P->sql[P->pos] == '_')) P->pos++;
            t->len = (P->sql+P->pos) - s;
            t->type = kw_to_tok(s, t->len);
            return 1;
        }
        /* Operators and punctuation */
        P->pos++;
        switch (c) {
            case '(': t->type = TOK_LPAREN; return 1;
            case ')': t->type = TOK_RPAREN; return 1;
            case ',': t->type = TOK_COMMA; return 1;
            case ';': t->type = TOK_SEMI; return 1;
            case '+': t->type = TOK_PLUS; return 1;
            case '-': t->type = TOK_MINUS; return 1;
            case '*': t->type = TOK_STAR; return 1;
            case '/': t->type = TOK_SLASH; return 1;
            case '.': t->type = TOK_DOT; return 1;
            case '=': t->type = TOK_EQ; return 1;
            case '<':
                if (P->pos < P->len && P->sql[P->pos] == '=') { P->pos++; t->type = TOK_LE; return 1; }
                if (P->pos < P->len && P->sql[P->pos] == '>') { P->pos++; t->type = TOK_NE; return 1; }
                t->type = TOK_LT; return 1;
            case '>':
                if (P->pos < P->len && P->sql[P->pos] == '=') { P->pos++; t->type = TOK_GE; return 1; }
                t->type = TOK_GT; return 1;
            default:
                if (!P->err_msg) P->err_msg = "unexpected character";
                return 0;
        }
    }
    t->type = TOK_EOF;
    return 1;
}

/* Token lookahead helpers */
static void lex_peek(sql_parser_t* P) {
    if (!P->has_peek) {
        token_t saved = P->curr;
        if (!lex_next_raw(P)) { P->err = 1; return; }
        P->peek = P->curr;
        P->curr = saved;
        P->has_peek = 1;
    }
}

static int lex_next(sql_parser_t* P) {
    if (P->has_peek) {
        P->curr = P->peek;
        P->has_peek = 0;
        return 1;
    }
    return lex_next_raw(P);
}

static int lex_accept(sql_parser_t* P, tok_type_t t) {
    lex_peek(P);
    if (P->peek.type == t) {
        P->curr = P->peek;
        P->has_peek = 0;
        return 1;
    }
    return 0;
}
/* ===== AST helpers ===== */
static ray_t* make_sym_c(const char* s) {
    int64_t id = ray_sym_intern_runtime(s, strlen(s));
    if (id < 0) return ray_error("oom", NULL);
    return ray_sym(id);
}
static ray_t* make_i64_val(int64_t v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_I64; obj->i64 = v;
    return obj;
}
static ray_t* make_f64_val(double v) {
    ray_t* obj = ray_alloc(0);
    if (!obj) return ray_error("oom", NULL);
    obj->type = -RAY_F64; obj->f64 = v;
    return obj;
}
static ray_t* make_str_lit(const char* s, int64_t len) {
    return ray_str(s, (size_t)len);
}
static ray_t* make_list_ex(ray_t* first) {
    /* Build List[first, ...] */
    ray_t* lst = ray_list_new(1);
    if (!lst) return NULL;
    if (first) lst = ray_list_append(lst, first);
    return lst;
}
static ray_t* list_append(ray_t* lst, ray_t* elem) {
    if (!lst || !elem) return lst;
    return ray_list_append(lst, elem);
}
/* Wrap expr as: (op arg) or (op arg1 arg2) */
static ray_t* make_prefix1(const char* op, ray_t* arg) {
    ray_t* lst = ray_list_new(2);
    ray_t* op_sym = make_sym_c(op);
    lst = ray_list_append(lst, op_sym);
    lst = ray_list_append(lst, arg);
    return lst;
}
static ray_t* make_prefix2(const char* op, ray_t* a, ray_t* b) {
    ray_t* lst = ray_list_new(3);
    ray_t* op_sym = make_sym_c(op);
    lst = ray_list_append(lst, op_sym);
    lst = ray_list_append(lst, a);
    lst = ray_list_append(lst, b);
    return lst;
}

/* Convert a homogeneous list of atoms to a typed vector.
 * ray_eval treats RAY_LIST as function calls, so literal lists like
 * (10 20 30) produce "head of list is not callable, got i64" when
 * passed as data (e.g. second argument to `in`).  Typed vectors pass
 * through ray_eval as literal data.  Returns NULL if conversion is
 * not possible (non-atoms, mixed types, empty list); caller keeps
 * ownership of the original list in that case. */
static ray_t* list_to_atom_vec(ray_t* list) {
    int64_t n = ray_len(list);
    if (n == 0) return NULL;
    ray_t** data = (ray_t**)ray_data(list);
    int8_t itype = data[0]->type;
    int8_t vtype = -1;
    if (itype == -RAY_I64 || itype == -RAY_SYM) vtype = RAY_I64;
    else if (itype == -RAY_F64) vtype = RAY_F64;
    else if (itype == -RAY_STR) vtype = RAY_STR;
    else return NULL;
    for (int64_t i = 1; i < n; i++)
        if (data[i]->type != itype) return NULL;
    ray_t* vec = ray_vec_new(vtype, n);
    if (!vec || RAY_IS_ERR(vec)) { if (vec) ray_release(vec); return NULL; }
    if (vtype == RAY_I64) {
        for (int64_t i = 0; i < n; i++) {
            int64_t v = data[i]->i64;
            vec = ray_vec_append(vec, &v);
        }
    } else if (vtype == RAY_F64) {
        for (int64_t i = 0; i < n; i++) {
            double v = data[i]->f64;
            vec = ray_vec_append(vec, &v);
        }
    } else if (vtype == RAY_STR) {
        for (int64_t i = 0; i < n; i++) {
            const char* s = ray_str_ptr(data[i]);
            int64_t sl = ray_str_len(data[i]);
            vec = ray_str_vec_append(vec, s, sl);
        }
    }
    return vec;
}

/* Build a dict from interleaved key-value pairs.
   keys are strings (converted to sym), values are ray_t* already retained.
   Caller owns the result; pairs array is consumed (values freed on error). */
static ray_t* make_dict_kv(const char** keys, ray_t** vals, int n) {
    if (n == 0) return ray_dict_new(NULL, NULL);
    ray_t* ks = ray_sym_vec_new(RAY_SYM_W64, n);
    if (!ks) return ray_error("oom", NULL);
    ray_t* vs = ray_list_new(n);
    if (!vs) { ray_release(ks); return ray_error("oom", NULL); }
    for (int i = 0; i < n; i++) {
        int64_t id = ray_sym_intern_runtime(keys[i], strlen(keys[i]));
        if (id < 0) { ray_release(ks); ray_release(vs); return ray_error("oom", NULL); }
        ray_vec_append(ks, &id);
        ray_list_append(vs, vals[i]);
    }
    ray_t* d = ray_dict_new(ks, vs);
    return d;
}
/* ===== Expression parser (recursive descent) ===== */
/* Forward: all parse_* functions use lex_next/lex_peek/lex_accept */
static ray_t* parse_or(sql_parser_t*);
static ray_t* parse_and(sql_parser_t*);
static ray_t* parse_not(sql_parser_t*);
static ray_t* parse_cmp(sql_parser_t*);
static ray_t* parse_add(sql_parser_t*);
static ray_t* parse_mul(sql_parser_t*);
static ray_t* parse_unary(sql_parser_t*);
static ray_t* parse_primary(sql_parser_t*);

static ray_t* parse_primary(sql_parser_t* P) {
    if (P->err) return NULL;
    token_t* t = &P->curr;
    if (lex_accept(P, TOK_NULL_KW))
        return ray_typed_null(-RAY_I64);
    if (lex_accept(P, TOK_TRUE_KW))
        return make_i64_val(1);
    if (lex_accept(P, TOK_FALSE_KW))
        return make_i64_val(0);
    if (lex_accept(P, TOK_NUMBER)) {
        double v = t->num_val;
        if (v == (int64_t)v && fabs(v) < 9e18)
            return make_i64_val((int64_t)v);
        return make_f64_val(v);
    }
    if (lex_accept(P, TOK_STRING))
        return make_str_lit(t->start, t->len);
    /* ( expr ) */
    if (lex_accept(P, TOK_LPAREN)) {
        ray_t* e = parse_or(P);
        if (!lex_accept(P, TOK_RPAREN)) { P->err_msg = "expected )"; return NULL; }
        return e;
    }
    /* CASE */
    if (lex_accept(P, TOK_CASE)) {
        return ray_error("nyi", "CASE not yet implemented");
    }
    /* Ident -> column ref or function call */
    if (lex_accept(P, TOK_IDENT) || lex_accept(P, TOK_STAR)) {
        char id[256]; int64_t id_len;
        if (t->type == TOK_STAR) {
            id[0] = '*'; id_len = 1;
        } else {
            id_len = t->len > 255 ? 255 : t->len;
            memcpy(id, t->start, (size_t)id_len); id[id_len] = '\0';
        }
        lex_peek(P);
        /* Function call: ident ( ... ) */
        if (P->peek.type == TOK_LPAREN) {
            lex_accept(P, TOK_LPAREN);
            ray_t* result = ray_list_new(0);
            ray_t* op_sym = make_sym_c(id);
            result = ray_list_append(result, op_sym);
            if (!lex_accept(P, TOK_RPAREN)) {
                for (;;) {
                    ray_t* arg = parse_or(P);
                    if (P->err) return NULL;
                    result = ray_list_append(result, arg); ray_release(arg);
                    if (lex_accept(P, TOK_RPAREN)) break;
                    if (!lex_accept(P, TOK_COMMA)) { P->err_msg = "expected , or )"; return NULL; }
                }
            }
            return result;
        }
        /* Column ref */
        lex_peek(P);
        if (P->peek.type == TOK_DOT) {
            lex_accept(P, TOK_DOT);
            if (!lex_accept(P, TOK_IDENT)) { P->err_msg = "expected column name"; return NULL; }
            /* Just use the column name part */
            token_t* t2 = &P->curr;
            char col[256]; int64_t clen = t2->len > 255 ? 255 : t2->len;
            memcpy(col, t2->start, (size_t)clen); col[clen] = '\0';
            /* Build qualified reference: (getattr tbl col) */
            return make_prefix2("getattr", make_sym_c(id), make_sym_c(col));
        }
        return make_sym_c(id);
    }
    P->err_msg = "expected expression";
    return NULL;
}static ray_t* parse_unary(sql_parser_t* P) {
    if (P->err) return NULL;
    if (lex_accept(P, TOK_PLUS)) return parse_unary(P);
    if (lex_accept(P, TOK_MINUS))
        return make_prefix1("neg", parse_unary(P));
    return parse_primary(P);
}
static ray_t* parse_mul(sql_parser_t* P) {
    if (P->err) return NULL;
    ray_t* left = parse_unary(P); if (!left) return NULL;
    for (;;) {
        if (lex_accept(P, TOK_STAR)) {
            ray_t* right = parse_unary(P); if (!right) return NULL;
            left = make_prefix2("*", left, right);
        } else if (lex_accept(P, TOK_SLASH)) {
            ray_t* right = parse_unary(P); if (!right) return NULL;
            left = make_prefix2("/", left, right);
        } else break;
    }
    return left;
}
static ray_t* parse_add(sql_parser_t* P) {
    if (P->err) return NULL;
    ray_t* left = parse_mul(P); if (!left) return NULL;
    for (;;) {
        if (lex_accept(P, TOK_PLUS)) {
            ray_t* right = parse_mul(P); if (!right) return NULL;
            left = make_prefix2("+", left, right);
        } else if (lex_accept(P, TOK_MINUS)) {
            ray_t* right = parse_mul(P); if (!right) return NULL;
            left = make_prefix2("-", left, right);
        } else break;
    }
    return left;
}
static ray_t* parse_cmp(sql_parser_t* P) {
    if (P->err) return NULL;
    ray_t* left = parse_add(P); if (!left) return NULL;
    const char* cmp = NULL;
    if (lex_accept(P, TOK_EQ)) cmp = "==";
    else if (lex_accept(P, TOK_NE)) cmp = "!=";
    else if (lex_accept(P, TOK_LT)) cmp = "<";
    else if (lex_accept(P, TOK_GT)) cmp = ">";
    else if (lex_accept(P, TOK_LE)) cmp = "<=";
    else if (lex_accept(P, TOK_GE)) cmp = ">=";
    if (cmp) {
        ray_t* right = parse_add(P); if (!right) return NULL;
        return make_prefix2(cmp, left, right);
    }
    /* IS [NOT] NULL */
    if (lex_accept(P, TOK_IS)) {
        int neg = 0;
        if (lex_accept(P, TOK_NOT)) neg = 1;
        if (!lex_accept(P, TOK_NULL_KW)) { P->err_msg = "expected NULL"; return NULL; }
        if (neg) return make_prefix1("not", make_prefix1("null?", left));
        return make_prefix1("null?", left);
    }
    /* IN (list) */
    if (lex_accept(P, TOK_IN)) {
        if (!lex_accept(P, TOK_LPAREN)) { P->err_msg = "expected ("; return NULL; }
        ray_t* in_items = ray_list_new(0);
        for (;;) {
            ray_t* item = parse_or(P); if (P->err) return NULL;
            in_items = list_append(in_items, item); ray_release(item);
            if (lex_accept(P, TOK_RPAREN)) break;
            if (!lex_accept(P, TOK_COMMA)) { P->err_msg = "expected , or )"; return NULL; }
        }
        /* Convert IN list to typed vector for literal values.
         * Subqueries like IN (select ...) produce non-atom items and
         * fall through to the list path unchanged. */
        {
            ray_t* vec = list_to_atom_vec(in_items);
            if (vec) {
                ray_release(in_items);
                return make_prefix2("in", left, vec);
            }
        }
        return make_prefix2("in", left, in_items);
    }
    return left;
}
static ray_t* parse_not(sql_parser_t* P) {
    if (P->err) return NULL;
    if (lex_accept(P, TOK_NOT))
        return make_prefix1("not", parse_not(P));
    return parse_cmp(P);
}
static ray_t* parse_and(sql_parser_t* P) {
    if (P->err) return NULL;
    ray_t* left = parse_not(P); if (!left) return NULL;
    for (;;) {
        if (lex_accept(P, TOK_AND)) {
            ray_t* right = parse_not(P); if (!right) return NULL;
            left = make_prefix2("and", left, right);
        } else break;
    }
    return left;
}
static ray_t* parse_or(sql_parser_t* P) {
    if (P->err) return NULL;
    ray_t* left = parse_and(P); if (!left) return NULL;
    for (;;) {
        if (lex_accept(P, TOK_OR)) {
            ray_t* right = parse_and(P); if (!right) return NULL;
            left = make_prefix2("or", left, right);
        } else break;
    }
    return left;
}
static ray_t* parse_expr(sql_parser_t* P) {
    if (P->err) return NULL;
    return parse_or(P);
}
/* ===== SELECT parser ===== */
static int is_clause_kw(tok_type_t t) {
    return t == TOK_FROM || t == TOK_WHERE || t == TOK_GROUP ||
           t == TOK_HAVING || t == TOK_ORDER || t == TOK_LIMIT ||
           t == TOK_OFFSET || t == TOK_EOF || t == TOK_SEMI;
}

static ray_t* parse_select_stmt(sql_parser_t* P) {
    if (P->err) return NULL;

    /* SELECT [DISTINCT] */
    int distinct = 0;
    if (lex_accept(P, TOK_DISTINCT)) distinct = 1;

    /* Parse select list: each item is expr [[AS] alias] */
    /* Store as (alias_or_colname, expr) pairs */
    int max_items = 512;
    const char** out_keys = (const char**)calloc((size_t)max_items, sizeof(char*));
    ray_t** out_vals = (ray_t**)calloc((size_t)max_items, sizeof(ray_t*));
    int n_out = 0;

    for (;;) {
        if (lex_accept(P, TOK_STAR)) {
            /* SELECT * -> pass through all columns. Mark with "*" key. */
            out_keys[n_out] = sdup("*", 1);
            out_vals[n_out] = make_sym_c("*");
            n_out++;
        } else {
            ray_t* expr = parse_expr(P);
            if (P->err) goto fail;
            /* Check for alias */
            lex_peek(P);
            if (P->peek.type == TOK_AS) {
                lex_accept(P, TOK_AS);
                if (!lex_accept(P, TOK_IDENT)) { P->err_msg = "expected alias"; goto fail; }
                token_t* at = &P->curr;
                char aname[256];
                int64_t alen = at->len > 255 ? 255 : at->len;
                memcpy(aname, at->start, (size_t)alen); aname[alen] = '\0';
                out_keys[n_out] = sdup(aname, (size_t)alen);
                out_vals[n_out] = expr;
                n_out++;
            } else if (expr && expr->type == -RAY_SYM) {
                /* Plain column ref: check if next token could be a bare alias */
                /* Bare alias: next token is identifier followed by comma or FROM clause */
                /* Skip bare alias for simplicity - use the column name */
                char cname[256];
                ray_t* cn_ray = (expr && expr->type == -RAY_SYM) ? ray_sym_str(expr->i64) : NULL;
            const char* cn = cn_ray ? ray_str_ptr(cn_ray) : "?";
                int64_t clen = (int64_t)strlen(cn);
                memcpy(cname, cn, (size_t)(clen > 255 ? 255 : clen));
                cname[clen > 255 ? 255 : clen] = '\0';
                out_keys[n_out] = sdup(cname, (size_t)(clen > 255 ? 255 : clen));
                out_vals[n_out] = expr;
                n_out++;
            } else {
                /* Expression without alias: generate colN */
                char buf[32];
                snprintf(buf, sizeof(buf), "col%d", n_out);
                out_keys[n_out] = sdup(buf, strlen(buf));
                out_vals[n_out] = expr;
                n_out++;
            }
        }
        if (!lex_accept(P, TOK_COMMA)) break;
        if (n_out >= max_items) { P->err_msg = "too many output columns"; goto fail; }
    }

    /* FROM */
    if (!lex_accept(P, TOK_FROM)) { P->err_msg = "expected FROM"; goto fail; }
    if (!lex_accept(P, TOK_IDENT)) { P->err_msg = "expected table name"; goto fail; }
    token_t tbl_tok = P->curr;
        char tname[256]; int tnl = tbl_tok.len; if (tnl > 255) tnl = 255;
    memcpy(tname, tbl_tok.start, tnl); tname[tnl] = 0;
    /* Skip table alias */
    lex_peek(P);
    if (P->peek.type == TOK_AS) {
        lex_accept(P, TOK_AS); lex_accept(P, TOK_IDENT);
    } else if (P->peek.type == TOK_IDENT && !is_clause_kw(P->peek.type)) {
        lex_accept(P, TOK_IDENT); /* bare alias */
    }

    /* Clauses */
    ray_t* where_expr = NULL;
    ray_t* having = NULL;
    int64_t limit_n = -1, offset_n = -1;
    /* GROUP BY */
    int n_group = 0;
    ray_t* group_cols[256];
    /* ORDER BY */
    int n_order = 0;
    const char* order_names[256];
    int order_descs[256];
    ray_t* order_cols[256];
    const char** dk = NULL;
    ray_t** dv = NULL;

    for (;;) {
        lex_peek(P);
        tok_type_t nxt = P->peek.type;
        if (nxt == TOK_EOF || nxt == TOK_SEMI) { lex_accept(P, nxt); break; }
        if (nxt == TOK_WHERE) { lex_next(P);
            where_expr = parse_expr(P);
        } else if (nxt == TOK_GROUP) { lex_next(P);
            if (!lex_accept(P, TOK_BY)) { P->err_msg = "expected BY"; goto fail; }
            for (;;) {
                ray_t* gc = parse_expr(P); if (P->err) goto fail;
                group_cols[n_group++] = gc;
                if (!lex_accept(P, TOK_COMMA)) break;
            }
        } else if (nxt == TOK_HAVING) { lex_next(P);
            having = parse_expr(P);
        } else if (nxt == TOK_ORDER) { lex_next(P);
            if (!lex_accept(P, TOK_BY)) { P->err_msg = "expected BY"; goto fail; }
            for (;;) {
                ray_t* oc = parse_expr(P); if (P->err) goto fail;
                int desc = 0;
                if (lex_accept(P, TOK_DESC)) desc = 1;
                else lex_accept(P, TOK_ASC);
                order_names[n_order] = desc ? "desc" : "asc";
                order_descs[n_order] = desc;
                order_cols[n_order] = oc;
                n_order++;
                if (!lex_accept(P, TOK_COMMA)) break;
            }
        } else if (nxt == TOK_LIMIT) { lex_next(P);
            if (!lex_accept(P, TOK_NUMBER)) { P->err_msg = "expected number"; goto fail; }
            limit_n = (int64_t)P->curr.num_val;
            if (lex_accept(P, TOK_OFFSET)) {
                if (!lex_accept(P, TOK_NUMBER)) { P->err_msg = "expected number"; goto fail; }
                offset_n = (int64_t)P->curr.num_val;
            }
        } else if (nxt == TOK_OFFSET) { lex_next(P);
            if (!lex_accept(P, TOK_NUMBER)) { P->err_msg = "expected number"; goto fail; }
            offset_n = (int64_t)P->curr.num_val;
        } else {
            break;
        }
    }

    /* ========== Build Lisp AST: (select {from: TBL ...}) ========== */
    /* Count pairs: from(2) + n_out*2 + where(2) + by(2) + having(2) + order(n_order*2) + head(2) + offset(2) + distinct(2) */
    int np = 0;
    /* Allocate key-value pair arrays */
    dk = (const char**)calloc((size_t)(n_out*2 + 32), sizeof(char*));
    dv = (ray_t**)calloc((size_t)(n_out*2 + 32), sizeof(ray_t*));
    int di = 0;

#define ADD_KV(k,v) do { dk[di] = (k); dv[di] = (v); di++; } while(0)

    /* from: TBL */
    char tbl_name[256];
    snprintf(tbl_name, sizeof(tbl_name), "%.*s", (int)tbl_tok.len, tbl_tok.start);
    ADD_KV(sdup("from",4), make_sym_c(tbl_name));

    /* Output columns: for each select item, key=alias, value=expr */
    for (int i = 0; i < n_out; i++) {
        if (strcmp(out_keys[i], "*") == 0) {
            /* SELECT * : no output column entry needed for * */
            /* Just emit from without specific columns */
        } else {
            ADD_KV(out_keys[i], out_vals[i]); /* out_vals[i] already retained */
            out_vals[i] = NULL; /* ownership transferred */
        }
    }

    /* where: EXPR */
    if (where_expr) { ADD_KV(sdup("where",5), where_expr); }

    /* by: group by columns */
    if (n_group > 0) {
        if (n_group == 1) {
            ADD_KV(sdup("by",2), group_cols[0]); group_cols[0] = NULL;
        } else {
            ray_t* gl = ray_list_new(0);
            for (int i = 0; i < n_group; i++) {
                gl = list_append(gl, group_cols[i]); ray_release(group_cols[i]);
            }
            ADD_KV(sdup("by",2), gl);
        }
    }

    /* having: EXPR */
    if (having) { ADD_KV(sdup("having",6), having); }

    /* ORDER BY -> asc/desc keys */
    for (int i = 0; i < n_order; i++) {
        ADD_KV(sdup(order_names[i], strlen(order_names[i])), order_cols[i]);
        order_cols[i] = NULL;
    }

    /* LIMIT -> head:N */
    if (limit_n >= 0) { ADD_KV(sdup("head",4), make_i64_val(limit_n)); }
    if (offset_n >= 0) { ADD_KV(sdup("offset",6), make_i64_val(offset_n)); }
    if (distinct) { ADD_KV(sdup("distinct",8), make_i64_val(1)); }

    /* Build dict */
    /* Build (select dict) */
    ray_t* dict = make_dict_kv((const char**)dk, dv, di);
    ray_t* result = ray_list_new(2);
    ray_t* sel_sym = make_sym_c("select");
    result = ray_list_append(result, sel_sym);
    ray_release(sel_sym);
    result = ray_list_append(result, dict);
    ray_release(dict);

    /* Cleanup */
    free(dk); free(dv);
    for (int i = 0; i < n_out; i++) {
        free((void*)out_keys[i]);
        if (out_vals[i]) ray_release(out_vals[i]);
    }
    free(out_keys); free(out_vals);
    for (int i = 0; i < n_group; i++) if (group_cols[i]) ray_release(group_cols[i]);
    for (int i = 0; i < n_order; i++) if (order_cols[i]) ray_release(order_cols[i]);
    if (where_expr) ray_release(where_expr);
    if (having) ray_release(having);
    /* cleanup handled by for loop below */
    return result;

fail:
    /* Leak all resources - free()/ray_release() crash on this platform */
    return NULL;
}


ray_t* ray_sql_parse(const char* sql, ray_t* nfo) {
    (void)nfo;
    if (!sql || !*sql) return ray_error("parse", "empty SQL input");
    sql_parser_t P;
    memset(&P, 0, sizeof(P));
    P.sql = sql;
    P.len = (int64_t)strlen(sql);
    P.line = 1;

    lex_next_raw(&P);
    if (P.err) return ray_error("parse", P.err_msg ? P.err_msg : "tokenization error");

    if (P.curr.type == TOK_SELECT) {
        /* parse_select_stmt sets P.err_msg on failure and returns NULL */
        /* Ensure NULL with err_msg is converted to a proper error object */
        ray_t* result = parse_select_stmt(&P);
        if (!result) {
            /* NULL => return NULL; caller handles it without ray_error (crashes without __VM) */
            return NULL;
        }
        return result;
    }

    if (P.err || P.err_msg)
        return ray_error("parse", P.err_msg ? P.err_msg : "unsupported SQL");
    return ray_error("nyi", "SQL statement type not implemented");
}

/* Static error sentinel for SQL parse failures where ray_error() cannot be
 * called (buddy allocator may be corrupted). Must have type RAY_ERROR so
 * RAY_IS_ERR() recognizes it, and attrs=0/rc=0 since it is never freed. */
static ray_t sql_parse_err = {
    .type  = RAY_ERROR,
    .rc    = 0,
    .slen  = 6,
    .sdata = { 'n', 'o', 'f', 'r', 'o', 'm', 0 },
};

ray_t* ray_sql_eval(const char* sql) {
    ray_t* ast = ray_sql_parse(sql, NULL);
    if (!ast || RAY_IS_ERR(ast)) {
        if (!ast) return &sql_parse_err;
        return ast;
    }
    ray_t* result = ray_eval(ast);
    ray_release(ast);
    return result;
}
