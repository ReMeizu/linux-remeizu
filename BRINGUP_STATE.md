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

## Complete Android integration work, 10 October 2026 — M5c source merge

User requires complete latest-downstream/component/Android integration, not
only a mainline native build. The full17-board/six-family scope remains.
PROPER-FIX: integrate previously audited M5c MT6735M/MT6737M PWRAP, MT6328
regulator/keys and MMC source1ebc6f2ed6b5bf56ab47e4fd943948d61de22119 with
existing tested MT6755/U10 source995de406. Merge conflicts were only disjoint
board enumeration/DTB Makefile additions; both identities are retained.
Evidence: existing m5c source/stock/register audits and standalone DTB receipt;
no actual C/runtime acceptance is added before the new native build.
Files/why: the M5c tranche supplies actual providers/resources; this journal
records the integration; YAML/Makefile retain all four separate board DTBs.
Expected next marker: native MT6328/PWRAP/keys/MMC objects and full Image/DTB
from the same source, together with Android-core configuration checks.
Rollback: stock/source contradiction, native API error or lost board addition
blocks promotion. Verification: source audit fixtures, generated config,
actual ARM64 object/full Image make and per-board DTB identity. Boot/runtime
remain open. Original branches/current phone owners are not modified.
