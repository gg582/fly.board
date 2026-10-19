/**
 * @file tag_layout_core.h
 * @brief Deterministic continent-seeded force-directed tag graph layout and
 *        1-hop / 2-hop neighborhood BFS kernel.
 *
 * Shared by the Emscripten browser module (wasm-client/tagmap_module.c,
 * exposing zero-copy TypedArray views via CWIST v4's <cwist/wasm/typedarray.h>)
 * and the native reference driver used by wasm-client/test_tagmap_diff.py.
 */

#ifndef FLYBOARD_TAG_LAYOUT_CORE_H
#define FLYBOARD_TAG_LAYOUT_CORE_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define FB_TAGMAP_MAX_NODES 1024
#define FB_TAGMAP_MAX_EDGES 8192
#define FB_TAGMAP_MAX_BOARDS 128

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int n_nodes;
    int n_edges;
    int32_t node_ids[FB_TAGMAP_MAX_NODES];
    int32_t node_counts[FB_TAGMAP_MAX_NODES];
    int32_t node_boards[FB_TAGMAP_MAX_NODES];
    int32_t node_continent[FB_TAGMAP_MAX_NODES];
    int32_t edge_a[FB_TAGMAP_MAX_EDGES]; /* node index a (0 .. n_nodes-1) */
    int32_t edge_b[FB_TAGMAP_MAX_EDGES]; /* node index b (0 .. n_nodes-1) */
    int32_t edge_w[FB_TAGMAP_MAX_EDGES]; /* co-occurrence weight */

    double node_x[FB_TAGMAP_MAX_NODES];
    double node_y[FB_TAGMAP_MAX_NODES];
    double node_r[FB_TAGMAP_MAX_NODES];
    int32_t node_rank[FB_TAGMAP_MAX_NODES];
    int32_t node_hop[FB_TAGMAP_MAX_NODES]; /* 0=focus, 1=1-hop, 2=2-hop, -1=other */

    /* Continent anchor centers for label rendering: [cx, cy, board_id] per continent */
    int n_continents;
    int32_t continent_boards[FB_TAGMAP_MAX_BOARDS];
    double continent_x[FB_TAGMAP_MAX_BOARDS];
    double continent_y[FB_TAGMAP_MAX_BOARDS];
} fb_tagmap_state_t;

static inline int fb_tagmap_find_node_by_id(const fb_tagmap_state_t *st, int32_t id) {
    for (int i = 0; i < st->n_nodes; i++) {
        if (st->node_ids[i] == id) return i;
    }
    return -1;
}

