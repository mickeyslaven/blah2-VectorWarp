# Cleanup audit

Scope: safe removal of proven-unused legacy files outside packaging, build,
install, repository workflows, API and C++ processing. Audit date: 2026-09-10.

## Removed

Exact pre-removal bytes and checksums are retained in the private cleanup ledger
outside this repository.

| Path | Bytes | Reason |
| --- | ---: | --- |
| `html/control.js` | 400 | Old space-key capture toggle. No page loaded it; current recording UI uses the acknowledged capture routes. |
| `html/lib/bootstrap-5.2.3.min.css` | 194,855 | No HTML page or build/install/test wiring referenced it. The UI uses `blah2.css`. |
| `html/lib/bootstrap-5.2.3.min.js` | 60,362 | No HTML page or build/install/test wiring referenced it. |
| `html/lib/jquery-3.6.4.min.js` | 89,795 | It was loaded by eight plot pages, but no application script used its API after the old control script was removed. Those pages now load only their actual dependencies. |
| `example.png` and `doc/html/example.png` symlink | 275,720 plus a 17-byte link | The README screenshot showed the old upstream interface, not VectorWarp. Removed the misleading embedding and its generated-doc link. |

Total source-file reduction in this audit: 621,149 bytes, including the symlink.

## Runtime packages

Native artifacts omit application test files and developer-only review/work logs;
these remain available in the source repository. Release packages additionally
omit npm CLI shims and duplicate integration templates/helpers. The API uses its
bundled Node runtime, so the RPM does not also require a system Node installation.
User guides, application dependencies and required license notices remain.

## Retained deliberately

- Plotly, all display routes, favicon assets, replay tests, SDR profiles, GPU
  options, licences and historical correctness/benchmark evidence remain in
  scope and are not obsolete merely because of their age or upstream naming.
- Packaging/build/install/repository workflows were inspected for references but
  are owned by other workstreams and were not modified here.
- The separate source/dependency audit found every C++ translation unit referenced
  by CMake and no confirmed unused SDR or processing dependency.

## Pending candidates

No additional file is approved for deletion from this audit. Future cleanup must
again prove a candidate is not loaded by a user view, test, build or install path.
