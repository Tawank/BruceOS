# BruceOS libjpeg-turbo fork

This component was vendored from the `espressif/libjpeg-turbo` registry
package, version `3.1.1~2`, as fetched into `managed_components/` by the
ESP-IDF component manager. It's forked here (instead of left as a plain
managed dependency) for one patch:

`libjpeg-turbo/src/jmorecfg.h` undefines `BLOCK_SMOOTHING_SUPPORTED`.

Progressive-JPEG decoding always needs a full-image virtual coefficient
array per component, but only a small "resident window" of it has to be an
actual contiguous heap allocation at once (the rest streams through a
backing store). `jdcoefct.c`'s `_jinit_d_coef_controller()` sizes that
window as `v_samp_factor` rows -- multiplied by 5 whenever
`BLOCK_SMOOTHING_SUPPORTED` is compiled in and the image is progressive,
"just in case" block smoothing gets turned on for that decode. BruceOS
never turns it on: `core/image/jpg/jpg.c` sets `do_block_smoothing = FALSE`
unconditionally on its one decode call site. On memory-constrained boards
(no PSRAM, a few tens of KB of contiguous internal RAM) that 5x pad was the
difference between a progressive JPEG's minimum resident window fitting in
the largest free block and not -- see the "jpg not loading" investigation
this fork came out of.

Everything else is untouched upstream source. Re-vendor by re-fetching
`espressif/libjpeg-turbo` into a scratch `managed_components/` and
re-applying the `jmorecfg.h` change above if bumping the pinned version.
