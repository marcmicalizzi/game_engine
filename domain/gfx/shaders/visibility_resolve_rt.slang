// The material resolve with ray-traced shadows: visibility_resolve.slang compiled a second time
// with the shadow rays in, same entry points (vs_fullscreen, fs_resolve) and same ResolveParams.
//
// It is a second module rather than a runtime branch because the rays read `g_scenes[]` at
// binding 3 of the bindless set, and a device without acceleration structures has no binding 3:
// a pipeline whose shader declares a descriptor the layout does not have is invalid, so the
// module every device draws with cannot declare it. An app that has the structures picks this
// one; the baseline tier keeps the plain one, and both come from the one source file.

#define RESOLVE_RAY_SHADOWS 1
#include "visibility_resolve.slang"
