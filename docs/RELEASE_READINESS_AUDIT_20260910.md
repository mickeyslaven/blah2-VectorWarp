# Release readiness audit — 2026-09-10

This is a maintainer/security handoff. It records observed state and commands;
it does not claim that a release or package repository exists.

## Initial and current GitHub state

Repository: `mickeyslaven/blah2-VectorWarp`. At the time of inspection:

- `main` was unprotected at `4840a44e167ee6c87ad9300af1866f866ca0ab9d`.
- The reviewed integration branch was at
  `ac1f25bda382eedb7b137424a8902b559ca1c974`; do not tag the old `main`.
- Actions were enabled with read-only default workflow permissions and only
  GitHub-owned actions allowed. Every referenced action was pinned by full SHA.
- Pages did not exist. There were no environments, Actions variables, Actions
  secret names, rulesets, releases or tags. Only `CI` and `Build release
  packages` were active because the repository-publication workflow was not on
  `main`.

The following controls were then applied and independently read back:

- `main` requires a pull request with zero approvals (solo-maintainer policy),
  stale-review dismissal, conversation resolution and 13 strict checks bound to
  the GitHub Actions app. Administrators are included; force pushes and deletion
  are blocked. The required package jobs depend on the separate version-validation
  job, so validation is transitively required.
- Active tag rules block updates and deletion of `v*` and `test-*` with no
  bypass. A separate creation rule permits only the repository owner.
- `release-signing` and `github-pages` permit only branch `main` and tags `v*`.
  Neither requires a reviewer, preserving unattended metadata renewal.
- Pages uses GitHub Actions at the planned URL but has not deployed content.
- Repository variable `VECTORWARP_ARCHIVE_KEY_FINGERPRINT` is configured with
  primary fingerprint `A3063601F4C8309F285362ACCC7593248896A175`.
  Environment secret name `VECTORWARP_ARCHIVE_SIGNING_KEY` was present in
  `release-signing` at the final read-back. Its value was never queried.

## Verified artifacts and remaining claims

Run `34534957708` at source `9cfc783ca133308d28b73ef06cb66f89c9b3d367`
completed all ten package jobs. An independent download found exactly ten
packages and ten sidecar manifests, the expected target matrix, matching byte
sizes and SHA-256 hashes. Every package version is `0.0.0`. They expire on
2026-09-24 and are unsigned PR-test inputs, not a release and not an APT/DNF
repository.

A later run, `34536576235`, passed the full package workflow at reviewed branch
HEAD `ac1f25bda382eedb7b137424a8902b559ca1c974`, including all ten package jobs.
That still does not make the branch releasable until the source hardening and
public key are merged to protected `main`.

The declared binary matrix is Ubuntu 22.04, 24.04 and 26.04, Debian 13, and
Fedora 44, each on x86-64 and ARM64. DragonOS and Raspberry Pi OS select a
compatible package; they are not independent builds or physical-device proof.
No Windows, macOS, 32-bit or OS-image package is produced.

The signing integration test now models the intended custody boundary: a
certification-only primary, a separate signing subkey, and a CI import made with
`--export-secret-subkeys`. GPG selects the signing subkey when workflows name
the full primary fingerprint. APT release signatures, RPM signatures, checksum
signatures and renewal without RPM re-signing all pass. Production key size
does not alter that selection behavior.

The actual maintainer-side clean-import test showed an unavailable primary
secret sentinel and exactly one usable signing subkey. A detached signature
selected subkey `342DC7F7B525BE15431B24341856593718CE92B9` and verified through
a clean public-only import. The root-only backup directory is mode 0700 and its
files are mode 0600. The primary expires 2028-09-09; the subkey expires
2027-09-10. This is a maintainer-host backup, not an encrypted, off-host or
air-gapped backup.

Existing APT clients retain a static keyring. Subkey rotation therefore needs an
overlap phase while the old subkey still signs metadata: publish the updated
public export and have existing clients rerun the fingerprint-pinned installer,
or first ship a reviewed keyring-package upgrade. Do not switch the signer until
an existing-install test proves the new subkey is trusted. Merely replacing the
Pages key file is not sufficient for APT clients.

## Trust boundary and hardening

The accompanying isolated changes fail closed unless:

- stable tags and manual unsigned builds point to the exact current `main`;
- all ten package targets have one version and one full source commit;
- a published release tag still resolves to the commit bound into its assets;
- renewal receives that exact version/source/matrix;
- the release's `SHA256SUMS` detached signature verifies with the public key
  committed on trusted `main`; and
- the release-carried public key and fingerprint match the committed key and
  protected repository variable.

Native APT/DNF metadata and detached release checksums use one OpenPGP trust
root. This is not TUF: there is no offline threshold root, delegated roles or
independent rollback metadata. APT has a 30-day `Valid-Until`; DNF has signed
`repomd.xml` but no equivalent expiry implemented here. GitHub source, Releases
and Pages also share one control plane. Record explicit acceptance or schedule
TUF/independent mirroring as follow-up. SBOMs and GitHub artifact attestations
are not currently produced; source commit binding and signed checksums provide
basic provenance but do not claim SLSA provenance.

## One-time administration

The chosen solo-maintainer policy is compatible with unattended weekly renewal:

