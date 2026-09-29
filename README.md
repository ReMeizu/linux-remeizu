# ReMeizu kernel

Shared Linux 6.18 source for the ReMeizu phone ports. Development branch:
`remeizu-6.18`, based on Linux stable `v6.18.54`.

This is an experimental hardware port. No Meizu board has been validated with
an image built from this branch yet; it does not currently provide an
installable Android ROM or a GKI compatibility guarantee.

| Component | Source status | Validation |
| --- | --- | --- |
| MT6735 / MT6735M GPIO, pinmux and EINT | Added through the upstream Paris pinctrl core | Full ARM64 Image links; MT6735 register fields and pull-table bounds checked |
| MT6755 GPIO and pinmux | Added with vendor register corrections | Full ARM64 Image links; board validation pending |
| MT6735 / MT6797 clocks | Existing upstream drivers | Driver symbols verified in the common Image |
| MT6797 and MSM8953 pinctrl | Existing upstream drivers | Linked into the common Image; Meizu board integration pending |
| MT6750, MT6737, MT6753 and MT6752 variants | Planned stock-backed SoC descriptions | No compatibility alias assumed |
| Display, storage, charging, modem, cameras and suspend | Board integration pending | No hardware acceptance claimed |

The intended layout is one kernel source and shared SoC drivers, with a
separate device tree for each board. M681 and L681 target the same compiled
kernel core while retaining their different peripherals and board data.

The latest complete compile check used GCC 9.4 and source commit
[`9a1fe1954`](https://github.com/ReMeizu/linux-remeizu/commit/9a1fe1954a161d15ebdbfc530aeaba73f28e393e)
on 2026-09-29. It verified the ARM64 ELF, linked driver symbols and three
upstream reference DTBs. MT6755 GPIO32 receive-delay configuration and drive
strength in milliamps remain unsupported pending resolution of vendor data.

For an ARM64 cross-compilation check with GCC and the usual kernel build
dependencies installed:

```sh
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- remeizu.config
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j4 Image dtbs
```

The resulting `Image` is a development artifact. Reference-board DTBs are
compile controls and must not be substituted for Meizu board descriptions.

Upstream kernel documentation remains in [README](README) and
[Documentation](Documentation/). Original driver authorship and licenses are
retained in the source files.
