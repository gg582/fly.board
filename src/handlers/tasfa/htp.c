#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tasfa_internal.h"

/* --- Lagrange Extrapolation for RTT Prediction ---
 * For large files we collect per-chunk RTT samples and use
 * Lagrange polynomial extrapolation to estimate the RTT of
 * the tail chunks.  Math is kept light: max 8 samples,
 * O(n^2) with tiny n, and clamped to avoid runaway values.
 */

static double lagrange_predict(const double *xs, const double *ys, int n, double x) {
    double result = 0.0;
    for (int i = 0; i < n; i++) {
        double term = ys[i];
        for (int j = 0; j < n; j++) {
            if (i == j) continue;
            double denom = xs[i] - xs[j];
            if (denom == 0.0) denom = 1e-9;
            term *= (x - xs[j]) / denom;
        }
        result += term;
    }
    return result;
}

void append_rtt_sample(cJSON *meta, int chunk_index, double rtt_ms) {
    if (!meta || rtt_ms < 0.0) return;
    cJSON *samples = cJSON_GetObjectItem(meta, "rtt_samples");
    if (!samples || !cJSON_IsArray(samples)) {
        samples = cJSON_CreateArray();
        cJSON_AddItemToObject(meta, "rtt_samples", samples);
    }
    cJSON *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "chunk_index", chunk_index);
    cJSON_AddNumberToObject(item, "rtt_ms", rtt_ms);
    cJSON_AddItemToArray(samples, item);
    while (cJSON_GetArraySize(samples) > TASFA_RTT_MAX_SAMPLES)
        cJSON_DeleteItemFromArray(samples, 0);
}

double predict_remaining_ms(cJSON *meta) {
    cJSON *samples = cJSON_GetObjectItem(meta, "rtt_samples");
    if (!samples || !cJSON_IsArray(samples)) return -1.0;
    int n = cJSON_GetArraySize(samples);
    if (n < 3) return -1.0; /* need at least 3 points for extrapolation */

    double xs[TASFA_RTT_MAX_SAMPLES];
    double ys[TASFA_RTT_MAX_SAMPLES];
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_GetArrayItem(samples, i);
        xs[i] = json_double(item, "chunk_index", 0.0);
        ys[i] = json_double(item, "rtt_ms", 0.0);
    }

    int chunk_count = json_int(meta, "chunk_count", 0);
    int received    = json_int(meta, "received_chunks", 0);
    int remaining   = chunk_count - received;
    if (remaining <= 0) return 0.0;

    /* Extrapolate to the last chunk index to get a tail RTT estimate. */
    double target_x = (double)(chunk_count - 1);
    double predicted_rtt = lagrange_predict(xs, ys, n, target_x);
    if (predicted_rtt < 0.0) predicted_rtt = 0.0;
    if (predicted_rtt > TASFA_RTT_CLAMP_MAX) predicted_rtt = TASFA_RTT_CLAMP_MAX;

    /* Naive total: remaining chunks * predicted single-chunk RTT.
     * This is intentionally simple to keep CPU load negligible.
     */
    return predicted_rtt * (double)remaining;
}

/* --- HTP Protocol --- */

#if defined(__AVX2__)
#define TASFA_HTP_SIMD 1
#define TASFA_HTP_AVX2 1
typedef uint64_t tasfa_u64x4 __attribute__((vector_size(32)));
#elif defined(__SSE2__) || defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__VSX__) || defined(__mips_msa)
#define TASFA_HTP_SIMD 1
#define TASFA_HTP_SSE2 1
typedef uint64_t tasfa_u64x2 __attribute__((vector_size(16)));
#else
#define TASFA_HTP_SIMD 0
#endif

const char *htp_simd_backend(void) {
#if defined(TASFA_HTP_AVX2)
    return "avx2";
#elif defined(__SSE2__)
    return "sse2";
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    return "neon";
#elif defined(__VSX__)
    return "vsx";
#elif defined(__mips_msa)
    return "msa";
#else
    return "scalar";
#endif
}

