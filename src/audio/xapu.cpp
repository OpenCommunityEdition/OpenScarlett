// SPDX-License-Identifier: AGPL-3.0-or-later
// xapu.dll: the console's audio decode/sample-rate-convert hardware (Opus decode, PCM convert),
// driven through a command queue. There is no such device on a PC; this is a software stand-in.
// The process type DecodeOpus is decoded for real with the Opus decoder that ships with Windows
// (a Media Foundation transform, one per stream). The other process types still answer each
// command with silence of the length the hardware would have produced, so titles that require the
// connection (some stop when XApuConnect fails) keep their audio pipeline running.
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstring>

#include <deque>
#include <mutex>
#include <vector>

#include <mf_x/xapu.h>

#include "xsl/log.h"

#define CH "audio"

struct XAPU {
    XApuConnectInputParameters in{};
    void* base = nullptr;
    HANDLE event = nullptr;
    std::mutex mu;
    std::deque<XApuResult> results;
    struct Stream {
        bool active = false; uint32_t channels = 2; float pitch = 1.0f; uint32_t stored = 0;
        struct Decoder* decoder = nullptr;   // DecodeOpus only; made at the first packet
        bool decoder_failed = false;         // the decoder could not be made: this stream stays silent
        uint32_t skip = 0;                   // decoded frames still to drop (the pre-skip at a stream's start)
    };
    std::vector<Stream> streams;
    uint32_t commands = 0, taken = 0;
};

// ---- Opus decoding through the Windows decoder ----
// Media Foundation and COM are loaded at run time, so this DLL needs no extra import libraries
// and still loads (and plays silence) on a system without them.
static const GUID kOpusDecoderClass = {0x63e17c10, 0x2d43, 0x4c42, {0x8f, 0xe3, 0x8d, 0x8b, 0x63, 0xe4, 0x6a, 0x6a}};
static const GUID kOpusSubtype = {0x0000704f, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const uint32_t kRate = 48000;

struct MediaApi {
    bool ok = false;
    HRESULT(WINAPI* create_type)(IMFMediaType**) = nullptr;
    HRESULT(WINAPI* create_sample)(IMFSample**) = nullptr;
    HRESULT(WINAPI* create_buffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(WINAPI* create_instance)(REFCLSID, IUnknown*, DWORD, REFIID, void**) = nullptr;
};

static MediaApi load_media_api() {
    MediaApi m;
    HMODULE ole = LoadLibraryW(L"ole32.dll");
    HMODULE com = LoadLibraryW(L"combase.dll");
    HMODULE plat = LoadLibraryW(L"mfplat.dll");
    if (!ole || !plat) return m;
    // Keep the multithreaded apartment alive for the whole process, so any of the title's threads
    // can make and use a decoder without initialising COM itself.
    auto mta = (HRESULT(WINAPI*)(void**))(com ? GetProcAddress(com, "CoIncrementMTAUsage") : nullptr);
    if (!mta) mta = (HRESULT(WINAPI*)(void**))GetProcAddress(ole, "CoIncrementMTAUsage");
    void* cookie = nullptr;
    if (!mta || FAILED(mta(&cookie))) {
        if (auto init = (HRESULT(WINAPI*)(void*, DWORD))GetProcAddress(ole, "CoInitializeEx")) init(nullptr, 0 /* multithreaded */);
    }
    auto startup = (HRESULT(WINAPI*)(ULONG, DWORD))GetProcAddress(plat, "MFStartup");
    m.create_type = (decltype(m.create_type))GetProcAddress(plat, "MFCreateMediaType");
    m.create_sample = (decltype(m.create_sample))GetProcAddress(plat, "MFCreateSample");
    m.create_buffer = (decltype(m.create_buffer))GetProcAddress(plat, "MFCreateMemoryBuffer");
    m.create_instance = (decltype(m.create_instance))GetProcAddress(ole, "CoCreateInstance");
    if (!startup || !m.create_type || !m.create_sample || !m.create_buffer || !m.create_instance) return m;
    m.ok = SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE));
    return m;
}
static MediaApi& media_api() { static MediaApi m = load_media_api(); return m; }

struct Decoder {
    IMFTransform* mft = nullptr;
    bool float_out = true;        // the decoder's output is 32-bit float (else 16-bit integer)
    bool own_samples = false;     // the decoder hands out its own output samples
    uint32_t out_channels = 2;
    uint32_t out_size = 0;        // bytes to offer per output sample
    int64_t position = 0;         // frames fed so far, for the sample times
    std::vector<float> pcm;       // decoded, interleaved in the stream's channel count, not yet given out
};

