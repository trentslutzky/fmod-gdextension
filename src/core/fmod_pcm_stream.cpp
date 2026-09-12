#include "fmod_pcm_stream.h"

#include "helpers/common.h"

#include <chrono>
#include <cstring>

using namespace godot;

namespace {
    int64_t now_usec() {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    uint32_t next_power_of_two(uint32_t p_value) {
        uint32_t result = 1;
        while (result < p_value) { result <<= 1; }
        return result;
    }
}// namespace

// ---- PcmStreamCore ----

uint32_t PcmStreamCore::queued_samples() const {
    return write_idx.load(std::memory_order_acquire) - read_idx.load(std::memory_order_acquire);
}

uint32_t PcmStreamCore::free_samples() const {
    return static_cast<uint32_t>(data.size()) - queued_samples();
}

void PcmStreamCore::release_sound() {
    if (released.exchange(true)) { return; }
    if (sound == nullptr) { return; }
    if (system_alive.load()) {
        ERROR_CHECK(sound->release());
    }
    sound = nullptr;
}

PcmStreamCore* PcmStreamCore::from_sound(FMOD::Sound* p_sound) {
    if (p_sound == nullptr) { return nullptr; }
    void* user_data = nullptr;
    if (p_sound->getUserData(&user_data) != FMOD_OK || user_data == nullptr) { return nullptr; }
    auto* core = static_cast<PcmStreamCore*>(user_data);
    return core->magic == MAGIC ? core : nullptr;
}

// Runs on FMOD's stream thread. Never touches Godot, never allocates.
FMOD_RESULT F_CALL PcmStreamCore::pcm_read(FMOD_SOUND* p_sound, void* p_data, unsigned int p_datalen) {
    PcmStreamCore* core = from_sound(reinterpret_cast<FMOD::Sound*>(p_sound));
    auto* out = static_cast<float*>(p_data);
    const uint32_t wanted = p_datalen / sizeof(float);

    if (core == nullptr) {
        memset(p_data, 0, p_datalen);
        return FMOD_OK;
    }

    if (core->clear_requested.exchange(false)) {
        core->read_idx.store(core->write_idx.load(std::memory_order_acquire), std::memory_order_release);
        core->primed = false;
    }

    const uint32_t queued = core->queued_samples();
    const uint32_t channels = static_cast<uint32_t>(core->channels > 0 ? core->channels : 1);

    if (!core->primed) {
        if (queued < core->preroll_frames.load() * channels) {
            memset(p_data, 0, p_datalen);
            return FMOD_OK;
        }
        core->primed = true;
        const int64_t first_push = core->first_push_usec.exchange(0);
        if (first_push > 0) { core->last_start_latency_usec.store(now_usec() - first_push); }
    }

    const uint32_t available = queued < wanted ? queued : wanted;
    const uint32_t read = core->read_idx.load(std::memory_order_relaxed);
    const uint32_t mask = core->mask;
    const float* ring = core->data.data();

    for (uint32_t i = 0; i < available; ++i) { out[i] = ring[(read + i) & mask]; }
    core->read_idx.store(read + available, std::memory_order_release);

    if (available < wanted) {
        memset(out + available, 0, (wanted - available) * sizeof(float));
        // Only a burst that had started counts as an underrun; the buffer running dry between sentences
        // is normal. Going back to unprimed makes the next burst pre-roll again.
        if (available > 0 || queued > 0) { core->underruns.fetch_add(1); }
        core->primed = false;
    }

    return FMOD_OK;
}

// The ring is ordered in time, not addressable by position, so seeks (including the loop restart) are
// ignored on purpose.
FMOD_RESULT F_CALL PcmStreamCore::pcm_setpos(FMOD_SOUND*, int, unsigned int, FMOD_TIMEUNIT) {
    return FMOD_OK;
}

// ---- FmodPcmStream ----

void FmodPcmStream::_bind_methods() {
    ClassDB::bind_method(D_METHOD("push_frames", "interleaved_frames"), &FmodPcmStream::push_frames);
    ClassDB::bind_method(D_METHOD("get_queued_frames"), &FmodPcmStream::get_queued_frames);
    ClassDB::bind_method(D_METHOD("get_free_frames"), &FmodPcmStream::get_free_frames);
    ClassDB::bind_method(D_METHOD("get_capacity_frames"), &FmodPcmStream::get_capacity_frames);
    ClassDB::bind_method(D_METHOD("clear"), &FmodPcmStream::clear);
    ClassDB::bind_method(D_METHOD("set_preroll_frames", "frames"), &FmodPcmStream::set_preroll_frames);
    ClassDB::bind_method(D_METHOD("get_preroll_frames"), &FmodPcmStream::get_preroll_frames);
    ClassDB::bind_method(D_METHOD("get_underrun_count"), &FmodPcmStream::get_underrun_count);
    ClassDB::bind_method(D_METHOD("get_last_start_latency_usec"), &FmodPcmStream::get_last_start_latency_usec);
    ClassDB::bind_method(D_METHOD("get_sample_rate"), &FmodPcmStream::get_sample_rate);
    ClassDB::bind_method(D_METHOD("get_channels"), &FmodPcmStream::get_channels);
    ClassDB::bind_method(D_METHOD("is_valid"), &FmodPcmStream::is_valid);

    ADD_PROPERTY(PropertyInfo(Variant::INT, "preroll_frames", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NONE), "set_preroll_frames", "get_preroll_frames");
}

Ref<FmodPcmStream> FmodPcmStream::create(FMOD::System* p_core_system, int p_sample_rate, int p_channels, float p_capacity_sec, int p_decode_buffer_frames) {
    if (p_core_system == nullptr) {
        GODOT_LOG_ERROR("FMOD Sound System: cannot create a pcm stream before the system is initialized.")
        return {};
    }
    if (p_sample_rate <= 0 || p_channels <= 0 || p_channels > 2 || p_capacity_sec <= 0.0f) {
        GODOT_LOG_ERROR(vformat("FMOD Sound System: invalid pcm stream format (%d Hz, %d channels, %f s).", p_sample_rate, p_channels, p_capacity_sec))
        return {};
    }

    auto core = std::make_shared<PcmStreamCore>();
    core->sample_rate = p_sample_rate;
    core->channels = p_channels;

    const uint32_t capacity_samples = next_power_of_two(static_cast<uint32_t>(p_capacity_sec * static_cast<float>(p_sample_rate)) * static_cast<uint32_t>(p_channels));
    core->data.assign(capacity_samples, 0.0f);
    core->mask = capacity_samples - 1;

    // FMOD reads two decode buffers ahead as soon as the sound plays, so this is the floor of the
    // stream's own latency; 512 frames is about 10 ms at 48 kHz.
    const int decode_buffer_frames = p_decode_buffer_frames > 0 ? p_decode_buffer_frames : 512;
    // Enough to cover that read-ahead, so a burst does not start with an underrun.
    core->preroll_frames.store(static_cast<uint32_t>(decode_buffer_frames) * 2u);

    FMOD_CREATESOUNDEXINFO exinfo {};
    exinfo.cbsize = sizeof(FMOD_CREATESOUNDEXINFO);
    exinfo.numchannels = p_channels;
    exinfo.defaultfrequency = p_sample_rate;
    exinfo.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    exinfo.decodebuffersize = static_cast<unsigned int>(decode_buffer_frames);
    // The sound never ends; declaring an hour keeps the loop restart out of practical reach and stops
    // FMOD Studio from treating the instrument as a short one-shot.
    exinfo.length = static_cast<unsigned int>(p_sample_rate) * static_cast<unsigned int>(p_channels) * sizeof(float) * 3600u;
    exinfo.pcmreadcallback = &PcmStreamCore::pcm_read;
    exinfo.pcmsetposcallback = &PcmStreamCore::pcm_setpos;
    exinfo.userdata = core.get();

    FMOD::Sound* sound = nullptr;
    if (!ERROR_CHECK_WITH_REASON(
          p_core_system->createSound(nullptr, FMOD_OPENUSER | FMOD_CREATESTREAM | FMOD_LOOP_NORMAL | FMOD_2D, &exinfo, &sound),
          "FMOD Sound System: cannot create the pcm stream sound."
        )) {
        return {};
    }
    core->sound = sound;

    Ref<FmodPcmStream> ref;
    ref.instantiate();
    ref->_core = core;
    return ref;
}

FmodPcmStream::~FmodPcmStream() {
    if (_core == nullptr) { return; }
    _core->orphaned.store(true);
    // A programmer instrument still holding the sound releases it from its DESTROY callback instead.
    if (_core->in_use.load() == 0) { _core->release_sound(); }
}

int FmodPcmStream::push_frames(const PackedFloat32Array& p_interleaved_frames) {
    if (!is_valid()) { return 0; }
    PcmStreamCore& core = *_core;
    const uint32_t channels = static_cast<uint32_t>(core.channels);
    const uint32_t incoming = static_cast<uint32_t>(p_interleaved_frames.size()) / channels * channels;
    const uint32_t free_now = core.free_samples();
    const uint32_t accepted = incoming < free_now ? incoming : free_now / channels * channels;

    if (accepted == 0) { return 0; }

    if (core.queued_samples() == 0) { core.first_push_usec.store(now_usec()); }

    const uint32_t write = core.write_idx.load(std::memory_order_relaxed);
    const float* src = p_interleaved_frames.ptr();
    float* ring = core.data.data();
    const uint32_t mask = core.mask;

    for (uint32_t i = 0; i < accepted; ++i) { ring[(write + i) & mask] = src[i]; }
    core.write_idx.store(write + accepted, std::memory_order_release);

    return static_cast<int>(accepted / channels);
}

int FmodPcmStream::get_queued_frames() const {
    if (_core == nullptr) { return 0; }
    return static_cast<int>(_core->queued_samples() / static_cast<uint32_t>(_core->channels));
}

int FmodPcmStream::get_free_frames() const {
    if (_core == nullptr) { return 0; }
    return static_cast<int>(_core->free_samples() / static_cast<uint32_t>(_core->channels));
}

int FmodPcmStream::get_capacity_frames() const {
    if (_core == nullptr) { return 0; }
    return static_cast<int>(_core->data.size() / static_cast<size_t>(_core->channels));
}

void FmodPcmStream::clear() {
    if (_core == nullptr) { return; }
    _core->clear_requested.store(true);
}

void FmodPcmStream::set_preroll_frames(int p_frames) {
    if (_core == nullptr) { return; }
    _core->preroll_frames.store(static_cast<uint32_t>(p_frames < 0 ? 0 : p_frames));
}

int FmodPcmStream::get_preroll_frames() const {
    if (_core == nullptr) { return 0; }
    return static_cast<int>(_core->preroll_frames.load());
}

int FmodPcmStream::get_underrun_count() const {
    if (_core == nullptr) { return 0; }
    return static_cast<int>(_core->underruns.load());
}

int FmodPcmStream::get_last_start_latency_usec() const {
    if (_core == nullptr) { return -1; }
    return static_cast<int>(_core->last_start_latency_usec.load());
}

int FmodPcmStream::get_sample_rate() const {
    return _core == nullptr ? 0 : _core->sample_rate;
}

int FmodPcmStream::get_channels() const {
    return _core == nullptr ? 0 : _core->channels;
}

bool FmodPcmStream::is_valid() const {
    return _core != nullptr && _core->sound != nullptr && !_core->released.load() && _core->system_alive.load();
}

FMOD::Sound* FmodPcmStream::get_sound() const {
    return is_valid() ? _core->sound : nullptr;
}
