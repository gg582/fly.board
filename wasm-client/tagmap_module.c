/**
 * @file tagmap_module.c
 * @brief Emscripten WASM kernel for the /archive interactive tag world map.
 *
 * Uses CWIST v4's <cwist/wasm/typedarray.h> zero-copy view helpers
 * (CWIST_WASM_INSTALL_VIEWS / CWIST_WASM_EXPOSE_F64 / CWIST_WASM_EXPOSE_I32)
 * so the browser reads computed node coordinates, radii, ranks, continent
 * assignments, and 1-hop / 2-hop BFS distances directly from WASM linear
 * memory without JSON serialization overhead.
 *
 * Native reference protocol (stdin -> stdout) for test_tagmap_diff.py:
 *   in:  i32le n_nodes, i32le n_edges, i32le focus_idx, i32le iterations,
 *        n_nodes * 3 x i32le (id, count, board_id),
 *        n_edges * 3 x i32le (a_idx, b_idx, weight)
 *   out: n_nodes x f64le x, n_nodes x f64le y, n_nodes x f64le r,
 *        n_nodes x i32le rank, n_nodes x i32le hop
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

#ifndef __EMSCRIPTEN__
int main(void) {
    int32_t hdr[4];
    if (fread(hdr, sizeof(int32_t), 4, stdin) != 4) return 1;
    int n_nodes = hdr[0];
    int n_edges = hdr[1];
    int focus_idx = hdr[2];
    int iterations = hdr[3];
    if (n_nodes < 0 || n_nodes > FB_TAGMAP_MAX_NODES ||
        n_edges < 0 || n_edges > FB_TAGMAP_MAX_EDGES) {
        return 1;
    }

    int32_t *nt = (int32_t *)malloc((size_t)(n_nodes > 0 ? n_nodes : 1) * 3 * sizeof(int32_t));
    int32_t *et = (int32_t *)malloc((size_t)(n_edges > 0 ? n_edges : 1) * 3 * sizeof(int32_t));
    if (!nt || !et) {
        free(nt);
        free(et);
        return 1;
    }
    if (n_nodes > 0 && fread(nt, sizeof(int32_t), (size_t)n_nodes * 3, stdin) != (size_t)n_nodes * 3) {
        free(nt);
        free(et);
        return 1;
    }
    if (n_edges > 0 && fread(et, sizeof(int32_t), (size_t)n_edges * 3, stdin) != (size_t)n_edges * 3) {
        free(nt);
        free(et);
        return 1;
    }

    fb_tagmap_init();
    fb_tagmap_layout(nt, n_nodes, et, n_edges, iterations);
    fb_tagmap_bfs(focus_idx);

    fwrite(g_state.node_x, sizeof(double), (size_t)n_nodes, stdout);
    fwrite(g_state.node_y, sizeof(double), (size_t)n_nodes, stdout);
    fwrite(g_state.node_r, sizeof(double), (size_t)n_nodes, stdout);
    fwrite(g_state.node_rank, sizeof(int32_t), (size_t)n_nodes, stdout);
    fwrite(g_state.node_hop, sizeof(int32_t), (size_t)n_nodes, stdout);

    free(nt);
    free(et);
    return 0;
}
#endif
