#include "fmod_studio.hpp"
#include <core/fmod_pcm_stream.h>
#include <studio/fmod_event.h>
#include "fmod_server.h"

#include <callback/event_callbacks.h>

#include <variant/callable.hpp>
#include <variant/dictionary.hpp>

namespace Callbacks {

    FMOD_RESULT F_CALL event_callback(FMOD_STUDIO_EVENT_CALLBACK_TYPE type, FMOD_STUDIO_EVENTINSTANCE* event, void* parameters) {
        auto* instance = reinterpret_cast<FMOD::Studio::EventInstance*>(event);
        godot::FmodEvent* event_instance;
        instance->getUserData((void**) &event_instance);

        if (!event_instance && type == FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND) {
            auto* props { reinterpret_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters) };
            if (godot::PcmStreamCore* core = godot::PcmStreamCore::from_sound((FMOD::Sound*) props->sound)) {
                if (core->in_use.fetch_sub(1) == 1 && core->orphaned.load()) { core->release_sound(); }
            }
            return FMOD_OK;
        }

        if (event_instance) {
            if (type == FMOD_STUDIO_EVENT_CALLBACK_CREATE_PROGRAMMER_SOUND) {
                auto* props { reinterpret_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters) };

                // A pcm stream attached to the event takes precedence over an audio table key. The stream
                // owns its sound, so it is handed over as-is and only counted, never created or released here.
                if (FMOD::Sound* stream_sound = event_instance->get_programmer_stream_sound()) {
                    if (godot::PcmStreamCore* core = godot::PcmStreamCore::from_sound(stream_sound)) {
                        core->in_use.fetch_add(1);
                        props->sound = (FMOD_SOUND*) stream_sound;
                        props->subsoundIndex = -1;
                        return FMOD_OK;
                    }
                }

                const godot::String& sound_key {event_instance->get_programmers_callback_sound_key()};
                FMOD_STUDIO_SOUND_INFO sound_info {godot::FmodServer::get_singleton()->get_sound_info(sound_key)};
                FMOD::Sound* sound {
                        godot::FmodServer::get_singleton()->create_sound(sound_info, FMOD_LOOP_NORMAL | FMOD_CREATECOMPRESSEDSAMPLE | FMOD_NONBLOCKING)
                };

                props->sound = (FMOD_SOUND*) sound;
                props->subsoundIndex = sound_info.subsoundindex;

                return FMOD_OK;
            }
            if (type == FMOD_STUDIO_EVENT_CALLBACK_DESTROY_PROGRAMMER_SOUND) {
                auto* props { reinterpret_cast<FMOD_STUDIO_PROGRAMMER_SOUND_PROPERTIES*>(parameters) };
                auto* sound {(FMOD::Sound*) props->sound};

                // Stream sounds outlive the instrument; the stream (or, once its wrapper is gone, the last
                // instrument to let go) releases them.
                if (godot::PcmStreamCore* core = godot::PcmStreamCore::from_sound(sound)) {
                    if (core->in_use.fetch_sub(1) == 1 && core->orphaned.load()) { core->release_sound(); }
                    return FMOD_OK;
                }

                ERROR_CHECK(sound->release());

                return FMOD_OK;
            }

            godot::Dictionary dictionary;
            if (type == FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_MARKER) {
                auto* props { reinterpret_cast<FMOD_STUDIO_TIMELINE_MARKER_PROPERTIES*>(parameters) };
                dictionary["name"] = props->name;
                dictionary["position"] = props->position;
            } else if (type == FMOD_STUDIO_EVENT_CALLBACK_TIMELINE_BEAT) {
                auto* props { reinterpret_cast<FMOD_STUDIO_TIMELINE_BEAT_PROPERTIES*>(parameters) };
                dictionary["beat"] = props->beat;
                dictionary["bar"] = props->bar;
                dictionary["tempo"] = props->tempo;
                dictionary["time_signature_upper"] = props->timesignatureupper;
                dictionary["time_signature_lower"] = props->timesignaturelower;
                dictionary["position"] = props->position;
            }
            const godot::Callable& callback {event_instance->get_callback()};
            if (!callback.is_null() && callback.is_valid()) {
                godot::FmodServer::get_singleton()->add_callback(
                        {
                            type,
                            callback,
                            dictionary
                        }
                );
            }
        }

        return FMOD_OK;
    }
}// namespace Callbacks