htp_line_sums_t htp_line_sums(const uint64_t v[6], uint64_t modulus_M) {
    if (modulus_M == 0) modulus_M = 1;
    htp_line_sums_t sums;
#if defined(TASFA_HTP_AVX2)
    tasfa_u64x4 v_left  = { v[0], v[2], v[4], 0 };
    tasfa_u64x4 v_mid   = { v[1], v[3], v[5], 0 };
    tasfa_u64x4 v_right = { v[2], v[4], v[0], 0 };
    tasfa_u64x4 v_sum   = v_left + v_mid + v_right;
    sums.l1 = v_sum[0] % modulus_M;
    sums.l2 = v_sum[1] % modulus_M;
    sums.l3 = v_sum[2] % modulus_M;
#elif defined(TASFA_HTP_SSE2)
    tasfa_u64x2 a = { v[0], v[2] };
    tasfa_u64x2 b = { v[1], v[3] };
    tasfa_u64x2 c = { v[2], v[4] };
    tasfa_u64x2 pair = a + b + c;
    sums.l1 = pair[0] % modulus_M;
    sums.l2 = pair[1] % modulus_M;
    sums.l3 = (v[4] + v[5] + v[0]) % modulus_M;
#else
    sums.l1 = (v[0] + v[1] + v[2]) % modulus_M;
    sums.l2 = (v[2] + v[3] + v[4]) % modulus_M;
    sums.l3 = (v[4] + v[5] + v[0]) % modulus_M;
#endif
    return sums;
}

void htp_analyze_group(uint64_t v[6], uint64_t modulus_M, int group_start,
                       htp_suspect_t *out, int *out_count) {
    htp_line_sums_t sums = htp_line_sums(v, modulus_M);
    uint64_t L1 = sums.l1;
    uint64_t L2 = sums.l2;
    uint64_t L3 = sums.l3;

    bool all_equal = (L1 == L2 && L2 == L3);
    if (all_equal) {
        *out_count = 0;
        return;
    }

    bool e1_fail = false, e2_fail = false, e3_fail = false;
    if (L1 != L2) { e1_fail = true; e2_fail = true; }
    if (L2 != L3) { e2_fail = true; e3_fail = true; }
    if (L1 != L3) { e1_fail = true; e3_fail = true; }

    int total_fail = (e1_fail ? 1 : 0) + (e2_fail ? 1 : 0) + (e3_fail ? 1 : 0);
    if (total_fail == 0) total_fail = 1;

    for (int i = 0; i < 6; i++) {
        int in_fail = 0;
        if (i == 0) { if (e1_fail) in_fail++; if (e3_fail) in_fail++; }
        if (i == 1) { if (e1_fail) in_fail++; }
        if (i == 2) { if (e1_fail) in_fail++; if (e2_fail) in_fail++; }
        if (i == 3) { if (e2_fail) in_fail++; }
        if (i == 4) { if (e2_fail) in_fail++; if (e3_fail) in_fail++; }
        if (i == 5) { if (e3_fail) in_fail++; }

        if (in_fail == 0) continue;

        /* Deterministic score derived directly from topology:
         * fraction of failed equations this slot participates in. */
        double score = (double)in_fail / (double)total_fail;

        out[*out_count].chunk_index = group_start + i;
        out[*out_count].suspicion_score = score;
        (*out_count)++;
    }
}

int compare_suspect_desc(const void *a, const void *b) {
    const htp_suspect_t *sa = (const htp_suspect_t *)a;
    const htp_suspect_t *sb = (const htp_suspect_t *)b;
    if (sa->suspicion_score > sb->suspicion_score) return -1;
    if (sa->suspicion_score < sb->suspicion_score) return 1;
    return sa->chunk_index - sb->chunk_index;
}

/* Try every permutation of a 6-slot group's balanced scalars.
 * If any permutation restores L1==L2==L3, apply it to memory and htp.bin
 * so the server recovers without retransmission. */
static bool next_permutation_int(int *a, int n) {
    int i = n - 2;
    while (i >= 0 && a[i] >= a[i + 1]) i--;
    if (i < 0) return false;
    int j = n - 1;
    while (a[j] <= a[i]) j--;
    int tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    for (int l = i + 1, r = n - 1; l < r; l++, r--) {
        tmp = a[l]; a[l] = a[r]; a[r] = tmp;
    }
    return true;
}

