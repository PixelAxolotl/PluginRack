#include "WasapiLoopbackCapture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#if JUCE_WINDOWS
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace {
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

    ComClass* release() noexcept
    {
        ComClass* raw = p;
        p = nullptr;
        return raw;
    }

private:
    ComClass* p = nullptr;
};

juce::String hresultMessage (const char* what, HRESULT hr)
{
    return juce::String (what) + " (0x" + juce::String::toHexString ((int) hr) + ")";
}
} // namespace

// Reader owns the pumping thread only. The IAudioClient/IAudioCaptureClient
// belong to the device (opened in open(), released in close()) and stay alive
// across start/stop, so the thread never AddRefs: it uses the raw pointers
// while running and stop() joins before close() can release them.
class WasapiLoopbackDevice::Reader
{
public:
    Reader() = default;
    ~Reader() { stopThread(); }

    bool startThread (IAudioClient* client, IAudioCaptureClient* capture,
                      HANDLE eventHandle, WasapiLoopbackDevice* owner,
                      int numChannels, int blockSize, int srcChannels,
                      int srcTag, int srcBits)
    {
        stopThread();
        client_ = client;
        capture_ = capture;
        event_ = eventHandle;
        owner_ = owner;
        numChannels_ = numChannels;
        blockSize_ = blockSize;
        srcChannels_ = srcChannels;
        srcTag_ = srcTag;
        srcBits_ = srcBits;
        pending_ = 0;
        exit_.store (false, std::memory_order_release);
        planar_.assign ((size_t) juce::jmax (1, numChannels_), {});
        for (auto& ch : planar_)
            ch.assign ((size_t) juce::jmax (1, blockSize_ * 2), 0.0f);
        inputPtrs_.assign ((size_t) juce::jmax (1, numChannels_), nullptr);
        try
        {
            thread_ = std::thread ([this] { run(); });
        }
        catch (...)
        {
            return false;
        }
        running_.store (true, std::memory_order_release);
        return true;
    }

    void stopThread()
    {
        if (! running_.load (std::memory_order_acquire))
            return;
        exit_.store (true, std::memory_order_release);
        if (event_ != nullptr)
            SetEvent (event_);
        if (thread_.joinable())
            thread_.join();
        running_.store (false, std::memory_order_release);
        client_ = nullptr;
        capture_ = nullptr;
        event_ = nullptr;
    }

    bool isRunning() const { return running_.load (std::memory_order_acquire); }

private:
    void run()
    {
        const auto init = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        const bool shouldUninit = SUCCEEDED (init);
        if (client_ != nullptr)
            client_->Start();
        for (;;)
        {
            const DWORD wait = event_ != nullptr
                ? WaitForSingleObject (event_, 2000) : WAIT_TIMEOUT;
            juce::ignoreUnused (wait);
            if (exit_.load (std::memory_order_acquire))
                break;
            if (capture_ != nullptr && owner_ != nullptr)
                pumpOnce();
            if (exit_.load (std::memory_order_acquire))
                break;
        }
        if (client_ != nullptr)
            client_->Stop();
        if (shouldUninit)
            CoUninitialize();
    }

