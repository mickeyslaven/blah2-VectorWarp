# Provisioning requirements (measured 2026-10-25)

No image listed here was downloaded during preparation. Sizes are final HTTP
`Content-Length` values from the pinned/current URLs used by the existing lab.

| architecture | base image | bytes |
| --- | --- | ---: |
| amd64 | Ubuntu 22.04 | 728,218,624 |
| amd64 | Ubuntu 24.04 | 621,284,352 |
| amd64 | Ubuntu 26.04 | 859,090,944 |
| amd64 | Debian 13 | 342,294,528 |
| x86_64 | Fedora 44 | 583,729,152 |
| amd64 | DragonOS FocalX R37.1 ISO | 4,240,943,104 |
| amd64 | DragonOS Noble R9 ISO | 4,166,520,832 |
| amd64 | DragonOS Resolute R1 ISO | 3,954,450,432 |
| arm64 | Ubuntu 22.04 | 698,816,000 |
| arm64 | Ubuntu 24.04 | 616,522,752 |
| arm64 | Ubuntu 26.04 | 940,373,504 |
| arm64 | Debian 12 | 341,311,488 |
| arm64 | Debian 13 | 336,265,216 |
| aarch64 | Fedora 44 | 528,154,624 |

Base downloads total 15,496,531,968 bytes (14.432 GiB) for the amd64 set and
3,461,443,584 bytes (3.224 GiB) for the arm64 set. The two-path gate requires
two disposable instances per target. Disk use depends on the selected guest
image and hypervisor; monitor actual allocation during native vcpkg builds.

Run targets serially unless the controller has independently verified resource
headroom. DragonOS still needs one graphical installer pass per disposable
disk. Apply controller-specific resource and workload guards outside this
repository.
