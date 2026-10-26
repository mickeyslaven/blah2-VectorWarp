# Release matrix runner

`release-matrix.tsv` is the local counterpart to the GitHub release package
matrix. Each Linux target gets two distinct disposable native-architecture
guests: one for a source build/test plus clean candidate install, and one for a
published-package-to-candidate upgrade. The upgrade assertion snapshots all
installed GNU Radio, UHD, and SDRplay packages and requires exact preservation.

DragonOS uses its Ubuntu package target while preserving the installed image
ecosystem:

| image | Ubuntu base | package |
| --- | --- | --- |
| FocalX R37.1 | Jammy 22.04 | `ubuntu22.04/amd64` |
| Noble R9 | Noble 24.04 | `ubuntu24.04/amd64` |
| Resolute R1 | Resolute 26.04 | `ubuntu26.04/amd64` |

Create separate disposable overlays/guests before running a target. The runner
does not download images, reset disks, alter boot configuration, or install on
a controller host (Fedora x86_64 also uses disposable guests). On Strix it refuses to run outside the Qwen unload/restore
guard; restoration succeeds only after `qwen-window.sh reload` completes its
health check. On M2 it checks Lima reports exactly 2 GiB and runs only one
target command at a time.

```sh
sudo vm-lab/with-qwen-unloaded.sh vm-lab/run-release-target.sh \
  dragonos-focalx-r37.1 clean@guest-a upgrade@guest-b \
  vectorwarp-source.tar.gz vectorwarp_9999.0.0-1_ubuntu22.04_amd64.deb \
  vectorwarp_0.1.29-1_ubuntu22.04_amd64.deb
```

The macOS source matrix remains in `.github/workflows/macos.yml`; clean
Homebrew installs run on Intel and Apple Silicon, while the public-tap upgrade
path runs on Apple Silicon in `.github/workflows/homebrew.yml`.
