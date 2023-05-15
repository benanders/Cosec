
#include <stdlib.h>
#include <string.h>

#include "reg_alloc.h"
#include "encode.h"

// The register allocator is based on the classic graph colouring algorithm
// presented in Modern Parser Implementation in C, Andrew W. Appel, Chapter 11.
//
// Additional resources:
// * A set of slides on the graph colouring algorithm:
//   http://web.cecs.pdx.edu/~mperkows/temp/register-allocation.pdf
// * An article on the graph colouring algorithm:
//   https://www.lighterra.com/papers/graphcoloring/
// * A set of slides on liveness analysis:
//   https://proglang.informatik.uni-freiburg.de/teaching/compilerbau/2016ws/10-liveness.pdf
// * Useful practical information on implementing liveness analysis (including
//   the worklist-based algorithm used in 'Liveness.c'):
//   https://engineering.purdue.edu/~milind/ece573/2015fall/project/step7/step7.html
// * Conceptual overview of coalescing:
//   https://www.cs.cmu.edu/afs/cs/academic/class/15745-s16/www/lectures/L23-Register-Coalescing.pdf

enum {
    REG_GROUP_GPR,
    REG_GROUP_SSE,
};

typedef struct RegAlloc { // Allocator for a register group (GPR or SSE)
    AsmFn *fn;
    size_t num_ins;
    int reg_group;
    int num_regs /* pregs + vregs */, num_pregs;
    int debug;
} RegAlloc;


// ---- Control Flow Graph Analysis -------------------------------------------

static void add_pair(AsmBB *before, AsmBB *after) {
    vec_push(before->succ, after);
    vec_push(after->pred, before);
}

// Populates predecessor and successor basic blocks for each basic block in the
// function
static void cfg_analysis(AsmFn *fn) {
    for (AsmBB *bb = fn->entry; bb; bb = bb->next) { // Allocate pred and succ
        bb->pred = vec_new();
        bb->succ = vec_new();
    }
    for (AsmBB *bb = fn->entry; bb; bb = bb->next) {
        AsmIns *last = bb->last;
        if (last->op >= X64_JE && last->op <= X64_JAE) {
            add_pair(bb, last->bb);
        }
        if (last->op == X64_JMP) {
            add_pair(bb, last->bb);
        } else if (bb->next) {
            add_pair(bb, bb->next);
        }
    }
}


// ---- Live Range Intervals --------------------------------------------------

typedef struct {
    size_t start, end;
} Interval;

static Interval * new_interval(size_t start, size_t end) {
    Interval *in = malloc(sizeof(Interval));
    in->start = start;
    in->end = end;
    return in;
}

static int intervals_intersect(Interval *a, Interval *b) {
    return !((a->end - 1) < b->start || a->start > (b->end - 1));
}

static int ranges_intersect(Vec *a, Vec *b) {
    for (size_t i = 0; i < vec_len(a); i++) {
        Interval *aa = vec_get(a, i);
        for (size_t j = 0; j < vec_len(b); j++) {
            Interval *bb = vec_get(b, i);
            if (intervals_intersect(aa, bb)) {
                return 1;
            }
        }
    }
    return 0;
}

static void mark_idx_live(Vec *live_range, size_t idx) {
    for (size_t i = 0; i < vec_len(live_range); i++) {
        Interval *in = vec_get(live_range, i);
        if (idx >= in->start && idx <= in->end) {
            return; // Already inside an interval
        } else if (idx == in->start - 1) {
            in->start--; // Right before an existing interval
            return;
        } else if (idx == in->end + 1) {
            in->end++; // Right after an existing interval
            return;
        }
    }
    Interval *in = new_interval(idx, idx);
    vec_push(live_range, in);
}


// ---- Liveness Analysis -----------------------------------------------------

// Instructions that define their left operand
static int X64_DEFS_LEFT[X64_LAST] = {
    [X64_MOV] = 1, [X64_MOVSX] = 1, [X64_MOVZX] = 1, [X64_MOVSS] = 1,
    [X64_MOVSD] = 1, [X64_LEA] = 1,
    [X64_ADD] = 1, [X64_SUB] = 1, [X64_IMUL] = 1, [X64_AND] = 1, [X64_OR] = 1,
    [X64_XOR] = 1, [X64_SHL] = 1, [X64_SHR] = 1, [X64_SAR] = 1,
    [X64_ADDSS] = 1, [X64_ADDSD] = 1, [X64_SUBSS] = 1, [X64_SUBSD] = 1,
    [X64_MULSS] = 1, [X64_MULSD] = 1, [X64_DIVSS] = 1, [X64_DIVSD] = 1,
    [X64_SETE] = 1, [X64_SETNE] = 1, [X64_SETL] = 1, [X64_SETLE] = 1,
    [X64_SETG] = 1, [X64_SETGE] = 1, [X64_SETB] = 1, [X64_SETBE] = 1,
    [X64_SETA] = 1, [X64_SETAE] = 1,
    [X64_CVTSS2SD] = 1, [X64_CVTSD2SS] = 1, [X64_CVTSI2SS] = 1,
    [X64_CVTSI2SD] = 1, [X64_CVTTSS2SI] = 1, [X64_CVTTSD2SI] = 1,
    [X64_POP] = 1,
};

