/*
 * select.h - Interactive file selection mode
 */

#ifndef L_SELECT_H
#define L_SELECT_H

#include "ui.h"

/*
 * Run interactive selection mode on a tree.
 * Returns the selected path (caller must free), or NULL if cancelled.
 */
char *select_run(TreeNode **trees, int tree_count, PrintContext *ctx);

#endif /* L_SELECT_H */
