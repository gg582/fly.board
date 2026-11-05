# Community roles and suspension

## Method

The optional community model adds one global role, `moderator`, and account suspension without changing the existing `admin`/`user` model by default. Load the configuration once during startup, after configuration storage is available and before worker processes fork:

```c
#include "engine/community.h"
community_config_load("community.settings");
```

The file uses `key=value` lines and supports blank lines and `#` comments:

```ini
roles=false
```

Set `roles=true` (or `roles=1`) to enable the model. A missing file, `roles=false`, or `roles=0` leaves the legacy behavior unchanged. Invalid boolean values are rejected and fail closed to `roles=false`. Configuration is process state inherited by workers; changing the file requires a restart and a reload through the same startup call.

## Authorization model

- `admin` retains all existing privileges and can always process reports.
- With roles enabled, an admin can assign `user`, `moderator`, or `admin` through the existing user-role endpoint.
- A global `moderator` can read and process the report queue: dismiss or resolve reports and delete reported posts or comments.
- Moderators cannot open user administration, change roles, manage admins, or change global settings. Those handlers continue to use `auth_require_admin()`.
- Report handlers use `auth_require_moderator()`. This admits admins and, only while roles are enabled, moderators.
- The `admin.settings` account cannot be demoted or suspended. Startup repair also keeps its users row active with the admin role.

Suspension reuses the existing `users.active` column: `1` means active and `0` means suspended. Only non-admin users and moderators can be suspended or restored. Status changes use `community_user_set_active()` and are included in the existing user-row synchronization journal.

With roles enabled, request authentication reloads the current users row. Suspension therefore rejects existing sessions, and role changes take effect without waiting for JWT expiry. With roles disabled, no extra users-table read or active-status check occurs, preserving prior authentication behavior.

## Threat model and limits

The controls protect server-side handlers; hiding or showing a form is not an authorization boundary. A moderator is a trusted operations role because report processing can permanently delete content and its attachments. Give the role only to accounts that need it, and protect admin and moderator accounts with strong passwords and two-factor authentication.

Board-scoped moderation and notice grants are separately opt-in with `board_delegations=true`. Admins manage them on the existing board permissions page, optionally with a UTC expiration. Delegated moderators can review their boards' approval queues and delete their posts and comments; notice grants allow creating, changing, or removing board notices. Expiration is checked on every authorization query and revocation deletes the grant immediately. Grants use the FlyWire row journal. They do not grant global report, user, or settings administration.

This is deliberately a minimal model. It has no audit-log interface, timed suspension, appeal workflow, delegated content-edit permission, or granular report permission. It does not prevent an admin from deleting accounts through pre-existing administration features. Disabling roles after granting moderator access removes report privileges on restart, but does not rewrite stored role values; re-enabling roles restores those stored assignments. Suspension is also enforced only while roles are enabled.

Database access and TLS remain required trust boundaries. Anyone who can directly edit the users table or signing secret can bypass application-level role decisions. Back up the database before operational changes and review report deletions promptly because they are destructive.
