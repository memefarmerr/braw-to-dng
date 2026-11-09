#include "braw_decoder.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <objbase.h>
#include <windows.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#endif

#include "BlackmagicRawAPI.h"
#include "BlackmagicRawAPIDispatch.h"

#ifndef S_OK
#define S_OK 0
#endif
#ifndef E_FAIL
#define E_FAIL ((HRESULT)0x80004005L)
#endif

namespace braw {
namespace {

#ifdef _WIN32
using NativeString = BSTR;
using NativeVariant = VARIANT;
#else
using NativeString = CFStringRef;
using NativeVariant = Variant;
#endif

constexpr uint32_t kFrameTimeoutSeconds = 300;

std::string describeHResult(HRESULT result) {
    char buffer[16] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(result));
    return buffer;
}

NativeString createString(const char* utf8) {
#ifdef _WIN32
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (length <= 0)
        return nullptr;
    std::wstring wide(static_cast<size_t>(length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide.data(), length);
    return SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
#else
    return CFStringCreateWithCString(kCFAllocatorDefault, utf8, kCFStringEncodingUTF8);
#endif
}

NativeString createStringFromPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    return SysAllocStringLen(wide.c_str(), static_cast<UINT>(wide.size()));
#else
    const std::string utf8 = path.string();
    return CFStringCreateWithCString(kCFAllocatorDefault, utf8.c_str(), kCFStringEncodingUTF8);
#endif
}

void destroyString(NativeString value) {
    if (value == nullptr)
        return;
#ifdef _WIN32
    SysFreeString(value);
#else
    CFRelease(value);
#endif
}

std::string stringToUtf8(NativeString value) {
    if (value == nullptr)
        return {};
#ifdef _WIN32
    const int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string utf8(static_cast<size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
#else
    char buffer[1024] = {};
    if (!CFStringGetCString(value, buffer, sizeof(buffer), kCFStringEncodingUTF8))
        return {};
    return buffer;
#endif
}

class ScopedString {
public:
    explicit ScopedString(NativeString value) : m_value(value) {}
    ~ScopedString() { destroyString(m_value); }
    ScopedString(const ScopedString&) = delete;
    ScopedString& operator=(const ScopedString&) = delete;

    NativeString get() const { return m_value; }
    bool valid() const { return m_value != nullptr; }

private:
    NativeString m_value;
};

template <typename T>
class ScopedRelease {
public:
    explicit ScopedRelease(T* object) : m_object(object) {}
    ~ScopedRelease() {
        if (m_object != nullptr)
            m_object->Release();
    }
    ScopedRelease(const ScopedRelease&) = delete;
    ScopedRelease& operator=(const ScopedRelease&) = delete;

private:
    T* m_object;
};

bool setStringAttribute(IBlackmagicRawClipProcessingAttributes* attributes,
                        BlackmagicRawClipProcessingAttribute attribute,
                        const char* value) {
    NativeVariant variant = {};
    variant.vt = blackmagicRawVariantTypeString;
    variant.bstrVal = createString(value);
    if (variant.bstrVal == nullptr)
        return false;
    const HRESULT result = attributes->SetClipAttribute(attribute, &variant);
    destroyString(variant.bstrVal);
    return result == S_OK;
}

bool setUIntAttribute(IBlackmagicRawClipProcessingAttributes* attributes,
                      BlackmagicRawClipProcessingAttribute attribute,
                      uint16_t value) {
    NativeVariant variant = {};
    variant.vt = blackmagicRawVariantTypeU16;
    variant.uiVal = value;
    return attributes->SetClipAttribute(attribute, &variant) == S_OK;
}

struct GamutCandidate {
    const char* name;
    double xyzToCamera[9];
};

// ColorMatrix1 for the gamuts the tool is willing to encode a DNG in, in order
// of preference. Values are the standard XYZ (D65) to native RGB matrices.
const GamutCandidate kDngGamuts[] = {
    {"Rec.2020",
     {1.7166512, -0.3556708, -0.2533663,
      -0.6666844, 1.6164812, 0.0157685,
      0.0176399, -0.0427706, 0.9421031}},
    {"P3 D65",
     {2.4934969, -0.9313836, -0.4027108,
      -0.8294890, 1.7626641, 0.0236247,
      0.0358458, -0.0761724, 0.9568845}},
    {"Display P3",
     {2.4934969, -0.9313836, -0.4027108,
      -0.8294890, 1.7626641, 0.0236247,
      0.0358458, -0.0761724, 0.9568845}},
    {"sRGB",
     {3.2404542, -1.5371385, -0.4985314,
      -0.9692660, 1.8760108, 0.0415560,
      0.0556434, -0.2040259, 1.0572252}},
    {"Rec.709",
     {3.2404542, -1.5371385, -0.4985314,
      -0.9692660, 1.8760108, 0.0415560,
      0.0556434, -0.2040259, 1.0572252}},
};

const char* const kPngGammas[] = {"sRGB", "Rec.709", "Blackmagic Design Extended Video"};
const char* const kPngGamuts[] = {"sRGB", "Rec.709", "Blackmagic Design"};


struct JobState {
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool success = false;
    HRESULT status = E_FAIL;
    std::string failure;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;
    IBlackmagicRawClipProcessingAttributes* attributes = nullptr;
    BlackmagicRawResourceFormat format = blackmagicRawResourceFormatRGBAU8;

    void finish(HRESULT result, const std::string& description) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status = result;
            success = SUCCEEDED(result) && description.empty();
            failure = description;
            complete = true;
        }
        condition.notify_all();
    }

