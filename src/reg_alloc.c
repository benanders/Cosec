
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


// ---- Interference and Coalescing Graphs ------------------------------------

// The interference graph tells us if two regs are live at the same time.
// (reg1, reg2) is an edge in the graph if their live ranges intersect.
static Graph * interference_graph(RegAlloc *a, Vec **live_ranges) {
    // Intersect every pair of regs; iterate the upper half triangle of the
    // matrix since it's symmetric about the leading diagonal
    Graph *g = graph_new(a->num_regs);
    for (int reg1 = 0; reg1 < a->num_regs; reg1++) {
        Vec *range1 = live_ranges[reg1];
        if (!vec_len(range1)) { // Reg not used
            continue;
        }
        add_node(g, reg1);
        for (int reg2 = 0; reg2 < reg1; reg2++) {
            Vec *range2 = live_ranges[reg2];
            if (!vec_len(range2)) {
                continue;
            }
            if (reg1 < a->num_pregs && reg2 < a->num_pregs) {
                continue; // Don't care about preg interference
            }
            add_node(g, reg2);
            if (ranges_intersect(range1, range2)) {
                add_edge(g, reg1, reg2);
                if (a->debug) {
                    print_reg(a, reg1);
                    printf(" interferes with ");
                    print_reg(a, reg2);
                    printf("\n");
                }
            }
        }
    }
    return g;
}

static int is_coalescing_candidate(RegAlloc *a, AsmIns *ins) {
    if (a->reg_group == REG_GROUP_GPR) {
        return ins->op >= X64_MOV && ins->op <= X64_MOVZX && // is mov?
               (ins->l->k == OPR_GPR && ins->r->k == OPR_GPR) && // both regs?
               (ins->l->reg >= LAST_GPR || ins->r->reg >= LAST_GPR); // at least one vreg?
    } else {
        return ins->op >= X64_MOVSS && ins->op <= X64_MOVSD && // is mov?
               (ins->l->k == OPR_XMM && ins->r->k == OPR_XMM) && // both regs?
               (ins->l->reg >= LAST_XMM || ins->r->reg >= LAST_XMM); // at least one vreg?
    }
}

// The coalescing graph tells us if two regs are candidates for coalescing.
// (reg1, reg2) is an edge in the graph if both regs are related by a move
// and their live ranges don't otherwise intersect.
static Graph * coalescing_graph(RegAlloc *a, Vec **live_ranges) {
    Graph *g = graph_new(a->num_regs);
    for (AsmBB *bb = a->fn->entry; bb; bb = bb->next) {
        for (AsmIns *ins = bb->head; ins; ins = ins->next) {
            if (is_coalescing_candidate(a, ins) &&
                    !ranges_intersect(live_ranges[ins->l->reg],
                                      live_ranges[ins->r->reg])) {
                add_node(g, ins->l->reg);
                add_node(g, ins->r->reg);
                add_edge(g, ins->l->reg, ins->r->reg);
            }
        }
    }
    return g;
}


// ---- Graph Colouring -------------------------------------------------------

// Remove one non-move related node of insignificant degree (<num_pregs) from
// the interference graph and push it onto the stack
static int simplify(RegAlloc *a, Graph *ig, Graph *cg, int *stack, int *num_stack) {
    // Find a non-move related node of insignificant degree
    for (int vreg = a->num_pregs; vreg < a->num_regs; vreg++) {
        if (!has_node(ig, vreg)) {
            continue; // The reg doesn't exist
        }
        if (num_edges(cg, vreg) > 0) {
            continue; // The reg is move related
        }
        if (num_edges(ig, vreg) >= a->num_pregs) {
            continue; // The reg is of significant degree (>=num_pregs edges)
        }
        stack[(*num_stack)++] = vreg; // Add to stack
        remove_node(ig, vreg); // Remove from graphs
        remove_node(cg, vreg);
        if (a->debug) {
            printf("simplifying ");
            print_reg(a, vreg);
            printf("\n");
        }
        return 1;
    }
    return 0; // No nodes to simplify
}

// Brigg's criteria: nodes a and b can be coalesced if the resulting node ab has
// fewer than 'num_pregs' nodes of significant degree
// Basically, calculate the degree of every (unique) neighbour of a and b and
// count the number of these neighbours that have significant degree
static int briggs_criteria(RegAlloc *a, Graph *ig, int reg1, int reg2) {
    int count = 0;
    int seen[a->num_regs];
    memset(seen, 0, sizeof(int) * a->num_regs);
    for (int neighbour = 0; neighbour < a->num_regs; neighbour++) {
        if ((has_edge(ig, reg1, neighbour) ||      // Neighbour of reg1?
                 has_edge(ig, reg2, neighbour)) && // or of reg2?
                 !seen[neighbour]) {               // Unique?
            seen[neighbour] = 1;
            if (num_edges(ig, neighbour) >= a->num_pregs) { // Significant?
                count++;
            }
        }
    }
    return count;
}

