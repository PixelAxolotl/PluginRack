#include "WindowsEndpointFormat.h"

#if JUCE_WINDOWS
 #include <mmdeviceapi.h>
 #include <mmreg.h>
 #include <objbase.h>
 #include <propidl.h>
 #include <cstring>
#endif

namespace pluginrack::endpointformat {

#if JUCE_WINDOWS

namespace {

// Releases on scope exit so every early return below stays leak-free.
template <typename ComClass>
class ComRef
{
public:
    ComRef() = default;
    ComRef (const ComRef&) = delete;
    ComRef& operator= (const ComRef&) = delete;
    ~ComRef() { if (p != nullptr) p->Release(); }

    ComClass* operator->() const noexcept { return p; }
    ComClass* get() const noexcept { return p; }
    explicit operator bool() const noexcept { return p != nullptr; }

    ComClass** resetAndGetPointerAddress()
    {
        if (p != nullptr)
        {
            p->Release();
            p = nullptr;
        }
        return &p;
    }

private:
    ComClass* p = nullptr;
};

GUID makeGuid (unsigned long a, unsigned short b, unsigned short c,
               const unsigned char (&tail)[8])
{
    GUID g {};
    g.Data1 = a; g.Data2 = b; g.Data3 = c;
    for (int i = 0; i < 8; ++i) g.Data4[i] = tail[i];
    return g;
}

// PKEY_Device_FriendlyName: the value JUCE names its WASAPI endpoints with.
PROPERTYKEY friendlyNameKey()
{
    PROPERTYKEY key {};
    key.fmtid = makeGuid (0xa45c254e, 0xdf1c, 0x4efd,
                          { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 });
    key.pid = 14;
    return key;
}

GUID ksdFloatSubFormat()
{
    return makeGuid (0x00000003, 0x0000, 0x0010,
                     { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 });
}

GUID policyConfigVistaClassId()
{
    return makeGuid (0x294935ce, 0xf637, 0x4e7c,
                     { 0xa4, 0x1b, 0xab, 0x25, 0x54, 0x60, 0xb8, 0x62 });
}

GUID policyConfigVistaInterfaceId()
{
    return makeGuid (0x568b9108, 0x44bf, 0x40b4,
                     { 0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20 });
}

// The client is the documented-in-the-wild Vista-era one; the newer client does
// not answer for this interface on current Windows builds. Its member order is
// not published, so only the three members used here are declared. GetMixFormat
// leads the list but returns "not supported" for capture endpoints, which is
// why the device format is read instead.
struct IPolicyConfigVista : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE getMixFormat (PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE getDeviceFormat (PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE setDeviceFormat (PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};

bool looksLikeAudioFormat (const WAVEFORMATEX* format)
{
    if (format == nullptr)
        return false;

    return format->nChannels > 0 && format->nChannels <= 64
        && format->nSamplesPerSec >= 8000 && format->nSamplesPerSec <= 384000
        && (format->wBitsPerSample == 8 || format->wBitsPerSample == 16
            || format->wBitsPerSample == 24 || format->wBitsPerSample == 32);
}

// The endpoint id is what PolicyConfig keys on; the app only knows the name.
juce::String findEndpointId (bool wantInput, const juce::String& name)
{
    ComRef<IMMDeviceEnumerator> enumerator;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (enumerator.resetAndGetPointerAddress()))))
        return {};

    ComRef<IMMDeviceCollection> devices;
    if (FAILED (enumerator->EnumAudioEndpoints (wantInput ? eCapture : eRender,
                                                DEVICE_STATE_ACTIVE,
                                                devices.resetAndGetPointerAddress())))
        return {};

    UINT count = 0;
    if (FAILED (devices->GetCount (&count)))
        return {};

    const auto key = friendlyNameKey();

    for (UINT i = 0; i < count; ++i)
    {
        ComRef<IMMDevice> device;
        if (FAILED (devices->Item (i, device.resetAndGetPointerAddress())))
            continue;

        ComRef<IPropertyStore> store;
        if (FAILED (device->OpenPropertyStore (STGM_READ, store.resetAndGetPointerAddress())))
            continue;

        PROPVARIANT value;
        PropVariantInit (&value);
        const auto read = store->GetValue (key, &value);
        const juce::String friendly (SUCCEEDED (read) && value.vt == VT_LPWSTR && value.pwszVal != nullptr
                                         ? juce::String (value.pwszVal)
                                         : juce::String());
        PropVariantClear (&value);

        if (friendly.isEmpty() || friendly != name)
            continue;

        LPWSTR id = nullptr;
        if (SUCCEEDED (device->GetId (&id)) && id != nullptr)
        {
            const juce::String result (id);
            CoTaskMemFree (id);
            return result;
        }
    }