    bool wait(std::string& error) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool finished = condition.wait_for(lock, std::chrono::seconds(kFrameTimeoutSeconds),
                                                 [this] { return complete; });
        if (!finished) {
            error = "timed out after " + std::to_string(kFrameTimeoutSeconds) +
                    "s waiting for the codec";
            return false;
        }
        if (!success) {
            error = failure.empty() ? "the codec reported " + describeHResult(status) : failure;
            return false;
        }
        return true;
    }
};

class FrameCallback final : public IBlackmagicRawCallback {
public:
    explicit FrameCallback(JobState& state) : m_state(state) {}

    void ReadComplete(IBlackmagicRawJob* job, HRESULT result, IBlackmagicRawFrame* frame) override {
        IBlackmagicRawJob* processJob = nullptr;
        HRESULT status = result;
        const char* stage = "reading the frame";

        if (SUCCEEDED(status)) {
            stage = "selecting the full resolution";
            status = frame->SetResolutionScale(blackmagicRawResolutionScaleFull);
        }
        if (SUCCEEDED(status)) {
            stage = "selecting the output pixel format";
            status = frame->SetResourceFormat(m_state.format);
        }
        if (SUCCEEDED(status)) {
            stage = "creating the decode job";
            status = frame->CreateJobDecodeAndProcessFrame(m_state.attributes, nullptr, &processJob);
        }
        if (SUCCEEDED(status)) {
            stage = "submitting the decode job";
            status = processJob->Submit();
        }

        if (FAILED(status)) {
            if (processJob != nullptr)
                processJob->Release();
            m_state.finish(status, std::string("failed while ") + stage + " (" +
                                       describeHResult(status) + ")");
        }

        job->Release();
    }

    void ProcessComplete(IBlackmagicRawJob* job,
                         HRESULT result,
                         IBlackmagicRawProcessedImage* processedImage) override {
        HRESULT status = result;
        std::string failure;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t sizeBytes = 0;
        void* resource = nullptr;

        if (SUCCEEDED(status))
            status = processedImage->GetWidth(&width);
        if (SUCCEEDED(status))
            status = processedImage->GetHeight(&height);
        if (SUCCEEDED(status))
            status = processedImage->GetResourceSizeBytes(&sizeBytes);
        if (SUCCEEDED(status))
            status = processedImage->GetResource(&resource);

        if (SUCCEEDED(status) && (resource == nullptr || sizeBytes == 0 || width == 0 || height == 0)) {
            status = E_FAIL;
            failure = "the codec produced an empty image";
        }

        if (SUCCEEDED(status)) {
            const uint8_t* bytes = static_cast<const uint8_t*>(resource);
            const size_t expected = m_state.format == blackmagicRawResourceFormatRGBAU8
                                        ? static_cast<size_t>(width) * height * 4
                                        : static_cast<size_t>(width) * height * 12;
            if (sizeBytes < expected) {
                status = E_FAIL;
                failure = "the codec returned " + std::to_string(sizeBytes) +
                          " bytes for a " + std::to_string(width) + "x" +
                          std::to_string(height) + " image";
            } else {
                std::lock_guard<std::mutex> lock(m_state.mutex);
                m_state.width = width;
                m_state.height = height;
                m_state.pixels.assign(bytes, bytes + expected);
            }
        }

        if (FAILED(status) && failure.empty())
            failure = "decoding the frame failed with " + describeHResult(status);

        job->Release();
        m_state.finish(status, failure);
    }

