// Suheyl Baghban Sahebkar 251ADB192
// (optional) https://github.com/<your-username>/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: AI (Gemini): I gave it only the assignment text and calc-tests.zip (not my code)
// and asked for the expected output of extra inputs, e.g. prompt "can you go
// through these inputs and tell me what the output file should be for each one?".
// I compared its answers with my program's output.
// Link: https://share.gemini.google/o3GXbVpNvxjC

#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define STUDENT_NAME     "suheyl"
#define STUDENT_LASTNAME "baghban_sahebkar"
#define STUDENT_ID       "251ADB192"

#define MAX_PAREN_DEPTH 3000

typedef struct {
    int is_float;
    long long i;
    double d;
} Value;

static double as_double(Value v) { return v.is_float ? v.d : (double)v.i; }

typedef enum {
    T_NUM, T_PLUS, T_MINUS, T_MUL, T_DIV, T_POW,
    T_LPAREN, T_RPAREN,
    T_EOL,
    T_BAD
} TokKind;

typedef struct {
    TokKind kind;
    size_t start;
    Value num;
} Token;

typedef struct {
    Value v;
    size_t start;
    size_t op_pos;
} PowItem;

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
    Token tok;
    size_t err_pos;
    int depth;
    int oom;
    PowItem *pw;
    size_t pw_len, pw_cap;
} Parser;

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// How positions are tracked: the whole file is one buffer and every token
// stores the byte offset where it starts, so its position is offset + 1.
// The offset counts from the start of the file, so every '\n' and every
// character of comment lines is included automatically.

static void next_token(Parser *p) {
    const char *s = p->s;
    size_t i = p->pos;

    while (i < p->len && (s[i] == ' ' || s[i] == '\t'))
        i++;

    Token *t = &p->tok;
    t->start = i;

    if (i >= p->len || s[i] == '\n') {
        t->kind = T_EOL;
        p->pos = i;
        return;
    }

    switch (s[i]) {
    case '+': t->kind = T_PLUS;   i++; break;
    case '-': t->kind = T_MINUS;  i++; break;
    case '/': t->kind = T_DIV;    i++; break;
    case '(': t->kind = T_LPAREN; i++; break;
    case ')': t->kind = T_RPAREN; i++; break;
    case '*':
        if (i + 1 < p->len && s[i + 1] == '*') { t->kind = T_POW; i += 2; }
        else                                    { t->kind = T_MUL; i += 1; }
        break;
    default:
        if (is_digit(s[i])) {
            size_t j = i;
            unsigned long long acc = 0;
            while (j < p->len && is_digit(s[j])) {
                acc = acc * 10u + (unsigned)(s[j] - '0');
                j++;
            }
            // The '.' is only part of the number when a digit follows it.

            if (j + 1 < p->len && s[j] == '.' && is_digit(s[j + 1])) {
                j++;
                while (j < p->len && is_digit(s[j]))
                    j++;
                size_t n = j - i;
                char small[64];
                char *buf = small;
                if (n >= sizeof small) {
                    buf = malloc(n + 1);
                    if (!buf) { p->oom = 1; t->kind = T_BAD; p->pos = j; return; }
                }
                memcpy(buf, s + i, n);
                buf[n] = '\0';
                t->num.is_float = 1;
                t->num.d = strtod(buf, NULL);
                t->num.i = 0;
                if (buf != small)
                    free(buf);
            } else {
                t->num.is_float = 0;
                t->num.i = (long long)acc;
                t->num.d = 0.0;
            }
            t->kind = T_NUM;
            i = j;
        } else {
            t->kind = T_BAD;
            i++;
        }
        break;
    }
    p->pos = i;
}

static int fail(Parser *p, size_t offset) {
    p->err_pos = offset + 1;
    return 0;
}

static long long ipow(long long base, long long exp) {
    unsigned long long r = 1, b = (unsigned long long)base;
    while (exp > 0) {
        if (exp & 1)
            r *= b;
        exp >>= 1;
        if (exp > 0)
            b *= b;
    }
    return (long long)r;
}

// Integer or float: if both operands are integers the operation is done in
// long long (so '/' truncates toward zero like C); if either one is a float,
// both are converted to double. 7 / 2 is 3, but 7 / 2.0 is 3.5.
//
// Why each run-time error is reported where it is:
//  1. division by zero: at the start of the right operand, because that is
//    the part that is zero (8 / (4 - 2 * 2) points at the '(').
//  2. negative integer exponent: at the start of the exponent, because the
//    exponent is what makes the operation invalid.

