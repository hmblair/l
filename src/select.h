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
 * updates *trees so the caller frees the current one). cfg is the same config
 * ctx renders from, passed mutably because the picker toggles -m live.
 * Returns the selected path (caller must free), an empty string when an action
 * completed without a selection (yank: print nothing, exit 0), or NULL if
 * cancelled.
 */
char *select_run(TreeNode ***trees, int tree_count, char *const *dirs,
                 Config *cfg, PrintContext *ctx);

#endif /* L_SELECT_H */
