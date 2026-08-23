#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_websocket_client.h"

#include "audio_io.h"
#include "dg_agent.h"
#include "faces.h"
#include "ui.h"
#include "voices.h"

static const char *TAG = "dg_agent";

#define DG_AGENT_URI "wss://agent.deepgram.com/v1/agent/converse"

/* Frames bigger than this are delivered to WEBSOCKET_EVENT_DATA in slices. */
#define WS_RX_BUFFER      4096
/* Ceiling on a reassembled JSON message. AgentThinking with a long chain of
 * thought is the biggest thing the server sends; 8 kB has plenty of room. */
#define JSON_REASSEMBLY_MAX 8192

/* Deepgram closes an idle Agent socket after ~10 s of no audio. */
#define KEEPALIVE_PERIOD_MS 5000

#define WS_OPCODE_CONT   0x00
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02

#define SEND_TIMEOUT pdMS_TO_TICKS(2000)

/*
 * Mic audio uses the same deadline as everything else, and it must stay
 * generous. A short one looks attractive -- this send runs on the priority-7
 * capture task, so blocking stalls esp_codec_dev_read() -- but it is a trap:
 * esp_transport_ssl_write() returns 0 when its write poll times out, and the
 * WebSocket client treats a zero-length write as fatal and tears the session
 * down. At 31 sends a second over a link that is also carrying TTS audio down,
 * a 200 ms deadline dropped the session every few seconds and the agent
 * re-greeted on every reconnect.
 *
 * So the cost of being impatient here is the whole conversation, not one 32 ms
 * chunk. If capture stalls become a real problem, the fix is to move the send
 * off the capture task, not to shorten this.
 */
#define AUDIO_SEND_TIMEOUT SEND_TIMEOUT

static esp_websocket_client_handle_t s_client;
static dg_agent_callbacks_t s_cb;
static volatile bool s_ready;
static TaskHandle_t s_keepalive_task;
static volatile bool s_suppress_state;

/* Reassembly buffer for JSON messages split across several DATA events. */
static char *s_json;
static int s_json_len;

/*
 * The last few turns, replayed into the next session's Settings so that
 * reopening the socket resumes the conversation rather than restarting it.
 *
 * Deliberately small. Every entry is re-sent on every connect, Settings is
 * already ~1.8 kB with the voice catalogue in it, and this device has spent
 * real effort keeping the uplink healthy -- so this is the last few exchanges
 * for continuity, not a transcript.
 */
#define HISTORY_TURNS   6
#define HISTORY_CONTENT 160

typedef struct {
    char role[10];                  /* "user" / "assistant" */
    char content[HISTORY_CONTENT];
} history_turn_t;

static history_turn_t s_history[HISTORY_TURNS];
static int s_history_count;         /* saturates at HISTORY_TURNS */
static int s_history_next;          /* ring cursor */

/* Set when a setting change needs a new session; acted on once the agent has
 * finished saying so, then cleared. */
static bool s_reload_pending;

void dg_agent_clear_history(void)
{
    s_history_count = 0;
    s_history_next = 0;
}

static void history_add(const char *role, const char *content)
{
    history_turn_t *t = &s_history[s_history_next];
    strlcpy(t->role, role, sizeof(t->role));
    strlcpy(t->content, content, sizeof(t->content));
    s_history_next = (s_history_next + 1) % HISTORY_TURNS;
    if (s_history_count < HISTORY_TURNS) {
        s_history_count++;
    }
}

/* Oldest first, which is the order the server expects. */
static void history_to_json(cJSON *agent)
{
    if (s_history_count == 0) {
        return;
    }
    cJSON *messages = cJSON_AddArrayToObject(
        cJSON_AddObjectToObject(agent, "context"), "messages");

    int start = (s_history_count == HISTORY_TURNS) ? s_history_next : 0;
    for (int i = 0; i < s_history_count; i++) {
        const history_turn_t *t = &s_history[(start + i) % HISTORY_TURNS];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "type", "History");
        cJSON_AddStringToObject(m, "role", t->role);
        cJSON_AddStringToObject(m, "content", t->content);
        cJSON_AddItemToArray(messages, m);
    }
}