1. Create `release-signing` and `github-pages`, restricted to `main` and `v*`.
   Do not require reviewers, because a reviewer gate would stall every renewal.
   Publishing the already-reviewed draft release is the manual stable-release
   approval.
2. Put only `VECTORWARP_ARCHIVE_SIGNING_KEY` in the `release-signing`
   environment. It is the armored `--export-secret-subkeys` result, never the
   primary private key. Put the 40-hex primary fingerprint in repository variable
   `VECTORWARP_ARCHIVE_KEY_FINGERPRINT`. Commit the public export, made after
   adding the signing subkey, as `packaging/keys/vectorwarp.asc`.
3. Enable Pages with GitHub Actions as its source. `github-pages` needs no
   secret. Preserve the workflow's job-scoped `pages: write` and `id-token:
   write`; only the draft-creation job receives `contents: write`.
4. Protect `main`: require a pull request, all agreed CI/package checks,
   conversation resolution, and block force pushes/deletion. Zero approvals is
   workable for one maintainer; one approval requires a second person. Enforce
   the rule for administrators, with only an explicitly documented emergency
   bypass if desired.
5. Protect `v*` and `test-*` tags against update and deletion with no bypass;
   use a separate owner-only creation rule.
6. Keep the primary private key and root-only backup off GitHub. Calendar
   signing-subkey rotation before its one-year expiry and primary renewal before
   two years. A failed renewal must alert; never relax signature or APT-expiry
   checks.

## Unsigned prerelease path

This path publishes downloadable local-test packages only. It never runs the
repository workflow and must not advertise APT/DNF:

```bash
REPO=mickeyslaven/blah2-VectorWarp
TEST_VERSION=0.1.0                 # reserve it; stable must be byte-identical or higher
MAIN_SHA=$(gh api "repos/$REPO/commits/main" --jq .sha)
gh workflow run release-packages.yml -R "$REPO" --ref main -f version="$TEST_VERSION"
gh run list -R "$REPO" --workflow 'Build release packages' --event workflow_dispatch \
  --branch main --limit 5
gh run watch RUN_ID -R "$REPO" --exit-status
gh run download RUN_ID -R "$REPO" --dir test-artifacts
```

Verify each sidecar before copying any asset:

```bash
find test-artifacts -type f -name '*.manifest.json' -print0 |
while IFS= read -r -d '' manifest; do
  package="$(dirname "$manifest")/$(jq -r .filename "$manifest")"
  test "$(stat -c %s "$package")" = "$(jq -r .size "$manifest")"
  printf '%s  %s\n' "$(jq -r .sha256 "$manifest")" "$package" | sha256sum --check --strict
  test "$(jq -r .version "$manifest")" = "$TEST_VERSION"
done
test "$(find test-artifacts -type f \( -name '*.deb' -o -name '*.rpm' \) | wc -l)" -eq 10
```

After explicit approval to create a public prerelease, stage the ten packages,
an aggregate manifest carrying `MAIN_SHA`, and package checksums. Publish with a
new immutable `test-*` tag, `--prerelease --latest=false`, source commit and run
URL in the notes, and the words **unsigned, local-test-only**. Install only a
downloaded local file with `apt install ./file.deb` or `dnf install ./file.rpm`;
do not add a repository or disable signature checking. A stable release after a
test build should normally use a higher numeric package version because stable
RPM signing changes the bytes.

## Stable release command boundary

The tag and workflow run are public; release assets remain in a draft until
publication. Replace placeholders only after the exact merged `main`, version,
key fingerprint and protections have been approved:

```bash
REPO=mickeyslaven/blah2-VectorWarp
VERSION=1.0.0
TAG="v$VERSION"
MAIN_SHA=$(gh api "repos/$REPO/commits/main" --jq .sha)
git fetch origin main --tags
test "$(git rev-parse origin/main)" = "$MAIN_SHA"
git tag -a "$TAG" "$MAIN_SHA" -m "VectorWarp $VERSION"
git push origin "refs/tags/$TAG"
gh run list -R "$REPO" --workflow 'Build release packages' --event push --limit 5
gh run watch RUN_ID -R "$REPO" --exit-status
gh release download "$TAG" -R "$REPO" --dir "review-$TAG"
```

Before public release, compare the carried fingerprint with the approved
out-of-band value, dearmor the carried public key, verify `SHA256SUMS.asc`, check
all ten packages, and verify `package-manifest.json` has `version == VERSION`,
`source_commit == MAIN_SHA`, ten unique expected targets, and the expected
upstream baseline. Inspect the draft notes and package/service behavior. Do not
advance `main` during the tag validation/build window.

Publishing the draft is a separate user approval and triggers Pages generation:

```bash
gh release edit "$TAG" -R "$REPO" --draft=false --latest
gh run list -R "$REPO" --workflow 'Publish verified package repository' --event release --limit 5
gh run watch PUBLISH_RUN_ID -R "$REPO" --exit-status
```

On disposable clean hosts, verify APT and DNF metadata signatures, fingerprint,
install, ordinary update, config preservation, and that neither processor nor
radio starts automatically. Only after those checks pass may installation docs
say the repository is live. Never publish a stable release, change `latest`,
push a tag or deploy Pages without the explicit approval at that boundary.