    void DecodeComplete(IBlackmagicRawJob*, HRESULT) override {}
    void TrimProgress(IBlackmagicRawJob*, float) override {}
    void TrimComplete(IBlackmagicRawJob*, HRESULT) override {}
    void SidecarMetadataParseWarning(IBlackmagicRawClip*, BSTR, uint32_t, BSTR) override {}
    void SidecarMetadataParseError(IBlackmagicRawClip*, BSTR, uint32_t, BSTR) override {}
    void PreparePipelineComplete(void*, HRESULT) override {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID*) override { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 0; }
    ULONG STDMETHODCALLTYPE Release() override { return 0; }

private:
    JobState& m_state;
};

IBlackmagicRawFactory* factoryForPath(const std::filesystem::path& sdkLibraryPath,
                                      std::string& error) {
    ScopedString path(createStringFromPath(sdkLibraryPath));
    if (!path.valid()) {
        error = "failed to convert the Blackmagic RAW SDK path";
        return nullptr;
    }

    IBlackmagicRawFactory* factory = CreateBlackmagicRawFactoryInstanceFromPath(path.get());
    if (factory == nullptr) {
        error = "failed to load the Blackmagic RAW codec from '" + sdkLibraryPath.string() +
                "' - check that the Blackmagic RAW SDK is installed or pass --sdkPath";
    }
    return factory;
}

bool configureProcessing(IBlackmagicRawClipProcessingAttributes* attributes,
                         OutputFormat format,
                         RenderResult& result,
                         std::string& error) {
    if (format == OutputFormat::Png) {
        for (const char* gamma : kPngGammas) {
            if (setStringAttribute(attributes, blackmagicRawClipProcessingAttributeGamma, gamma))
                break;
        }
        for (const char* gamut : kPngGamuts) {
            if (setStringAttribute(attributes, blackmagicRawClipProcessingAttributeGamut, gamut)) {
                result.colorGamut = gamut;
                break;
            }
        }
        setStringAttribute(attributes, blackmagicRawClipProcessingAttributePost3DLUTMode, "Disabled");
        return true;
    }

    if (!setStringAttribute(attributes, blackmagicRawClipProcessingAttributeGamma, "Linear")) {
        error = "the Blackmagic RAW codec rejected the Linear gamma attribute";
        return false;
    }
    setUIntAttribute(attributes, blackmagicRawClipProcessingAttributeGamutCompressionEnable, 0);
    setStringAttribute(attributes, blackmagicRawClipProcessingAttributePost3DLUTMode, "Disabled");

    for (const GamutCandidate& candidate : kDngGamuts) {
        if (!setStringAttribute(attributes, blackmagicRawClipProcessingAttributeGamut, candidate.name))
            continue;
        std::memcpy(result.xyzToCamera, candidate.xyzToCamera, sizeof(result.xyzToCamera));
        result.colorGamut = candidate.name;
        return true;
    }

    // Fall back to the clip's own gamut; sRGB is the closest guess for the matrix.
    std::memcpy(result.xyzToCamera, kDngGamuts[3].xyzToCamera, sizeof(result.xyzToCamera));
    result.colorGamut = "clip default (mapped as sRGB)";
    return true;
}

bool convertToLinear16(const JobState& state, RenderResult& result, std::string& error) {
    const size_t sampleCount = static_cast<size_t>(state.width) * state.height * 3;
    if (state.pixels.size() < sampleCount * 4) {
        error = "codec returned an unexpectedly small RGBF32 buffer";
        return false;
    }

    std::vector<float> samples(sampleCount);
    std::memcpy(samples.data(), state.pixels.data(), sampleCount * sizeof(float));

    result.rgb.resize(sampleCount);
    float maximum = 0.0f;
    for (size_t index = 0; index < sampleCount; ++index) {
        float value = samples[index];
        if (!(value > 0.0f))  // also filters NaN
            value = 0.0f;
        if (value > maximum)
            maximum = value;
        const float scaled = value * 65535.0f;
        result.rgb[index] = static_cast<uint16_t>(scaled >= 65535.0f ? 65535u
                                                                    : std::lround(scaled));
    }
    result.maxLinearValue = maximum;
    result.whiteLevel = 65535;
    return true;
}

}  // namespace

bool initialize(std::string& error) {
#ifdef _WIN32
    const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) {
        error = "failed to initialise COM (" + describeHResult(result) + ")";
        return false;
    }
#endif
    (void)error;
    return true;
}

void shutdown() {
#ifdef _WIN32
    CoUninitialize();
#endif
}

