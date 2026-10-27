# PBR material enlargement: StableMaterials, PBRify and Krea2

- **Question:** can image upscaling or material-map conditioning improve generated PBR textures without losing their correspondence? This is the first local prototype for [G30](../roadmap.md), under [07 §7.7](../plan/07-content-pipeline.md#77-ml-asset-generation-novelty-content).
- **Date:** 2026-10-18. **Machine:** Windows development workstation, 64 GiB RAM, RTX 5090 32 GiB. **Runtime:** ComfyUI 0.41.3, PyTorch 2.10.0+cu130; Blender 5.2.2 LTS for CPU previews.
- **Machine state:** shared workstation; GPU inference held `tools/gpu-lock.ps1` and checked the owner's main ComfyUI queue before starting. NVIDIA utilization/memory snapshots are in each local execution record. CPU activity was not sampled, and no quiet-machine performance claim is made. The preview renderer used eight CPU threads.
- **Decision:** keep texture-specific colour enlargement as the baseline for further experiments. This height-guided Krea pass is not suitable for retaining the original material maps unchanged. G30 remains in progress; no production backend or shipping material is established.

## Setup and reproduction

All scripts, weights, editable workflows and generated assets are local, under `D:\workspace\game_engine_local\workflows\pbr-material-lab`. Nothing generated is committed. `README.txt` in that folder explains the launcher and limitations. From PowerShell:

```powershell
pwsh -NoProfile -File D:\workspace\game_engine_local\workflows\pbr-material-lab\launch.ps1 -Mode Generate
pwsh -NoProfile -File D:\workspace\game_engine_local\workflows\pbr-material-lab\launch.ps1 -Mode Open
```

`Generate` runs the comparison under the GPU lock, shuts down its isolated ComfyUI server, renders CPU previews through `tools/blender-run.ps1`, then assembles the report. `Open` serves the editable workflows on loopback port 8001 until Enter is pressed in its terminal; that terminal holds the GPU lock. The owner's port-8000 instance and its installed nodes are unchanged. The lab shares the installed Python/core and existing Krea weights, but has separate model additions, nodes, input, output and user folders. Its startup queue check cannot prevent the owner from subsequently submitting work to the main instance.

StableMaterials used revision `4d452731535bd5c74261d6645623a573326f6f36`, 50 steps, CFG 7.5, seed 1837 and tileable generation. The prompt requested warm beige limestone with pores, shallow pits, mineral grains and cream/ochre mottling. It produced one 512-square material with base colour, normal, height, roughness and metallic maps.

| Branch | Colour processing | Other maps |
|---|---|---|
| A | Circular padding and bilinear enlargement, 512 to 2048 | Same resize; normals decoded to vectors, interpolated, normalized and re-encoded |
| B | Circular pad 64 pixels, PBRify UpscalerV4 4x, crop 256 pixels from every edge | A's maps |
| C | B colour reduced to 1024; Krea2 Turbo image-to-image conditioned on source height through its depth Control LoRA; bilinear enlargement to 2048 | A's maps |

The installed Krea stack was `krea2_turbo_int8_convrot.safetensors`, `qwen3vl_4b_bf16.safetensors`, `qwen_image_vae.safetensors` and `krea2-depth-control-lora.safetensors`. C used 16 Euler/simple steps, CFG 1, seed 1837 and LoRA weight 0.8. Height was interpreted as front-facing depth with high values nearer the camera: `0.5 + (height - 0.5) * relief_strength`. It was not min/max normalized. Denoise and relief-strength combinations were 0.25/0.3, 0.25/0.7 and 0.45/0.3. This is an approximation of depth conditioning, not a physically calibrated surface camera.

The report at `results\comparison.html` includes whole colour, identical 512-pixel crops, 2-by-2 repeats and fifteen fixed-camera renders: five materials under soft, grazing and opposite-side light. A repeating plane, bevelled block and sphere share the maps. Cycles used 32 samples and denoising. No displacement was used. Normal convention was interpreted as OpenGL tangent space, without calibration against the source height.

## Results

| Branch | Downsample colour MAE | Seam jump / adjacent-pixel difference, X | Y |
|---|---:|---:|---:|
| A: plain enlargement | 0.0057 | 1.06 | 1.15 |
| B: PBRify | 0.0121 | 1.02 | 0.94 |
| C: denoise 0.25, relief 0.3 | 0.0852 | 3.08 | 3.48 |
| C: denoise 0.25, relief 0.7 | 0.0882 | 2.98 | 3.56 |
| C: denoise 0.45, relief 0.3 | 0.1373 | 3.11 | 3.90 |

MAE compares each enlarged colour downsampled with Lanczos to the original 512-square colour, on RGB values in [0,1]. It measures source drift, not accuracy against a high-resolution ground truth. The seam ratios compare the mean absolute difference across opposite edges with the mean difference between interior adjacent pixels in the same direction. A ratio near one is useful evidence, not a proof of seamlessness.

PBRify sharpened grain while retaining the broad fracture layout and approximately the baseline edge continuity. Krea introduced new aggregate-like colour features, moved parts of the pattern and increased edge discontinuities. Increasing relief strength from 0.3 to 0.7 did not resolve this in the tested pair. All three C variants retained the original normal/roughness/metallic maps, so their new colour features have no corresponding new material detail. Relighting makes that limitation easier to inspect.

The source itself missed the requested appearance: saturated yellow instead of beige, with strong-looking relief. None of these branches makes it a convincing finished limestone material. The comparison deliberately kept this source fixed to isolate each processing step.

## Lessons for the workflow

Image files encode different things. Base colour can benefit from a texture-trained colour upscaler. A normal map encodes vectors; roughness and metallic encode bounded parameters. Independently applying a colour upscaler to all of them is not a coherent PBR refinement method. Vector renormalization repairs vector length but cannot make hallucinated normals physically consistent with height.

The verified Krea control checkpoint accepts depth semantics. Being able to encode another image does not make that checkpoint understand metallic or tangent-space normals. No suitable Krea normal/metallic control weights were established in this experiment. Height-to-depth is a viable executable input path, but these results do not establish it as a preservation constraint.

For procedural models, the next candidate is a screened tileable source, wrapped texture upscaling for colour, preserved material masks, and deliberately shared height detail for any new micro-normal. Object-specific normal/AO bakes belong to the mesh. For generated-model UV atlases, use island padding and masks, retain the tangent basis and bake geometry, and validate seams on the mesh; a whole-atlas diffusion pass can mix unrelated islands. This experiment has not tested atlas repair.

## Reproducibility and limits

The four API graphs executed successfully, and their required nodes/inputs were checked against the lab's object catalogue. Editable UI JSON exports accompany them; the StableMaterials/upscaler graph was also imported in the ComfyUI frontend. The three Krea UI exports were not individually imported. Execution records include graph parameters, output hashes, source-model revision, local runner/node hashes, Comfy history and GPU snapshots. `results\materials` contains five complete 2K sets, with ORM packed as white AO, roughness, metallic. Experimental generation records are not yet the engine's `asset_provenance` schema; Blender captures have the wrapper's sidecars.

The lab uses a local custom StableMaterials node because a community wrapper's image argument did not match the upstream pipeline. The pinned custom pipeline requires `trust_remote_code=True` even when loaded locally through its class with current Diffusers; execution is offline after download. Its tileable code also consumes the global Torch RNG, so the node seeds both that RNG and the explicit generator inside a forked RNG scope. WebSocket client support was installed only into the lab's dependency folder.

There is one material and one seed, no high-resolution reference, no metallic-material test and no calibrated displacement. Saved data maps are 8-bit PNGs in this first comparison. C is generated at 1024, not native 2048. The nominal 0.5 m tile size in material metadata is illustrative. Normal direction, height amplitude and roughness calibration need explicit checks before production use. The square-map resize node is not an atlas-aware filter. The full Krea weight stack's redistribution obligations are not resolved by this experiment.

## Sources

- [StableMaterials model and pinned pipeline](https://huggingface.co/gvecchio/StableMaterials/tree/4d452731535bd5c74261d6645623a573326f6f36): source model, pipeline and OpenRAIL metadata.
- [PBRify Remix](https://github.com/Kim2091/PBRify_Remix): texture upscaler and CC0 weight metadata; this run used UpscalerV4 from `v1.7.2_ComfyOnly`.
- [Krea2 control nodes](https://github.com/facok/comfyui-krea2-controlnet) and [depth checkpoint](https://huggingface.co/Patil/Krea-2-depth-controlnet): executable depth-conditioning path.

These are published inference models used without training. Model/code licenses remain separate from generated-output provenance and require the project's normal review before redistribution.