static void set_state(dg_agent_state_t state)
{
    s_ready = (state == DG_AGENT_READY);
    /* s_ready still tracks reality while suppressed -- only the notification is
     * withheld, so nothing starts sending audio into a closing socket. */
    if (s_cb.on_state && !s_suppress_state) {
        s_cb.on_state(state, s_cb.ctx);
    }
}

void dg_agent_suppress_state_events(bool suppress)
{
    s_suppress_state = suppress;
}

static esp_err_t send_json(cJSON *root, const char *what)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }
#if CONFIG_DEEPGRAM_LOG_WIRE_JSON
    ESP_LOGI(TAG, "-> %s", text);
#endif

    int sent = esp_websocket_client_send_text(s_client, text, strlen(text), SEND_TIMEOUT);
    esp_err_t err = (sent < 0) ? ESP_FAIL : ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to send %s", what);
    } else {
        ESP_LOGI(TAG, "sent %s (%d bytes)", what, sent);
    }

    cJSON_free(text);
    return err;
}

/*
 * Applies a voice and tells the agent it worked.
 *
 * ORDERING MATTERS. UpdateSpeak goes first, because the FunctionCallResponse is
 * what prompts the agent's next turn and the next turn is exactly when Flux
 * starts using the new voice. Send them the other way round and the
 * confirmation is spoken in the old voice, one turn late.
 *
 * Both are plain text frames, so sending them from here -- the WebSocket task,
 * inside event dispatch -- is safe: client->lock is recursive and this task
 * already owns it. send_settings() has always done the same thing from
 * WEBSOCKET_EVENT_CONNECTED. What must never happen here is a stop/close, which
 * the client refuses by comparing task handles.
 */
static void send_function_response(const char *id, const char *name, const char *content)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "FunctionCallResponse");
    cJSON_AddStringToObject(root, "id", id);
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "content", content);
    send_json(root, "FunctionCallResponse");
    cJSON_Delete(root);
}


/*
 * WHY THIS REOPENS THE SESSION
 *
 * UpdateSpeak is the documented way to change the voice in place. On this
 * account it answers SpeakUpdated and then changes nothing -- reproduced with a
 * bare UpdateSpeak sent nowhere near a function call, with both a Flux v2 and
 * an Aura v1 provider, against JSON matching the documented example exactly. A
 * fresh Settings message does work, so that is what this does.
 *
 * The cost is a new session, which is why dg_agent keeps a short history and
 * replays it -- see history_to_json().
 */
/*
 * `arguments` arrives as a JSON-encoded *string*, not a nested object, so it
 * needs a second parse. Caller owns the result and must cJSON_Delete it.
 */
static cJSON *function_args(const cJSON *fn)
{
    const cJSON *args_str = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
    if (!cJSON_IsString(args_str)) {
        return NULL;
    }
    return cJSON_Parse(args_str->valuestring);
}

