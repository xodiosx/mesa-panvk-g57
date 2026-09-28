# Mesa PanVK Mali-G57 — kbase JM / Android

Experimental Mesa PanVK work for **Mali-G57 / Valhall** using the Arm **kbase JM (Job Manager)** interface on Android/Adrenotools/termux 

> [!WARNING]
> Experimental / WIP. Not currently a conformant or production-ready Vulkan implementation.

## Target
- GPU: Mali-G57 MC2
- Architecture: Valhall
- Backend: kbase JM
- Tested uAPI: 11.46
- Environment: Android / Termux /adrenotools
- Window system: Termux:X11
- Driver: Mesa PanVK

##l Mesa PanVK Mali-G57 — kbase JM / Android

Experimental Mesa PanVK Vulkan driver for **ARM Mali-G57 MC2 / Valhall** using the Arm **kbase JM (Job Manager)** interface on Android / Termux.

> [!NOTE]
> This driver enables hardware-accelerated Vulkan on Mali-G57 inside Termux:X11 without requiring Linux mainline DRM/KMS `panfrost.ko`.

---

## Target Hardware & Environment
* **SoC:** MediaTek Dimensity 6300 (MT6835)
* **GPU:** ARM Mali-G57 MC2 (Valhall v9?)
* **Architecture:** Valhall v9? (Job Manager / JM)
* **Kernel Driver:** ARM `kbase` (`/dev/mali0`, uAPI JM 11.38 / 11.46)
* **Environment:** Android / Termux
* **Display Server:** Termux:X11 (via MIT-SHM `userbuf` import)
* **Driver:** Mesa panVcake (`libvulkan_panfrost.so`)

---

## Confirmed Working & Benchmarks

* **Device & Queue:** Mali-G57 MC2 detection, device initialization, queue creation via `kbase` JM uAPI.
* **WSI / Display:** Termux:X11 swapchain presentation via `userbuf` host import and MIT-SHM blit.
