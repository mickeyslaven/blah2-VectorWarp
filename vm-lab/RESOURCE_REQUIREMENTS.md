# Provisioning requirements (measured 2026-10-25)

No image listed here was downloaded during preparation. Sizes are final HTTP
`Content-Length` values from the pinned/current URLs used by the existing lab.

| host | base image | bytes |
| --- | --- | ---: |
| Strix | Ubuntu 22.04 amd64 | 728,218,624 |
| Strix | Ubuntu 24.04 amd64 | 621,284,352 |
| Strix | Ubuntu 26.04 amd64 | 859,090,944 |
| Strix | Debian 13 amd64 | 342,294,528 |
| Strix | Fedora 44 x86_64 | 583,729,152 |
| Strix | DragonOS FocalX R37.1 ISO | 4,240,943,104 |
| Strix | DragonOS Noble R9 ISO | 4,166,520,832 |
| Strix | DragonOS Resolute R1 ISO | 3,954,450,432 |
| M2 | Ubuntu 22.04 arm64 | 698,816,000 |
| M2 | Ubuntu 24.04 arm64 | 616,522,752 |
| M2 | Ubuntu 26.04 arm64 | 940,373,504 |
| M2 | Debian 12 arm64 | 341,311,488 |
| M2 | Debian 13 arm64 | 336,265,216 |
| M2 | Fedora 44 aarch64 | 528,154,624 |

Base downloads total 15,496,531,968 bytes (14.432 GiB) on Strix and
3,461,443,584 bytes (3.224 GiB) on M2. The two-path gate requires two
disposable instances per target. With the existing lab defaults, budget 16
sparse 48-GiB Strix disks (768 GiB logical maximum) and 12 sparse 15-GiB M2
disks (180 GiB logical maximum), plus build growth. Actual allocated qcow2/Lima
space starts lower but must be monitored during native vcpkg builds.

Execution is serial: at most one 4-GiB/2-vCPU Strix guest, or one exactly
2-GiB/2-vCPU M2 guest, runs at a time. DragonOS still needs one graphical
installer pass per disposable disk. Strix execution additionally requires the
Qwen unload/restore guard and a successful post-restore health check.
