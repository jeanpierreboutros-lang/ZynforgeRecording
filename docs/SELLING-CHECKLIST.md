# Before selling ZynForge Recording

The owner's checklist. Facts were checked on 2026-10-31; re-check the prices and terms before acting.
Nothing here has been bought, signed or accepted for the owner.

## JUCE licence

The app is built on JUCE 8.0.4, which is dual-licensed: AGPLv3 or the commercial JUCE 8 licence.
Sources: <https://juce.com/legal/juce-8-licence/> (EULA, last amended 17 July 2025) and
<https://juce.com/get-juce/>.

| Tier | Price | Revenue or funding limit, previous 12 months |
| --- | --- | --- |
| Starter | Free, perpetual | Up to $20,000 |
| Indie | $40 per user per month (1 month minimum), or $800 perpetual per user | Up to $300,000 |
| Pro | $175 per user per month (12 month minimum), or $3,500 perpetual per user | No limit |
| Educational | Free | No limit, but qualifying educational and academic use only |

What selling a closed-source app requires, from the EULA:

- A closed-source binary needs a JUCE licence, maintained for at least as long as such binaries are distributed.
- Clause 1.2.1: for an individual, the limit counts all revenue from using the framework. For a company it counts total revenue of the entity and its affiliates from all sources, with no offsets.
- Clause 1.7: everyone who changes code that depends on JUCE needs a seat. Build and test machines do not.
- Clause 1.9: the owner may distribute and sell the product under the agreement.
- Clause 1.10: subscription seats must stay active to keep distributing. If a subscription lapses, distribution must stop unless matching perpetual seats are bought.
- Clause 1.13: Starter, Indie and Pro cannot be mixed at the same time.
- Clause 2.3: nothing may be done that would make JUCE subject to a licence requiring source disclosure, derivative licensing or free redistribution.
- No splash screen is required. JUCE 8 ignores the `JUCE_DISPLAY_SPLASH_SCREEN` flag this project still sets, and the EULA text has no splash clause.

The AGPLv3 route means conveying the whole app's source under the AGPLv3, which cannot coexist with an
all-rights-reserved closed product. `LICENSE` says all rights reserved, so distribution now rests on the JUCE
licence, and that includes the DMGs already in `dist/` on this public repository.

## Apple

An Apple Developer ID is needed before `tools/notarize.sh` can produce a build that opens without a Gatekeeper warning.
See [INSTALL.md](../INSTALL.md) and the notarized-build steps in [FIELD-TEST.md](../FIELD-TEST.md).

## Notices must travel with binaries

FLAC, Ogg Vorbis, libpng, zlib, HarfBuzz and the OFL font all require their notices in binary distributions, and the
IJG licence requires the sentence "this software is based in part on the work of the Independent JPEG Group".
`notarize.sh` copies `LICENSE` and `THIRD-PARTY-NOTICES.md` into the DMG root. Nothing in the app's UI shows them yet.

## Worth a look

- JUCE 8.0.4 vendors zlib 1.2.3 (2005) and libpng 1.6.37. Both predate published CVEs. They are only reached by this app's own PNG and gzip use, but a JUCE update is the way to move them.
- The app does not call `F_FULLFSYNC`. It goes through JUCE `FileOutputStream::flush`, which is plain `fsync`, and on macOS that does not force the drive's cache to the platter or flash. A power cut can therefore lose what `fsync` reported as written. This is an owner decision; nothing was changed.
