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

## MX6 tranche integrated

PROPER-FIX: merge sourcec429a17599d2e3c85a7418b9f7587880a122a1cd without
changing existing device owners. Real MT6351 regulator/IRQ/MFD children,
MT6797 EINT/pad/reset and MMC compatibility join the same kernel source.
Evidence: original MX6 native register/pad/IRQ audits and per-file receipts;
the merge was clean, including disjoint MT6735/MT6797 MMC/regulator wiring.
Expected next marker: actual MT6351/MT6797 objects, all five board DTBs and
full common Image, without Android/core config symbols dropping. Rollback:
source/register contradiction, lost chip match or native API error blocks
acceptance. Source presence is not a chip probe or enabled board hardware;
MX6 eMMC/PWRAP/AFE activation and all component runtime remain pending.
Verification: immutable merged-source config/object/link/DTB cloud job, then
source/stock audit and actual hardware evidence at each device's own gate.

## Android core and real legacy ashmem port

PROPER-FIX: the previously compiled defconfig lacked Binder/SELinux and the
new kernel lacks the ashmem device needed by Android9/legacy vendor userspace.
A separate remeizu_android.config selects the actual Binder/binderfs devices,
SELinux dependencies, seccomp/cgroups/BPF, dm-verity and shared-memory heaps.
Each generated symbol must be checked after olddefconfig, not merely set here.
The M5c fragment's nonexistent MTK_WATCHDOG spelling is corrected to actual
MEDIATEK_WATCHDOG, proven by the real watchdog Kconfig.

The real ashmem driver is imported from pinned latest MT6755 downstream
7de958eabcf37f07a34d8eb682c1db95ac3b7ce0, retaining its Google GPL attribution,
legacy32/64 ioctl values and the prior FMODE_LSEEK correction. No ioctl or
initialization result is stubbed. Actual source provenance and header hashes
are retained privately in fleet-android-full-port/ASHMEM_SOURCE_PORT_RECEIPT.
Owner-local adaptations use the real Linux6.18 interfaces: vfs_read with ssize_t,
loff_t seek returns, vm_flags_clear, VMA file reference transfer/vfs_mmap, and
allocated/registered shrinker with failure cleanup. Hole punching reports a
purged range only after real vfs_fallocate succeeds; scan counts real pages.

Evidence: packaged old kernel .config has Binder off; local upstream6.18
headers remove __vfs_read/shmem_set_file/register_shrinker and provide the
real replacements. The imported native driver contains actual ioctl/map/read/
pin/purge logic, not an Android9 emulation shim. Actual native compile and
QEMU-based generic kernel ABI tests are required before acceptance.
Files/why: Android driver/Kconfig/Makefile and UAPI header implement/select the
real legacy device; common Android fragment supplies missing framework core;
M5c watchdog config corrects an observed symbol mismatch; this journal links
source/API rationale and the unclosed hardware scope.
Expected next marker: Binder/ashmem/SELinux and provider objects compile/link,
then real guest Binder IPC and ashmem map/pin/purge/seek semantics. Guest tests
are generic ABI tests, not MediaTek board/peripheral acceptance.
Rollback: allocation/lifetime/refcount/ioctl regression, source/UAPI drift,
missing generated symbol or native/runtime failure blocks promotion.
Verification: frozen complete-source cloud build, actual generated-symbol and
ARM64 object/map checks; QEMU boots the resulting Image with separate owned
free-software initramfs and bounded real-userspace tests. No phone write.
Legacy ION/Mali/HWC/audio/radio/camera and remaining per-board hardware ports
still require actual implementations and evidence; this core does not close
those rows by configuration alone.

PROPER-FIX follow-up: Linux6.18 separates CPUSETS_V1 (default n) from CPUSETS.
Existing Android init/task profiles still use the legacy cpuset hierarchy;
the Android compatibility fragment selects the actual v1 controller in
addition to CPUSETS, with generated retention and real guest mount required.
No missing symbol is papered over or replaced by a dummy hierarchy.

## First complete-source native attempt: API include dependency

FACT: actual source22772ebb passed all69 generated config/value checks and
reached the23-object GCC14 build. MT6351 IRQ compilation failed because
dev_fwnode was undeclared; the implicit int then mismatched the real regmap
fwnode pointer argument. Actual6.18 include/linux/property.h declares the
typed dev_fwnode macro. PROPER-FIX: include this header in the owning IRQ
translation unit; no pointer cast, warning suppression or handler change.
The other log markers include actual Binder/binderfs/ashmem compilation.
Evidence: private native-22772ebb/NATIVE_FAILURE_R1.json and driver log.
Expected next marker: all23 real objects and full Image link, then actual
QEMU core ABI tests. Rollback: a different declaration/API requirement or
unrelated IRQ behavior change blocks promotion. Config/source pins and old
failed output are retained; next attempt uses a fresh directory/output.

## UAPI export license selection

FACT: native sourceaa140360 compiled/linked all23 real objects, retained all69
config/value assertions and produced Image/five DTBs. The generic guest's ARM32
freestanding program compiled; ARM64 userspace compilation correctly demanded
installed/exported UAPI headers. The real headers_install target then rejected
ashmem's GPL-containing SPDX expression without Linux-syscall-note.

PROPER-FIX: select Apache-2.0 for the public ashmem header under its existing
explicit original Apache2-or-GPL2 grant. Preserve the original Google author/
dual-license notice, all ABI declarations and ioctl values. Do not invent a
new GPL syscall exception or weaken the export checker. Kernel private driver
headers retain their original licensing/attribution. This is a metadata-only
UAPI change; the compiled code identity remainsaa140360 until a new build.
Expected next marker: standard headers_install sanitizes the public header
and actual ARM64 userspace builds; actual kernel/guest ABI tests follow.
Rollback: contradicting original license grant or any changed ABI token blocks
acceptance. Verification: compare old/new header after comments are removed,
run real headers_install.sh, then actual exported-header compile and guest.

## MT6755 PMIC wrapper reset provider

PROPER-FIX: export the real MT6755 infrastructure PMIC-wrapper reset output.
The modern MT6755 clock provider lacks the reset needed by a genuine PWRAP
host. Exact native MT6755 pwrap_hal.h defines set/clear offsets0x140/0x144;
pwrap_hal.c toggles bit0 through0x1 in the actual initialization sequence.
Use the existing managed MTK_RST_SET_CLR controller with one native-backed
reset ID only. Probe propagates registration errors; no status, unrelated
reset bit, display route, forced-ready or physical board activation is added.
Files/why: typed reset binding, infrasys driver map/registration, SoC reset
cells and conditional clock schema describe the implemented provider.
Expected: real ARM64 clock/reset objects, schema-compatible DTBs and a PWRAP
consumer acquiring reset0 when its own verified resources are wired.
Rollback: native register/bit contradiction, registration/compiler/DTB error,
or unrelated clock/reset behavior regression blocks promotion.
Verification: source offset/bit audit, actual compiler/Kbuild/link and DTB
roundtrip; later actual PWRAP/PMIC hardware readback. Current Imageaa140360
predates this change; no new native or hardware acceptance is claimed yet.
The full PWRAP register/capability/clock/PMIC host remains a separate current
implementation task, not solved by this reset descriptor alone.
