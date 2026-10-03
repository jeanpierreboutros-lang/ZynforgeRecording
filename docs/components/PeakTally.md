# PeakTally — removed component

The global clip-tally bar was removed at the engineer's request. `Source/UI/PeakTally.h` and its host wiring are no longer present in source build `c563b00`.

Clip indication remains on each strip's [LedMeter](LedMeter.md), including its clip pip / CLIP count. Click a meter to clear its latch. The [master meter](MasterStrip.md) follows the same local clear interaction.

The older global four-pixel pulsing bar and click-to-clear-all API are historical behavior recorded in [CHANGELOG.md](../../CHANGELOG.md). Do not instantiate PeakTally or use its former API as a current component contract.