static void handle_function_call(const cJSON *root)
{
    const cJSON *functions = cJSON_GetObjectItemCaseSensitive(root, "functions");
    if (!cJSON_IsArray(functions)) {
        return;
    }

    const cJSON *fn;
    cJSON_ArrayForEach(fn, functions) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(fn, "id");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        const cJSON *client_side = cJSON_GetObjectItemCaseSensitive(fn, "client_side");
        if (!cJSON_IsString(id) || !cJSON_IsString(name)) {
            continue;
        }
        /* false means Deepgram already ran it against an endpoint of ours. */
        if (cJSON_IsBool(client_side) && !cJSON_IsTrue(client_side)) {
            continue;
        }

        char content[128];

        if (strcmp(name->valuestring, "reset_voice") == 0) {
            const voice_t *v = voices_default();
            voices_reset();
            s_reload_pending = true;
            snprintf(content, sizeof(content),
                     "Switching back to %s. Tell the user you are changing voice now.",
                     v->name);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "adjust_volume") == 0) {
            int delta = 0;
            cJSON *vargs = function_args(fn);
            if (vargs != NULL) {
                const cJSON *d = cJSON_GetObjectItemCaseSensitive(vargs, "delta");
                if (cJSON_IsNumber(d)) {
                    delta = d->valueint;
                }
                cJSON_Delete(vargs);
            }

            int before = audio_io_get_volume();
            int now = audio_io_adjust_volume(delta);
            /*
             * No reload: volume is a codec register, not a Settings field, so
             * it is already in effect -- the agent's confirmation is itself
             * spoken at the new level. Phrased as done, not as about to happen.
             */
            if (now == before && delta > 0) {
                snprintf(content, sizeof(content),
                         "Already at maximum volume, %d. Say so and do not try again.", now);
            } else if (now == before && delta < 0) {
                snprintf(content, sizeof(content),
                         "Already at minimum volume, %d. Say so and do not try again.", now);
            } else {
                snprintf(content, sizeof(content), "Volume is now %d out of 100.", now);
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_face") == 0) {
            int index = -1;
            const char *wanted = NULL;
            cJSON *fargs = function_args(fn);
            if (fargs != NULL) {
                const cJSON *f = cJSON_GetObjectItemCaseSensitive(fargs, "face");
                if (cJSON_IsString(f)) {
                    wanted = f->valuestring;
                    index = faces_find(wanted);
                }
            }

            if (index < 0) {
                ESP_LOGW(TAG, "EVT setface req=\"%s\" -> unknown",
                         (wanted != NULL) ? wanted : "");
                snprintf(content, sizeof(content),
                         "There is no '%s' face. Ask which one they want.",
                         (wanted != NULL) ? wanted : "");
            } else {
                /*
                 * No reload: the face is a local display setting, already in
                 * effect by the time this is spoken. Phrased as done, not as
                 * about to happen -- the opposite of set_voice below.
                 */
                ui_set_face(index);
                /* The requested string, not just the resolved face: whether the
                 * model picks this function from an indirect phrasing is the
                 * thing being tested, and that is only visible in what it sent. */
                ESP_LOGI(TAG, "EVT setface req=\"%s\" -> %s", wanted,
                         faces_name((size_t)index));
                snprintf(content, sizeof(content),
                         "The screen is now showing the %s. Say so briefly.",
                         faces_name((size_t)index));
            }
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_voice") != 0) {
            send_function_response(id->valuestring, name->valuestring, "Unknown function.");
            continue;
        }

        const voice_t *v = NULL;
        cJSON *args = function_args(fn);
        if (args != NULL) {
            const cJSON *want = cJSON_GetObjectItemCaseSensitive(args, "voice");
            if (cJSON_IsString(want)) {
                v = voices_find(want->valuestring);
            }
            cJSON_Delete(args);
        }

        if (v == NULL) {
            /* Say so rather than staying silent: the agent turns this into an
             * explanation, and nothing is applied or saved. */
            send_function_response(id->valuestring, name->valuestring,
                                   "That voice is not available on this device.");
            continue;
        }

        ESP_LOGI(TAG, "voice change requested: %s", v->model);
        /* Persisted now rather than on an acknowledgement, because the new
         * session reads it while building its Settings. */
        voices_set(v);
        s_reload_pending = true;
        snprintf(content, sizeof(content),
                 "Switching to %s. Tell the user you are changing voice now.", v->name);
        send_function_response(id->valuestring, name->valuestring, content);
    }
}

/*
 * Builds the one message the session cannot start without.
 *
 * The shape below is the whole contract: `audio.input` describes what we will
 * send, `audio.output` what we want back, and `agent.{listen,think,speak}`
 * picks the three models Deepgram wires together internally. Omitting
 * audio.output entirely is legal but leaves the format to the server's default,
 * which is not what a fixed-rate codec on the other end wants.
 */
