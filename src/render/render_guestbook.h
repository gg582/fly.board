#ifndef FLYBOARD_RENDER_GUESTBOOK_H
#define FLYBOARD_RENDER_GUESTBOOK_H

#include <cwist/core/db/sql.h>
#include <cwist/core/sstring/sstring.h>

/* Guestbook section for a user's profile page.
 *
 * render_profile() embeds a placeholder div plus a small fetch() to
 * /guestbook/list, which returns this partial HTML. That keeps viewer
 * permissions (delete buttons, post form, owner toggle) and ?page=
 * pagination correct without threading request state through the cached
 * profile render: the handler authenticates the fetch normally (the session
 * cookie is sent same-origin) and calls render_guestbook_section(). */
cwist_sstring *render_guestbook_section(cwist_db *db, int owner_uid, int viewer_uid,
                                        const char *viewer_role, int page);

#endif
