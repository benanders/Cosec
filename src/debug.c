
#include <ctype.h>
#include <limits.h>
#include <inttypes.h>

#include "debug.h"
#include "compile.h"


// ---- AST -------------------------------------------------------------------

static char * AST_NAMES[N_LAST] = {
    "imm", "fp", "str", "init", "local", "global", "kval", "kptr",
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
    "==", "!=", "<", "<=", ">", ">=", "&&", "||",
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    ",", "?",
    "-", "~", "!", "++", "--", "++", "--", "*", "&", "conv",
    "idx", "call", ".",
    "fn def", "typedef", "decl", "if", "while", "do while", "for", "switch",
    "case", "default", "break", "continue", "goto", "label", "return",
};

static void print_nodes(AstNode *n, int indent);
static void print_node(AstNode *n, int indent);
static void print_type(AstType *t);
static void print_expr(AstNode *n);

static void print_struct_fields(AstType *t) {
    if (!t->fields) return; // Anonymous
    printf("{ ");
    for (size_t i = 0; i < vec_len(t->fields); i++) {
        Field *f = vec_get(t->fields, i);
        print_type(f->t);
        if (f->name) printf(" %s", f->name);
        printf(", ");
    }
    printf("}");
}

static void print_enum_consts(AstType *t) {
    if (!t->consts) return; // Anonymous
    printf("{ (");
    print_type(t->num_t);
    printf(") ");
    for (size_t i = 0; i < vec_len(t->consts); i++) {
        EnumConst *k = vec_get(t->consts, i);
        printf("%s = %" PRIu64 ", ", k->name, k->val);
    }
    printf("}");
}

static void print_type(AstType *t) {
    if (!t) return;
    switch (t->k) {
    case T_VOID:    printf("void"); break;
    case T_CHAR:    printf(t->is_unsigned ? "uchar" : "char"); break;
    case T_SHORT:   printf(t->is_unsigned ? "ushort" : "short"); break;
    case T_INT:     printf(t->is_unsigned ? "uint" : "int"); break;
    case T_LONG:    printf(t->is_unsigned ? "ulong" : "long"); break;
    case T_LLONG:   printf(t->is_unsigned ? "ullong" : "llong"); break;
    case T_FLOAT:   printf("float"); break;
    case T_DOUBLE:  printf("double"); break;
    case T_LDOUBLE: printf("ldouble"); break;
    case T_PTR:     print_type(t->ptr); printf("*"); break;
    case T_ARR:
        print_type(t->elem);
        printf("[");
        if (t->len && t->len->k == N_IMM) printf("%" PRIu64, t->len->imm);
        else if (t->len) print_expr(t->len);
        printf("]");
        break;
    case T_FN:
        print_type(t->ret);
        printf("(");
        for (size_t i = 0; i < vec_len(t->params); i++) {
            AstType *arg = vec_get(t->params, i);
            print_type(arg);
            if (i < vec_len(t->params) - 1 || t->is_vararg) {
                printf(", ");
            }
        }
        if (t->is_vararg) {
            printf("...");
        }
        printf(")");
        break;
    case T_STRUCT: printf("struct "); print_struct_fields(t); break;
    case T_UNION:  printf("union ");  print_struct_fields(t); break;
    case T_ENUM:   printf("enum ");   print_enum_consts(t); break;
    }
}

