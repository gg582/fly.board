# Trust and spam methodology

All four controls in `community.settings` are optional and default to `false`: `new_user_limits`, `trust`, `spam_detection`, and `approval_queue`. With all controls disabled, submission and publication follow the original behavior, including anonymous posts and comments. Administrators and moderators are exempt from submission limits and automatic queuing.

## New-account limits

An account is new for 24 hours after its `users.created_at` timestamp. During that period it may create at most **one post** and **three comments**, counted over the preceding 24 hours. A submission beyond the applicable limit is rejected. Draft creation is not subject to the post-publication check. Anonymous submissions are not account-age limited. When `trust` is also enabled, a trusted account is exempt from these limits.

## Trust

Trust is deliberately simple and deterministic. An authenticated account is trusted when either:

- its account timestamp is at least **7 days** old; or
- at least **3 posts or comments** attributed to that account have been approved through the moderation queue.

Anonymous submissions are never trusted. Trust bypasses new-account limits and the blanket approval queue, but it does not bypass spam heuristics.

## Spam heuristics

Spam detection never deletes or rejects content by itself. A match creates a pending item for moderator review, even when the blanket approval queue is disabled. Exactly two heuristics are used:

- **Excessive links:** more than **5** case-insensitive occurrences of `http://` or `https://` in the post or comment body.
- **Duplicate content:** the normalized body exactly matches an earlier body by the same account and of the same content type. For anonymous writers, the comparison group is all anonymous writers. Normalization trims leading and trailing ASCII whitespace, collapses each ASCII whitespace run to one space, and maps `A`–`Z` to `a`–`z`. Post titles are not part of the comparison. Pending comment snapshots are included in duplicate checks.

## Approval and rejection

When `approval_queue` is enabled, every non-trusted submission is pending. Spam matches are also pending when `spam_detection` is enabled. Pending posts use the `pending` publication status, so public listings, feeds, and searches exclude them. Pending comments are soft-deleted placeholders; their original text is stored only in the moderation queue and is restored on approval. Neither pending type generates a public notification or announcement.

Administrators and moderators can approve or reject items at `/admin/moderation`. Approval publishes a post or restores a comment. Rejection deletes a pending post; a pending comment remains soft-deleted. Editing a pending post does not make it public. Existing public content is never retroactively hidden when these settings are enabled or disabled.

## Limitations

These are abuse-friction measures, not a general spam classifier. There is no Bayesian model, reputation network, URL reputation lookup, or semantic analysis. Duplicate checks compare only exact normalized text and can be evaded by small edits; legitimate reposts and link-heavy references can produce false positives. Link counting is a simple substring count, not HTML or URL parsing. Account age and approval counts can be gamed over time. Per-account duplicate and recent-count queries are intentionally simple and may become expensive for very prolific accounts. Cross-database post/comment actions are not one distributed transaction. Moderators should review context before acting, and sites facing coordinated abuse still need rate limits, access controls, and operational monitoring.
