#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

#if JUCE_MAC || JUCE_LINUX
 #include <dlfcn.h>
#endif

namespace zynforge
{
    // NDI audio transmitter — broadcasts ZynForge's master mix onto
    // the LAN as a stereo NDI Audio source. Any NDI receiver (NDI
    // Tools, OBS Studio, vMix, TouchDesigner …) on the same network
    // can subscribe.
    //
    // NDI SDK is NOT linked at build time. We dlopen libndi at runtime;
    // if the dylib is absent, every call is a graceful no-op and the
    // engineer sees a clear status message. The user must install the
    // free NDI runtime from ndi.tv to actually transmit.
    //
    // Stereo only for v1 — the master bus is already a stereo sum.
    // Multi-channel NDI Audio v3 + AES67 support are future work.
    class NDIBridge
    {
    public:
        NDIBridge() = default;
        ~NDIBridge() { stop(); }

        // True iff the NDI runtime dylib was found AND we successfully
        // started a transmit instance.
        bool isEnabled() const noexcept { return enabled.load (std::memory_order_relaxed); }

        // Best-effort discovery of the NDI runtime on macOS. Looks at
        // the standard system paths Audinate / NewTek install to.
        static juce::String findRuntimePath()
        {
           #if JUCE_MAC
            for (const char* p : {
                "/usr/local/lib/libndi.dylib",
                "/usr/local/lib/libndi_advanced.dylib",
                "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib"
            })
            {
                if (juce::File (p).existsAsFile()) return juce::String (p);
            }
           #endif
            return {};
        }

        // Start broadcasting under the given source name (visible in
        // NDI receivers on the LAN). sampleRate is needed so we can
        // tell NDI the rate of the audio frames we'll push.
        // Returns false if the runtime is absent or send_create failed.
        bool start (const juce::String& sourceName, double sampleRate)
        {
            stop();
            sr = sampleRate;
            if (! loadLibrary()) return false;

            // NDIlib_initialize() — bring the runtime up.
            using InitFn = bool (*)();
            auto initFn = (InitFn) sym ("NDIlib_initialize");
            if (initFn == nullptr || ! initFn()) return false;

            // NDIlib_send_create_v2 takes a struct; we lay it out as
            // { const char* p_ndi_name; const char* p_groups; bool
            // clock_video; bool clock_audio; } which matches the
            // ABI of NDIlib_send_create_t across SDK versions.
            struct CreateDesc
            {
                const char* p_ndi_name;
                const char* p_groups;
                bool        clock_video;
                bool        clock_audio;
            };
            std::string name = sourceName.toStdString();
            CreateDesc desc { name.c_str(), nullptr, false, false };

            using CreateFn = void* (*)(const CreateDesc*);
            auto createFn = (CreateFn) sym ("NDIlib_send_create");
            if (createFn == nullptr) { unloadLibrary(); return false; }
            sender = createFn (&desc);
            if (sender == nullptr) { unloadLibrary(); return false; }

            sendAudioFn = sym ("NDIlib_send_send_audio_v2");
            if (sendAudioFn == nullptr)
            {
                // Older SDKs use NDIlib_send_send_audio.
                sendAudioFn = sym ("NDIlib_send_send_audio");
            }
            enabled.store (sendAudioFn != nullptr, std::memory_order_relaxed);
            return enabled.load();
        }

        void stop()
        {
            if (sender != nullptr)
            {
                using DestroyFn = void (*)(void*);
                if (auto fn = (DestroyFn) sym ("NDIlib_send_destroy"))
                    fn (sender);
                sender = nullptr;
            }
            if (using InitFn = void (*)(); auto fn = (InitFn) sym ("NDIlib_destroy"))
                fn();
            unloadLibrary();
            enabled.store (false, std::memory_order_relaxed);
        }

        // Push numSamples of interleaved stereo audio. Called from the
        // audio thread after the master bus downcast. Cheap: just a
        // memcpy into our scratch + a single libndi call.
        void pushStereo (const float* L, const float* R, int numSamples) noexcept
        {
            if (! enabled.load (std::memory_order_relaxed) || sender == nullptr
                || sendAudioFn == nullptr || L == nullptr || R == nullptr) return;

            // NDIlib_audio_frame_v2_t layout — channel-interleaved
            // float32, channel_stride_in_bytes set to one channel
            // length so the receiver reads planar.
            struct AudioFrame
            {
                int          sample_rate;
                int          no_channels;
                int          no_samples;
                juce::int64  timecode;
                float*       p_data;
                int          channel_stride_in_bytes;
                const char*  p_metadata;
                juce::int64  timestamp;
            };

            if ((int) scratch.size() < numSamples * 2)
                scratch.resize ((size_t) numSamples * 2);
            // Planar: [L0 L1 ... Lk] [R0 R1 ... Rk]
            std::memcpy (scratch.data(),                  L, sizeof (float) * (size_t) numSamples);
            std::memcpy (scratch.data() + numSamples,     R, sizeof (float) * (size_t) numSamples);

            AudioFrame f {};
            f.sample_rate              = (int) sr;
            f.no_channels              = 2;
            f.no_samples               = numSamples;
            f.timecode                 = (juce::int64) -1;
            f.p_data                   = scratch.data();
            f.channel_stride_in_bytes  = numSamples * (int) sizeof (float);
            f.p_metadata               = nullptr;
            f.timestamp                = 0;

            using SendFn = void (*)(void*, const AudioFrame*);
            ((SendFn) sendAudioFn) (sender, &f);
        }

    private:
        bool loadLibrary()
        {
           #if JUCE_MAC || JUCE_LINUX
            const auto p = findRuntimePath();
            if (p.isEmpty()) return false;
            lib = dlopen (p.toRawUTF8(), RTLD_NOW);
            return lib != nullptr;
           #else
            return false;
           #endif
        }

        void unloadLibrary()
        {
           #if JUCE_MAC || JUCE_LINUX
            if (lib != nullptr) { dlclose (lib); lib = nullptr; }
           #endif
            sendAudioFn = nullptr;
        }

        void* sym (const char* name)
        {
           #if JUCE_MAC || JUCE_LINUX
            return lib != nullptr ? dlsym (lib, name) : nullptr;
           #else
            juce::ignoreUnused (name); return nullptr;
           #endif
        }

        void*               lib         { nullptr };
        void*               sender      { nullptr };
        void*               sendAudioFn { nullptr };
        std::atomic<bool>   enabled     { false };
        double              sr          { 48000.0 };
        std::vector<float>  scratch;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NDIBridge)
    };
}