static void print_expr(AstNode *n) {
    switch (n->k) {
        // Operands
    case N_IMM:
        print_type(n->t);
        if (n->t->k == T_CHAR && !n->t->is_unsigned &&
                n->imm < CHAR_MAX && !iscntrl((char) n->imm)) {
            printf(" '%c'", (char) n->imm);
        } else {
            printf(" %" PRId64, n->imm);
        }
        break;
    case N_FP:
        print_type(n->t);
        printf(" %g", n->fp);
        break;
    case N_STR:
        print_type(n->t);
        printf(" ");
        switch (n->enc) {
            case ENC_NONE:   break;
            case ENC_CHAR16: printf("u"); break;
            case ENC_CHAR32: printf("U"); break;
            case ENC_WCHAR:  printf("L"); break;
        }
        printf("\"%s\"", quote_str(n->tk->str, n->tk->len));
        break;
    case N_INIT:
        print_type(n->t);
        printf(" { ");
        for (size_t i = 0; i < vec_len(n->elems); i++) {
            AstNode *elem = vec_get(n->elems, i);
            if (n->t->k == T_STRUCT) {
                Field *ft = vec_get(n->t->fields, i);
                printf(".%s = ", ft->name);
            }
            if (!elem) {
                printf("∅");
            } else {
                print_expr(elem);
            }
            printf(", ");
        }
        printf("}");
        break;
    case N_LOCAL: case N_GLOBAL:
        print_type(n->t);
        printf(" %s", n->var_name);
        break;
    case N_KVAL: UNREACHABLE();
    case N_KPTR:
        print_type(n->t);
        assert(!n->g || n->g->k == N_GLOBAL);
        printf(" ");
        if (n->g) {
            printf("&%s ", n->g->var_name);
            if (n->offset >= 0) {
                printf("+ %" PRIu64, n->offset);
            } else if (n->offset < 0) {
                printf("- %" PRIu64, -n->offset);
            }
        } else {
            if (n->offset >= 0) {
                printf("+%" PRIu64, n->offset);
            } else if (n->offset < 0) {
                printf("%" PRIu64, n->offset);
            }
        }
        break;

        // Operations
    case N_POST_INC: case N_POST_DEC:
        print_type(n->t);
        printf(" ( ");
        print_expr(n->l);
        printf(" %s )", AST_NAMES[n->k]);
        break;
    case N_CALL:
        print_type(n->t);
        printf(" ( call ");
        print_expr(n->fn);
        printf(" ");
        for (size_t i = 0; i < vec_len(n->args); i++) {
            AstNode *arg = vec_get(n->args, i);
            print_expr(arg);
            printf(", ");
        }
        printf(")");
        break;
    case N_FIELD:
        print_type(n->t);
        printf(" ( . ");
        print_expr(n->obj);
        Field *f = vec_get(n->obj->t->fields, n->field_idx);
        printf(" %s )", f->name);
        break;
    case N_TERNARY:
        print_type(n->t);
        printf(" ( ");
        print_expr(n->cond);
        printf(" ? ");
        print_expr(n->body);
        printf(" : ");
        print_expr(n->els);
        printf(" )");
        break;
    default:
        print_type(n->t);
        printf(" ( %s ", AST_NAMES[n->k]);
        print_expr(n->l);
        if (n->r) {
            printf(" ");
            print_expr(n->r);
        }
        printf(" )");
        break;
    }
}

static void print_fn_def(AstNode *n) {
    assert(n->t->k == T_FN);
    if (n->t->linkage == LINK_STATIC) {
        printf("static ");
    }
    print_type(n->t);
    if (n->fn_name) {
        printf(" %s", n->fn_name);
    }
    printf(" (");
    for (size_t i = 0; i < vec_len(n->param_names); i++) {
        Token *name = vec_get(n->param_names, i);
        if (name) {
            assert(name->k == TK_IDENT);
            printf("%s", name->ident);
        }
        if (i < vec_len(n->param_names) - 1 || n->t->is_vararg) {
            printf(", ");
        }
    }
    if (n->t->is_vararg) {
        printf("...");
    }
    printf(")\n");
    print_nodes(n->body, 1);
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("    ");
    }
}

