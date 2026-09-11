# Installer failure and recovery audit — 2026-09-10

Scope: `script/install-release.sh` only. This audit did not install a package,
write host/container repository configuration, call a real package manager or
service manager, use production key material, access radio hardware, or test
package lifecycle scripts.

## Confirmed findings and fixes

1. Preflight previously checked only the primary fingerprint. An expired or
   revoked identity could pass, and an accidentally supplied secret-key export
   could be dearmored into the world-readable package-manager keyring. The
   installer now requires exactly one matching, unexpired, non-revoked primary;
   at least one unexpired, non-revoked signing subkey; and no `sec`/`ssb`
   records. This deliberately supports the certification-only-primary design.
2. Repository URLs previously accepted credential-bearing, hostless,
   query/fragment, backslash and invalid-port forms. The parser now permits a
   plain HTTPS hostname/IPv4 origin with an optional valid port and path, and
   rejects ambiguous forms before any key or system operation.
3. Key downloads were unbounded. Curl now has connection/overall timeouts and a
   1-MiB transfer limit; both downloaded and local files are size-checked.
   Local key input must be a readable, non-symlink regular file.
4. Missing `dpkg` could surface as a misleading architecture error. Supported
   Debian-family platforms now fail explicitly when `dpkg` is absent.
5. Package-manager and system-file failures previously exited without stating
   what remained. Errors now identify whether no package manager ran or whether
   repository configuration was retained for an idempotent retry. A failed
   explicit API start says the package succeeded, the installer did not start
   radar, and existing service state was not verified. Success output likewise
   avoids claiming pre-existing services are disabled or stopped.

## Coverage boundary

| Case | Coverage | Result |
|---|---|---|
| Valid cert-only primary plus signing subkey | Real ephemeral GPG packets | Pass |
| Wrong fingerprint | Real ephemeral key | Rejected before system operations |
| Multiple primary keys | Real concatenated public exports | Rejected |
| Secret-bearing key input | Real ephemeral secret-subkey export | Rejected |
| Expired primary/signing identity | Real key generated under a past GPG clock | Rejected |
| Revoked primary or signing subkey | Simulated GPG `--with-colons` status records | Rejected by parser |
| Unsafe repository URLs and ports | Direct parser execution | Rejected |
| Partial/oversized key download | Stubbed curl with real file/size checks | Rejected before system operations |
| Non-root preflight and dry-run | Real non-root process, ephemeral key | Pass with no recorder calls |
| Non-root non-dry installation | Real non-root process | Rejected before system writes |
| Repeat installation | Stubbed system install/APT recorders, two complete passes | Same safe operation sequence |
| System-file, APT update and APT install failures | Stubbed failing commands | Stops at boundary; retry state reported |
| Default service behavior | Full simulated APT flow | No `systemctl` call; does not claim existing state |
| `--start-web` success/failure | Full simulated APT flow | Only `vectorwarp-api.service`; never processor; existing radar state not claimed |

The Fedora selector and fail-closed DNF configuration remain covered by the
existing source/static tests. A real Fedora/DNF transaction was intentionally
not run in this Ubuntu test container. Revocation was simulated because creating
and importing a real revocation certificate adds no parser coverage beyond the
actual GPG colon status consumed by the function. Revocation rejection also
depends on the downloaded public export containing the revocation certificate;
a stale public-key copy with the same primary fingerprint cannot communicate a
new revocation by itself. Actual clean-host APT/DNF
metadata, transaction rollback, systemd state and package lifecycle remain
release acceptance tests, not claims of this bounded audit.

Run the standalone test in the existing bounded Ubuntu packaging container:

```bash
python3 test/packaging/test_install_release.py -v
```

The test substitutes recorders for `curl`, system-destination `install`,
`apt-get` and `systemctl`. Temporary GPG homes and keys are test-only and are
removed at completion.