// Some instructions clobber GPRs that aren't explicitly used as arguments
// (e.g., 'call' clobbers the caller-saved registers)
static int CLOBBERS[X64_LAST][LAST_GPR] = {
    [X64_CWD]  = { [RDX] = 1, },
    [X64_CDQ]  = { [RDX] = 1, },
    [X64_CQO]  = { [RDX] = 1, },
    [X64_IDIV] = { [RAX] = 1, [RDX] = 1, },
    [X64_DIV]  = { [RAX] = 1, [RDX] = 1, },
    [X64_CALL] = { [RAX] = 1, [RDI] = 1, [RSI] = 1, [RDX] = 1, [RCX] = 1,
                   [R8] = 1, [R9] = 1, [R10] = 1, [R11] = 1, },
};

static void mark_opr_live(RegAlloc *a, AsmOpr *opr, int *live) {
    if (!opr) return;
    if (a->reg_group == REG_GROUP_GPR) {
        if (opr->k == OPR_GPR) {
            live[opr->reg] = 1;
        } else if (opr->k == OPR_MEM) {
            if (opr->base) live[opr->base] = 1;
            if (opr->idx)  live[opr->idx] = 1;
        }
    } else {
        if (opr->k == OPR_XMM) {
            live[opr->reg] = 1;
        }
    }
}

static void mark_live(RegAlloc *a, AsmIns *ins, int *live) {
    mark_opr_live(a, ins->l, live); // Mark regs used in ins args as live
    mark_opr_live(a, ins->r, live);
    if (a->reg_group == REG_GROUP_GPR) {
        // Mark rsp, rbp live for every instruction
        live[RSP] = 1;
        live[RBP] = 1;

        // Some instructions clobber pregs not explicitly used as arguments
        for (int preg = 0; preg < a->num_pregs; preg++) {
            if (CLOBBERS[ins->op][preg]) {
                live[preg] = 1;
            }
        }
    }
}

static int live_ranges_for_bb(RegAlloc *a, AsmBB *bb, Vec **live_ranges) {
    // Tracks which regs are live at the current program point
    int *live = calloc(a->num_regs, sizeof(int));

    // Live-out is union of live-in over all successors
    for (size_t i = 0; i < vec_len(bb->succ); i++) {
        AsmBB *succ = vec_get(bb->succ, i);
        for (int reg = 0; reg < a->num_regs; reg++) {
            live[reg] |= succ->live_in[reg];
        }
    }

    // Mark everything live-out as live for the program point BEYOND the last
    // instruction in the BB
    for (int reg = 0; reg < a->num_regs; reg++) {
        if (live[reg]) {
            mark_idx_live(live_ranges[reg], bb->last->n + 1);
        }
    }

    // Instructions in reverse
    for (AsmIns *ins = bb->last; ins; ins = ins->prev) {
        mark_live(a, ins, live);

        // Copy everything that's live here into 'live_ranges'
        for (int reg = 0; reg < a->num_regs; reg++) {
            if (live[reg]) {
                mark_idx_live(live_ranges[reg], ins->n);
            }
        }

        // Regs defined are no longer live before the program point
        if (ins->l && ins->l->k == OPR_GPR && X64_DEFS_LEFT[ins->op]) {
            live[ins->l->reg] = 0;
        }

        // All pregs are live for only ONE instruction
        for (int preg = 0; preg < a->num_pregs; preg++) {
            live[preg] = 0;
        }
    }

    // Everything left over is live-in for the BB
    int changed = 0;
    for (int reg = 0; reg < a->num_regs; reg++) {
        if (live[reg]) {
            changed |= !bb->live_in[reg];
            bb->live_in[reg] = 1;
        }
    }
    return changed; // Return 1 if live-in for the BB was changed
}

static Vec ** live_ranges_for_fn(RegAlloc *a) {
    Vec **live_ranges = malloc(sizeof(Vec *) * a->num_regs);
    for (int reg = 0; reg < a->num_regs; reg++) { // Alloc live ranges
        live_ranges[reg] = vec_new();
    }
    for (AsmBB *bb = a->fn->entry; bb; bb = bb->next) { // Alloc live-in for BBs
        bb->live_in = calloc(a->num_regs, sizeof(int));
    }

    Vec *worklist = vec_new(); // of 'AsmBB *'
    for (AsmBB *bb = a->fn->entry; bb; bb = bb->next) { // Add all BBs
        vec_push(worklist, bb);
    }

    while (vec_len(worklist) > 0) {
        AsmBB *bb = vec_pop(worklist); // Pop BBs in reverse order
        if (live_ranges_for_bb(a, bb, live_ranges)) { // live-in changed?
            for (size_t i = 0; i < vec_len(bb->pred); i++) { // Add predecessors
                AsmBB *pred = vec_get(bb->pred, i);
                vec_push(worklist, pred);
            }
        }
    }
    return live_ranges;
}