    void pumpOnce() noexcept
    {
        try
        {
            UINT32 packets = 0;
            if (FAILED (capture_->GetNextPacketSize (&packets)))
                return;
            while (packets > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED (capture_->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                    return;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0
                    || data == nullptr;
                appendPacket (silent ? nullptr : data, (int) frames);
                capture_->ReleaseBuffer (frames);
                if (exit_.load (std::memory_order_acquire))
                    return;
                if (FAILED (capture_->GetNextPacketSize (&packets)))
                    return;
            }
        }
        catch (...)
        {
        }
    }

    void appendPacket (const BYTE* data, int frames) noexcept
    {
        int consumed = 0;
        while (consumed < frames)
        {
            const int room = blockSize_ - pending_;
            const int take = juce::jmin (room, frames - consumed);
            if (take <= 0)
            {
                fireBlock();
                continue;
            }
            if (data == nullptr)
            {
                for (int ch = 0; ch < numChannels_; ++ch)
                    std::memset (planar_[(size_t) ch].data() + pending_, 0,
                                 sizeof (float) * (size_t) take);
            }
            else
            {
                writeSamples (data, consumed, pending_, take);
            }
            pending_ += take;
            consumed += take;
            if (pending_ >= blockSize_)
                fireBlock();
        }
    }

    void writeSamples (const BYTE* data, int srcOffset, int dstOffset, int count) noexcept
    {
        const int kept = juce::jmin (numChannels_, 2);
        for (int i = 0; i < count; ++i)
        {
            float s0 = 0.0f, s1 = 0.0f;
            readFrame (data, srcOffset + i, s0, s1);
            if (kept > 0)
                planar_[0].data()[dstOffset + i] = s0;
            if (kept > 1)
                planar_[1].data()[dstOffset + i] = (srcChannels_ > 1 ? s1 : s0);
        }
    }

    void readFrame (const BYTE* data, int frame, float& s0, float& s1) const noexcept
    {
        if (srcTag_ == 3) // IEEE float
        {
            const auto* f = reinterpret_cast<const float*> (data);
            s0 = f[(size_t) frame * (size_t) srcChannels_];
            s1 = srcChannels_ > 1 ? f[(size_t) frame * (size_t) srcChannels_ + 1] : s0;
            return;
        }
        if (srcBits_ == 16)
        {
            const auto* v = reinterpret_cast<const std::int16_t*> (data);
            s0 = (float) v[(size_t) frame * (size_t) srcChannels_] / 32768.0f;
            s1 = srcChannels_ > 1
                ? (float) v[(size_t) frame * (size_t) srcChannels_ + 1] / 32768.0f : s0;
            return;
        }
        if (srcBits_ == 24)
        {
            const BYTE* p = data + (size_t) frame * (size_t) srcChannels_ * 3;
            auto lane = [] (const BYTE* q) {
                std::int32_t v = (std::int32_t) (q[0] | (q[1] << 8) | (q[2] << 16));
                if ((v & 0x800000) != 0) v |= ~0xffffff;
                return (float) v / 8388608.0f;
            };
            s0 = lane (p);
            s1 = srcChannels_ > 1 ? lane (p + 3) : s0;
            return;
        }
        // 32-bit PCM fallback.
        const auto* v = reinterpret_cast<const std::int32_t*> (data);
        s0 = (float) v[(size_t) frame * (size_t) srcChannels_] / 2147483648.0f;
        s1 = srcChannels_ > 1
            ? (float) v[(size_t) frame * (size_t) srcChannels_ + 1] / 2147483648.0f : s0;
    }

    void fireBlock() noexcept
    {
        if (pending_ <= 0 || owner_ == nullptr)
        {
            pending_ = 0;
            return;
        }
        for (int ch = 0; ch < numChannels_; ++ch)
            inputPtrs_[(size_t) ch] = planar_[(size_t) ch].data();
        owner_->fireCallback (inputPtrs_.data(), numChannels_);
        pending_ = 0;
    }

    std::thread thread_;
    std::atomic<bool> exit_ { false };
    std::atomic<bool> running_ { false };
    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE event_ = nullptr;
    WasapiLoopbackDevice* owner_ = nullptr;
    int numChannels_ = 0;
    int blockSize_ = 0;
    int srcChannels_ = 0;
    int srcTag_ = 0;
    int srcBits_ = 0;
    int pending_ = 0;
    std::vector<std::vector<float>> planar_;
    std::vector<const float*> inputPtrs_;
};

struct WasapiLoopbackHandles
{
    ComRef<IAudioClient> client;
    ComRef<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    WAVEFORMATEXTENSIBLE srcFormat {};
    REFERENCE_TIME streamLatency = 0;
    int srcChannels = 0;
    int srcTag = 0;
    int srcBits = 0;
    void releaseClients()
    {
        client.resetAndGetPointerAddress();
        capture.resetAndGetPointerAddress();
        event = nullptr;
        srcChannels = 0;
        srcTag = 0;
        srcBits = 0;
        streamLatency = 0;
        std::memset (&srcFormat, 0, sizeof (srcFormat));
    }
    static WasapiLoopbackHandles& get();
};

WasapiLoopbackHandles& WasapiLoopbackHandles::get()
{
    static WasapiLoopbackHandles instance;
    return instance;
}

WasapiLoopbackDevice::WasapiLoopbackDevice()
    : juce::AudioIODevice (kDeviceName, "WASAPI Loopback")
{
}

WasapiLoopbackDevice::~WasapiLoopbackDevice()
{
    close();
}

juce::StringArray WasapiLoopbackDevice::getOutputChannelNames()
{
    return {}; // capture-only: outputs are cleared
}

juce::StringArray WasapiLoopbackDevice::getInputChannelNames()
{
    const int n = openFlag_.load (std::memory_order_acquire)
        ? numCaptureChannels_ : 2;
    juce::StringArray names;
    for (int i = 0; i < n; ++i)
        names.add ("Loopback " + juce::String (i + 1));
    return names;
}

juce::Array<double> WasapiLoopbackDevice::getAvailableSampleRates()
{
    return { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
}

juce::Array<int> WasapiLoopbackDevice::getAvailableBufferSizes()
{
    return { 64, 128, 256, 512, 1024, 2048 };
}

int WasapiLoopbackDevice::getDefaultBufferSize()
{
    return 480;
}

juce::String WasapiLoopbackDevice::open (const juce::BigInteger& inputChannels,
                                         const juce::BigInteger& outputChannels,
                                         double sampleRate, int bufferSizeSamples)
{
    juce::ignoreUnused (outputChannels);
    const juce::ScopedLock sl (stateLock_);
    close();
    lastError_.clear();

    if (sampleRate <= 0.0 || bufferSizeSamples <= 0)
    {
        lastError_ = "Invalid sample rate or buffer size.";
        return lastError_;
    }

    int requested = 0;
    for (int i = 0; i < 64; ++i)
        if (inputChannels[i]) ++requested;
    if (requested <= 0)
        requested = 2;

    const auto init = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool shouldUninit = SUCCEEDED (init);

    juce::String error;
    auto& h = WasapiLoopbackHandles::get();
    h.releaseClients();

    ComRef<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof (IMMDeviceEnumerator),
                                   reinterpret_cast<void**> (enumerator.resetAndGetPointerAddress()));
    if (FAILED (hr) || ! enumerator)
    {
        error = hresultMessage ("No default render endpoint", hr);
    }

    ComRef<IMMDevice> endpoint;
    if (error.isEmpty())
    {
        hr = enumerator->GetDefaultAudioEndpoint (eRender, eMultimedia,
                                                 endpoint.resetAndGetPointerAddress());
        if (FAILED (hr) || ! endpoint)
            error = hresultMessage ("No default render endpoint", hr);
    }

    WAVEFORMATEX* mix = nullptr;
    if (error.isEmpty())
    {
        ComRef<IAudioClient> probe;
        hr = endpoint->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**> (probe.resetAndGetPointerAddress()));
        if (FAILED (hr) || ! probe)
        {
            error = hresultMessage ("Audio client unavailable", hr);
        }
        else
        {
            hr = probe->GetMixFormat (&mix);
            if (FAILED (hr) || mix == nullptr)
                error = hresultMessage ("Mix format unavailable", hr);
        }
    }

