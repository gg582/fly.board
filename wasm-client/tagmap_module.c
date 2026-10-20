/**
 * @file tagmap_module.c
 * @brief Emscripten WASM kernel for the /archive interactive tag world map.
 *
 * Uses CWIST v4's <cwist/wasm/typedarray.h> zero-copy view helpers
 * (CWIST_WASM_INSTALL_VIEWS / CWIST_WASM_EXPOSE_F64 / CWIST_WASM_EXPOSE_I32)
 * so the browser reads computed node coordinates, radii, ranks, continent
 * assignments, and 1-hop / 2-hop BFS distances directly from WASM linear
 * memory without JSON serialization overhead.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tag_layout_core.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#include <cwist/wasm/typedarray.h>
#define FB_KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
#define FB_KEEPALIVE
#endif

static fb_tagmap_state_t g_state;

#if defined(__EMSCRIPTEN__)
CWIST_WASM_INSTALL_VIEWS()
CWIST_WASM_EXPOSE_F64(fb_tagmap_x, g_state.node_x, g_state.n_nodes)
CWIST_WASM_EXPOSE_F64(fb_tagmap_y, g_state.node_y, g_state.n_nodes)
CWIST_WASM_EXPOSE_F64(fb_tagmap_r, g_state.node_r, g_state.n_nodes)
CWIST_WASM_EXPOSE_I32(fb_tagmap_rank, g_state.node_rank, g_state.n_nodes)
CWIST_WASM_EXPOSE_I32(fb_tagmap_hop, g_state.node_hop, g_state.n_nodes)
CWIST_WASM_EXPOSE_I32(fb_tagmap_continent, g_state.node_continent, g_state.n_nodes)
CWIST_WASM_EXPOSE_F64(fb_tagmap_cx, g_state.continent_x, g_state.n_continents)
CWIST_WASM_EXPOSE_F64(fb_tagmap_cy, g_state.continent_y, g_state.n_continents)
CWIST_WASM_EXPOSE_I32(fb_tagmap_cboard, g_state.continent_boards, g_state.n_continents)
#endif

FB_KEEPALIVE
int fb_tagmap_init(void) {
#if defined(__EMSCRIPTEN__)
    cwist_wasm_install_views();
#endif
    memset(&g_state, 0, sizeof(g_state));
    return 0;
}

/**
 * Load graph topology and compute layout.
 * @param nodes_triples Flat int32 array of [id, count, board_id] * n_nodes
 * @param n_nodes Number of nodes
 * @param edges_triples Flat int32 array of [a_idx, b_idx, weight] * n_edges
 * @param n_edges Number of edges
 * @param iterations Force simulation iterations (default 120 when <= 0)
 */
FB_KEEPALIVE
int fb_tagmap_layout(const int32_t *nodes_triples, int n_nodes,
                     const int32_t *edges_triples, int n_edges,
                     int iterations) {
    if (n_nodes < 0 || n_edges < 0) return -1;
    if (n_nodes > FB_TAGMAP_MAX_NODES) n_nodes = FB_TAGMAP_MAX_NODES;
    if (n_edges > FB_TAGMAP_MAX_EDGES) n_edges = FB_TAGMAP_MAX_EDGES;

    g_state.n_nodes = n_nodes;
    g_state.n_edges = n_edges;

    for (int i = 0; i < n_nodes; i++) {
        g_state.node_ids[i] = nodes_triples[i * 3 + 0];
        g_state.node_counts[i] = nodes_triples[i * 3 + 1];
        g_state.node_boards[i] = nodes_triples[i * 3 + 2];
    }
    for (int e = 0; e < n_edges; e++) {
        g_state.edge_a[e] = edges_triples[e * 3 + 0];
        g_state.edge_b[e] = edges_triples[e * 3 + 1];
        g_state.edge_w[e] = edges_triples[e * 3 + 2];
    }

    fb_tagmap_compute(&g_state, iterations);
    return n_nodes;
}

FB_KEEPALIVE
int fb_tagmap_bfs(int focus_idx) {
    fb_tagmap_compute_hops(&g_state, focus_idx);
    return 0;
}

FB_KEEPALIVE
int fb_tagmap_hit_test(double sx, double sy,
                       double cam_x, double cam_y, double cam_z,
                       double view_w, double view_h) {
    return fb_tagmap_hit_test_node(&g_state, sx, sy, cam_x, cam_y, cam_z, view_w, view_h);
}
