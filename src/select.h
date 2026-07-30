/*
 * select.h - Interactive file selection mode
 */

#ifndef L_SELECT_H
#define L_SELECT_H

#include "view.h"
#include "render.h"

/*
 * Run interactive selection mode over the built forest. dirs are the original
 * command-line arguments: the reload key rebuilds the forest from them (and
 * updates *trees so the caller frees the current one). Returns the selected
 * path (caller must free), or NULL if cancelled.
 */
char *select_run(TreeNode ***trees, int tree_count, char *const *dirs,
                 PrintContext *ctx);

#endif /* L_SELECT_H */