// Chooses the decoder's output format: float when it offers it, 16-bit integer otherwise.
static bool pick_output(Decoder& d) {
    IMFMediaType* chosen = nullptr;
    bool chosen_float = false;
    for (DWORD i = 0; i < 32; i++) {
        IMFMediaType* t = nullptr;
        if (FAILED(d.mft->GetOutputAvailableType(0, i, &t)) || !t) break;
        GUID sub{};
        UINT32 bits = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        t->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        const bool is_float = sub == MFAudioFormat_Float && (bits == 32 || bits == 0);
        const bool is_pcm16 = sub == MFAudioFormat_PCM && bits == 16;
        if (is_float || (is_pcm16 && !chosen)) {
            if (chosen) chosen->Release();
            chosen = t;
            chosen_float = is_float;
            if (is_float) break;
        } else t->Release();
    }
    if (!chosen) return false;
    HRESULT hr = d.mft->SetOutputType(0, chosen, 0);
    UINT32 ch = 0;
    chosen->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
    chosen->Release();
    if (FAILED(hr) || (ch != 1 && ch != 2)) return false;
    d.float_out = chosen_float;
    d.out_channels = ch;
    MFT_OUTPUT_STREAM_INFO info{};
    d.mft->GetOutputStreamInfo(0, &info);
    d.own_samples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
    // Room for the longest Opus packet (120 ms) in stereo float, or what the decoder asks for.
    d.out_size = info.cbSize > 5760 * 2 * 4 ? info.cbSize : 5760 * 2 * 4;
    return true;
}

static void destroy_decoder(Decoder* d) {
    if (!d) return;
    if (d->mft) d->mft->Release();
    delete d;
}

static Decoder* create_decoder(uint32_t channels) {
    MediaApi& m = media_api();
    if (!m.ok) return nullptr;
    auto* d = new Decoder;
    IMFMediaType* in = nullptr;
    bool ok = SUCCEEDED(m.create_instance(kOpusDecoderClass, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void**)&d->mft)) && d->mft;
    if (ok) ok = SUCCEEDED(m.create_type(&in)) && in;
    if (ok) {
        // The identification header of an Opus stream: no pre-skip here, the title names the
        // frames to skip itself in each command.
        uint8_t head[19] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, (uint8_t)channels, 0, 0, 0x80, 0xBB, 0, 0, 0, 0, 0};
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, kOpusSubtype);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        in->SetBlob(MF_MT_USER_DATA, head, sizeof head);
        ok = SUCCEEDED(d->mft->SetInputType(0, in, 0)) && pick_output(*d);
    }
    if (in) in->Release();
    if (!ok) { destroy_decoder(d); return nullptr; }
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return d;
}

// Appends decoded bytes to the stream's pending audio as float, in the stream's channel count.
static void append_pcm(Decoder& d, const uint8_t* data, uint32_t length, uint32_t channels) {
    const uint32_t width = d.float_out ? 4 : 2;
    const uint32_t frames = length / (width * d.out_channels);
    for (uint32_t i = 0; i < frames; i++) {
        float v[2] = {0, 0};
        for (uint32_t c = 0; c < d.out_channels; c++) {
            const uint8_t* at = data + (size_t)(i * d.out_channels + c) * width;
            if (d.float_out) memcpy(&v[c], at, 4);
            else { int16_t w; memcpy(&w, at, 2); v[c] = w / 32768.0f; }
        }
        if (d.out_channels == 1) v[1] = v[0];
        if (channels == 1) d.pcm.push_back(d.out_channels == 2 ? (v[0] + v[1]) * 0.5f : v[0]);
        else { d.pcm.push_back(v[0]); d.pcm.push_back(v[1]); }
    }
}

