
#include <ctype.h>
#include <limits.h>
#include <inttypes.h>

#include "debug.h"
#include "compile.h"


// ---- AST Printing ----------------------------------------------------------

static char * AST_NAMES[N_LAST] = {
    "imm", "fp", "str", "array", "init", "local", "global", "kptr", "kval",
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
    "==", "!=", "<", "<=", ">", ">=", "&&", "||",
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    ",", "?",
    "-", "~", "!", "++", "--", "++", "--", "*", "&", "conv",
    "idx", "call", ".",
    "fn def", "typedef", "decl", "if", "while", "do while", "for", "switch",
    "case", "default", "break", "continue", "goto", "label", "return",
};

static void print_nodes(Node *n, int indent);
static void print_node(Node *n, int indent);
static void print_type(Type *t);
static void print_expr(Node *n);

static void print_struct_fields(Type *t) {
    if (!t->fields) return; // Anonymous
    printf("{ ");
    for (size_t i = 0; i < vec_len(t->fields); i++) {
        Field *f = vec_get(t->fields, i);
        print_type(f->t);
        if (f->name) printf(" %s", f->name);
        if (t->k == T_STRUCT) printf(" (%" PRIu64 ")", f->offset);
        printf(", ");
    }
    printf("}");
}

static void print_enum_consts(Type *t) {
    if (!t->fields) return; // Anonymous
    printf("{ ");
    for (size_t i = 0; i < vec_len(t->fields); i++) {
        Field *f = vec_get(t->fields, i);
        if (i == 0) { // f->t is same for all enum consts
            printf("(");
            print_type(f->t);
            printf(") ");
        }
        printf("%s = %" PRIu64 ", ", f->name, f->offset);
    }
    printf("}");
}