bool htp_try_swap_repair(const char *upload_id, uint64_t *balanced_scalars,
                         uint64_t *raw_scalars, char *tags,
                         int group_start, uint64_t modulus_M) {
    uint64_t v[6];
    for (int i = 0; i < 6; i++) v[i] = balanced_scalars[group_start + i];

    htp_line_sums_t sums0 = htp_line_sums(v, modulus_M);
    if (sums0.l1 == sums0.l2 && sums0.l2 == sums0.l3) return false;

    int perm[6] = {0, 1, 2, 3, 4, 5};
    while (next_permutation_int(perm, 6)) {
        uint64_t p[6];
        for (int i = 0; i < 6; i++) p[i] = v[perm[i]];
        htp_line_sums_t sums = htp_line_sums(p, modulus_M);
        if (sums.l1 == sums.l2 && sums.l2 == sums.l3) {
            uint64_t new_balanced[6], new_raw[6];
            char new_tags[6][HTP_TAG_LEN];
            for (int i = 0; i < 6; i++) {
                int src = group_start + perm[i];
                new_balanced[i] = balanced_scalars[src];
                new_raw[i] = raw_scalars[src];
                memcpy(new_tags[i], tags + (src * HTP_TAG_LEN), HTP_TAG_LEN);
            }
            for (int i = 0; i < 6; i++) {
                int dst = group_start + i;
                balanced_scalars[dst] = new_balanced[i];
                raw_scalars[dst] = new_raw[i];
                memcpy(tags + (dst * HTP_TAG_LEN), new_tags[i], HTP_TAG_LEN);
            }

            char path[PATH_MAX];
            upload_session_dir(path, sizeof(path), upload_id);
            strncat(path, "/htp.bin", sizeof(path) - strlen(path) - 1);
            int fd = open(path, O_RDWR);
            if (fd >= 0) {
                char recs[6][HTP_RECORD_SIZE];
                bool ok = true;
                for (int i = 0; i < 6; i++) {
                    off_t off = (off_t)(group_start + i) * HTP_RECORD_SIZE;
                    if (pread(fd, recs[i], HTP_RECORD_SIZE, off) != HTP_RECORD_SIZE) ok = false;
                }
                if (ok) {
                    for (int i = 0; i < 6; i++) {
                        off_t off = (off_t)(group_start + i) * HTP_RECORD_SIZE;
                        pwrite(fd, recs[perm[i]], HTP_RECORD_SIZE, off);
                    }
                }
                close(fd);
            }
            return true;
        }
    }
    return false;
}

bool htp_repair_worthwhile(int suspect_count, int total_chunks, int chunk_size, double rtt_ms) {
    (void)total_chunks; (void)chunk_size; (void)rtt_ms;
    if (suspect_count < 12) return false;
    long long retry_bytes = (long long)suspect_count * chunk_size;
    double rtt_factor = (rtt_ms <= 0.0) ? 1.0 : (rtt_ms / 100.0);
    double retry_cost = (double)retry_bytes * rtt_factor;
    double repair_cost = (double)suspect_count * HTP_REPAIR_CPU_PER_SUSPECT
                       + (double)(total_chunks / 6) * HTP_REPAIR_CPU_PER_L0_GROUP;
    return retry_cost > repair_cost;
}

/* Contraction: failed groups become higher-level vertices.
 * Each complete level-0 group is collapsed to a single scalar (sum of its
 * balanced scalars). Level-1 groups are consecutive 6-slot groups of these
 * aggregates. Suspects from level-0 groups in failing level-1 groups are kept.
 */
