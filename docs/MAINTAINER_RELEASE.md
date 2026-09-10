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

The intended build matrix is Ubuntu 22.04/24.04 amd64+arm64 DEBs and Fedora 44
x86_64+aarch64 RPMs. The default package supports the Kraken/Heimdall network
receiver and CPU processing with Vulkan auto-detection where available.

1. `CI` validates CPU-only Kraken CTest, portable replay and API/UI tests. Use
   the immutable `vMAJOR.MINOR.PATCH` tag with `Build release packages`; its
   manual version input is dry-run only.
2. Confirm the six package assets, `SHA256SUMS`, `package-manifest.json` and
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

The planned Pages layout is `/apt/dists/jammy|noble` for APT,
`/rpm/fedora/44/$basearch` for DNF, and `/keys/vectorwarp.asc` for the public
key. It is a contract for the release workflow, not proof that those endpoints
currently exist.

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