static int apply(Parser *p, TokKind op, Value a, Value b,
                 size_t op_pos, size_t rhs_start, Value *out) {
    if (!a.is_float && !b.is_float) {
        unsigned long long x = (unsigned long long)a.i, y = (unsigned long long)b.i;
        long long r = 0;
        switch (op) {
        case T_PLUS:  r = (long long)(x + y); break;
        case T_MINUS: r = (long long)(x - y); break;
        case T_MUL:   r = (long long)(x * y); break;
        case T_DIV:
            if (b.i == 0)
                return fail(p, rhs_start);
            r = (b.i == -1) ? (long long)(0u - x) : a.i / b.i;
            break;
        case T_POW:
            if (b.i < 0)
                return fail(p, rhs_start);
            r = ipow(a.i, b.i);
            break;
        default: break;
        }
        out->is_float = 0;
        out->i = r;
        out->d = 0.0;
        return 1;
    }

    double x = as_double(a), y = as_double(b), r = 0.0;
    switch (op) {
    case T_PLUS:  r = x + y; break;
    case T_MINUS: r = x - y; break;
    case T_MUL:   r = x * y; break;
    case T_DIV:
        if (y == 0.0)
            return fail(p, rhs_start);
        r = x / y;
        break;
    case T_POW:   r = pow(x, y); break;
    default: break;
    }
    if (!isfinite(r))
        return fail(p, op_pos);
    out->is_float = 1;
    out->d = r;
    out->i = 0;
    return 1;
}

// Why the grammar gives the right precedence:
//   expr    := term  { ('+' | '-') term }
//   term    := power { ('*' | '/') power }
//   power   := primary [ '**' power ]
//   primary := NUMBER | '(' expr ')'
// Each level gets its operands from the next higher level, so in
// 3 + 2 * 5 parse_term has already reduced 2 * 5 to 10 before '+' is applied.
// The loops in parse_expr and parse_term fold each new operand into the
// result at once, which makes + - * / left-associative.


// Why syntax errors are reported where they are: when the parser cannot use
// the current token, it fails at that token's start. In primary an operator
// means two operators in a row (or a unary '-'), ')' means "()", and T_EOL
// means a missing operand; a missing ')' is also seen as T_EOL.

static int parse_expr(Parser *p, Value *out);

static int parse_primary(Parser *p, Value *out, size_t *start) {
    const Token t = p->tok;
    *start = t.start;

    if (t.kind == T_NUM) {
        if (t.num.is_float && !isfinite(t.num.d))
            return fail(p, t.start);
        *out = t.num;
        next_token(p);
        return 1;
    }
    if (t.kind == T_LPAREN) {
        if (p->depth >= MAX_PAREN_DEPTH)
            return fail(p, t.start);
        p->depth++;
        next_token(p);
        if (!parse_expr(p, out))
            return 0;
        p->depth--;
        if (p->tok.kind != T_RPAREN)
            return fail(p, p->tok.start);
        next_token(p);
        return 1;
    }
    return fail(p, t.start);
}

static int pw_push(Parser *p, const PowItem *it) {
    if (p->pw_len == p->pw_cap) {
        size_t cap = p->pw_cap ? p->pw_cap * 2 : 64;
        PowItem *n = realloc(p->pw, cap * sizeof *n);
        if (!n) { p->oom = 1; return 0; }
        p->pw = n;
        p->pw_cap = cap;
    }
    p->pw[p->pw_len++] = *it;
    return 1;
}

// Why ** is right-associative:  the rule power := primary [ '**' power ] is
// right-recursive, so 2 ** 3 ** 2 groups as 2 ** (3 ** 2) = 512. Instead of
// real recursion this function collects all operands of the chain and then
// folds them from the right.

static int parse_power(Parser *p, Value *out) {
    const size_t base = p->pw_len;
    size_t op_pos = 0;

    for (;;) {
        PowItem it;
        if (!parse_primary(p, &it.v, &it.start)) {
            p->pw_len = base;
            return 0;
        }
        it.op_pos = op_pos;
        if (!pw_push(p, &it)) {
            p->pw_len = base;
            return 0;
        }
        if (p->tok.kind != T_POW)
            break;
        op_pos = p->tok.start;
        next_token(p);
    }

    Value acc = p->pw[p->pw_len - 1].v;
    for (size_t k = p->pw_len - 1; k > base; k--) {
        if (!apply(p, T_POW, p->pw[k - 1].v, acc, p->pw[k].op_pos, p->pw[k].start, &acc)) {
            p->pw_len = base;
            return 0;
        }
    }
    p->pw_len = base;
    *out = acc;
    return 1;
}

static int parse_term(Parser *p, Value *out) {
    Value acc;
    if (!parse_power(p, &acc))
        return 0;
    while (p->tok.kind == T_MUL || p->tok.kind == T_DIV) {
        const TokKind op = p->tok.kind;
        const size_t op_pos = p->tok.start;
        next_token(p);
        const size_t rhs_start = p->tok.start;
        Value rhs;
        if (!parse_power(p, &rhs))
            return 0;
        if (!apply(p, op, acc, rhs, op_pos, rhs_start, &acc))
            return 0;
    }
    *out = acc;
    return 1;
}

