# RADV WSI allocation residency repro

One 1×1 RGBA8 image, one output buffer, two GPU submissions. The first clears the image and transitions it for sampling. The second samples it once through a combined image/sampler descriptor and writes the result to the output buffer. No Gamescope, Steam, game, capture, window system or presentation is involved.

On Linux with RADV, a C compiler, Vulkan headers/loader, libdrm headers, glslangValidator, pkg-config and Python 3:

```sh
./repro.sh
```

The script compiles the program and ioctl tracer, then executes four variants with five-second per-case timeouts. It emits JSON summaries and saves raw mapping/submission traces. All variants use the same image, allocations, shader and commands. Only the allocation's private WSI structure changes. `run.py` selects RADV, requires exactly two submissions, identifies the image by mappings within its allocation interval, and checks initialization/output-buffer controls before reporting the sampled image's BO membership. The target is 256 Vulkan bytes / one 4 KiB mapped page on the tested device; the output buffer is 4 KiB.

Observed on CachyOS, AMD BC-250, Mesa/RADV 26.2.4:

| Variant | Image in initialization BO list | Image in compute BO list |
| --- | --- | --- |
| Legacy hint | Yes | No |
| PR 2036: add `dma_buf_sync_file=false` | Yes | No |
| PR 2247: add that field plus six reserved booleans | Yes | No |
| Remove allocation hint | Yes | Yes |

The output-buffer control is present in all four compute submissions. Each process took 22–23 ms. These runs reproduce the missing-residency mechanism; they did not reproduce a GPU hang. No memory pressure is applied.

Legacy padding is deliberately zeroed. All three hint structures occupy 24 bytes on this ABI. This tests the missing-residency effect with defined false fields; it does not reproduce the separate undefined-padding bug reported upstream. The upstream fixes can be independently useful while leaving this residency issue unresolved.

The tracer wraps `ioctl`/`drmIoctl` without changing calls or results. It records GEM VA mappings and `AMDGPU_CHUNK_ID_BO_HANDLES` before submission. libdrm is preloaded explicitly because this standalone Vulkan executable does not otherwise link it globally. Existing trace data and `image-results.json` preserve the observed run; rerunning writes current data.

Sources: https://github.com/ValveSoftware/gamescope/pull/2036 and https://github.com/ValveSoftware/gamescope/pull/2247. The private WSI type value and field layout mirror those sources. This is a diagnostic for the observed RADV allocation/submission contract, not a portable public-Vulkan conformance test.
