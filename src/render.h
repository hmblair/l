/*
 * render.h - Rendering: row printing, the view renderer, and the summary
 * card. Renderers read FileEntry fields and the View; they perform no
 * filesystem or git queries at draw time.
 */

#ifndef L_RENDER_H
#define L_RENDER_H

#include "common.h"
#include "icons.h"
#include "tree.h"
#include "format.h"
#include "config.h"
#include "view.h"

/* ============================================================================
 * Print Context
 * ============================================================================ */

typedef struct {
    GitCache *git;
    const Icons *icons;
    const FileTypes *filetypes;
    const Shebangs *shebangs;
    const Config *cfg;
    Column *columns;
    int *continuation;
    int diff_add_width;
    int diff_del_width;
    const char *line_prefix;
    int selected;
    int term_width;
} PrintContext;

/* ============================================================================
 * Printing Functions
 * ============================================================================ */

void print_entry(const FileEntry *fe, int depth, int was_expanded,
                 const PrintContext *ctx);
void render_view(const View *view, PrintContext *ctx);

/* Summary mode: summary_prepare computes what the card shows (type stats,
 * git info); print_summary draws it. */
void summary_prepare(TreeNode *node, PrintContext *ctx);
void print_summary(TreeNode *node, PrintContext *ctx);

/* ============================================================================
 * Git Status Indicator
 * ============================================================================ */

const char *git_indicator_from_flags(unsigned flags, const Icons *icons,
                                     const Config *cfg);

/* ============================================================================
 * Terminal Helpers
 * ============================================================================ */

int get_terminal_width(void);

/* Helper macros */
#define CLR(cfg, c) ((cfg)->disp.is_tty ? (c) : "")
#define RST(cfg)    ((cfg)->disp.is_tty ? COLOR_RESET : "")

#endif /* L_RENDER_H */