static inline void fb_tagmap_compute(fb_tagmap_state_t *st, int iterations) {
    int n = st->n_nodes;
    if (n <= 0) return;
    if (n > FB_TAGMAP_MAX_NODES) n = FB_TAGMAP_MAX_NODES;
    if (iterations <= 0) iterations = 120;

    /* 1. Compute degree per node and identify distinct boards (continents). */
    int32_t deg[FB_TAGMAP_MAX_NODES];
    memset(deg, 0, sizeof(int32_t) * (size_t)n);
    for (int e = 0; e < st->n_edges; e++) {
        int a = st->edge_a[e];
        int b = st->edge_b[e];
        int w = st->edge_w[e] > 0 ? st->edge_w[e] : 1;
        if (a >= 0 && a < n && b >= 0 && b < n && a != b) {
            deg[a] += w;
            deg[b] += w;
        }
    }

    int32_t b_ids[FB_TAGMAP_MAX_BOARDS];
    int32_t b_weights[FB_TAGMAP_MAX_BOARDS];
    int nb = 0;
    for (int i = 0; i < n; i++) {
        int32_t bid = st->node_boards[i];
        int found = -1;
        for (int k = 0; k < nb; k++) {
            if (b_ids[k] == bid) {
                found = k;
                break;
            }
        }
        if (found < 0 && nb < FB_TAGMAP_MAX_BOARDS) {
            found = nb++;
            b_ids[found] = bid;
            b_weights[found] = 0;
        }
        if (found >= 0) {
            b_weights[found] += (st->node_counts[i] > 0 ? st->node_counts[i] : 1);
        }
    }

    /* Sort continents by total weight descending (deterministic). */
    for (int i = 0; i < nb; i++) {
        for (int j = i + 1; j < nb; j++) {
            if (b_weights[j] > b_weights[i] ||
                (b_weights[j] == b_weights[i] && b_ids[j] < b_ids[i])) {
                int32_t tid = b_ids[i]; b_ids[i] = b_ids[j]; b_ids[j] = tid;
                int32_t tw = b_weights[i]; b_weights[i] = b_weights[j]; b_weights[j] = tw;
            }
        }
    }

    st->n_continents = nb;
    const double golden = 2.39996322972865332; /* pi * (3 - sqrt(5)) */
    for (int k = 0; k < nb; k++) {
        st->continent_boards[k] = b_ids[k];
        if (nb == 1) {
            st->continent_x[k] = 0.0;
            st->continent_y[k] = 0.0;
        } else if (k == 0) {
            st->continent_x[k] = 0.0;
            st->continent_y[k] = 0.0;
        } else {
            double ring_r = 220.0 * sqrt((double)k) + 60.0;
            double ang = (double)k * golden;
            st->continent_x[k] = ring_r * cos(ang);
            st->continent_y[k] = ring_r * sin(ang);
        }
    }

    /* 2. Rank nodes globally by (count * 4 + degree) descending. */
    int32_t order[FB_TAGMAP_MAX_NODES];
    int32_t score[FB_TAGMAP_MAX_NODES];
    for (int i = 0; i < n; i++) {
        order[i] = i;
        score[i] = (st->node_counts[i] > 0 ? st->node_counts[i] : 1) * 4 + deg[i];
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int oi = order[i], oj = order[j];
            if (score[oj] > score[oi] || (score[oj] == score[oi] && st->node_ids[oj] < st->node_ids[oi])) {
                order[i] = oj;
                order[j] = oi;
            }
        }
    }
    for (int r = 0; r < n; r++) {
        st->node_rank[order[r]] = r;
    }

    /* 3. Seed initial positions around each node's continent center using a
     *    Phyllotaxis spiral in rank order within that continent. */
    int32_t cont_local_idx[FB_TAGMAP_MAX_BOARDS];
    memset(cont_local_idx, 0, sizeof(cont_local_idx));

    for (int r = 0; r < n; r++) {
        int i = order[r];
        int32_t bid = st->node_boards[i];
        int cidx = 0;
        for (int k = 0; k < nb; k++) {
            if (b_ids[k] == bid) {
                cidx = k;
                break;
            }
        }
        st->node_continent[i] = cidx;
        int m = cont_local_idx[cidx]++;
        double c_ang = (double)m * golden + (double)(cidx * 0.7);
        double c_rad = (m == 0) ? 0.0 : (36.0 * sqrt((double)m));
        st->node_x[i] = st->continent_x[cidx] + c_rad * cos(c_ang);
        st->node_y[i] = st->continent_y[cidx] + c_rad * sin(c_ang);

        double cnt = (double)(st->node_counts[i] > 0 ? st->node_counts[i] : 1);
        st->node_r[i] = 6.0 + 3.8 * sqrt(cnt);
        st->node_hop[i] = -1;
    }

    /* 4. Deterministic force-directed + collision-resolution refinement. */
    double vx[FB_TAGMAP_MAX_NODES];
    double vy[FB_TAGMAP_MAX_NODES];
    memset(vx, 0, sizeof(double) * (size_t)n);
    memset(vy, 0, sizeof(double) * (size_t)n);

    for (int it = 0; it < iterations; it++) {
        double temp = 1.0 - (double)it / (double)iterations;
        double step = 0.25 + 0.75 * temp;

        /* Pairwise repulsion + minimum separation padding */
        for (int i = 0; i < n; i++) {
            double xi = st->node_x[i], yi = st->node_y[i], ri = st->node_r[i];
            int ci = st->node_continent[i];
            for (int j = i + 1; j < n; j++) {
                double dx = xi - st->node_x[j];
                double dy = yi - st->node_y[j];
                double d2 = dx * dx + dy * dy;
                if (d2 < 1e-4) {
                    /* Deterministic nudge if exactly coincident */
                    dx = 0.1 * (double)((i - j) & 1 ? 1 : -1);
                    dy = 0.1 * (double)((i + j) & 1 ? 1 : -1);
                    d2 = dx * dx + dy * dy;
                }
                double d = sqrt(d2);
                double min_sep = ri + st->node_r[j] + (ci == st->node_continent[j] ? 24.0 : 42.0);
                double f = 0.0;
                if (d < min_sep) {
                    f = (min_sep - d) * 0.45;
                } else if (d < 420.0) {
                    f = 520.0 / (d2 + 120.0);
                }
                if (f > 0.0) {
                    double fx = (dx / d) * f;
                    double fy = (dy / d) * f;
                    vx[i] += fx;
                    vy[i] += fy;
                    vx[j] -= fx;
                    vy[j] -= fy;
                }
            }
        }

        /* Co-occurrence edge springs */
        for (int e = 0; e < st->n_edges; e++) {
            int a = st->edge_a[e];
            int b = st->edge_b[e];
            if (a < 0 || a >= n || b < 0 || b >= n || a == b) continue;
            double w = (double)(st->edge_w[e] > 0 ? st->edge_w[e] : 1);
            double dx = st->node_x[b] - st->node_x[a];
            double dy = st->node_y[b] - st->node_y[a];
            double d = sqrt(dx * dx + dy * dy + 1e-4);
            bool same_c = (st->node_continent[a] == st->node_continent[b]);
            double target_d = (same_c ? 68.0 : 165.0) / (1.0 + 0.18 * log(1.0 + w));
            double diff = d - target_d;
            double k_spring = (same_c ? 0.045 : 0.014) * (1.0 + 0.35 * log(1.0 + w));
            double fx = (dx / d) * diff * k_spring;
            double fy = (dy / d) * diff * k_spring;
            vx[a] += fx;
            vy[a] += fy;
            vx[b] -= fx;
            vy[b] -= fy;
        }

        /* Continent anchor gravity (keeps same-board tags clustered cleanly) */
        for (int i = 0; i < n; i++) {
            int cidx = st->node_continent[i];
            double gx = st->continent_x[cidx] - st->node_x[i];
            double gy = st->continent_y[cidx] - st->node_y[i];
            vx[i] += gx * 0.055;
            vy[i] += gy * 0.055;

            /* Clamp velocity and integrate */
            double vmag = sqrt(vx[i] * vx[i] + vy[i] * vy[i]);
            double max_v = 28.0 * step + 1.5;
            if (vmag > max_v) {
                vx[i] = (vx[i] / vmag) * max_v;
                vy[i] = (vy[i] / vmag) * max_v;
            }
            st->node_x[i] += vx[i];
            st->node_y[i] += vy[i];
            vx[i] *= 0.62;
            vy[i] *= 0.62;
        }
    }

    /* 5. Recompute continent centroids from actual settled member nodes, and
     *    re-center the entire map around (0, 0). */
    double cx_all = 0.0, cy_all = 0.0;
    for (int i = 0; i < n; i++) {
        cx_all += st->node_x[i];
        cy_all += st->node_y[i];
    }
    cx_all /= (double)n;
    cy_all /= (double)n;
    for (int i = 0; i < n; i++) {
        st->node_x[i] -= cx_all;
        st->node_y[i] -= cy_all;
    }

    int c_cnt[FB_TAGMAP_MAX_BOARDS];
    memset(c_cnt, 0, sizeof(c_cnt));
    for (int k = 0; k < nb; k++) {
        st->continent_x[k] = 0.0;
        st->continent_y[k] = 0.0;
    }
    for (int i = 0; i < n; i++) {
        int c = st->node_continent[i];
        if (c >= 0 && c < nb) {
            st->continent_x[c] += st->node_x[i];
            st->continent_y[c] += st->node_y[i];
            c_cnt[c]++;
        }
    }
    for (int k = 0; k < nb; k++) {
        if (c_cnt[k] > 0) {
            st->continent_x[k] /= (double)c_cnt[k];
            st->continent_y[k] /= (double)c_cnt[k];
        }
    }
}

