#pragma once

// **What the displays are and what a window on each is offered** (E39's first measurement,
// docs/experiments/hdr-output-proposal.md; docs/subsystems/gfx.md, "The surface's offers"): every
// display output Windows reports — its name, its rectangle, the bits it is driven at, its colour
// space (which says whether Windows' "Use HDR" is on), the luminances DXGI reports for the panel
// (`IDXGIOutput6::GetDesc1`) and Windows' SDR white level for it — and, for every Vulkan device,
// the formats, colour spaces and present modes a surface on a hidden window placed on that output
// is offered, on an instance with VK_EXT_swapchain_colorspace (without which a surface lists sRGB
// alone). The protocol's `gpu.displays` returns it; engine-view reads `display_outputs` for the
// output its window is on, to default the HDR encode's peak and paper white to what the display
// reports.
//
// **It shows nothing and changes nothing.** The windows are never shown (a Win32 popup without
// WS_VISIBLE, destroyed before the call returns), no device is opened — so it takes no GPU lock —
// and nothing here writes a display setting: Windows' HDR switch and the driver's control panel
// are the owner's.
//
// **Windows only.** Vulkan reports no luminance at all, and the surface needs a window of the
// platform's; DXGI and a Win32 window are the shortest way to both. Elsewhere both calls return
// false with a sentence, and a presenting host's own swapchain reports its window's offers
// (backend/vulkan/swapchain.h, `Swapchain::offered`).

#include <core/base/types.h>
#include <core/containers/vector.h>

#include <schemas/gfx.h>
#include <span>
#include <string>

namespace engine::gfx {

// The outputs and, per Vulkan device, what a hidden window on each is offered.
// `swapchain_colorspace` says whether the loader had the extension. False, with `error`, when the
// probe cannot run (no loader, not Windows, DXGI refused); an output whose surface could not be
// made is reported with no `surfaces`.
bool probe_displays(Vector<DisplayOutput>& out, bool& swapchain_colorspace,
                    std::string* error = nullptr);

// The outputs alone: DXGI and the display configuration, no Vulkan, no window. What a presenting
// host reads for the display its window is on. False off Windows.
bool display_outputs(Vector<DisplayOutput>& out, std::string* error = nullptr);

// The output whose desktop rectangle holds the point (x, y), else the one at the desktop's origin
// (the primary), else the first; null for none.
const DisplayOutput* output_at(std::span<const DisplayOutput> outputs, i32 x, i32 y) noexcept;

}  // namespace engine::gfx
