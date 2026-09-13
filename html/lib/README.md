# Plotly 2.20.0 notice provenance

The existing `plotly-2.20.0.min.js` is unchanged. On 2026-09-11 it was
byte-identical to [Plotly's versioned CDN bundle](https://cdn.plot.ly/plotly-2.20.0.min.js),
SHA-256 `d908ccb86ab3c39f41a0196ca0d59ed4e54e76885bb32904a8c937aec963f394`.

The following texts are retained unchanged under their own terms, not relicensed
under VectorWarp's project license:

| Local file | Version-specific source | SHA-256 |
| --- | --- | --- |
| [plotly.min.js.LICENSE.txt](plotly.min.js.LICENSE.txt) | [Official CDN companion](https://cdn.plot.ly/plotly-2.20.0.min.js.LICENSE.txt); independently identical [npm full-package copy](https://unpkg.com/plotly.js@2.20.0/dist/plotly.min.js.LICENSE.txt) | `49349c63dd1280d54b6c55d4573c672374bacc380ec192b4904852df083ace34` |
| [plotly-LICENSE.txt](plotly-LICENSE.txt) | [Plotly v2.20.0 MIT license](https://github.com/plotly/plotly.js/blob/v2.20.0/LICENSE) | `67a26cf80f03ff388f26945dfc1f6caed1a0746ff43871190339b3aee49b94bb` |
| [ieee754-LICENSE.txt](ieee754-LICENSE.txt) | [ieee754 1.2.1 BSD-3-Clause license](https://unpkg.com/ieee754@1.2.1/LICENSE) | `18d45466ba3253deae04667e267a91ea8de8548f18c1125264d1c9db28194cc1` |

The companion's filename matches the existing bundle's notice reference. It
contains upstream-extracted dependency notices. The
[tagged .gitignore](https://github.com/plotly/plotly.js/blob/v2.20.0/.gitignore)
excludes `dist/*.LICENSE.txt`, explaining why the companion was absent from the
Git tag even though it is distributed through the CDN and full npm package.
The [tagged package lock](https://github.com/plotly/plotly.js/blob/v2.20.0/package-lock.json)
pins ieee754 1.2.1; its full license accompanies the companion's abbreviated BSD
notice. The separately retained Plotly MIT text does not replace those notices.

This restores the specific missing upstream companion and the identified license
texts. It is not an exhaustive audit of every transitive dependency, a finding
about past compliance, or export-control clearance. A future bundle replacement
requires its own version-matched notices and distribution review; do not silently
reuse these hashes or assume every bundled component is MIT-licensed.