static int parse_expr(Parser *p, Value *out) {
    Value acc;
    if (!parse_term(p, &acc))
        return 0;
    while (p->tok.kind == T_PLUS || p->tok.kind == T_MINUS) {
        const TokKind op = p->tok.kind;
        const size_t op_pos = p->tok.start;
        next_token(p);
        const size_t rhs_start = p->tok.start;
        Value rhs;
        if (!parse_term(p, &rhs))
            return 0;
        if (!apply(p, op, acc, rhs, op_pos, rhs_start, &acc))
            return 0;
    }
    *out = acc;
    return 1;
}

static int parse_line(Parser *p, size_t start, Value *out) {
    p->pos = start;
    p->depth = 0;
    p->pw_len = 0;
    next_token(p);
    if (!parse_expr(p, out))
        return 0;
    if (p->tok.kind != T_EOL)
        return fail(p, p->tok.start);
    return 1;
}

typedef struct {
    char *data;
    size_t len, cap;
} OutBuf;

static int ob_append(OutBuf *ob, const char *src, size_t n) {
    if (ob->len + n > ob->cap) {
        size_t cap = ob->cap ? ob->cap : 4096;
        while (cap < ob->len + n)
            cap *= 2;
        char *d = realloc(ob->data, cap);
        if (!d)
            return 0;
        ob->data = d;
        ob->cap = cap;
    }
    memcpy(ob->data + ob->len, src, n);
    ob->len += n;
    return 1;
}

static int ob_append_value(OutBuf *ob, Value v) {
    char tmp[64];
    int n = v.is_float ? snprintf(tmp, sizeof tmp, "%.15g\n", v.d)
                       : snprintf(tmp, sizeof tmp, "%lld\n", v.i);
    return n > 0 && ob_append(ob, tmp, (size_t)n);
}

// How large files are handled without slowing down: the file is read once
// into one buffer and never copied again; lines are not copied into separate
// strings, the tokenizer reads directly from the buffer by offset.
// The output is kept in memory until the end
// because one error means the file must contain only that error.

static int eval_buffer(const char *s, size_t len, OutBuf *ob) {
    Parser p;
    memset(&p, 0, sizeof p);
    p.s = s;
    p.len = len;

    int ok = 1;
    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= len)
            break;
        if (s[j] == '\n') {
            i = j + 1;
            continue;
        }
        if (s[j] == '#') {
            const char *nl = memchr(s + j, '\n', len - j);
            if (!nl)
                break;
            i = (size_t)(nl - s) + 1;
            continue;
        }

        Value v;
        if (!parse_line(&p, j, &v)) {
            if (p.oom) { ok = 0; break; }
            char tmp[64];
            int n = snprintf(tmp, sizeof tmp, "ERROR:%zu\n", p.err_pos);
            ob->len = 0;
            ok = ob_append(ob, tmp, (size_t)n);
            break;
        }
        if (p.oom || !ob_append_value(ob, v)) { ok = 0; break; }
        i = p.tok.start + 1;
    }
    free(p.pw);
    return ok;
}

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "calc: cannot open '%s': %s\n", path, strerror(errno));
        return 0;
    }
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        fprintf(stderr, "calc: out of memory reading '%s'\n", path);
        return 0;
    }
    for (;;) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(f);
                fprintf(stderr, "calc: out of memory reading '%s'\n", path);
                return 0;
            }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }
    if (ferror(f)) {
        fprintf(stderr, "calc: cannot read '%s'\n", path);
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *out = buf;
    *out_len = len;
    return 1;
}

static int write_file(const char *path, const OutBuf *ob) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "calc: cannot write '%s': %s\n", path, strerror(errno));
        return 0;
    }
    int ok = (ob->len == 0 || fwrite(ob->data, 1, ob->len, f) == ob->len);
    if (fclose(f) != 0)
        ok = 0;
    if (!ok)
        fprintf(stderr, "calc: error while writing '%s'\n", path);
    return ok;
}

static char *concat6(const char *a, const char *b, const char *c,
                     const char *d, const char *e, const char *f) {
    size_t n = strlen(a) + strlen(b) + strlen(c) + strlen(d) + strlen(e) + strlen(f) + 1;
    char *r = malloc(n);
    if (r)
        snprintf(r, n, "%s%s%s%s%s%s", a, b, c, d, e, f);
    return r;
}

static char *base_name(const char *path, int strip_txt) {
    size_t end = strlen(path);
    while (end > 1 && path[end - 1] == '/')
        end--;
    size_t start = end;
    while (start > 0 && path[start - 1] != '/')
        start--;
    size_t n = end - start;
    if (strip_txt && n > 4 && memcmp(path + end - 4, ".txt", 4) == 0)
        n -= 4;
    char *r = malloc(n + 1);
    if (r) {
        memcpy(r, path + start, n);
        r[n] = '\0';
    }
    return r;
}

