# adsb2dd adaptation

Adapted from 30hours/adsb2dd, commit
`1d004a0f0b741214422d55763aa672b0bbb6ae79` (MIT, 2024).

Also incorporates the relevant submitted upstream fixes from PR #5 (position
timestamp and processing-history duplicate checks) and PR #3 (bounded inactive
state lifecycle). The URL-key lifecycle is not copied because this adaptation
has one configuration-bound in-process history; stale per-aircraft state is
evicted by `max_position_age`.

VectorWarp retains WGS-84 ECEF bistatic geometry and median motion derivatives,
but runs them in-process against the configured tar1090 feed. It removes the
separate HTTP converter, query-supplied geometry, and external dependency.
Position time is `feed.now - seen_pos`; invalid, stale, repeated, and
non-monotonic points are excluded. Outputs are numeric and histories are
bounded.