static void print_node(AstNode *n, int indent) {
    switch (n->k) {
    case N_FN_DEF:
        print_fn_def(n);
        break;
    case N_TYPEDEF:
        print_indent(indent);
        printf("typedef %s = ", n->var_name);
        print_type(n->t);
        printf("\n");
        break;
    case N_DECL:
        print_indent(indent);
        if (n->var->t->linkage == LINK_STATIC) {
            printf("static ");
        } else if (n->var->t->linkage == LINK_EXTERN) {
            printf("extern ");
        }
        print_expr(n->var);
        if (n->val) {
            printf(" = ");
            print_expr(n->val);
        }
        printf("\n");
        break;
    case N_IF:
        print_indent(indent);
        while (1) {
            printf("if ");
            print_expr(n->cond);
            printf("\n");
            print_nodes(n->body, indent + 1);
            if (n->els) {
                print_indent(indent);
                printf("else ");
                if (n->els->cond) { // else if
                    n = n->els;
                    continue;
                } else {
                    printf("\n");
                    print_nodes(n->els, indent + 1);
                }
            }
            break;
        }
        break;
    case N_WHILE:
        print_indent(indent);
        printf("while ");
        print_expr(n->cond);
        printf("\n");
        print_nodes(n->body, indent + 1);
        break;
    case N_DO_WHILE:
        print_indent(indent);
        printf("do\n");
        print_nodes(n->body, indent + 1);
        print_indent(indent);
        printf("while ");
        print_expr(n->cond);
        printf("\n");
        break;
    case N_FOR:
        print_node(n->init, indent);
        print_indent(indent);
        printf("for ");
        print_expr(n->cond);
        printf("; ");
        print_expr(n->inc);
        printf("\n");
        print_nodes(n->body, indent + 1);
        break;
    case N_SWITCH:
        print_indent(indent);
        printf("switch ");
        print_expr(n->cond);
        printf("\n");
        print_nodes(n->body, indent + 1);
        break;
    case N_CASE:
        print_indent(indent - 1);
        printf("case ");
        print_expr(n->cond);
        printf(":\n");
        print_node(n->body, indent);
        break;
    case N_DEFAULT:
        print_indent(indent - 1);
        printf("default:\n");
        print_node(n->body, indent);
        break;
    case N_BREAK:
        print_indent(indent);
        printf("break\n");
        break;
    case N_CONTINUE:
        print_indent(indent);
        printf("continue\n");
        break;
    case N_GOTO:
        print_indent(indent);
        printf("goto %s\n", n->label);
        break;
    case N_LABEL:
        print_indent(0);
        printf("%s:\n", n->label);
        print_node(n->body, indent);
        break;
    case N_RET:
        print_indent(indent);
        printf("return ");
        if (n->ret) {
            print_expr(n->ret);
        }
        printf("\n");
        break;
    default:
        print_indent(indent);
        print_expr(n);
        printf("\n");
        break;
    }
}

static void print_nodes(AstNode *n, int indent) {
    while (n) {
        print_node(n, indent);
        n = n->next;
    }
}

void print_ast(AstNode *n) {
    print_nodes(n, 0);
}


// ---- SSA IR ----------------------------------------------------------------

#define BB_PREFIX "._BB"

static char *IR_OP_NAMES[IR_LAST] = {
    "IMM", "FP", "GLOBAL",
    "FARG", "ALLOC", "LOAD", "STORE", "COPY", "ZERO", "IDX",
    "ADD", "SUB", "MUL", "SDIV", "UDIV", "FDIV", "SMOD", "UMOD",
    "AND", "OR", "XOR", "SHL", "SAR", "SHR",
    "EQ", "NEQ", "LT", "LE", "GT", "GE",
    "TRUNC", "SEXT", "ZEXT", "PTR2I", "I2PTR", "BITCAST",
    "FTRUNC", "FEXT", "FP2I", "I2FP",
    "PHI", "BR", "CONDBR", "CALL", "CARG", "RET",
};

