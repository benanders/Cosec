
#include "debug.h"

static char * AST_NAMES[N_LAST] = {
    "imm", "fp", "str", "var",
    "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>",
    "==", "!=", "<", "<=", ">", ">=", "&&", "||",
    "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
    ",", "?",
    "-", "~", "!", "++", "--", "++", "--", "*", "&", "conv", "sizeof",
    "idx", "call", ".", "->",
    "fn def", "typedef", "decl", "if", "while", "do while", "for", "switch",
    "case", "default", "break", "continue", "goto", "label", "return",
};

static void print_nodes(Node *n, int indent);
static void print_node(Node *n, int indent);

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
    case T_PTR:
        print_type(t->ptr);
        printf("*");
        break;
    case T_ARR:
        print_type(t->ptr);
        printf("[%llu]", t->len);
        break;
    case T_FN:
        print_type(t->ret);
        printf("(");
        for (int i = 0; i < vec_len(t->params); i++) {
            Type *arg = vec_get(t->params, i);
            print_type(arg);
            if (i < vec_len(t->params) - 1) {
                printf(", ");
            }
        }
        printf(")");
        break;
    }
}

static void print_expr(Node *n) {
    switch (n->k) {
        // Operands
    case N_IMM:
        print_type(n->t);
        if (n->t->k == T_CHAR && n->imm < 256) {
            printf(" '%c'", (char) n->imm);
        } else {
            printf(" %llu", n->imm);
        }
        break;
    case N_FP:
        print_type(n->t);
        printf(" %g", n->fp);
        break;
    case N_STR:
        print_type(n->t);
        printf(" \"%s\"", quote_str(n->str, n->len));
        break;
    case N_VAR:
        print_type(n->t);
        printf(" %s", n->var_name);
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
        for (int i = 0; i < vec_len(n->args); i++) {
            Node *arg = vec_get(n->args, i);
            print_expr(arg);
            printf(", ");
        }
        printf(")");
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
    for (int i = 0; i < vec_len(n->param_names); i++) {
        char *name = vec_get(n->param_names, i);
        if (name) {
            printf("%s", name);
        }
        if (i < vec_len(n->param_names) - 1) {
            printf(", ");
        }
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
        if (n->val) {
            print_expr(n->val);
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
    printf("\n");
}