// Coalesce one move-related pair of nodes using the Brigg's criteria
static int coalesce(RegAlloc *a, Graph *ig, Graph *cg, int *coalesce_map) {
    // Find two move-related nodes
    for (int reg1 = 0; reg1 < a->num_regs; reg1++) {
        if (!has_node(cg, reg1)) {
            continue; // Node isn't move-related to anything
        }
        for (int reg2 = 0; reg2 < reg1; reg2++) { // Only iterate upper half
            if (!has_node(cg, reg1)) {
                continue; // Node isn't move-related to anything
            }
            if (!has_edge(cg, reg1, reg2)) {
                continue; // Nodes aren't move-related
            }
            if (briggs_criteria(a, ig, reg1, reg2) >= a->num_pregs) {
                continue; // Not profitable to coalesce
            }
            // Coalesce one node into the other; if one of the regs is a preg,
            // then make sure we coalesce the vreg into it
            int to_coalesce = (reg1 < a->num_pregs) ? reg2 : reg1;
            int target = (to_coalesce == reg1) ? reg2 : reg1;
            copy_edges(ig, to_coalesce, target);
            copy_edges(cg, to_coalesce, target);
            remove_node(ig, to_coalesce);
            remove_node(cg, to_coalesce);
            coalesce_map[to_coalesce] = target;
            if (a->debug) {
                printf("coalescing ");
                print_reg(a, to_coalesce);
                printf(" into ");
                print_reg(a, target);
                printf("\n");
            }
            return 1;
        }
    }
    return 0; // No nodes to coalesce
}

// Look for a move-related node of insignificant degree that we can freeze the
// moves for (give up hope of coalescing them)
static int freeze(RegAlloc *a, Graph *ig, Graph *cg) {
    // Find a move related node of insignificant degree
    for (int vreg = a->num_pregs; vreg < a->num_regs; vreg++) {
        if (!has_node(ig, vreg)) {
            continue; // The reg doesn't exist
        }
        if (num_edges(cg, vreg) == 0) {
            continue; // The reg is NOT move related
        }
        if (num_edges(ig, vreg) >= a->num_pregs) {
            continue; // The reg is of significant degree (>=num_pregs edges)
        }
        remove_node(cg, vreg); // Remove from coalesce
        if (a->debug) {
            printf("freezing ");
            print_reg(a, vreg);
            printf("\n");
        }
        return 1;
    }
    return 0; // No nodes to freeze
}

// Look for a significant degree node to remove from the interference graph and
// push on to the stack as a potential spill (we won't know for sure until we
// select registers though)
static int spill(RegAlloc *a, Graph *ig, Graph *cg, int *stack, int *num_stack) {
    // Find a node of significant degree
    for (int vreg = a->num_pregs; vreg < a->num_regs; vreg++) {
        if (!has_node(ig, vreg)) {
            continue; // The reg doesn't exist
        }
        if (num_edges(ig, vreg) < a->num_pregs) {
            continue; // This reg isn't of significant degree
        }
        stack[(*num_stack)++] = vreg; // Add to the stack
        remove_node(ig, vreg); // Remove from graphs
        remove_node(cg, vreg);
        if (a->debug) {
            printf("spilling ");
            print_reg(a, vreg);
            printf("\n");
        }
        return 1;
    }
    return 0; // No nodes to spill
}

static void select(RegAlloc *a, Graph *ig, int *stack, int num_stack,
                   int *reg_map, int *coalesce_map) {
    // For each of the coalesced regs, we need to copy across their
    // interferences in the original interference graph to the target reg they
    // were coalesced into
    for (int vreg = a->num_pregs; vreg < a->num_regs; vreg++) {
        int target = coalesce_map[vreg];
        if (target) { // 'vreg' was coalesced into 'target'
            copy_edges(ig, vreg, target);
        }
    }

    // Work our way down the stack allocating regs
    while (num_stack) {
        int vreg = stack[--num_stack]; // Pop from the stack

        // Find the first preg not interfering with 'vreg'
        int preg = 1; // 0 is R_NONE
        while (has_edge(ig, vreg, preg) && preg < a->num_pregs) {
            preg++;
        }
        if (preg >= a->num_pregs) { // All pregs interfere -> spill
            assert(0); // TODO: spilling
        }
        reg_map[vreg] = preg;

        // Copy the regs that interfere with this vreg to the allocated preg
        copy_edges(ig, vreg, preg);
        if (a->debug) {
            printf("allocating ");
            print_reg(a, vreg);
            printf(" to ");
            print_reg(a, preg);
            printf("\n");
        }
    }
}