// Feeds one Opus packet and collects everything the decoder has ready. False when it fails.
static bool decode_packet(Decoder& d, const void* packet, uint32_t length, uint32_t frames, uint32_t channels) {
    MediaApi& m = media_api();
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    bool ok = SUCCEEDED(m.create_sample(&sample)) && sample && SUCCEEDED(m.create_buffer(length, &buffer)) && buffer;
    if (ok) {
        BYTE* at = nullptr;
        ok = SUCCEEDED(buffer->Lock(&at, nullptr, nullptr)) && at;
        if (ok) {
            memcpy(at, packet, length);
            buffer->Unlock();
            buffer->SetCurrentLength(length);
            sample->AddBuffer(buffer);
            sample->SetSampleTime(d.position * 10000000ll / kRate);
            sample->SetSampleDuration((int64_t)frames * 10000000ll / kRate);
            ok = SUCCEEDED(d.mft->ProcessInput(0, sample, 0));
        }
    }
    if (buffer) buffer->Release();
    if (sample) sample->Release();
    if (!ok) return false;
    d.position += frames;
    for (int round = 0; round < 16; round++) {
        MFT_OUTPUT_DATA_BUFFER out{};
        DWORD status = 0;
        if (!d.own_samples) {
            IMFMediaBuffer* room = nullptr;
            if (FAILED(m.create_sample(&out.pSample)) || !out.pSample) return false;
            if (FAILED(m.create_buffer(d.out_size, &room)) || !room) { out.pSample->Release(); return false; }
            out.pSample->AddBuffer(room);
            room->Release();
        }
        HRESULT hr = d.mft->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();
        if (SUCCEEDED(hr) && out.pSample) {
            IMFMediaBuffer* got = nullptr;
            if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&got)) && got) {
                BYTE* at = nullptr;
                DWORD have = 0;
                if (SUCCEEDED(got->Lock(&at, nullptr, &have)) && at) {
                    append_pcm(d, at, have, channels);
                    got->Unlock();
                }
                got->Release();
            }
        }
        if (out.pSample) out.pSample->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { if (!pick_output(d)) return false; continue; }
        if (FAILED(hr)) return false;
    }
    return true;
}

// One Process command of the type DecodeOpus: one Opus packet in, float PCM out.
static void decode_process(XAPU::Stream& s, const XApuDecodeConvertCommand* p, XApuResult& r) {
    static bool said_no_decoder = false, said_bad_packet = false;
    const uint32_t channels = s.channels;
    // A packet holds 20 ms (960 frames) unless the command names another sane length.
    const uint32_t packet = p->frameCount && p->frameCount <= 5760 ? p->frameCount : 960;
    const uint32_t first = p->firstFrameIndex;
    uint32_t want = packet > first ? packet - first : 0;
    void* const out = p->outputData;
    const uint32_t room = out ? p->maxOutputDataLength / (channels * 4) : 0;
    if (want > room) want = room;

    // XSL_OPUS=0: no decoding, every stream is silence (to tell decoding problems from others)
    static const bool opusOff = [] { char b[4]; return GetEnvironmentVariableA("XSL_OPUS", b, 4) && b[0] == '0'; }();
    if (opusOff) s.decoder_failed = true;
    if (!s.decoder && !s.decoder_failed) {
        s.decoder = create_decoder(channels);
        if (!s.decoder) {
            s.decoder_failed = true;
            if (!said_no_decoder) {
                said_no_decoder = true;
                XSL_WARN(CH, "the Windows Opus decoder could not be made: Opus streams play as silence");
            }
        }
    }
    bool decoded = false;
    if (s.decoder && p->inputData && p->inputDataLength && p->inputDataLength <= 0x10000) {
        decoded = decode_packet(*s.decoder, p->inputData, p->inputDataLength, packet, channels);
        if (!decoded && !said_bad_packet) {
            said_bad_packet = true;
            XSL_WARN(CH, "an Opus packet of %u bytes did not decode: silence for that packet", p->inputDataLength);
        }
    }
    uint32_t have = 0;
    if (decoded) {
        Decoder& d = *s.decoder;
        // Drop the frames the title asked to skip (the pre-skip on a stream's first packet).
        s.skip += first < packet ? first : packet;
        uint32_t pending = (uint32_t)(d.pcm.size() / channels);
        const uint32_t drop = s.skip < pending ? s.skip : pending;
        d.pcm.erase(d.pcm.begin(), d.pcm.begin() + (size_t)drop * channels);
        s.skip -= drop;
        pending -= drop;
        // Should the decoder ever run far ahead of the title, keep only the newest audio.
        if (pending > want + 4 * 960) {
            d.pcm.erase(d.pcm.begin(), d.pcm.begin() + (size_t)(pending - want - 4 * 960) * channels);
            pending = want + 4 * 960;
        }
        have = pending < want ? pending : want;
    }
    if (want) {
        // Silence first where the decoder has less than asked, then the decoded frames.
        const size_t frame = (size_t)channels * 4;
        const uint32_t pad = want - have;
        if (pad) memset(out, 0, pad * frame);
        if (have) {
            memcpy((uint8_t*)out + pad * frame, s.decoder->pcm.data(), have * frame);
            s.decoder->pcm.erase(s.decoder->pcm.begin(), s.decoder->pcm.begin() + (size_t)have * channels);
        }
    }
    r.state = (XApuStateOptions)((uint32_t)XApuStateOptions::OutputReady | (uint32_t)XApuStateOptions::NeedMoreInput);
    r.outputFrameCount = want;
    r.outputDataLength = want * channels * 4;
    r.outputData = p->outputData;
    r.inputDataLength = p->inputDataLength;
    r.inputData = p->inputData;
}

