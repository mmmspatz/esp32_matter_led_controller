# hal-patches

Patches applied to `modules/hal/espressif` by `bootstrap.sh` after every
`west update` (which resets the module tree, discarding uncommitted edits).

Each patch should be small, carry a comment header explaining why it exists,
and reference an upstream issue/PR where one has been filed. The goal is for
this directory to trend toward empty.

Current set:
- `0001-recursion-safe-esp-os-critical-section.patch` — the Zephyr-port
  `esp_os_*_critical*` macros stored the `irq_lock()` key in the lock word and
  broke on nested acquisition of the same lock. The ESP32-C6 BLE controller
  init nests the modem-clock lock, so IRQs were left disabled and the BLE host
  panicked on its first blocking wait. Fix uses a shared depth counter + saved
  key (single-core, LIFO). Upstream candidate.
