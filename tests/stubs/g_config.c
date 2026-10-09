/* Link stub for the render regression tests (tests/render_file,
 * tests/test_render_md_*).  Their link line compiles src/render/render_md.c
 * directly without src/config/config.o, so the global config object is
 * missing at link time.  This provides the same symbol config.c defines,
 * zero-initialized just like the real one; tests never load a config file,
 * so the all-empty defaults are intentional.  Add this file to the test
 * link lines (it is not referenced by any Makefile yet). */
#include "config/config.h"

blog_config_t g_config = {0};