extern "C" {
// A title's audio mixer sleeps a few milliseconds per block of a few milliseconds (the console's
// timer is that fine). At the Windows default of 15.6 ms most blocks come late and the sound
// stutters, so the timer is set to 1 ms for the process, also when the window is in the background
// (Windows 11 otherwise ignores the request for windows it throttles).
static void fine_timer() {
    static bool done = false;
    if (done) return;
    done = true;
    struct { ULONG Version, ControlMask, StateMask; } pt{1, 0x4 /* IGNORE_TIMER_RESOLUTION */, 0};
    if (auto set = (BOOL(WINAPI*)(HANDLE, int, void*, DWORD))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation"))
        set(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &pt, sizeof pt);
    // The finest period the system offers (0.5 ms as a rule), in 100 ns units.
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    auto query = (LONG(WINAPI*)(ULONG*, ULONG*, ULONG*))GetProcAddress(nt, "NtQueryTimerResolution");
    auto set = (LONG(WINAPI*)(ULONG, BOOLEAN, ULONG*))GetProcAddress(nt, "NtSetTimerResolution");
    ULONG coarsest = 0, finest = 0, was = 0, now = 0;
    if (query && set && query(&coarsest, &finest, &was) >= 0 && finest && set(finest, TRUE, &now) >= 0)
        XSL_INFO(CH, "timer resolution %.2f ms for the title's audio threads (was %.2f ms)", now / 10000.0, was / 10000.0);
    else if (HMODULE mm = LoadLibraryW(L"winmm.dll")) {
        if (auto begin = (UINT(WINAPI*)(UINT))GetProcAddress(mm, "timeBeginPeriod")) begin(1);
        XSL_INFO(CH, "timer resolution 1 ms for the title's audio threads");
    }
}
STDAPI XApuConnect(XApuConnectInputParameters* params, XApuConnectOutputParameters* data, XApuHandle* handle) {
    fine_timer();
    if (!params || !data || !handle || !params->maxStreamCount || params->maxStreamCount > 500) return E_INVALIDARG;
    auto* x = new XAPU;
    x->in = *params;
    x->streams.resize(params->maxStreamCount);
    x->base = VirtualAlloc(nullptr, params->baseDataLength ? params->baseDataLength : 16, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!x->base) { delete x; return E_OUTOFMEMORY; }
    if ((uint32_t)params->options & (uint32_t)XApuConnectOptions::EnableResultReadyEvent) x->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    data->baseData = x->base;
    data->resultReadyEvent = x->event;
    *handle = x;
    XSL_INFO(CH, "XApuConnect(process type %u, %u streams x %u commands, %u bytes, %u frames per output, options %#x): software stand-in, %s",
             (unsigned)params->processType, params->maxStreamCount, params->maxQueuedCommandsPerStream, params->baseDataLength,
             params->maxFrameCountPerOutput, (unsigned)params->options,
             params->processType == XApuProcessType::DecodeOpus ? "Opus decoded with the Windows decoder" : "output is silence");
    return S_OK;
}

STDAPI XApuDisconnect(XApuHandle x) {
    if (!x) return E_INVALIDARG;
    {
        std::lock_guard<std::mutex> g(x->mu);
        for (auto& s : x->streams) { destroy_decoder(s.decoder); s.decoder = nullptr; }
    }
    if (x->event) CloseHandle(x->event);
    VirtualFree(x->base, 0, MEM_RELEASE);
    delete x;
    return S_OK;
}

STDAPI XApuEnqueueCommand(XApuHandle x, XApuCommandId* c, uint32_t* total) {
    if (!x || !c) return E_INVALIDARG;
    std::lock_guard<std::mutex> g(x->mu);
    if (c->streamIndex >= x->streams.size()) return XAPU_E_INVALID_STREAM_INDEX;
    if (x->results.size() >= (size_t)x->in.maxStreamCount * (x->in.maxQueuedCommandsPerStream ? x->in.maxQueuedCommandsPerStream : 1)) return XAPU_E_QUEUE_FULL;
    auto& s = x->streams[c->streamIndex];
    // A title may fill in only the type and the stream of a command it builds on its stack, so
    // nothing else of the header is checked, and of each command only the fields its type uses are read.
    const bool decode_only = x->in.processType == XApuProcessType::DecodeOpus;
    XApuResult r{};
    r.id = *c;
    r.errorCode = S_OK;
    r.currentPitch = s.pitch;
    switch (c->type) {
    case XApuCommandType::Activate: {
        auto* a = (XApuDecodeConvertActivateCommand*)c;
        destroy_decoder(s.decoder);
        s = {};
        s.active = true;
        s.channels = a->channelCount == 1 ? 1 : 2;
        s.pitch = !decode_only && a->startPitch > 0 ? a->startPitch : 1.0f;
        r.currentPitch = s.pitch;
        break;
    }
    case XApuCommandType::Deactivate:
        s.active = false;
        destroy_decoder(s.decoder);
        s.decoder = nullptr;
        s.decoder_failed = false;
        s.skip = 0;
        break;
    case XApuCommandType::Reset:
        s.stored = 0;
        s.skip = 0;
        if (s.decoder) {
            // Back to the state right after Activate: forget what the decoder holds.
            s.decoder->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            s.decoder->pcm.clear();
            s.decoder->position = 0;
        }
        break;
    case XApuCommandType::Notify: break;
    case XApuCommandType::Process: {
        auto* p = (XApuDecodeConvertCommand*)c;
        if (decode_only) { decode_process(s, p, r); break; }
        const bool pcm = x->in.processType == XApuProcessType::ConvertPCM;
        const bool convert = pcm || x->in.processType == XApuProcessType::DecodeConvertOpus;
        uint32_t frames = p->frameCount == 0xFFFFFFFFu ? (pcm ? p->inputDataLength / (4 * s.channels) : 960) : p->frameCount;
        if (frames > 960) frames = 960;
        frames = frames > p->firstFrameIndex ? frames - p->firstFrameIndex : 0;
        uint32_t out = frames;
        if (convert) {
            // Input frames at the target pitch give frames / pitch output frames; output leaves in
            // blocks of maxFrameCountPerOutput (any remainder with Flush).
            if (p->targetPitch > 0) s.pitch = p->targetPitch;
            s.stored += (uint32_t)(frames / s.pitch);
            const uint32_t block = x->in.maxFrameCountPerOutput ? x->in.maxFrameCountPerOutput : 512;
            const bool flush = ((uint32_t)c->options & (uint32_t)XApuCommandOptions::Flush) != 0;
            out = s.stored >= block ? block : (flush ? s.stored : 0);
            s.stored -= out;
            uint32_t st = out ? (uint32_t)XApuStateOptions::OutputReady : (uint32_t)XApuStateOptions::NeedMoreInput;
            if (s.stored >= block) st |= (uint32_t)XApuStateOptions::MoreOutputAvailable;
            r.state = (XApuStateOptions)st;
            ((uint16_t*)&r.stateData)[0] = (uint16_t)s.stored;
            ((uint16_t*)&r.stateData)[1] = (uint16_t)frames;
            r.currentPitch = s.pitch;
        }
        uint32_t bytes = out * s.channels * 4;
        if (bytes > p->maxOutputDataLength) { bytes = p->maxOutputDataLength & ~15u; out = bytes / (s.channels * 4); }
        if (p->outputData && p->maxOutputDataLength) memset(p->outputData, 0, p->maxOutputDataLength);
        r.outputFrameCount = out;
        r.outputDataLength = bytes;
        r.outputData = p->outputData;
        r.inputDataLength = p->inputDataLength;
        r.inputData = p->inputData;
        break;
    }
    default: return XAPU_E_INVALID_COMMAND;
    }
    x->results.push_back(r);
    x->commands++;
    if (total) *total = x->commands;
    // In decode-only mode the options word is not trustworthy (see above), so the event is always
    // set there; a wake-up too many costs the title one empty poll.
    if (x->event && (decode_only || ((uint32_t)c->options & (uint32_t)XApuCommandOptions::SignalOnCompletion))) SetEvent(x->event);
    return S_OK;
}

STDAPI XApuDequeueResult(XApuHandle x, XApuResult* out) {
    if (!x || !out) return E_INVALIDARG;
    std::lock_guard<std::mutex> g(x->mu);
    if (x->results.empty()) return XAPU_E_PENDING_RESULTS;
    *out = x->results.front();
    x->results.pop_front();
    x->taken++;
    return S_OK;
}

STDAPI_(uint32_t) XApuGetResultCount(XApuHandle x, uint32_t* total) {
    if (!x) return 0;
    std::lock_guard<std::mutex> g(x->mu);
    if (total) *total = x->taken + (uint32_t)x->results.size();
    return (uint32_t)x->results.size();
}

STDAPI XApuGetUtilization(XApuProcessType, uint32_t, XApuUtilization* u) {
    if (!u) return E_INVALIDARG;
    u->availableStreamCount = 500;
    u->maximumStreamCount = 500;
    return S_OK;
}
}
BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