std::filesystem::path defaultSdkLibraryPath() {
#ifdef _WIN32
    const wchar_t* programFilesX86 = _wgetenv(L"ProgramFiles(x86)");
    const wchar_t* programFiles = _wgetenv(L"ProgramFiles");
    const std::filesystem::path roots[] = {
        programFilesX86 != nullptr ? std::filesystem::path(programFilesX86) : std::filesystem::path(),
        programFiles != nullptr ? std::filesystem::path(programFiles) : std::filesystem::path(),
    };
    for (const std::filesystem::path& root : roots) {
        if (root.empty())
            continue;
        std::filesystem::path candidate = root / "Blackmagic Design" / "Blackmagic RAW" /
                                          "Blackmagic RAW SDK" / "Win" / "Libraries";
        if (std::filesystem::exists(candidate / "BlackmagicRawAPI.dll"))
            return candidate;
    }
    return {};
#else
    const std::filesystem::path candidate =
        "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries";
    return std::filesystem::exists(candidate) ? candidate : std::filesystem::path();
#endif
}

bool renderFrame(const std::filesystem::path& clipPath,
                 const RenderOptions& options,
                 RenderResult& result,
                 std::string& error) {
    IBlackmagicRawFactory* factory = factoryForPath(options.sdkLibraryPath, error);
    if (factory == nullptr)
        return false;
    ScopedRelease<IBlackmagicRawFactory> factoryGuard(factory);

    IBlackmagicRaw* codec = nullptr;
    HRESULT status = factory->CreateCodec(&codec);
    if (FAILED(status)) {
        error = "failed to create a Blackmagic RAW codec (" + describeHResult(status) + ")";
        return false;
    }
    ScopedRelease<IBlackmagicRaw> codecGuard(codec);

    ScopedString clipName(createStringFromPath(clipPath));
    if (!clipName.valid()) {
        error = "failed to convert '" + clipPath.string() + "' to a codec path";
        return false;
    }

    IBlackmagicRawClip* clip = nullptr;
    status = codec->OpenClip(clipName.get(), &clip);
    if (FAILED(status) || clip == nullptr) {
        error = "the codec could not open '" + clipPath.string() + "' (" +
                describeHResult(status) + ")";
        return false;
    }
    ScopedRelease<IBlackmagicRawClip> clipGuard(clip);

    unsigned long long frameCount = 0;
    if (SUCCEEDED(clip->GetFrameCount(&frameCount)))
        result.frameCount = frameCount;
    if (frameCount > 0 && options.frameIndex >= frameCount) {
        error = "'" + clipPath.string() + "' only has " + std::to_string(frameCount) +
                " frame(s), so frame " + std::to_string(options.frameIndex) +
                " does not exist";
        return false;
    }

    NativeString cameraType = nullptr;
    if (SUCCEEDED(clip->GetCameraType(&cameraType))) {
        result.cameraType = stringToUtf8(cameraType);
        destroyString(cameraType);
    }

    IBlackmagicRawClipProcessingAttributes* attributes = nullptr;
    status = clip->CloneClipProcessingAttributes(&attributes);
    if (FAILED(status) || attributes == nullptr) {
        error = "failed to read the clip processing attributes (" + describeHResult(status) + ")";
        return false;
    }
    ScopedRelease<IBlackmagicRawClipProcessingAttributes> attributesGuard(attributes);

    if (!configureProcessing(attributes, options.format, result, error))
        return false;

    JobState state;
    state.attributes = attributes;
    state.format = options.format == OutputFormat::Png ? blackmagicRawResourceFormatRGBAU8
                                                       : blackmagicRawResourceFormatRGBF32;

    FrameCallback callback(state);
    status = codec->SetCallback(&callback);
    if (FAILED(status)) {
        error = "failed to register the codec callback (" + describeHResult(status) + ")";
        return false;
    }

    IBlackmagicRawJob* readJob = nullptr;
    status = clip->CreateJobReadFrame(options.frameIndex, &readJob);
    if (FAILED(status) || readJob == nullptr) {
        error = "failed to create a frame read job (" + describeHResult(status) + ")";
        return false;
    }

    status = readJob->Submit();
    if (FAILED(status)) {
        readJob->Release();
        error = "failed to submit the frame read job (" + describeHResult(status) + ")";
        return false;
    }

    codec->FlushJobs();

    if (!state.wait(error)) {
        error = "could not decode '" + clipPath.filename().string() + "': " + error;
        return false;
    }

    result.width = state.width;
    result.height = state.height;

    if (options.format == OutputFormat::Png) {
        result.rgba = std::move(state.pixels);
        return true;
    }
    return convertToLinear16(state, result, error);
}

}  // namespace braw