static esp_err_t send_settings(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "type", "Settings");

    cJSON *audio = cJSON_AddObjectToObject(root, "audio");
    cJSON *input = cJSON_AddObjectToObject(audio, "input");
    cJSON_AddStringToObject(input, "encoding", DG_AUDIO_ENCODING);
    cJSON_AddNumberToObject(input, "sample_rate", DG_AUDIO_SAMPLE_RATE);
    cJSON *output = cJSON_AddObjectToObject(audio, "output");
    cJSON_AddStringToObject(output, "encoding", DG_AUDIO_ENCODING);
    cJSON_AddNumberToObject(output, "sample_rate", DG_AUDIO_SAMPLE_RATE);
    /* Already the default, but stated because Flux TTS *rejects* containers and
     * compressed encodings rather than ignoring them -- better a loud failure
     * than a silent format mismatch feeding the codec. */
    cJSON_AddStringToObject(output, "container", "none");

    cJSON *agent = cJSON_AddObjectToObject(root, "agent");

    cJSON *listen_provider = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(agent, "listen"), "provider");
    cJSON_AddStringToObject(listen_provider, "type", "deepgram");

#if CONFIG_SPEECH_STACK_FLUX
    /*
     * Flux. `version` is what selects it -- the model name alone is not enough,
     * and v1 is assumed when the field is absent.
     *
     * No `language` here: that is a v1 listen-provider option. Flux takes
     * `language_hints` instead, and flux-general-en already implies English.
     * Thresholds are omitted unless configured, because the server defaults are
     * the right starting point.
     */
    cJSON_AddStringToObject(listen_provider, "version", "v2");
    cJSON_AddStringToObject(listen_provider, "model", "flux-general-en");
    if (strlen(CONFIG_DEEPGRAM_FLUX_EOT_THRESHOLD) > 0) {
        cJSON_AddStringToObject(listen_provider, "eot_threshold",
                                CONFIG_DEEPGRAM_FLUX_EOT_THRESHOLD);
    }
#if CONFIG_DEEPGRAM_FLUX_EOT_TIMEOUT_MS > 0
    cJSON_AddNumberToObject(listen_provider, "eot_timeout_ms",
                            CONFIG_DEEPGRAM_FLUX_EOT_TIMEOUT_MS);
#endif
#else
    cJSON_AddStringToObject(agent, "language", "en");
    cJSON_AddStringToObject(listen_provider, "model", "nova-3");
