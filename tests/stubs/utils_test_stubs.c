/* Stubs for symbols src/utils objects under test reference but that belong
 * to subsystems outside the scope of these unit tests (engine BDR counter,
 * post-schedule and write-policy generation counters).
 *
 * Used by: test_utils_cache.c, test_utils_reqshare.c
 */
unsigned post_schedule_generation(void) { return 0; }
unsigned write_policy_generation(void) { return 0; }
void engine_bdr_bump(void) {}