static int ends_with_txt(const char *name) {
    size_t n = strlen(name);
    return n >= 4 && strcmp(name + n - 4, ".txt") == 0;
}

static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0)
        return 1;
    if (errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 1;
        fprintf(stderr, "calc: '%s' exists and is not a folder\n", path);
        return 0;
    }
    fprintf(stderr, "calc: cannot create folder '%s': %s\n", path, strerror(errno));
    return 0;
}

static int process_file(const char *in_path, const char *outdir) {
    char *data = NULL;
    size_t len = 0;
    if (!read_file(in_path, &data, &len))
        return 0;

    OutBuf ob = {0};
    int ok = eval_buffer(data, len, &ob);
    free(data);
    if (!ok) {
        fprintf(stderr, "calc: out of memory while evaluating '%s'\n", in_path);
        free(ob.data);
        return 0;
    }

    char *base = base_name(in_path, 1);
    char *out_path = base ? concat6(outdir, "/", base,
                                    "_" STUDENT_NAME "_" STUDENT_LASTNAME "_" STUDENT_ID,
                                    ".txt", "")
                          : NULL;
    ok = out_path && write_file(out_path, &ob);
    if (!out_path)
        fprintf(stderr, "calc: out of memory\n");
    free(base);
    free(out_path);
    free(ob.data);
    return ok;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int process_dir(const char *dir, const char *outdir) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "calc: cannot open folder '%s': %s\n", dir, strerror(errno));
        return 0;
    }
    char **paths = NULL;
    size_t n = 0, cap = 0;
    int ok = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!ends_with_txt(e->d_name))
            continue;
        char *path = concat6(dir, "/", e->d_name, "", "", "");
        if (!path) { ok = 0; break; }
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(path);
            continue;
        }
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **np = realloc(paths, nc * sizeof *np);
            if (!np) { free(path); ok = 0; break; }
            paths = np;
            cap = nc;
        }
        paths[n++] = path;
    }
    closedir(d);
    if (!ok)
        fprintf(stderr, "calc: out of memory\n");

    if (ok) {
        if (n > 0)
            qsort(paths, n, sizeof *paths, cmp_str);
        for (size_t k = 0; k < n; k++)
            if (!process_file(paths[k], outdir))
                ok = 0;
    }
    for (size_t k = 0; k < n; k++)
        free(paths[k]);
    free(paths);
    return ok;
}

static void usage(void) {
    fprintf(stderr,
            "usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n"
            "  calc -o OUTDIR input.txt   evaluate one file\n"
            "  calc -d DIR -o OUTDIR      evaluate every *.txt file directly inside DIR\n");
}

int main(int argc, char **argv) {
    const char *dir = NULL, *outdir = NULL, *input = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0 ||
            strcmp(a, "-o") == 0 || strcmp(a, "--output-dir") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "calc: option '%s' needs a value\n", a);
                usage();
                return 2;
            }
            int is_dir = (strcmp(a, "-d") == 0 || strcmp(a, "--dir") == 0);
            if (is_dir) dir = argv[++i];
            else        outdir = argv[++i];
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "calc: unknown option '%s'\n", a);
            usage();
            return 2;
        } else if (input) {
            fprintf(stderr, "calc: only one input file can be given\n");
            usage();
            return 2;
        } else {
            input = a;
        }
    }

    if (!dir && !input) {
        fprintf(stderr, "calc: no input file or folder given\n");
        usage();
        return 2;
    }
    if (dir && input) {
        fprintf(stderr, "calc: give either -d DIR or an input file, not both\n");
        usage();
        return 2;
    }

    const char *src = dir ? dir : input;
    struct stat st;
    if (stat(src, &st) != 0) {
        fprintf(stderr, "calc: '%s': %s\n", src, strerror(errno));
        return 1;
    }
    if (dir && !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is not a folder\n", dir);
        return 1;
    }
    if (!dir && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: '%s' is a folder; use -d to evaluate a folder\n", input);
        return 1;
    }

    char *default_out = NULL;
    if (!outdir) {
        char *base = base_name(src, !dir);
        default_out = base ? concat6(base, "_" STUDENT_NAME "_" STUDENT_ID, "", "", "", "") : NULL;
        free(base);
        if (!default_out) {
            fprintf(stderr, "calc: out of memory\n");
            return 1;
        }
        outdir = default_out;
    }

    int ok = ensure_dir(outdir) && (dir ? process_dir(dir, outdir) : process_file(input, outdir));
    free(default_out);
    return ok ? 0 : 1;
}