int htp_contract_groups(const uint64_t *balanced_scalars, int chunk_count, uint64_t modulus_M,
                        htp_suspect_t *suspects, int suspect_count) {
    int group_count = chunk_count / 6;
    if (group_count < 1 || suspect_count < 1) return suspect_count;

    uint64_t *group_agg = (uint64_t *)cwist_alloc((size_t)group_count * sizeof(uint64_t));
    int *group_suspect_mask = (int *)cwist_alloc((size_t)group_count * sizeof(int));
    memset(group_agg, 0, (size_t)group_count * sizeof(uint64_t));
    memset(group_suspect_mask, 0, (size_t)group_count * sizeof(int));

    for (int g = 0; g < group_count; g++) {
        uint64_t v0 = balanced_scalars[g * 6 + 0];
        uint64_t v1 = balanced_scalars[g * 6 + 1];
        uint64_t v2 = balanced_scalars[g * 6 + 2];
        uint64_t v3 = balanced_scalars[g * 6 + 3];
        uint64_t v4 = balanced_scalars[g * 6 + 4];
        uint64_t v5 = balanced_scalars[g * 6 + 5];
        uint64_t v[6] = {v0, v1, v2, v3, v4, v5};
        htp_line_sums_t sums = htp_line_sums(v, modulus_M);
        uint64_t L1 = sums.l1;
        uint64_t L2 = sums.l2;
        uint64_t L3 = sums.l3;
        /* Topology-preserving invariant signature:
         * encode the failure residuals (L1-L2, L2-L3) into a single scalar.
         * Multiplication in mod M is deterministic and sensitive to both
         * residuals without arbitrary constants. A passing group has
         * r12=r23=0, so its aggregate is 0. */
        uint64_t r12 = (L1 + modulus_M - L2) % modulus_M;
        uint64_t r23 = (L2 + modulus_M - L3) % modulus_M;
        group_agg[g] = (r12 * r23) % modulus_M;
    }

    for (int s = 0; s < suspect_count; s++) {
        int g = suspects[s].chunk_index / 6;
        if (g >= 0 && g < group_count) {
            group_suspect_mask[g] |= (1 << (suspects[s].chunk_index % 6));
        }
    }

    htp_suspect_t *orig = (htp_suspect_t *)cwist_alloc((size_t)suspect_count * sizeof(htp_suspect_t));
    memcpy(orig, suspects, (size_t)suspect_count * sizeof(htp_suspect_t));

    int next_count = 0;
    int l1_group_count = (group_count + 5) / 6;
    for (int lg = 0; lg < l1_group_count; lg++) {
        uint64_t lv[6] = {0};
        int lidx[6];
        int lgs = 0;
        for (int v = 0; v < 6; v++) {
            int g = lg * 6 + v;
            if (g < group_count) {
                lv[v] = group_agg[g];
                lidx[v] = g;
                lgs++;
            }
        }
        if (lgs < 3) continue;

        htp_line_sums_t level_sums = htp_line_sums(lv, modulus_M);
        uint64_t LL1 = level_sums.l1;
        uint64_t LL2 = (lgs > 3) ? level_sums.l2 : LL1;
        uint64_t LL3 = (lgs > 5) ? level_sums.l3 : LL1;

        if (LL1 == LL2 && LL2 == LL3) continue;

        /* Keep suspects from failing level-1 super-group */
        for (int v = 0; v < lgs; v++) {
            int g = lidx[v];
            for (int s = 0; s < 6; s++) {
                if (group_suspect_mask[g] & (1 << s)) {
                    int ci = g * 6 + s;
                    bool already = false;
                    for (int k = 0; k < next_count; k++) {
                        if (suspects[k].chunk_index == ci) { already = true; break; }
                    }
                    if (!already) {
                        /* Preserve original score */
                        double preserved_score = 0.0;
                        for (int j = 0; j < suspect_count; j++) {
                            if (orig[j].chunk_index == ci) {
                                preserved_score = orig[j].suspicion_score;
                                break;
                            }
                        }
                        suspects[next_count].chunk_index = ci;
                        suspects[next_count].suspicion_score = preserved_score;
                        next_count++;
                    }
                }
            }
        }
    }

    cwist_free(group_agg);
    cwist_free(group_suspect_mask);
    cwist_free(orig);
    return next_count;
}

bool save_htp_scalar_to_dir(const char *dir_path, int chunk_index, const char *hash_tag_hex, uint64_t raw_scalar, uint64_t balanced_scalar) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/htp.bin", dir_path);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return false;
    char tag[HTP_TAG_LEN] = {0};
    if (hash_tag_hex) strncpy(tag, hash_tag_hex, HTP_TAG_LEN - 1);
    off_t offset = (off_t)chunk_index * HTP_RECORD_SIZE;
    pwrite(fd, tag, HTP_TAG_LEN, offset);
    pwrite(fd, &raw_scalar, 8, offset + HTP_TAG_LEN);
    pwrite(fd, &balanced_scalar, 8, offset + HTP_TAG_LEN + 8);
    close(fd);
    return true;
}

bool save_htp_scalar(const char *upload_id, int chunk_index, const char *hash_tag_hex, uint64_t raw_scalar, uint64_t balanced_scalar) {
    char path[PATH_MAX];
    upload_session_dir(path, sizeof(path), upload_id);
    return save_htp_scalar_to_dir(path, chunk_index, hash_tag_hex, raw_scalar, balanced_scalar);
}

bool load_htp_scalars(const char *upload_id, int chunk_count, char **hash_tags_out, uint64_t **raw_scalars_out, uint64_t **balanced_scalars_out) {
    char path[PATH_MAX];
    upload_session_dir(path, sizeof(path), upload_id);
    strncat(path, "/htp.bin", sizeof(path) - strlen(path) - 1);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    char *tags = (char *)cwist_alloc((size_t)chunk_count * HTP_TAG_LEN);
    uint64_t *raw_scalars = (uint64_t *)cwist_alloc((size_t)chunk_count * sizeof(uint64_t));
    uint64_t *balanced_scalars = (uint64_t *)cwist_alloc((size_t)chunk_count * sizeof(uint64_t));
    if (!tags || !raw_scalars || !balanced_scalars) {
        cwist_free(tags); cwist_free(raw_scalars); cwist_free(balanced_scalars); close(fd); return false;
    }
    for (int i = 0; i < chunk_count; i++) {
        off_t offset = (off_t)i * HTP_RECORD_SIZE;
        if (pread(fd, tags + (i * HTP_TAG_LEN), HTP_TAG_LEN, offset) != HTP_TAG_LEN) {
            tags[i * HTP_TAG_LEN] = '\0';
        }
        if (pread(fd, &raw_scalars[i], 8, offset + HTP_TAG_LEN) != 8) {
            raw_scalars[i] = 0;
        }
        if (pread(fd, &balanced_scalars[i], 8, offset + HTP_TAG_LEN + 8) != 8) {
            balanced_scalars[i] = 0;
        }
    }
    close(fd);
    *hash_tags_out = tags;
    *raw_scalars_out = raw_scalars;
    *balanced_scalars_out = balanced_scalars;
    return true;
}