#endif

    cJSON *think = cJSON_AddObjectToObject(agent, "think");
    cJSON *think_provider = cJSON_AddObjectToObject(think, "provider");
    cJSON_AddStringToObject(think_provider, "type", "open_ai");
    cJSON_AddStringToObject(think_provider, "model", "gpt-4o-mini");
    cJSON_AddStringToObject(think, "prompt", CONFIG_DEEPGRAM_AGENT_PROMPT);

    /*
     * Client-side functions, which is signalled by the *absence* of an
     * "endpoint" -- with one, Deepgram would call a web service instead of
     * asking us.
     *
     * The array itself is not Flux-gated: volume is a local codec setting and
     * works on either speech stack. Only the voice functions are, because that
     * catalog is entirely flux-* models.
     */
    cJSON *functions = cJSON_AddArrayToObject(think, "functions");

    cJSON *adjust_volume = cJSON_CreateObject();
    cJSON_AddStringToObject(adjust_volume, "name", "adjust_volume");
    cJSON_AddStringToObject(adjust_volume, "description",
                            "Make yourself louder or quieter, relative to how loud you are "
                            "now. Negative is quieter, positive is louder. The scale runs "
                            "20 to 100, so once you are told you are at a limit, stop trying. "
                            "A small change is about 10, a big one about 30.");
    cJSON *vparams = cJSON_AddObjectToObject(adjust_volume, "parameters");
    cJSON_AddStringToObject(vparams, "type", "object");
    cJSON *vprops = cJSON_AddObjectToObject(vparams, "properties");
    cJSON *delta_prop = cJSON_AddObjectToObject(vprops, "delta");
    cJSON_AddStringToObject(delta_prop, "type", "integer");
    cJSON_AddNumberToObject(delta_prop, "minimum", -100);
    cJSON_AddNumberToObject(delta_prop, "maximum", 100);
    cJSON *vrequired = cJSON_AddArrayToObject(vparams, "required");
    cJSON_AddItemToArray(vrequired, cJSON_CreateString("delta"));
    cJSON_AddItemToArray(functions, adjust_volume);

    /*
     * Not Flux-gated either: the display is local and works on either speech
     * stack. The catalog goes in the description for the same reason set_voice
     * does it -- JSON Schema has nowhere to hang a per-enum-value note, and
     * without one the model is choosing between two bare nouns.
     */
    char faces[512];
    faces_describe(faces, sizeof(faces));
    char face_desc[700];
    snprintf(face_desc, sizeof(face_desc),
             "Change what the device's screen shows. Use when the user asks for a "
             "different look, mentions the display, or names one of these. "
             "Faces: %s.",
             faces);

    cJSON *set_face = cJSON_CreateObject();
    cJSON_AddStringToObject(set_face, "name", "set_face");
    cJSON_AddStringToObject(set_face, "description", face_desc);
    cJSON *fparams = cJSON_AddObjectToObject(set_face, "parameters");
    cJSON_AddStringToObject(fparams, "type", "object");
    cJSON *fprops = cJSON_AddObjectToObject(fparams, "properties");
    cJSON *face_prop = cJSON_AddObjectToObject(fprops, "face");
    cJSON_AddStringToObject(face_prop, "type", "string");
    faces_add_enum(face_prop, "enum");
    cJSON *frequired = cJSON_AddArrayToObject(fparams, "required");
    cJSON_AddItemToArray(frequired, cJSON_CreateString("face"));
    cJSON_AddItemToArray(functions, set_face);

#if CONFIG_SPEECH_STACK_FLUX
    /* The catalog goes in the description because JSON Schema has nowhere to
     * hang a per-enum-value note, and without it the model is choosing from
     * bare first names. */
    char catalog[768];
    voices_describe(catalog, sizeof(catalog));
    char description[900];
    snprintf(description, sizeof(description),
             "Change the voice you speak in. Use when the user asks you to sound "
             "different, or asks for a particular accent or gender. Voices: %s.",
             catalog);

    cJSON *set_voice = cJSON_CreateObject();
    cJSON_AddStringToObject(set_voice, "name", "set_voice");
    cJSON_AddStringToObject(set_voice, "description", description);
    cJSON *params = cJSON_AddObjectToObject(set_voice, "parameters");
    cJSON_AddStringToObject(params, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(params, "properties");
    cJSON *voice_prop = cJSON_AddObjectToObject(props, "voice");
    cJSON_AddStringToObject(voice_prop, "type", "string");
    voices_add_enum(voice_prop, "enum");
    cJSON *required = cJSON_AddArrayToObject(params, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("voice"));
    cJSON_AddItemToArray(functions, set_voice);

    cJSON *reset_voice = cJSON_CreateObject();
    cJSON_AddStringToObject(reset_voice, "name", "reset_voice");
    cJSON_AddStringToObject(reset_voice, "description",
                            "Go back to the device's default voice. Use when the user asks "
                            "you to reset your voice or return to how you normally sound.");
    cJSON *reset_params = cJSON_AddObjectToObject(reset_voice, "parameters");
    cJSON_AddStringToObject(reset_params, "type", "object");
    cJSON_AddObjectToObject(reset_params, "properties");
    cJSON_AddItemToArray(functions, reset_voice);
#endif

    cJSON *speak_provider = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(agent, "speak"), "provider");
    cJSON_AddStringToObject(speak_provider, "type", "deepgram");
#if CONFIG_SPEECH_STACK_FLUX
    /* Same story as listen: "v2" is what picks Flux TTS. Omitting agent.speak
     * entirely would also get Flux with flux-kit-en, but being explicit keeps
     * the voice configurable. */
    cJSON_AddStringToObject(speak_provider, "version", "v2");
    cJSON_AddStringToObject(speak_provider, "model", voices_current_model());