static void print_type(Type *t) {
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
            Type *arg = vec_get(t->params, i);
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

static void print_expr(Node *n) {
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
    case N_ARR:
        print_type(n->t);
        printf(" { ");
        for (size_t i = 0; i < vec_len(n->elems); i++) {
            Node *elem = vec_get(n->elems, i);
            if (!elem) {
                printf("∅");
            } else {
                print_expr(elem);
            }
            printf(", ");
        }
        printf("}");
        break;
    case N_STRUCT:
        print_type(n->t);
        printf(" { ");
        assert(vec_len(n->elems) == vec_len(n->t->fields));
        for (size_t i = 0; i < vec_len(n->fields); i++) {
            Node *f = vec_get(n->fields, i);
            Field *ft = vec_get(n->t->fields, i);
            printf(".%s = ", ft->name);
            if (!f) {
                printf("∅");
            } else {
                print_expr(f);
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
        assert(n->global->k == N_GLOBAL);
        printf(" &%s", n->global->var_name);
        if (n->offset > 0) {
            printf(" + %" PRIu64, n->offset);
        } else if (n->offset < 0) {
            printf(" - %" PRIu64, -n->offset);
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
            Node *arg = vec_get(n->args, i);
            print_expr(arg);
            printf(", ");
        }
        printf(")");
        break;
    case N_FIELD:
        print_type(n->t);
        printf(" ( %s ", n->k == N_FIELD ? "." : "->");
        print_expr(n->strct);
        printf(" ");
        printf("%s )", n->field_name);
        break;
    case N_TERNARY:
        print_type(n->t);
        printf(" ( ");
        print_expr(n->if_cond);
        printf(" ? ");
        print_expr(n->if_body);
        printf(" : ");
        print_expr(n->if_else);
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

static void print_fn_def(Node *n) {
    assert(n->t->k == T_FN);
    if (n->t->linkage == L_STATIC) {
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
    print_nodes(n->fn_body, 1);
}

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("    ");
    }
}

static void print_node(Node *n, int indent) {
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
        if (n->var->t->linkage == L_STATIC) {
            printf("static ");
        } else if (n->var->t->linkage == L_EXTERN) {
            printf("extern ");
        }
        print_expr(n->var);
        if (n->init) {
            printf(" = ");
            print_expr(n->init);
        }
        printf("\n");
        break;
    case N_IF:
        print_indent(indent);
        while (1) {
            printf("if ");
            print_expr(n->if_cond);
            printf("\n");
            print_nodes(n->if_body, indent + 1);
            if (n->if_else) {
                print_indent(indent);
                printf("else ");
                if (n->if_else->if_cond) { // else if
                    n = n->if_else;
                    continue;
                } else {
                    printf("\n");
                    print_nodes(n->if_else, indent + 1);
                }
            }
            break;
        }
        break;
    case N_WHILE:
        print_indent(indent);
        printf("while ");
        print_expr(n->loop_cond);
        printf("\n");
        print_nodes(n->loop_body, indent + 1);
        break;
    case N_DO_WHILE:
        print_indent(indent);
        printf("do\n");
        print_nodes(n->loop_body, indent + 1);
        print_indent(indent);
        printf("while ");
        print_expr(n->loop_cond);
        printf("\n");
        break;
    case N_FOR:
        print_node(n->for_init, indent);
        print_indent(indent);
        printf("for ");
        print_expr(n->for_cond);
        printf("; ");
        print_expr(n->for_inc);
        printf("\n");
        print_nodes(n->for_body, indent + 1);
        break;
    case N_SWITCH:
        print_indent(indent);
        printf("switch ");
        print_expr(n->switch_cond);
        printf("\n");
        print_nodes(n->switch_body, indent + 1);
        break;
    case N_CASE:
        print_indent(indent - 1);
        printf("case ");
        print_expr(n->case_cond);
        printf(":\n");
        print_node(n->case_body, indent);
        break;
    case N_DEFAULT:
        print_indent(indent - 1);
        printf("default:\n");
        print_node(n->case_body, indent);
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
        print_node(n->label_body, indent);
        break;
    case N_RET:
        print_indent(indent);
        printf("return ");
        if (n->ret_val) {
            print_expr(n->ret_val);
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

static void print_nodes(Node *n, int indent) {
    while (n) {
        print_node(n, indent);
        n = n->next;
    }
}

void print_ast(Node *n) {
    print_nodes(n, 0);
}


// ---- SSA IR Printing -------------------------------------------------------

#define BB_PREFIX ".BB"

static char *IR_OP_NAMES[IR_LAST] = {
    "IMM", "FP", "GLOBAL",
    "FARG", "ALLOC", "LOAD", "STORE", "ELEM",
    "ADD", "SUB", "MUL", "DIV", "MOD",
    "AND", "OR", "XOR", "SHL", "SHR",
    "EQ", "NEQ", "LT", "LE", "GT", "GE",
    "TRUNC", "EXT", "FP2I", "I2FP", "PTR2I", "I2PTR", "BITCAST",
    "PHI", "BR", "CONDBR", "CALL", "CARG", "RET",
};

static void print_ins(IrIns *ins) {
    printf("\t%.4d\t", ins->idx);
    print_type(ins->t);
    printf("\t%s\t", IR_OP_NAMES[ins->op]);
    switch (ins->op) {
    case IR_IMM:    printf("+%llu", ins->imm); break;
    case IR_FP:     printf("+%g", ins->fp); break;
    case IR_GLOBAL: printf("%s", ins->g->label); break;
    case IR_FARG:   printf("%zu", ins->arg_num); break;
    case IR_ALLOC:  print_type(ins->t->ptr); break;
    case IR_STORE:  printf("%.4d -> %.4d", ins->src->idx, ins->dst->idx); break;
    case IR_PHI:
        for (size_t i = 0; i < vec_len(ins->preds); i++) {
            IrBB *pred = vec_get(ins->preds, i);
            IrIns *def = vec_get(ins->defs, i);
            printf("[ " BB_PREFIX "%d -> %.4d ] ", pred->idx, def->idx);
        }
        break;
    case IR_BR: printf(BB_PREFIX "%d", ins->br ? ins->br->idx : -1); break;
    case IR_CONDBR:
        printf("%.4d\t", ins->cond->idx);
        printf(BB_PREFIX "%d\t", ins->true ? ins->true->idx : -1);
        printf(BB_PREFIX "%d", ins->false ? ins->false->idx : -1);
        break;
    default:
        if (ins->l) printf("%.4d", ins->l->idx);
        if (ins->r) printf("\t%.4d", ins->r->idx);
        break;
    }
    printf("\n");
}

static void print_bb(IrBB *bb) {
    printf(BB_PREFIX "%d:\n", bb->idx);
    for (IrIns *ins = bb->head; ins; ins = ins->next) {
        print_ins(ins);
    }
}

static void number_bbs(IrFn *fn) {
    int i = 0;
    for (IrBB *bb = fn->entry; bb; bb = bb->next) {
        bb->idx = i++;
    }
}

static void number_ins(IrFn *fn) {
    int i = 0;
    for (IrBB *bb = fn->entry; bb; bb = bb->next) {
        for (IrIns *ins = bb->head; ins; ins = ins->next) {
            ins->idx = i++;
        }
    }
}

static void print_fn(Global *g) {
    number_bbs(g->fn);
    number_ins(g->fn);
    print_type(g->t);
    printf(" %s:\n", g->label);
    for (IrBB *bb = g->fn->entry; bb; bb = bb->next) {
        print_bb(bb);
    }
}

static void print_global(Global *g) {
    print_type(g->t);
    printf(" %s", g->label);
    if (g->k == K_NONE) { // No init value
        return;
    }
    printf(" = ");
    switch (g->k) {
    case K_INT: printf("%llu", g->i); break;
    case K_FP:  printf("%g", g->f); break;
    case K_STR: printf("\"%s\"", quote_str(g->str, g->len)); break;
    case K_PTR:
        printf("&%s", g->ptr->label);
        if (g->offset > 0) {
            printf(" + %llu", g->offset);
        } else {
            printf(" - %llu", -g->offset);
        }
        break;
    }
    printf("\n");
}

void print_ir(Vec *globals) {
    for (size_t i = 0; i < vec_len(globals); i++) {
        Global *g = vec_get(globals, i);
        if (g->k != K_FN_DEF) {
            print_global(g);
        } else {
            print_fn(g);
        }
    }
}