static void color_graph(RegAlloc *a, Graph *ig, Graph *cg,
                        int *reg_map, int *coalesce_map) {
    int stack[a->num_regs]; // Defines the order vregs are allocated
    int num_stack = 0;
    Graph *ig2 = graph_copy(ig); // Copy that we can modify

simplify_and_coalesce:
    // Repeat simplification and coalescing until only significant-degree or
    // move-related nodes remain (do each at least once and keep going until
    // we can't anymore)
    if (simplify(a, ig2, cg, stack, &num_stack)) {
        while (simplify(a, ig2, cg, stack, &num_stack));
    } else {
        goto freeze_and_spill;
    }
    if (coalesce(a, ig2, cg, coalesce_map)) {
        while (coalesce(a, ig2, cg, coalesce_map));
        goto simplify_and_coalesce;
    } else {
        goto freeze_and_spill;
    }

freeze_and_spill:
    // Freeze a move-related node we can't simplify and try again. Keep freezing
    // until only significant degree nodes remain
    if (freeze(a, ig2, cg)) {
        goto simplify_and_coalesce;
    }
    // Only significant degree nodes remain -> spill one. Keep spilling until
    // we've removed all nodes from the interference graph
    if (spill(a, ig2, cg, stack, &num_stack)) {
        goto simplify_and_coalesce;
    }

    // All vregs dealt ith -> colour regs in the order they pop off the stack
    select(a, ig, stack, num_stack, reg_map, coalesce_map);
}


// ---- Register Replacement After Allocation ---------------------------------

static int map_vreg(RegAlloc *a, int reg, int *reg_map, int *coalesce_map) {
    if (reg < a->num_pregs) {
        return reg; // Not a vreg
    }
    while (reg >= a->num_pregs && coalesce_map[reg]) {
        reg = coalesce_map[reg]; // Find end of coalescing chain
    }
    if (reg >= a->num_pregs) { // If not coalesced into a preg
        reg = reg_map[reg]; // Find allocated preg
    }
    assert(reg > R_NONE);
    return reg;
}

static void replace_vregs_in_op(RegAlloc *a, AsmOpr *op, int *reg_map, int *coalesce_map) {
    if (!op) return;
    switch (op->k) {
    case OPR_GPR: case OPR_XMM:
        op->reg = map_vreg(a, op->reg, reg_map, coalesce_map);
        break;
    case OPR_MEM:
        op->base = map_vreg(a, op->base, reg_map, coalesce_map);
        op->idx = map_vreg(a, op->idx, reg_map, coalesce_map);
        break;
    }
}

static int is_redundant_mov(RegAlloc *a, AsmIns *ins) {
    if (a->reg_group == REG_GROUP_GPR) {
        return ins->op >= X64_MOV && ins->op <= X64_MOVZX && // is mov?
               ins->l->k == OPR_GPR && ins->r->k == OPR_GPR && // both regs?
               ins->l->reg == ins->r->reg && // same reg?
               !((ins->op == X64_MOVSX || ins->op == X64_MOVZX) &&
                 ins->l->size > ins->r->size); // Don't remove (e.g.) movsx rax, ax
    } else { // SSE
        return ins->op >= X64_MOVSS && ins->op <= X64_MOVSD && // is mov?
               ins->l->k == OPR_XMM && ins->r->k == OPR_XMM && // both regs?
               ins->l->reg == ins->r->reg; // same reg?
    }
}

static void replace_vregs(RegAlloc *a, int *reg_map, int *coalesce_map) {
    // Run through the code and replace each vreg with its allocated preg
    for (AsmBB *bb = a->fn->entry; bb; bb = bb->next) {
        for (AsmIns *ins = bb->head; ins; ins = ins->next) {
            replace_vregs_in_op(a, ins->l, reg_map, coalesce_map);
            replace_vregs_in_op(a, ins->r, reg_map, coalesce_map);
            if (is_redundant_mov(a, ins)) {
                delete_asm(ins); // Remove redundant mov
            }
        }
    }
}


// ---- Register Allocation ---------------------------------------------------

static void alloc_reg_group(RegAlloc *a) {
    if (a->num_regs == a->num_pregs) {
        return; // No vregs to allocate
    }
    Vec **live_ranges = live_ranges_for_fn(a);
    if (a->debug) {
        print_live_ranges(a, live_ranges);
    }
    Graph *ig = interference_graph(a, live_ranges);
    Graph *cg = coalescing_graph(a, live_ranges);
    int reg_map[a->num_regs]; // Maps vreg -> allocated preg
    memset(reg_map, 0, a->num_regs * sizeof(int));
    int coalesce_map[a->num_regs]; // Maps vreg -> coalesced vreg or preg
    memset(coalesce_map, 0, a->num_regs * sizeof(int));
    color_graph(a, ig, cg, reg_map, coalesce_map);
    replace_vregs(a, reg_map, coalesce_map);
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