#else
    cJSON_AddStringToObject(speak_provider, "model", "aura-2-thalia-en");
#endif
    /*
     * Replayed context, and the greeting only when there is none. Resuming a
     * conversation should not open with "Hi! I am running on an ESP32" -- and
     * this is also what stops an auto-reconnect after a network blip from
     * re-greeting, which used to be the most visible symptom of a dropped
     * socket.
     */
    history_to_json(agent);
    if (s_history_count == 0 && strlen(CONFIG_DEEPGRAM_AGENT_GREETING) > 0) {
        cJSON_AddStringToObject(agent, "greeting", CONFIG_DEEPGRAM_AGENT_GREETING);
    }

    esp_err_t err = send_json(root, "Settings");
    cJSON_Delete(root);
    return err;
}

/* ---------------- server messages ---------------- */

static void handle_json(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "unparseable message: %.*s", len, json);
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }
    const char *t = type->valuestring;

    if (strcmp(t, "Welcome") == 0) {
        const cJSON *rid = cJSON_GetObjectItemCaseSensitive(root, "request_id");
        /* Worth logging: this id is what Deepgram support asks for. */
        ESP_LOGI(TAG, "Welcome, request_id=%s",
                 cJSON_IsString(rid) ? rid->valuestring : "?");

    } else if (strcmp(t, "SettingsApplied") == 0) {
        static uint32_t sessions;
        /* Numbered: the greeting is spoken once per session, so a greeting you
         * did not ask for always has a new number in front of it. */
        ESP_LOGI(TAG, "SettingsApplied -- session #%" PRIu32 " is live", ++sessions);
        set_state(DG_AGENT_READY);

    } else if (strcmp(t, "ConversationText") == 0) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *content = cJSON_GetObjectItemCaseSensitive(root, "content");
        if (cJSON_IsString(role) && cJSON_IsString(content)) {
            ESP_LOGI(TAG, "%s: %s", role->valuestring, content->valuestring);
            history_add(role->valuestring, content->valuestring);
            if (s_cb.on_conversation_text) {
                s_cb.on_conversation_text(role->valuestring, content->valuestring, s_cb.ctx);
            }
        }

    } else if (strcmp(t, "UserStartedSpeaking") == 0) {
        ESP_LOGI(TAG, "user started speaking");
        if (s_cb.on_user_started_speaking) {
            s_cb.on_user_started_speaking(s_cb.ctx);
        }

    } else if (strcmp(t, "AgentThinking") == 0) {
        /* No audio accompanies thinking, so this message is the ONLY way the
         * display can know about it -- see resolve_behaviour() in ui.c. */
        if (s_cb.on_thinking) {
            s_cb.on_thinking(s_cb.ctx);
        }
        ESP_LOGD(TAG, "agent thinking");

    } else if (strcmp(t, "AgentAudioDone") == 0) {
        ESP_LOGI(TAG, "agent finished speaking");
        if (s_cb.on_agent_audio_done) {
            s_cb.on_agent_audio_done(s_cb.ctx);
        }
        if (s_reload_pending) {
            /* Deferred to here so the agent gets to say what it is doing before
             * the socket goes away -- that sentence is spoken in the old voice,
             * and everything after the reconnect is in the new one. */
            s_reload_pending = false;
            ESP_LOGI(TAG, "reopening session to apply new settings");
            if (s_cb.on_reload_required) {
                s_cb.on_reload_required(s_cb.ctx);
            }
        }

    } else if (strcmp(t, "FunctionCallRequest") == 0) {
        handle_function_call(root);

    } else if (strcmp(t, "Error") == 0) {
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ESP_LOGE(TAG, "server error [%s]: %s",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(desc) ? desc->valuestring : "?");
        set_state(DG_AGENT_ERROR);

    } else if (strcmp(t, "LatencyReport") == 0 || strcmp(t, "History") == 0 ||
               strcmp(t, "AgentStartedSpeaking") == 0) {
        /* Known and deliberately ignored. Named explicitly so the catch-all
         * below stays a signal rather than a flood -- LatencyReport alone
         * arrives several times a second while the agent speaks. */

    } else if (strcmp(t, "Warning") == 0) {
        /* The server's soft-failure channel -- e.g. a model it could not honour
         * and silently substituted. Previously this fell into the LOGD branch
         * below, which CONFIG_LOG_MAXIMUM_LEVEL_INFO compiles out entirely, so
         * it was invisible. */
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        ESP_LOGW(TAG, "server warning [%s]: %s",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(desc) ? desc->valuestring : "?");

    } else {
        /* INFO, not DEBUG: anything the server says that we do not model is
         * exactly what we want to see when behaviour does not match the docs. */
        ESP_LOGI(TAG, "unhandled message type %s", t);
    }

    cJSON_Delete(root);
}

