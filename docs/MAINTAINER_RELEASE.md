# Maintainer release guide

This is a one-time project-administration checklist, not an end-user install
procedure. The package repository is not operational until every item below has
completed successfully.

## One-time setup

1. Enable Actions in this fork, choose **Settings → Pages → Source: GitHub
   Actions**, and protect the `github-pages` environment. See
   [GitHub's Pages setup](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).
2. Create the archive signing key in the approved offline/maintainer process.
   Add it as `VECTORWARP_ARCHIVE_SIGNING_KEY` only in the protected
   `release-signing` environment; never commit a private key.
3. Commit only its public key at `packaging/keys/vectorwarp.asc`, and set the
   matching 40-hex `VECTORWARP_ARCHIVE_KEY_FINGERPRINT` repository variable.
   Publication must fail closed while that value is absent or invalid.
4. Grant the release workflow only `contents: write`; grant the Pages publication
   job only the required Pages OIDC/write permissions. Protect manual publication.
5. Verify the generated public key, APT metadata and RPM metadata in an isolated
   test host before exposing Pages as an installation source.

Use a dedicated package-signing key, not a personal authentication key. The
current workflow expects an ASCII-armored secret-key export usable without an
interactive passphrase; GitHub stores it as the protected environment secret.
Keep its backup outside the source tree, and never paste it into issues or chat.
Add the secret under **Settings → Environments → release-signing → Environment
secrets**, as described in [GitHub's secret guide](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets).
The fingerprint is public and goes under **Secrets and variables → Actions →
Variables**. Only the public `.asc` file belongs in Git.

## Release process

The intended build matrix is Ubuntu 22.04/24.04/26.04 and Debian 13 DEBs,
plus Fedora 44 RPMs, each for x86-64 (amd64 / x86_64) and
ARM64 (arm64 / aarch64): ten packages. The x86-64 aliases both cover Intel
and AMD CPUs. Preserve distribution-native architecture names in package
metadata, filenames and commands.
Ubuntu 22.04/26.04 and Debian 13 are built in their own
pinned userspaces; the containers are build conveniences only and are never a
VectorWarp runtime requirement. DragonOS receives the matching Ubuntu APT
selection through `/etc/os-release`; it is not an independently built or
boot-tested DragonOS target. Raspberry Pi OS Trixie selects the Debian 13
ARM64 (arm64 / aarch64) package; its installation and Pi hardware performance need separate
validation. No 32-bit or custom SD-card image is produced.
The default package supports the Kraken/Heimdall
network receiver and CPU processing with Vulkan auto-detection where available.

1. `CI` validates CPU-only Kraken CTest, portable replay and API/UI tests.
   `Build release packages` also performs unsigned `0.0.0` packaging smoke
   builds on pull requests targeting `main`; that path has no signing job and
   does not consume an environment-scoped signing secret. Configure and protect
   `release-signing` before any release tag. Use the immutable
   `vMAJOR.MINOR.PATCH` tag for a draft release; its manual version input is
   dry-run only.
2. Confirm the ten package assets, `SHA256SUMS`, `package-manifest.json` and
   `repository-manifest.json`. The tag creates a draft release for review; it
   does not publish packages.
3. After review, publish that release and run `Publish verified package
   repository`. It signs/generates repository metadata and deploys to Pages
   only when the protected environments and key/fingerprint validate.
4. On clean hosts, install from the APT and DNF repositories, verify the signing
   key fingerprint, confirm ordinary package-manager updates, and verify that no
   processor/radio starts automatically.
5. Only then change the user documentation from **pending first release** to
   published, with the verified release version and actual validation evidence.

The planned Pages layout is `/apt/dists/jammy|noble|resolute|trixie` for APT,
`/rpm/fedora/44/$basearch` for DNF, and `/keys/vectorwarp.asc` for the public
key. It is a contract for the release workflow, not proof that those endpoints
currently exist.

## Unsigned test packages

Before signing is configured, a maintainer may publish an explicitly unsigned
GitHub prerelease for local testing. It must not update the APT/DNF repository:

1. Merge passing CI, then manually run the package workflow on that exact
   `main` commit with a new numeric package version.
2. Require every native build/install check to pass. Download only that run's
   artifacts and verify every package against its manifest and checksum.
3. Publish under a `test-` tag, never a `v` tag, as a prerelease that is not
   marked latest. Include checksums, the exact source commit and workflow run,
   and clearly label it **unsigned, local-test-only**.
4. Reserve that package version. A later stable release must use the identical
   package bytes or a higher version; do not replace published files in place.

Do not disable repository signature checks to install a test package. Update
the README separately for downloadable test packages and the signed repository;
publishing one does not make the other available.

## Renewal

The repository is rebuilt weekly at Monday 03:17 UTC from the latest published
stable release. The renewal checksum-verifies published release assets before
generating and signing a fresh repository; it leaves the existing Pages content
unchanged if release selection, signing or generation fails.

An unattended renewal needs an environment policy that permits the scheduled
deployment. Requiring a reviewer gives an explicit human gate but turns the
weekly job into a pending approval; choose deliberately and record that policy.

APT metadata expires after 30 days. Watch failed/disabled renewal runs: GitHub
can delay scheduled jobs and disables schedules after 60 days of inactivity in
a public repository. Re-enable the workflow and run a manual renewal if needed;
do not disable APT signature or expiry checks. See
[GitHub's schedule limits](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule).

## Never claim

- Do not say the repository, packages, signing fingerprint or a GitHub Release
  exists before a verified workflow proves it.
- Do not describe Ubuntu/Debian/Fedora inclusion as automatic; this project
  maintains its own APT/DNF repository.
- Do not make package publishing or GitHub Pages part of an ordinary source build.