    const int mixChannels = (mix != nullptr) ? (int) mix->nChannels : 2;
    if (mix != nullptr)
        CoTaskMemFree (mix);

    const int numChannels = juce::jmin (2, juce::jmax (1, juce::jmin (requested, mixChannels)));

    WAVEFORMATEXTENSIBLE wanted {};
    std::memset (&wanted, 0, sizeof (wanted));
    wanted.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wanted.Format.nChannels = (WORD) numChannels;
    wanted.Format.nSamplesPerSec = (DWORD) std::lround (sampleRate);
    wanted.Format.wBitsPerSample = 32;
    wanted.Format.nBlockAlign = (WORD) (numChannels * 4);
    wanted.Format.nAvgBytesPerSec = wanted.Format.nSamplesPerSec * wanted.Format.nBlockAlign;
    wanted.Format.cbSize = (WORD) (sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX));
    wanted.Samples.wValidBitsPerSample = 32;
    wanted.dwChannelMask = (numChannels > 1) ? 3 : 4;
    wanted.SubFormat = { 0x00000003, 0x0000, 0x0010,
                         { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    if (error.isEmpty())
    {
        hr = endpoint->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**> (h.client.resetAndGetPointerAddress()));
        if (FAILED (hr) || ! h.client)
            error = hresultMessage ("Audio client unavailable", hr);
    }

    if (error.isEmpty())
    {
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = h.client->Initialize (AUDCLNT_SHAREMODE_SHARED, flags,
                                   0, 0, &wanted.Format, nullptr);
        if (FAILED (hr))
        {
            error = hresultMessage ("Loopback shared-mode init failed", hr);
        }
        else
        {
            h.event = CreateEvent (nullptr, FALSE, FALSE, nullptr);
            if (h.event == nullptr)
                error = "Loopback event unavailable.";
            else if (FAILED (hr = h.client->SetEventHandle (h.event)))
                error = hresultMessage ("Loopback event rejected", hr);
            else if (FAILED (hr = h.client->GetService (__uuidof (IAudioCaptureClient),
                                    reinterpret_cast<void**> (h.capture.resetAndGetPointerAddress())))
                     || ! h.capture)
                error = hresultMessage ("Capture client unavailable", hr);
            else if (FAILED (hr = h.client->GetStreamLatency (&h.streamLatency)))
                error = hresultMessage ("Stream latency unavailable", hr);
        }
    }

    // Remember what the endpoint hands us so the pump can convert it. With
    // AUTOCONVERTPCM the frames are already at the requested rate; the tag
    // below only selects int16/24/32 vs float32 conversion.
    if (error.isEmpty())
    {
        WAVEFORMATEX* current = nullptr;
        if (SUCCEEDED (h.client->GetMixFormat (&current)) && current != nullptr)
        {
            h.srcChannels = (int) current->nChannels;
            int tag = (int) current->wFormatTag;
            // Shared-mode endpoints report WAVE_FORMAT_EXTENSIBLE, whose real
            // format is the SubFormat GUID. Reading the 0xFFFE tag as a plain
            // PCM tag decoded float audio as int32 and played noise.
            const auto extra = (size_t) current->cbSize;
            const auto maxExtra = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
            if (tag == WAVE_FORMAT_EXTENSIBLE && extra >= maxExtra)
                tag = (int) reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (current)->SubFormat.Data1;
            h.srcTag = tag;
            h.srcBits = (int) current->wBitsPerSample;
            std::memcpy (&h.srcFormat, current,
                         sizeof (WAVEFORMATEX) + juce::jmin (extra, maxExtra));
            CoTaskMemFree (current);
        }
        else
        {
            h.srcChannels = numChannels;
            h.srcTag = 3;
            h.srcBits = 32;
        }
        if (h.srcChannels <= 0)
            h.srcChannels = numChannels;
    }

    if (shouldUninit)
        CoUninitialize();

    if (error.isNotEmpty())
    {
        if (h.event != nullptr)
        {
            CloseHandle (h.event);
            h.event = nullptr;
        }
        h.releaseClients();
        lastError_ = error;
        return lastError_;
    }

    numCaptureChannels_ = numChannels;
    bufferSizeSamples_ = bufferSizeSamples;
    sampleRate_ = sampleRate;
    const double latencySec = (double) h.streamLatency / 10000000.0;
    inputLatencySamples_ = (int) std::lround (latencySec * sampleRate) + bufferSizeSamples;
    openFlag_.store (true, std::memory_order_release);
    return {};
}