/*
 * Rebuilds a JSON message that the client split across events.
 *
 * esp_websocket_client hands over at most buffer_size bytes at a time and
 * reports where the slice sits inside the frame, so a 3 kB ConversationText
 * arrives as two events with the same op_code. Parsing each slice on its own
 * would throw away every message longer than the buffer.
 */
static void accumulate_json(const esp_websocket_event_data_t *ev)
{
    /* A TEXT opcode at offset 0 is the first slice of a new message; a CONT
     * opcode continues the previous one and must not reset the buffer even
     * though its own payload_offset starts back at 0. */
    if (ev->op_code == WS_OPCODE_TEXT && ev->payload_offset == 0) {
        s_json_len = 0;
    }

    if (s_json_len + ev->data_len > JSON_REASSEMBLY_MAX) {
        ESP_LOGW(TAG, "message exceeds %d byte reassembly buffer, dropping",
                 JSON_REASSEMBLY_MAX);
        s_json_len = 0;
        return;
    }

    memcpy(s_json + s_json_len, ev->data_ptr, ev->data_len);
    s_json_len += ev->data_len;

    /* Complete only when this is the last slice of the last frame: payload_len
     * covers one frame, fin covers the fragment chain. */
    if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
        handle_json(s_json, s_json_len);
        s_json_len = 0;
    }
}

static void on_ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const esp_websocket_event_data_t *ev = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "socket open");
        set_state(DG_AGENT_CONNECTED);
        /* Settings first, before anything else, or the server ignores us. */
        send_settings();
        break;

    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code == WS_OPCODE_BINARY) {
            if (s_cb.on_audio && ev->data_len > 0) {
                s_cb.on_audio((const uint8_t *)ev->data_ptr, ev->data_len, s_cb.ctx);
            }
        } else if (ev->op_code == WS_OPCODE_TEXT || ev->op_code == WS_OPCODE_CONT) {
            if (ev->data_len > 0) {
                accumulate_json(ev);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        /*
         * The client formats a human-readable reason into the event *data* and
         * only fills error_handle for handshake failures -- reading the handle
         * here printed uninitialised numbers and hid the actual cause.
         */
        ESP_LOGE(TAG, "transport error: %.*s",
                 ev->data_len, ev->data_ptr ? ev->data_ptr : "(no detail)");
        set_state(DG_AGENT_ERROR);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "socket closed (status %d)", ev->close_status_code);
        s_json_len = 0;
        set_state(DG_AGENT_DISCONNECTED);
        break;

    default:
        break;
    }
}

/*
 * Deepgram drops an Agent socket that has been quiet for ~10 s. Once a
 * microphone is streaming, the audio itself keeps it open and this becomes
 * redundant; until then it is the only thing holding the session up.
 *
 * This lives in its own task rather than an esp_timer callback because sending
 * a frame takes the client's transmit lock and can block.
 */