static inline void fb_tagmap_compute_hops(fb_tagmap_state_t *st, int focus_idx) {
    int n = st->n_nodes;
    for (int i = 0; i < n; i++) {
        st->node_hop[i] = -1;
    }
    if (focus_idx < 0 || focus_idx >= n) return;
    st->node_hop[focus_idx] = 0;

    /* 1-hop neighbors */
    for (int e = 0; e < st->n_edges; e++) {
        int a = st->edge_a[e];
        int b = st->edge_b[e];
        if (a < 0 || a >= n || b < 0 || b >= n) continue;
        if (a == focus_idx && st->node_hop[b] < 0) st->node_hop[b] = 1;
        else if (b == focus_idx && st->node_hop[a] < 0) st->node_hop[a] = 1;
    }

    /* 2-hop neighbors */
    for (int e = 0; e < st->n_edges; e++) {
        int a = st->edge_a[e];
        int b = st->edge_b[e];
        if (a < 0 || a >= n || b < 0 || b >= n) continue;
        if (st->node_hop[a] == 1 && st->node_hop[b] < 0) st->node_hop[b] = 2;
        else if (st->node_hop[b] == 1 && st->node_hop[a] < 0) st->node_hop[a] = 2;
    }
}

#endif /* FLYBOARD_TAG_LAYOUT_CORE_H */