void WasapiLoopbackDevice::close()
{
    const juce::ScopedLock sl (stateLock_);
    stop();
    auto& h = WasapiLoopbackHandles::get();
    if (h.event != nullptr)
    {
        CloseHandle (h.event);
        h.event = nullptr;
    }
    h.releaseClients();
    openFlag_.store (false, std::memory_order_release);
    bufferSizeSamples_ = 0;
    sampleRate_ = 0.0;
    inputLatencySamples_ = 0;
}

bool WasapiLoopbackDevice::isOpen()
{
    return openFlag_.load (std::memory_order_acquire);
}

void WasapiLoopbackDevice::start (juce::AudioIODeviceCallback* callback)
{
    const juce::ScopedLock sl (stateLock_);
    if (callback_ == callback && isPlaying())
        return;
    stop();
    if (! openFlag_.load (std::memory_order_acquire))
    {
        lastError_ = "Device is not open.";
        return;
    }
    if (callback == nullptr)
    {
        lastError_ = "No callback.";
        return;
    }
    auto& h = WasapiLoopbackHandles::get();
    if (h.client.get() == nullptr || h.capture.get() == nullptr || h.event == nullptr)
    {
        lastError_ = "Device is not open.";
        return;
    }
    callback_ = callback;
    callback_->audioDeviceAboutToStart (this);
    if (reader_ == nullptr)
        reader_ = std::make_unique<Reader>();
    const bool ok = reader_->startThread (h.client.get(), h.capture.get(), h.event, this,
                                          numCaptureChannels_, bufferSizeSamples_,
                                          h.srcChannels, h.srcTag, h.srcBits);
    if (! ok)
    {
        callback_ = nullptr;
        lastError_ = "Loopback thread failed to start.";
        return;
    }
    playingFlag_.store (true, std::memory_order_release);
}

