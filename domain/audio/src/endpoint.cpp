// The module's platform file (endpoint.h): every Windows include in domain/audio is here. The
// Windows half is COM — an IMMNotificationClient registered with the endpoint enumerator. Elsewhere
// there is nothing to watch with: PulseAudio moves a stream that follows the default sink by itself
// and miniaudio reports that as `rerouted`, and ALSA says nothing (docs/subsystems/audio.md,
// "Device changes at run time").
#include "endpoint.h"

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// The endpoint API assumes windows.h has been seen.
#include <mmdeviceapi.h>
// The standard library after the platform's.
#include <cwchar>
#include <new>
#endif

namespace engine::audio {

#if ENGINE_PLATFORM_WINDOWS

// The endpoint watcher. Its callbacks arrive on a thread of the audio service's choosing, and each
// does exactly one thing: set a `DeviceEvent` bit in the output's flags. Whatever follows — closing
// the device, opening the new default, logging — happens on the controlling thread, in
// `Output::update()`, because doing it here would race the device's own thread and the tick.
struct Output::Watcher final : public IMMNotificationClient {
  std::atomic<ULONG> references{1};
  std::atomic<u32>* events = nullptr;
  // The endpoint the output plays to, as IMMDevice::GetId spells it; empty when it fell back to
  // the null backend and only a new default matters. miniaudio's id holds 64 characters.
  wchar_t ours[64] = {};
  IMMDeviceEnumerator* enumerator = nullptr;
  bool owes_uninitialize = false;

  void raise(DeviceEvent event) noexcept {
    events->fetch_or(static_cast<u32>(event), std::memory_order_release);
  }
  bool is_ours(LPCWSTR id) const noexcept {
    return id != nullptr && ours[0] != L'\0' && std::wcscmp(id, ours) == 0;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return references.fetch_add(1) + 1; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = references.fetch_sub(1) - 1;
    if (left == 0) delete this;
    return left;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }

  // Sent once per role; `eConsole` is the one a game plays to, and the one miniaudio's default is.
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
    if (flow == eRender && role == eConsole) raise(DeviceEvent::DefaultChanged);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
    if ((state & DEVICE_STATE_ACTIVE) == 0 && is_ours(id)) raise(DeviceEvent::Lost);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
    if (is_ours(id)) raise(DeviceEvent::Lost);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
    return S_OK;
  }
};

namespace endpoint {

Output::Watcher* watch(std::atomic<u32>* events, const IdChar* ours) noexcept {
  // S_OK or S_FALSE: this thread is in the multithreaded apartment now and owes one
  // CoUninitialize. RPC_E_CHANGED_MODE: it is already single-threaded, which serves as well — the
  // enumerator's callbacks come on the service's own thread either way.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool owes = SUCCEEDED(com);
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator)))) {
    if (owes) CoUninitialize();
    return nullptr;
  }
  auto* watcher = new (std::nothrow) Output::Watcher;
  if (watcher == nullptr) {
    enumerator->Release();
    if (owes) CoUninitialize();
    return nullptr;
  }
  watcher->events = events;
  if (ours != nullptr) wcsncpy_s(watcher->ours, ours, _TRUNCATE);
  if (FAILED(enumerator->RegisterEndpointNotificationCallback(watcher))) {
    watcher->Release();
    enumerator->Release();
    if (owes) CoUninitialize();
    return nullptr;
  }
  watcher->enumerator = enumerator;
  watcher->owes_uninitialize = owes;
  return watcher;
}

void unwatch(Output::Watcher* watcher) noexcept {
  if (watcher == nullptr) return;
  IMMDeviceEnumerator* enumerator = watcher->enumerator;
  const bool owes = watcher->owes_uninitialize;
  // No callback starts after this returns; the reference the enumerator's registration relied on
  // is then ours to drop.
  enumerator->UnregisterEndpointNotificationCallback(watcher);
  enumerator->Release();
  watcher->Release();
  if (owes) CoUninitialize();
}

}  // namespace endpoint

#else

namespace endpoint {

Output::Watcher* watch(std::atomic<u32>*, const IdChar*) noexcept { return nullptr; }
void unwatch(Output::Watcher*) noexcept {}

}  // namespace endpoint

#endif

}  // namespace engine::audio