bool perform_xor_recovery(const char *upload_id, const char *temp_path, int chunk_size,
                          int group_start, int group_end, int target_chunk, int parity_chunk_idx, int data_chunks, long long total_size) {
    char parity_path[PATH_MAX];
    snprintf(parity_path, sizeof(parity_path), "%s/%s/parity_%d.bin", TASFA_UPLOAD_DIR, upload_id, parity_chunk_idx - data_chunks);

    unsigned char *parity_buf = (unsigned char *)cwist_alloc((size_t)chunk_size);
    if (!parity_buf) return false;
    memset(parity_buf, 0, (size_t)chunk_size);

    FILE *pf = fopen(parity_path, "rb");
    if (!pf) {
        cwist_free(parity_buf);
        return false;
    }
    size_t read_bytes = fread(parity_buf, 1, (size_t)chunk_size, pf);
    fclose(pf);
    if (read_bytes == 0) {
        cwist_free(parity_buf);
        return false;
    }

    int fd = open(temp_path, O_RDWR);
    if (fd < 0) {
        cwist_free(parity_buf);
        return false;
    }

    unsigned char *chunk_buf = (unsigned char *)cwist_alloc((size_t)chunk_size);
    if (!chunk_buf) {
        close(fd);
        cwist_free(parity_buf);
        return false;
    }

    for (int ci = group_start; ci < group_end; ci++) {
        if (ci == target_chunk) continue;

        long long offset = (long long)ci * (long long)chunk_size;
        long long current_chunk_size = total_size - offset;
        if (current_chunk_size > chunk_size) current_chunk_size = chunk_size;
        if (current_chunk_size <= 0) continue;

        memset(chunk_buf, 0, (size_t)chunk_size);
        if (pread(fd, chunk_buf, (size_t)current_chunk_size, (off_t)offset) != (ssize_t)current_chunk_size) {
            cwist_free(chunk_buf);
            cwist_free(parity_buf);
            close(fd);
            return false;
        }

        for (int i = 0; i < chunk_size; i++) {
            parity_buf[i] ^= chunk_buf[i];
        }
    }

    long long target_offset = (long long)target_chunk * (long long)chunk_size;
    long long target_size = total_size - target_offset;
    if (target_size > chunk_size) target_size = chunk_size;

    bool write_ok = pwrite_all(fd, parity_buf, (size_t)target_size, (off_t)target_offset);

    close(fd);
    cwist_free(chunk_buf);
    cwist_free(parity_buf);
    return write_ok;
}

bool tasfa_generate_htp_metadata_for_file(const char *file_path, int chunk_size, uint64_t modulus_M, const char *media_name) {
    if (!file_path || chunk_size <= 0 || modulus_M <= 1) return false;
    FILE *f = fopen(file_path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long long total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char htp_dir[PATH_MAX];
    snprintf(htp_dir, sizeof(htp_dir), "data/tasfa/media_htp/%s", media_name);
    if (!dir_ensure("data/tasfa/media_htp") || !dir_ensure(htp_dir)) {
        fclose(f);
        return false;
    }

    int chunk_count = (int)((total_size + (long long)chunk_size - 1) / (long long)chunk_size);
    unsigned char *buf = (unsigned char *)cwist_alloc((size_t)chunk_size);
    if (!buf) { fclose(f); return false; }

    for (int i = 0; i < chunk_count; i++) {
        size_t n = fread(buf, 1, (size_t)chunk_size, f);
        if (n <= 0) break;
        unsigned char hash[32];
        SHA256(buf, n, hash);
        char hash_hex[65];
        for (int j = 0; j < 32; j++) snprintf(hash_hex + (j * 2), 3, "%02x", hash[j]);
        uint64_t raw_scalar = 0;
        memcpy(&raw_scalar, hash, 8);
        raw_scalar %= modulus_M;
        save_htp_scalar_to_dir(htp_dir, i, hash_hex, raw_scalar, raw_scalar);
    }
    cwist_free(buf);
    fclose(f);
    return true;
}
