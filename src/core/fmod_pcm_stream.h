#ifndef GODOTFMOD_FMOD_PCM_STREAM_H
#define GODOTFMOD_FMOD_PCM_STREAM_H

#include "classes/ref_counted.hpp"
#include "fmod.hpp"
#include "variant/packed_float32_array.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

    // Shared state behind an FmodPcmStream. It is deliberately not a Godot object: FMOD reads it from its
    // own stream thread, and a programmer instrument can still be holding the sound after the GDScript side
    // has dropped the wrapper. FmodServer keeps it alive until the sound has been released.
    struct PcmStreamCore {
        static constexpr uint32_t MAGIC = 0x50434d53;// "PCMS"

        uint32_t magic = MAGIC;

        FMOD::Sound* sound = nullptr;
        int sample_rate = 0;
        int channels = 0;

        // Single-producer (Godot main thread) / single-consumer (FMOD stream thread) ring of interleaved
        // floats. Indices only ever grow; the mask maps them onto the buffer. Capacity is a power of two.
        std::vector<float> data;
        uint32_t mask = 0;
        std::atomic<uint32_t> write_idx {0};
        std::atomic<uint32_t> read_idx {0};

        // Producer asks, consumer applies, so the producer never writes the consumer's index.
        std::atomic<bool> clear_requested {false};

        // Programmer instruments currently holding the sound (CREATE / DESTROY callbacks).
        std::atomic<int> in_use {0};
        // The Godot wrapper is gone; whoever drops in_use to zero releases the sound.
        std::atomic<bool> orphaned {false};
        std::atomic<bool> released {false};
        std::atomic<bool> system_alive {true};

        // Frames that have to be queued before a fresh burst starts playing, to absorb network jitter.
        std::atomic<uint32_t> preroll_frames {0};
        // Consumer-only: whether the current burst has passed the pre-roll.
        bool primed = false;

        // Diagnostics.
        std::atomic<uint32_t> underruns {0};
        std::atomic<int64_t> first_push_usec {0};
        std::atomic<int64_t> last_start_latency_usec {-1};

        uint32_t queued_samples() const;
        uint32_t free_samples() const;
        void release_sound();

        static PcmStreamCore* from_sound(FMOD::Sound* p_sound);
        static FMOD_RESULT F_CALL pcm_read(FMOD_SOUND* p_sound, void* p_data, unsigned int p_datalen);
        static FMOD_RESULT F_CALL pcm_setpos(FMOD_SOUND* p_sound, int p_subsound, unsigned int p_position, FMOD_TIMEUNIT p_postype);
    };

    // A sound FMOD pulls PCM from as it plays, fed from GDScript with push_frames(). Hand it to an event
    // whose timeline has a programmer instrument via FmodEvent::set_programmer_sound_stream (or the same
    // method on an emitter) and the instrument plays whatever has been pushed, with the event's effect
    // chain applied. Silence is played while the buffer is empty.
    class FmodPcmStream : public RefCounted {
        GDCLASS(FmodPcmStream, RefCounted)

        std::shared_ptr<PcmStreamCore> _core;

    public:
        FmodPcmStream() = default;
        ~FmodPcmStream() override;

        static Ref<FmodPcmStream> create(FMOD::System* p_core_system, int p_sample_rate, int p_channels, float p_capacity_sec, int p_decode_buffer_frames);

        int push_frames(const PackedFloat32Array& p_interleaved_frames);
        int get_queued_frames() const;
        int get_free_frames() const;
        int get_capacity_frames() const;
        void clear();
        void set_preroll_frames(int p_frames);
        int get_preroll_frames() const;
        int get_underrun_count() const;
        int get_last_start_latency_usec() const;
        int get_sample_rate() const;
        int get_channels() const;
        bool is_valid() const;

        FMOD::Sound* get_sound() const;
        const std::shared_ptr<PcmStreamCore>& get_core() const { return _core; }

    protected:
        static void _bind_methods();
    };
}// namespace godot

#endif// GODOTFMOD_FMOD_PCM_STREAM_H
