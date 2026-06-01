#ifndef CWIST_BOARD_TREE_H
#define CWIST_BOARD_TREE_H

#include <cjson/cJSON.h>
#include <stdbool.h>

bool db_board_tree_init(const char *path);
void db_board_tree_close(void);

bool db_board_tree_set_parent(int board_id, int parent_board_id);
bool db_board_tree_remove(int board_id);
bool db_board_tree_promote_children(int parent_board_id);
int db_board_tree_get_parent(int board_id);
cJSON *db_board_tree_get_children(int parent_board_id);
cJSON *db_board_tree_get_roots(void);
cJSON *db_board_tree_get_all(void);

#endif
