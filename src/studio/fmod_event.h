#ifndef GODOTFMOD_FMOD_EVENT_H
#define GODOTFMOD_FMOD_EVENT_H

#include "classes/ref_counted.hpp"
#include "core/fmod_pcm_stream.h"
#include "fmod_studio.hpp"
#include "helpers/common.h"

#include <atomic>

namespace godot {
    class FmodEvent : public RefCounted {
        FMODCLASS(FmodEvent, RefCounted, FMOD::Studio::EventInstance);

        Callable eventCallback;
        String programmers_callback_sound_key;
        Ref<FmodPcmStream> programmer_sound_stream;
        // Read from FMOD's studio thread inside the programmer sound callbacks.
        std::atomic<FMOD::Sound*> programmer_stream_sound {nullptr};
        float distanceScale = 1.0f;
        uint32_t callback_mask = 0;

    public:
        FmodEvent() = default;
        ~FmodEvent() override;

        float get_parameter_by_name(const String& parameter_name) const;
        void set_parameter_by_name(const String& parameter_name, float value) const;
        void set_parameter_by_name_with_label(const String& parameter_name, const String& label, bool ignoreseekspeed = false) const;
        float get_parameter_by_id(uint64_t long_id) const;
        float get_parameter_by_fmod_id(const FMOD_STUDIO_PARAMETER_ID& parameter_id) const;
        void set_parameter_by_id(uint64_t long_id, float value) const;
        void set_parameter_by_fmod_id(const FMOD_STUDIO_PARAMETER_ID& parameter_id, float value) const;
        void set_parameter_by_fmod_id_with_label(const FMOD_STUDIO_PARAMETER_ID& parameter_id, const String& label, bool ignoreseekspeed = false) const;
        void set_parameter_by_id_with_label(uint64_t parameter_id, const String& label, bool ignoreseekspeed = false) const;
        void release() const;
        void start() const;
        void stop(int stopMode) const;
        void event_key_off() const;
        FMOD_STUDIO_PLAYBACK_STATE get_playback_state() const;
        bool get_paused() const;
        void set_paused(bool paused) const;
        float get_pitch() const;
        void set_pitch(float pitch) const;
        float get_volume();
        void set_volume(float volume) const;
        int get_timeline_position() const;
        void set_timeline_position(int position) const;
        float get_reverb_level(int index) const;
        void set_reverb_level(int index, float level) const;
        bool is_virtual() const;
        void set_listener_mask(unsigned int mask) const;
        uint32_t get_listener_mask() const;
        Transform3D get_3d_attributes() const;
        Transform2D get_2d_attributes() const;
        void set_2d_attributes(const Transform2D& position) const;
        void set_3d_attributes(const Transform3D& transform) const;
        void set_node_attributes(Node* node) const;
        void set_callback(const Callable& callback, uint32_t p_callback_mask);
        const Callable& get_callback() const;
        void set_programmer_callback(const String& p_programmers_callback_sound_key);
        const String& get_programmers_callback_sound_key() const;
        void set_programmer_sound_stream(const Ref<FmodPcmStream>& p_stream);
        Ref<FmodPcmStream> get_programmer_sound_stream() const;
        FMOD::Sound* get_programmer_stream_sound() const;
        void set_distance_scale(float scale);

    protected:
        static void _bind_methods();
    };
}// namespace godot

VARIANT_ENUM_CAST(FMOD_STUDIO_PLAYBACK_STATE)

#endif// GODOTFMOD_FMOD_EVENT_H