// ---- Register Group Allocation ---------------------------------------------

static void print_reg(RegAlloc *a, int reg) {
    if (a->reg_group == REG_GROUP_GPR) {
        encode_gpr(stdout, reg, R64);
    } else {
        encode_xmm(stdout, reg);
    }
}

static void print_live_range(Vec *live_range) {
    for (int i = (int) vec_len(live_range) - 1; i >= 0; i--) { // Reverse order
        Interval *in = vec_get(live_range, i);
        printf("[%zu, %zu) ", in->start, in->end);
    }
}

static void print_live_ranges(RegAlloc *a, Vec **live_ranges) {
    for (int reg = 0; reg < a->num_regs; reg++) {
        Vec *range = live_ranges[reg];
        if (!vec_len(range)) {
            continue; // Reg not used (no live range)
        }
        print_reg(a, reg);
        printf(" live at: ");
        print_live_range(range);
        printf("\n");
    }
}

static void alloc_reg_group(RegAlloc *a) {
    if (a->num_regs == a->num_pregs) {
        return; // No vregs to allocate
    }
    Vec **live_ranges = live_ranges_for_fn(a);
    if (a->debug) {
        print_live_ranges(a, live_ranges);
    }
//    Graph *ig = interference_graph(a, live_ranges);
//    Graph *cg = coalescing_graph(a, live_ranges);
//    int reg_map[a->num_regs]; // Maps vreg -> allocated preg
//    memset(reg_map, 0, a->num_regs * sizeof(int));
//    int coalesce_map[a->num_regs]; // Maps vreg -> coalesced vreg or preg
//    memset(coalesce_map, 0, a->num_regs * sizeof(int));
//    color_graph(a, ig, cg, reg_map, coalesce_map);
//    replace_vregs(a, reg_map, coalesce_map);
}


// ---- Register Allocation ---------------------------------------------------

//static int gpr_is_mov(AsmIns *ins) {
//    return ins->op >= X64_MOV && ins->op <= X64_MOVZX;
//}
//
//static int sse_is_mov(AsmIns *ins) {
//    return ins->op >= X64_MOVSS && ins->op <= X64_MOVSD;
//}
//
//static int gpr_is_redundant_mov(AsmIns *mov) {
//    return mov->l->k == OPR_GPR && mov->r->k == OPR_GPR && // both regs?
//           mov->l->reg == mov->r->reg && // same reg?
//           !((mov->op == X64_MOVSX || mov->op == X64_MOVZX) &&
//             mov->l->size > mov->r->size); // Don't remove (e.g.) movsx rax, ax
//}
//
//static int sse_is_redundant_mov(AsmIns *mov) {
//    return mov->l->k == OPR_XMM && mov->r->k == OPR_XMM && // both regs?
//           mov->l->reg == mov->r->reg; // same reg?
//}
//
//static int gpr_is_coalescing_candidate(AsmIns *mov) {
//    return (mov->l->k == OPR_GPR && mov->r->k == OPR_GPR) && // both regs?
//           (mov->l->reg >= LAST_GPR || mov->r->reg >= LAST_GPR); // at least one vreg?
//}
//
//static int sse_is_coalescing_candidate(AsmIns *mov) {
//    return (mov->l->k == OPR_XMM && mov->r->k == OPR_XMM) && // both regs?
//           (mov->l->reg >= LAST_XMM || mov->r->reg >= LAST_XMM); // at least one vreg?
//}

static RegAlloc * new_reg_alloc(AsmFn *fn, int reg_group, int debug) {
    RegAlloc *a = malloc(sizeof(RegAlloc));
    a->fn = fn;
    a->num_ins = fn->last->last->n + 1;
    a->reg_group = reg_group;
    a->num_regs = fn->num_gprs;
    a->num_pregs = LAST_GPR;
    a->debug = debug;
    return a;
}

static void number_ins(AsmFn *fn) {
    size_t i = 0;
    for (AsmBB *bb = fn->entry; bb; bb = bb->next) {
        for (AsmIns *ins = bb->head; ins; ins = ins->next) {
            ins->n = i++;
        }
        i++; // Extra program point at END of a BB for vregs that are live-out
    }
}

static void alloc_fn(AsmFn *fn, int debug) {
    number_ins(fn);
    cfg_analysis(fn);
    RegAlloc *gpr = new_reg_alloc(fn, REG_GROUP_GPR, debug);
    alloc_reg_group(gpr);
    RegAlloc *sse = new_reg_alloc(fn, REG_GROUP_SSE, debug);
    alloc_reg_group(sse);
}

void reg_alloc(Vec *globals, int debug) {
    for (size_t i = 0; i < vec_len(globals); i++) {
        Global *g = vec_get(globals, i);
        if (g->asm_fn) {
            if (debug) printf("Register allocation for '%s':\n", g->label);
            alloc_fn(g->asm_fn, debug);
            if (debug) printf("\n");
        }
    }
}
