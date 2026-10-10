# MT675x Compile Farm integration — 10 October 2026

FACT: this isolated source owner starts at U10 commit
`4207cb32d98f9cbf9aa47a90fe85d9af05f66748`, including the shared M681/L681
clock-provider tranche. Existing device branches and public base are preserved.
The first native farm attempt verified all 26 source input hashes and generated
the required five enabled symbols, then stopped with driver-object make RC2.
No full Image, board boot package, phone operation or runtime acceptance exists
from that failed attempt.

Private evidence:
`/home/n8n/cloud_oss_builds/outreach-20261010/platform-mt675x-4207cb32/NATIVE_FAILURE_R1.json`;
the same directory retains driver_objects.log and a log_triage_pack.py bundle.
Remote failed source/output/logs remain in
`/home/remeizu/remeizu-platform-mt675x-20261010-4207cb32`.

## Patch history

PROPER-FIX: include the real IRQ handler declaration before timer-of.h.

Hypothesis: the secure-counter translation unit does not include the header
defining irq_handler_t, while its timer-of.h dependency embeds that type even
when this driver uses only base/clock resources.

Evidence: GCC14 reports timer-of.h:16 "unknown type name irq_handler_t" while
compiling timer-mediatek-cpux-secure.c. The actual Linux header
include/linux/interrupt.h:104 declares irq_handler_t. The adjacent existing
timer-mediatek-cpux.c includes linux/interrupt.h before timer-of.h. The first
native compiler failure is header/type visibility, not a hardware register or
secure-call failure.

Files changed/why: timer-mediatek-cpux-secure.c adds linux/interrupt.h, bringing
the actual kernel typedef into scope; this record preserves the failed native
attempt and patch justification. There is no warning suppression, local typedef,
driver disable, control-flow change, register change, or fake initialization.

Expected next marker: all six required actual ARM64 driver objects compile;
full Image/DTBs and linked symbols are checked by the unchanged frozen worker.
Rollback condition: new native compiler evidence contradicts the include
dependency, or source/API/driver behavior drifts outside this correction.

Verification: repeat the recorded GCC14 ARCH=arm64 build command with the same
defconfig + remeizu.config + meizu_u10.config in a fresh named farm attempt;
require unchanged generated config, native driver make zero, full Image link,
ARM64 ELF machine183 and the three M681/L681/U10 DTBs with exact hashes.
Keep the original failed attempt. Source journal/commit history is excluded
from the free-software code-only farm overlay.

FACT: Build Station /api/health returned HTTP200, but /openapi.json returned
404 and /api/openapi.json timed out in the bounded intake. This independently
launched farm worker has no registered station build ID. Direct native-source
fallback is explicit; do not attribute the fix to station auto-fix or another
build's logs. Actual kernel compilation remains the acceptance gate, replacing
the earlier stub-only type context with real kernel headers.

## R2 native and independent verification PASS

FACT: the frozen code at995de406b6bc5cfa61de9918726f4b15b8756c65 completed
all configuration, six actual ARM64 driver objects and full Image/dtbs builds
with native RC0 on GCC14.2. Elapsed553.7s including preparation. Independent
verification rehashed13artifact paths and26source files, checked ARM64 ELF,
five linked driver symbols and zero-warning roundtrip for all three DTBs.
Generated config56273824896ecfc3fe9f33db54ff5482cb3390c067cac088a0039603c91707b5
is byte-identical to failed R1; only the missing header was corrected.
Image41474560B/SHA1d866dd6c9966e104e905a2508f2c21f78f26f2426c70d4831eebdac3f1b2565.
The first R2 SSH launch response was lost; a read-only check recovered actual
native progress, and the launch was not repeated. Exact verification/source
receipts remain in cloud_oss_builds/outreach-20261010/platform-mt675x-r2-995de406.
Package/root report: meizu-fleet/maintenance/builds/CFARM_MT675X_20261010.md.
No boot.img, hardware acceptance, common-branch merge or publication is claimed.
This follow-up is documentation only; the built source identity remains995de406.
