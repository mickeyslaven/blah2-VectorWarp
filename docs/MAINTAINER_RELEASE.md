# Maintainer release guide

This is a one-time project-administration checklist, not an end-user install
procedure. The package repository is not operational until every item below has
completed successfully.

## One-time setup

1. Enable Actions in this fork, choose **Settings → Pages → Source: GitHub
   Actions**, and protect the `github-pages` environment. See
   [GitHub's Pages setup](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).
2. Create the archive primary and signing subkey in the approved
   maintainer-controlled process. Export only the signing subkey for CI and add it
   as `VECTORWARP_ARCHIVE_SIGNING_KEY` in the protected `release-signing`
   environment; never upload or commit the primary private key.
3. Commit only its public key at `packaging/keys/vectorwarp.asc`, and set the
   matching 40-hex `VECTORWARP_ARCHIVE_KEY_FINGERPRINT` repository variable.
   Publication must fail closed while that value is absent or invalid.
4. Grant the release workflow only `contents: write`; grant the Pages publication
   job only the required Pages OIDC/write permissions. Protect manual publication.
5. Verify the generated public key, APT metadata and RPM metadata in an isolated
   test host before exposing Pages as an installation source.

Use a dedicated package-signing key, not a personal authentication key. The
configured trust anchor is certification-only RSA4096 primary fingerprint
`A306 3601 F4C8 309F 2853 62AC CC75 9324 8896 A175`, expiring 2028-09-09.
Its RSA4096 signing subkey is
`342D C7F7 B525 BE15 431B 2434 1856 5937 18CE 92B9`, expiring 2027-09-10.
The workflow expects the ASCII-armored output of `gpg --export-secret-subkeys`
for that primary, usable without an interactive passphrase. A clean import must
show an unavailable/stub primary secret (`sec#`) and the sole usable signing
subkey (`ssb` with signing capability); it must not contain the primary
secret. GitHub stores that subkey export as the protected environment secret.
Keep its backup outside the source tree, and never paste it into issues or chat.
The current backup is root-only on the maintainer host, not an encrypted,
off-host or air-gapped backup.
Add the secret under **Settings → Environments → release-signing → Environment
secrets**, as described in [GitHub's secret guide](https://docs.github.com/en/actions/how-tos/write-workflows/choose-what-workflows-do/use-secrets).
The full 40-hex primary fingerprint (without spaces) is public and goes under
**Secrets and variables → Actions → Variables**. Export the public key only
after adding the signing subkey. Only that public `.asc` file belongs in Git.

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
Each package includes all four receiver adapters and CPU processing with
Vulkan auto-detection. UHD and libhackrf use native distro dependencies;
SDRplay's API remains a separately licensed local installation.

Before running release builds, provision a licensed SDRplay SDK outside Git
on each trusted build runner and accept its terms for build use. Set
`VECTORWARP_SDRPLAY_BUILD_LICENSE_ACCEPTED=true` in `release-signing` and
select either local `BLAH2_SDRPLAY_INCLUDE_DIR` / `BLAH2_SDRPLAY_LIBRARY` paths
or an absolute local `VECTORWARP_SDRPLAY_SDK_ARCHIVE` path to the pinned 3.15.2
installer obtained from [SDRplay](https://sdrplay.com/hardware-api/).
The helper stages headers/link libraries without running or downloading the
installer. No SDK is retrieved from this repository or its history.

Optional repository/organization variables `VECTORWARP_SDRPLAY_RUNNER_X64`
and `VECTORWARP_SDRPLAY_RUNNER_ARM64` select trusted runner labels. Do not put
runner-selection variables in the environment: they are needed before the
job reaches that environment. Keep SDK paths and license attestation in the
protected `release-signing` environment, read only by its staging step.
Leave them unset only when the default hosted runners have separately been
provisioned with permitted inputs; ordinary hosted runners do not include
SDRplay. Protect those runners and the existing `release-signing` environment
from unreviewed refs. Never provide proprietary inputs to a public PR job or
place them in public Actions artifacts, caches, release assets or container
images. The staged SDK lives in the runner's private temporary directory and
is mounted read-only for packaging. Actual packages still require all four
adapters; missing SDK input fails the build, not silently reduced support.

1. `CI` validates CPU-only Kraken CTest, portable replay and API/UI tests.
   `Build release packages` runs all ten native package smoke checks on public
   PRs using Kraken, USRP and HackRF, plus hardware-free contracts and fake
   SDK-input fixtures. The reduced packages are test-only and are not uploaded
   as release assets; the stable repository generator rejects their receiver
    profile. Passing these checks does not establish a real RSPduo SDK build.
   PR builds use the empty `pr-packaging` environment; never place SDK inputs
   or signing secrets there.
   Actual all-adapter release packaging runs only from trusted main/tag
   releases after separately licensed SDK provisioning. Configure and protect
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

The configured policy permits only `main` and `v*` tags in `release-signing`
and `github-pages`, without required environment reviewers. The owner approves
new releases by publishing reviewed drafts; weekly metadata renewal is
unattended after the publication workflow is merged and a stable release exists.

APT metadata expires after 30 days. Watch failed/disabled renewal runs: GitHub
can delay scheduled jobs and disables schedules after 60 days of inactivity in
a public repository. Re-enable the workflow and run a manual renewal if needed;
do not disable APT signature or expiry checks. See
[GitHub's schedule limits](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule).

Calendar signing-subkey rotation well before 2027-09-10 and primary renewal
before 2028-09-09. The current repository does not automatically refresh the
static APT keyring already installed on client hosts. Before switching to a new
subkey, keep the old signer valid, publish the new public export through
protected `main`, and deliver it while metadata is still signed by the old
subkey (for example by requiring users to rerun the fingerprint-pinned installer
or by first adding a reviewed keyring-package upgrade). Prove the overlap on an
existing install before changing CI to the new signer. Rotation also requires a
matching CI subkey export and isolated APT/RPM verification. Never extend
service by removing fingerprint, repository-signature or APT-expiry checks.
Renewal requires the release-carried public key to exactly match trusted `main`;
coordinate the public-key update with a reviewed release carrying that export.
Do not leave scheduled renewal pointed at a release with the old key export.

## Never claim

- Do not describe packages or the repository as published before publication
  and install checks prove it. A locally verified public signing fingerprint
  does not prove that a release or package repository is available.
- Do not describe Ubuntu/Debian/Fedora inclusion as automatic; this project
  maintains its own APT/DNF repository.
- Do not make package publishing or GitHub Pages part of an ordinary source build.