    return {};
}

// Keeps the endpoint's channels, bit depth and sub-format and only moves the
// rate, so the driver is asked for a format it already accepted.
void buildTargetFormat (const WAVEFORMATEX* current, double sampleRate, WAVEFORMATEXTENSIBLE& out)
{
    std::memset (&out, 0, sizeof (out));

    if (current != nullptr)
    {
        const auto extra = static_cast<size_t> (current->cbSize);
        const auto maxExtra = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
        std::memcpy (&out, current, sizeof (WAVEFORMATEX) + juce::jmin (extra, maxExtra));
    }

    if (out.Format.nChannels == 0)
    {
        out.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        out.Format.nChannels = 2;
        out.Format.wBitsPerSample = 32;
        out.Format.nBlockAlign = 8;
        out.Format.cbSize = static_cast<WORD> (sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
        out.Samples.wValidBitsPerSample = 32;
        out.dwChannelMask = 3;
        out.SubFormat = ksdFloatSubFormat();
    }

    out.Format.nSamplesPerSec = static_cast<DWORD> (sampleRate);
    out.Format.nAvgBytesPerSec = out.Format.nSamplesPerSec * out.Format.nBlockAlign;
}

// Returns "" when the endpoint reports `sampleRate` afterwards.
juce::String applyThrough (IPolicyConfigVista* policy, const juce::String& endpointId,
                           double sampleRate)
{
    const auto idText = endpointId.toWideCharPointer();

    WAVEFORMATEX* before = nullptr;
    if (FAILED (policy->getDeviceFormat (idText, 0, &before)) || ! looksLikeAudioFormat (before))
    {
        if (before != nullptr) CoTaskMemFree (before);
        return "endpoint format unavailable";
    }

    const bool alreadyThere = std::abs (static_cast<double> (before->nSamplesPerSec) - sampleRate) < 1.0;
    if (alreadyThere)
    {
        CoTaskMemFree (before);
        return {};
    }

    WAVEFORMATEXTENSIBLE target {};
    buildTargetFormat (before, sampleRate, target);
    CoTaskMemFree (before);

    const auto setResult = policy->setDeviceFormat (idText, &target.Format, &target.Format);
    if (FAILED (setResult))
        return "SetDeviceFormat failed (0x" + juce::String::toHexString (static_cast<int> (setResult)) + ")";

    // Read back: a driver that ignored the request must not look like success.
    WAVEFORMATEX* after = nullptr;
    double nowRate = 0.0;
    if (SUCCEEDED (policy->getDeviceFormat (idText, 0, &after)) && after != nullptr)
        nowRate = static_cast<double> (after->nSamplesPerSec);
    if (after != nullptr) CoTaskMemFree (after);

    if (std::abs (nowRate - sampleRate) < 1.0)
        return {};

    return "endpoint kept " + juce::String (nowRate, 0) + " Hz";
}
}

juce::String setDefaultSampleRate (const juce::String& endpointName, bool wantInput, double sampleRate)
{
    if (sampleRate <= 0.0 || endpointName.isEmpty())
        return {};

    // The caller may already hold COM on this thread, so a changed-mode result
    // is not a failure; only a successful init needs the matching uninit.
    const auto init = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED (init);

    juce::String error = "endpoint not found";
    {
        const auto endpointId = findEndpointId (wantInput, endpointName);

        if (endpointId.isNotEmpty())
        {
            ComRef<IPolicyConfigVista> policy;
            if (FAILED (CoCreateInstance (policyConfigVistaClassId(), nullptr, CLSCTX_ALL,
                                          policyConfigVistaInterfaceId(),
                                          reinterpret_cast<void**> (policy.resetAndGetPointerAddress())))
                || ! policy)
            {
                error = "policy config unavailable";
            }
            else
            {
                error = applyThrough (policy.get(), endpointId, sampleRate);
            }
        }
    }

    if (shouldUninit)
        CoUninitialize();

    return error;
}

#else

juce::String setDefaultSampleRate (const juce::String&, bool, double)
{
    return "not supported on this platform";
}

#endif

} // namespace pluginrack::endpointformat
