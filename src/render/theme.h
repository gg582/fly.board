#ifndef DOCKER_BLOG_THEME_H
#define DOCKER_BLOG_THEME_H

#include <cwist/core/html/css.h>
#include <stdbool.h>

cwist_css_builder_t *theme_build(bool dark);

#endif