static void keepalive_task(void *arg)
{
    /* A constant string, so there is nothing to build and nothing to free. The
     * task outlives any one session: while stopped the guard below simply fails
     * and it goes back to sleep, which is why nothing has to join or delete it
     * on the teardown path. */
    static const char KEEPALIVE[] = "{\"type\":\"KeepAlive\"}";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_PERIOD_MS));
        if (s_ready && esp_websocket_client_is_connected(s_client)) {
            esp_websocket_client_send_text(s_client, KEEPALIVE, sizeof(KEEPALIVE) - 1,
                                           SEND_TIMEOUT);
        }
    }
}

/* ---------------- public API ---------------- */

esp_err_t dg_agent_init(const dg_agent_callbacks_t *callbacks)
{
    if (strlen(CONFIG_DEEPGRAM_API_KEY) == 0) {
        ESP_LOGE(TAG, "CONFIG_DEEPGRAM_API_KEY is empty -- run `idf.py menuconfig`");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (callbacks != NULL) {
        s_cb = *callbacks;
    }

    s_json = malloc(JSON_REASSEMBLY_MAX);
    if (s_json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const esp_websocket_client_config_t cfg = {
        .uri = DG_AGENT_URI,
        /* The Agent endpoint authenticates on the upgrade request only. */
        .headers = "Authorization: Token " CONFIG_DEEPGRAM_API_KEY "\r\n",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = WS_RX_BUFFER,
        /* JSON handling plus a user callback per frame needs more than the
         * 4 kB default. */
        .task_stack = 6144,
        .reconnect_timeout_ms = 5000,
        /*
         * esp_transport_connect() runs holding the client's own mutex, so this
         * is also the worst-case wait for a stop that lands mid-handshake. Ten
         * seconds of an unresponsive UI is too long now that stopping is a
         * gesture rather than a reboot.
         */
        .network_timeout_ms = 5000,
        .disable_auto_reconnect = false,
    };

    s_client = esp_websocket_client_init(&cfg);
    if (s_client == NULL) {
        free(s_json);
        s_json = NULL;
        return ESP_FAIL;
    }

    /*
     * Exactly once, for the life of the process. Registering per session would
     * stack duplicate handlers and fire every callback N times.
     */
    esp_err_t err = esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                                  on_ws_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event registration failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return err;
    }

    if (xTaskCreate(keepalive_task, "dg_keepalive", 3072, NULL, 4, &s_keepalive_task) != pdPASS) {
        ESP_LOGW(TAG, "keepalive task not created; idle sessions will drop");
    }
    return ESP_OK;
}

esp_err_t dg_agent_start(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stale bytes from a message that was cut off by the previous teardown. */
    s_json_len = 0;

    ESP_LOGI(TAG, "connecting to %s", DG_AGENT_URI);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "client start failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t dg_agent_stop(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_ready = false;

    /*
     * The CLOSE frame is worth sending: Deepgram bills on session duration, and
     * a half-open socket only finalises at the server's idle timeout. But only
     * when connected -- otherwise close() is a guaranteed no-op that just logs
     * an error. Neither call is checked: close() returns ESP_FAIL if the client
     * is already down, and stop() returns ESP_FAIL when a successful close has
     * already stopped the task. Both are expected, not failures.
     */
    if (esp_websocket_client_is_connected(s_client)) {
        (void)esp_websocket_client_close(s_client, pdMS_TO_TICKS(1000));
    }
    (void)esp_websocket_client_stop(s_client);

    ESP_LOGI(TAG, "session stopped");
    return ESP_OK;
}

bool dg_agent_is_ready(void)
{
    return s_ready;
}

esp_err_t dg_agent_send_audio(const void *pcm, size_t len)
{
    if (!s_ready || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    int sent = esp_websocket_client_send_bin(s_client, (const char *)pcm, (int)len,
                                            AUDIO_SEND_TIMEOUT);
    return sent < 0 ? ESP_FAIL : ESP_OK;
}

esp_err_t dg_agent_inject_user_message(const char *text)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(root, "type", "InjectUserMessage");
    cJSON_AddStringToObject(root, "content", text);

    esp_err_t err = send_json(root, "InjectUserMessage");
    cJSON_Delete(root);
    return err;
}