void WasapiLoopbackDevice::stop()
{
    if (reader_ != nullptr)
        reader_->stopThread();
    if (callback_ != nullptr)
    {
        callback_->audioDeviceStopped();
        callback_ = nullptr;
    }
    playingFlag_.store (false, std::memory_order_release);
}

bool WasapiLoopbackDevice::isPlaying()
{
    return playingFlag_.load (std::memory_order_acquire)
        && reader_ != nullptr && reader_->isRunning();
}

juce::String WasapiLoopbackDevice::getLastError()
{
    const juce::ScopedLock sl (stateLock_);
    return lastError_;
}

int WasapiLoopbackDevice::getCurrentBufferSizeSamples()
{
    return bufferSizeSamples_;
}

double WasapiLoopbackDevice::getCurrentSampleRate()
{
    return sampleRate_;
}

int WasapiLoopbackDevice::getCurrentBitDepth()
{
    return 32;
}

juce::BigInteger WasapiLoopbackDevice::getActiveOutputChannels() const
{
    return {}; // capture-only
}

juce::BigInteger WasapiLoopbackDevice::getActiveInputChannels() const
{
    juce::BigInteger active;
    for (int i = 0; i < numCaptureChannels_; ++i)
        active.setBit (i);
    return active;
}

int WasapiLoopbackDevice::getOutputLatencyInSamples()
{
    return 0;
}

int WasapiLoopbackDevice::getInputLatencyInSamples()
{
    return inputLatencySamples_;
}

void WasapiLoopbackDevice::fireCallback (const float* const* inputs, int numInputs) noexcept
{
    auto* cb = callback_; // set/cleared on the message thread, read here
    if (cb == nullptr)
        return;
    try
    {
        juce::AudioIODeviceCallbackContext context {};
        cb->audioDeviceIOCallbackWithContext (inputs, numInputs,
                                             nullptr, 0, bufferSizeSamples_, context);
    }
    catch (...)
    {
    }
}

WasapiLoopbackDeviceType::WasapiLoopbackDeviceType()
    : juce::AudioIODeviceType (kTypeName)
{
}

void WasapiLoopbackDeviceType::scanForDevices()
{
}

juce::StringArray WasapiLoopbackDeviceType::getDeviceNames (bool) const
{
    return { WasapiLoopbackDevice::kDeviceName };
}