static void print_irt(IrType *t) {
    if (!t) return;
    switch (t->k) {
    case IRT_I8: printf("i8"); break;
    case IRT_I16: printf("i16"); break;
    case IRT_I32: printf("i32"); break;
    case IRT_I64: printf("i64"); break;
    case IRT_F32: printf("f32"); break;
    case IRT_F64: printf("f64"); break;
    case IRT_PTR: printf("ptr"); break;
    case IRT_ARR:
        printf("[%zu x ", t->len);
        print_irt(t->elem);
        printf("]");
        break;
    case IRT_STRUCT:
        printf("struct { ");
        for (size_t i = 0; i < vec_len(t->fields); i++) {
            IrField *f = vec_get(t->fields, i);
            print_irt(f->t);
            printf(", ");
        }
        printf("}");
        break;
    }
}

static void print_ins(IrIns *ins) {
    printf("\t%.4zu\t", ins->n);
    print_irt(ins->t);
    printf("\t%s\t", IR_OP_NAMES[ins->op]);
    switch (ins->op) {
    case IR_IMM:    printf("+%" PRIi64, ins->imm); break;
    case IR_FP:     printf("+%g", ins->fp); break;
    case IR_GLOBAL: printf("%s", ins->g->label); break;
    case IR_FARG:   printf("%zu", ins->arg_num); break;
    case IR_ALLOC:
        print_irt(ins->alloc_t);
        if (ins->count) {
            printf("\t%.4zu", ins->count->n);
        }
        break;
    case IR_STORE: printf("%.4zu -> %.4zu", ins->src->n, ins->dst->n); break;
    case IR_COPY:  printf("%.4zu -> %.4zu (size %.4zu)", ins->src->n,
                          ins->dst->n, ins->len->n); break;
    case IR_PHI:
        for (size_t i = 0; i < vec_len(ins->preds); i++) {
            IrBB *pred = vec_get(ins->preds, i);
            IrIns *def = vec_get(ins->defs, i);
            printf("[ " BB_PREFIX "%zu -> %.4zu ] ", pred->n, def->n);
        }
        break;
    case IR_BR: printf(BB_PREFIX "%zu", ins->br ? ins->br->n : 0); break;
    case IR_CONDBR:
        printf("%.4zu\t", ins->cond->n);
        printf(BB_PREFIX "%zu\t", ins->true ? ins->true->n : 0);
        printf(BB_PREFIX "%zu", ins->false ? ins->false->n : 0);
        break;
    default:
        if (ins->l) printf("%.4zu", ins->l->n);
        if (ins->r) printf("\t%.4zu", ins->r->n);
        break;
    }
    printf("\n");
}

static void print_bb(IrBB *bb) {
    printf(BB_PREFIX "%zu:\n", bb->n);
    for (IrIns *ins = bb->head; ins; ins = ins->next) {
        print_ins(ins);
    }
}

static void number_bbs(IrFn *fn) {
    int i = 0;
    for (IrBB *bb = fn->entry; bb; bb = bb->next) {
        bb->n = i++;
    }
}

static void number_ins(IrFn *fn) {
    int i = 0;
    for (IrBB *bb = fn->entry; bb; bb = bb->next) {
        for (IrIns *ins = bb->head; ins; ins = ins->next) {
            ins->n = i++;
        }
    }
}

static void print_fn(Global *g) {
    number_bbs(g->ir_fn);
    number_ins(g->ir_fn);
    printf("%s:\n", g->label);
    for (IrBB *bb = g->ir_fn->entry; bb; bb = bb->next) {
        print_bb(bb);
    }
}

static void print_global(Global *g) {
    print_type(g->val->t);
    printf(" %s", g->label);
    if (g->val) {
        printf(" = ");
        print_node(g->val, 0);
    } else {
        printf("\n");
    }
}

void print_ir(Vec *globals) {
    for (size_t i = 0; i < vec_len(globals); i++) {
        Global *g = vec_get(globals, i);
        if (g->ir_fn) {
            print_fn(g);
        } else {
            print_global(g);
        }
    }
}