int WasapiLoopbackDeviceType::getDefaultDeviceIndex (bool) const
{
    return 0;
}

int WasapiLoopbackDeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool) const
{
    if (dynamic_cast<WasapiLoopbackDevice*> (device) != nullptr)
        return 0;
    return -1;
}

juce::AudioIODevice* WasapiLoopbackDeviceType::createDevice (const juce::String& outputDeviceName,
                                                            const juce::String& inputDeviceName)
{
    if (outputDeviceName == WasapiLoopbackDevice::kDeviceName
        || inputDeviceName == WasapiLoopbackDevice::kDeviceName)
        return new WasapiLoopbackDevice();
    return nullptr;
}

namespace WasapiLoopback {
juce::String registerWith (juce::AudioDeviceManager& manager)
{
    const juce::String typeName (WasapiLoopbackDeviceType::kTypeName);
    for (auto* type : manager.getAvailableDeviceTypes())
        if (type->getTypeName() == typeName)
            return typeName;
    auto fresh = std::make_unique<WasapiLoopbackDeviceType>();
    auto* raw = fresh.get();
    manager.addAudioDeviceType (std::move (fresh));
    raw->scanForDevices();
    return typeName;
}
} // namespace WasapiLoopback

#else // ! JUCE_WINDOWS

WasapiLoopbackDevice::WasapiLoopbackDevice()
    : juce::AudioIODevice (kDeviceName, "WASAPI Loopback")
{
}
WasapiLoopbackDevice::~WasapiLoopbackDevice() = default;
juce::StringArray WasapiLoopbackDevice::getOutputChannelNames() { return {}; }
juce::StringArray WasapiLoopbackDevice::getInputChannelNames() { return {}; }
juce::Array<double> WasapiLoopbackDevice::getAvailableSampleRates() { return {}; }
juce::Array<int> WasapiLoopbackDevice::getAvailableBufferSizes() { return {}; }
int WasapiLoopbackDevice::getDefaultBufferSize() { return 0; }
juce::String WasapiLoopbackDevice::open (const juce::BigInteger&, const juce::BigInteger&,
                                         double, int)
{
    lastError_ = "WASAPI loopback is not supported on this platform.";
    return lastError_;
}
void WasapiLoopbackDevice::close() {}
bool WasapiLoopbackDevice::isOpen() { return false; }
void WasapiLoopbackDevice::start (juce::AudioIODeviceCallback*) {}
void WasapiLoopbackDevice::stop() {}
bool WasapiLoopbackDevice::isPlaying() { return false; }
juce::String WasapiLoopbackDevice::getLastError() { return lastError_; }
int WasapiLoopbackDevice::getCurrentBufferSizeSamples() { return 0; }
double WasapiLoopbackDevice::getCurrentSampleRate() { return 0.0; }
int WasapiLoopbackDevice::getCurrentBitDepth() { return 0; }
juce::BigInteger WasapiLoopbackDevice::getActiveOutputChannels() const { return {}; }
juce::BigInteger WasapiLoopbackDevice::getActiveInputChannels() const { return {}; }
int WasapiLoopbackDevice::getOutputLatencyInSamples() { return 0; }
int WasapiLoopbackDevice::getInputLatencyInSamples() { return 0; }

WasapiLoopbackDeviceType::WasapiLoopbackDeviceType()
    : juce::AudioIODeviceType (kTypeName)
{
}
void WasapiLoopbackDeviceType::scanForDevices() {}
juce::StringArray WasapiLoopbackDeviceType::getDeviceNames (bool) const { return {}; }
int WasapiLoopbackDeviceType::getDefaultDeviceIndex (bool) const { return -1; }
int WasapiLoopbackDeviceType::getIndexOfDevice (juce::AudioIODevice*, bool) const { return -1; }
juce::AudioIODevice* WasapiLoopbackDeviceType::createDevice (const juce::String&, const juce::String&)
{
    return nullptr;
}

namespace WasapiLoopback {
juce::String registerWith (juce::AudioDeviceManager&)
{
    return {};
}
} // namespace WasapiLoopback

#endif
