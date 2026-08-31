#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
/* The local patches' one accessor; see components/tcp_transport. */
#include "transport_ws_local.h"

#include "agent_name.h"
#include "agent_prompt.h"
#include "api_key.h"
#include "audio_io.h"
#include "dg_agent.h"
#include "faces.h"
#include "orb_colors.h"
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

/*
 * Long-quiet fallback for the keepalive. NOT the primary rule -- see the task.
 *
 * This was 2000 and that was actively harmful: a congested socket blocks the
 * capture task, which stops the audio, which ages this clock past the threshold,
 * which fires the keepalive INTO the congestion that caused it. The stall
 * manufactured its own trigger, and killed a live session doing it.
 *
 * 6 s leaves margin under Deepgram's ~10 s while being longer than any stall
 * actually observed, so an ordinary congestion episode no longer trips it.
 */
#define KEEPALIVE_QUIET_MS 6000

#define WS_OPCODE_CONT   0x00
#define WS_OPCODE_TEXT   0x01
#define WS_OPCODE_BINARY 0x02

#define SEND_TIMEOUT pdMS_TO_TICKS(2000)

/*
 * Mic audio keeps the generous deadline, and the impatience lives in the
 * transport instead. This is worth understanding before shortening it again.
 *
 * The comment here used to explain why it had to be generous: a short deadline
 * made esp_transport_poll_write() time out, transport_ws returned 0, and the
 * WebSocket client treated a zero-length write as a broken socket and tore the
 * session down. 200 ms dropped the session every few seconds.
 *
 * LOCAL PATCH 2 in components/tcp_transport/transport_ws.c removes that trap --
 * a congested BINARY frame is now dropped and reported sent. But shortening THIS
 * was still wrong, twice over, and both were measured on the device:
 *
 *   1. This value is also the lock deadline. esp_websocket_client takes its
 *      client lock with it, so 200 ms produced a flood of "Could not lock
 *      ws-client within 200 timeout" and the client dropped mic frames before
 *      they ever reached the socket. CONFIG_ESP_WS_CLIENT_SEPARATE_TX_LOCK looks
 *      like the cure and is not -- it deadlocks the client outright; see the
 *      comment on it in sdkconfig.defaults. The deadline has to cover the lock.
 *
 *   2. It is also the deadline for the header and payload writes themselves.
 *      Those happen AFTER the frame is committed, and a timeout there tears a
 *      frame in half -- "esp_transport_write() returned 0" with no poll line
 *      above it, which is fatal and correctly so.
 *
 * The capture-task stall this was meant to fix is handled where it can be handled
 * safely: WS_AUDIO_POLL_MS caps the WRITABILITY POLL at 150 ms for audio frames,
 * so a congested socket costs one frame and 150 ms, while a frame that has
 * started going out still gets the full deadline to finish.
 *
 * The better structure is still to move the send off the capture task entirely.
 */
#define AUDIO_SEND_TIMEOUT SEND_TIMEOUT

static esp_websocket_client_handle_t s_client;
/* Written by the capture task, read by the keepalive task. One 32-bit store. */
static volatile uint32_t s_last_audio_ms;
static dg_agent_callbacks_t s_cb;
static volatile bool s_ready;
static TaskHandle_t s_keepalive_task;
static volatile bool s_suppress_state;

/*
 * THE UPLINK QUEUE, AND WHY THE SEND IS NOT ON THE CAPTURE TASK ANY MORE.
 *
 * esp_websocket_client takes its client lock for the duration of a send, with
 * AUDIO_SEND_TIMEOUT as the deadline (see the note on it above). While the
 * capture task owned that send, a congested socket meant the capture task was
 * the thing holding the lock -- and dg_agent_stop() had no way to know it, or to
 * wait for it. esp_websocket_client_stop() waits on STOPPED_BIT with
 * portMAX_DELAY (stop_wait_task() in the component), so a stop issued while a
 * send was wedged never returned: session_ctl's do_stop() stalled after painting
 * "stopping", s_busy latched, and every gesture was refused from then on. The
 * device kept rendering at 22 fps with the panel stuck on "stopping". Measured on
 * hardware, 2026-08-27.
 *
 * With the send on its own task, "no send is in flight" becomes something
 * dg_agent_stop() can assert before it touches the client -- which is the
 * property that was missing, and the reason this is worth the extra task.
 *
 * Four frames of slack. Each is 80 ms, so this is 320 ms -- enough to ride out a
 * retransmission burst, short enough that what finally goes out is still worth
 * hearing. Overflowing drops the NEWEST frame rather than growing a backlog of
 * stale speech, which is the same trade transport_ws.c's LOCAL PATCH 2 already
 * makes one layer down: drop a frame, never a session.
 */
#define AUDIO_QUEUE_FRAMES 4
/* From audio_io.h, not a literal repeated here: the queue is sized in whole
 * capture chunks, so the two must agree by construction. */
#define AUDIO_FRAME_BYTES  AUDIO_IO_CAPTURE_BYTES
#define AUDIO_QUEUE_BYTES  (AUDIO_QUEUE_FRAMES * (AUDIO_FRAME_BYTES + 16))

/* PSRAM: it is 10 kB that nothing touches from an ISR, and internal RAM is what
 * runs out first on this board once the display is up. */
static RingbufHandle_t s_audio_rb;
static TaskHandle_t s_send_task;
/*
 * Raised around a send that holds the client lock; what dg_agent_stop() waits on.
 *
 * One flag per sending task, because a shared counter would need atomics that
 * a volatile ++ does not give on this target. Covers the two sends that can be
 * in flight while a stop arrives: the audio frames, and the keepalive -- which
 * is TEXT, so transport_ws.c's LOCAL PATCH 2 cannot drop it and it blocks in
 * poll_write holding the lock (see the note in keepalive_task).
 *
 * BOTH ARE RAISED BEFORE THEIR TASK TESTS s_ready, NOT AFTER, and that ordering
 * is the property that makes them work at all. dg_agent_stop() clears s_ready
 * and then waits here; raising a flag after the test leaves a window in which
 * the sender has committed to a send and the stop cannot see it. Claim first,
 * test second, and every interleaving ends with either the stop waiting or the
 * send skipping. The full argument is at the top of audio_send_task().
 *
 * send_json() is deliberately NOT covered. It runs at session setup and on a
 * function-call response, not on the cadence a stop has to race, and adding it
 * would mean a flag written by two different tasks.
 */
static volatile bool s_sending;
static volatile bool s_sending_ka;

/*
 * No congestion heuristic lives here any more, and that is deliberate.
 *
 * Two were tried. The first read the send's return value, which is wrong for a
 * reason transport_ws.c states outright: LOCAL PATCH 2 drops a congested audio
 * frame and reports it as sent -- "strictly bounded, because the drop lies to
 * the caller" -- so it read healthy while fifty frames a minute went in the bin.
 * The second measured send duration, which does not lie, but was still a guess
 * guarding a call that must not be made at all. See dg_agent_stop().
 */
static volatile uint32_t s_audio_dropped;

/* Reassembly buffer for JSON messages split across several DATA events. */
static char *s_json;
static int s_json_len;
/*
 * Set when the message being reassembled has already overrun the buffer, so its
 * remaining slices are discarded instead of being taken for a new message.
 *
 * Without it, resetting s_json_len on overflow made the NEXT slices of the SAME
 * message accumulate from offset 0, and the fin slice then parsed a fragment
 * tail -- so an oversized message was reported as "unparseable message" against
 * a message that was perfectly well formed. The drop has to last until the end
 * of the message, not until the end of the slice.
 */
static bool s_json_dropping;

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
/* Same deferral as s_reload_pending, for the same reason: let the agent finish
 * the sentence before the socket it is speaking over goes away. */
static bool s_test_pending;

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

        if (strcmp(name->valuestring, "set_name") == 0) {
            const char *wanted = NULL;
            cJSON *nargs = function_args(fn);
            if (nargs != NULL) {
                const cJSON *n = cJSON_GetObjectItemCaseSensitive(nargs, "name");
                if (cJSON_IsString(n)) {
                    wanted = n->valuestring;
                }
            }

            /*
             * NO RELOAD, unlike set_voice. The voice is a Settings field and
             * this account ignores UpdateSpeak, so changing it costs a new
             * session; the name is only ever text inside the prompt, and telling
             * the model here is enough for this session. {{name}} carries it
             * from the next Settings onwards, so the two agree again as soon as
             * anything reconnects.
             */
            esp_err_t nerr = agent_name_set(wanted);
            if (nerr != ESP_OK) {
                /* Say so rather than staying silent, exactly as an unknown voice
                 * does: nothing is applied and nothing is saved. */
                send_function_response(id->valuestring, name->valuestring,
                                       "That name will not work on this device. Ask them "
                                       "for a shorter one, just the name on its own.");
            } else {
                ESP_LOGI(TAG, "EVT setname -> \"%s\"", agent_name_get());
                snprintf(content, sizeof(content),
                         "Your name is now %s. Answer to it from here on, and say "
                         "it back once.", agent_name_get());
                send_function_response(id->valuestring, name->valuestring, content);
            }
            cJSON_Delete(nargs);
            continue;
        }

        if (strcmp(name->valuestring, "reset_name") == 0) {
            agent_name_reset();
            ESP_LOGI(TAG, "EVT setname -> \"%s\" (default)", agent_name_get());
            snprintf(content, sizeof(content),
                     "You are called %s again. Answer to it from here on.",
                     agent_name_get());
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

        if (strcmp(name->valuestring, "set_volume") == 0) {
            int level = -1;
            cJSON *sargs = function_args(fn);
            if (sargs != NULL) {
                const cJSON *l = cJSON_GetObjectItemCaseSensitive(sargs, "level");
                if (cJSON_IsNumber(l)) {
                    level = l->valueint;
                }
                cJSON_Delete(sargs);
            }

            if (level < 0) {
                /* No usable level: ask rather than pick one on their behalf. */
                ESP_LOGW(TAG, "EVT setvolume -> no level");
                snprintf(content, sizeof(content),
                         "No level was given. Ask how loud they want you, 20 to 100.");
                send_function_response(id->valuestring, name->valuestring, content);
                continue;
            }

            /*
             * audio_io clamps to 20..100, so `now` can differ from what was
             * asked for. Say which stop it landed on rather than reporting the
             * number as if it were the request -- see adjust_volume above for
             * why no reload is needed.
             */
            int now = audio_io_set_volume(level);
            ESP_LOGI(TAG, "EVT setvolume req=%d -> %d", level, now);
            if (now < level) {
                snprintf(content, sizeof(content),
                         "The loudest I go is %d, and I am there now. Say so.", now);
            } else if (now > level) {
                snprintf(content, sizeof(content),
                         "The quietest I go is %d, and I am there now. Say so.", now);
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
            cJSON_Delete(fargs);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "set_color") == 0) {
            int index = -1;
            const char *wanted = NULL;
            cJSON *cargs = function_args(fn);
            if (cargs != NULL) {
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(cargs, "color");
                if (cJSON_IsString(c)) {
                    wanted = c->valuestring;
                    index = orb_colors_find(wanted);
                }
            }

            if (index < 0) {
                ESP_LOGW(TAG, "EVT setcolor req=\"%s\" -> unknown",
                         (wanted != NULL) ? wanted : "");
                snprintf(content, sizeof(content),
                         "The orb has no '%s' colour. Ask which one they want.",
                         (wanted != NULL) ? wanted : "");
            } else {
                /* No reload, like set_face and unlike set_voice: a local display
                 * setting, in effect by the time this is spoken. */
                ui_set_orb_color(index);
                /* The requested string as well as the resolved colour: whether
                 * the model routes an indirect phrasing here is the thing worth
                 * testing, and that is only visible in what it actually sent. */
                ESP_LOGI(TAG, "EVT setcolor req=\"%s\" -> %s", wanted,
                         orb_colors_name((size_t)index));
                /* "The orb is", not "the screen is": the colour applies whichever
                 * face is up, so this stays true while the spectrum is showing. */
                snprintf(content, sizeof(content),
                         "The orb is now %s. Say so briefly.",
                         orb_colors_name((size_t)index));
            }
            cJSON_Delete(cargs);
            send_function_response(id->valuestring, name->valuestring, content);
            continue;
        }

        if (strcmp(name->valuestring, "start_display_test") == 0) {
            /*
             * No arguments, but parse-and-free anyway: the model sometimes sends
             * "{}" and function_args() allocates regardless of content.
             */
            cJSON *targs = function_args(fn);
            cJSON_Delete(targs);

            /*
             * Logged at INFO with no detail to add, because the interesting
             * question is only ever whether this fired at all -- the trigger is a
             * spoken phrase that has to survive speech-to-text, and a miss looks
             * identical to a user who never said it. The transcript lines
             * either side of this in the log are what tell them apart.
             */
            ESP_LOGI(TAG, "EVT displaytest requested");
            s_test_pending = true;
            snprintf(content, sizeof(content),
                     "Starting the display test. Tell the user to tap the screen "
                     "to step through each state, briefly.");
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

    cJSON *think = cJSON_AddObjectToObject(agent, "think");
    cJSON *think_provider = cJSON_AddObjectToObject(think, "provider");
    cJSON_AddStringToObject(think_provider, "type", "open_ai");
    cJSON_AddStringToObject(think_provider, "model", "gpt-4o-mini");

    /*
     * The persona, assembled from the blocks in main/prompt. Built in PSRAM and freed
     * immediately -- cJSON copies the string, and this function's frame must not
     * grow with the prompt (see the stack note further down and agent_prompt.h).
     *
     * A resumed session gets told so: history_to_json() below replays the last
     * few turns, and without this the model reads them as a conversation it is
     * joining rather than one it has been having.
     */
    agent_prompt_ctx_t pctx = {
        .notes = (s_history_count > 0)
                     ? "You have already been talking with this person for a "
                       "few turns. What follows is that same conversation, not "
                       "a new one, so pick it up where it left off and do not "
                       "start over or greet them again."
                     : NULL,
    };
    char *prompt = agent_prompt_build(&pctx);
    if (prompt != NULL) {
        cJSON_AddStringToObject(think, "prompt", prompt);
        free(prompt);
    } else {
        /* A session with a default persona is worth far more than no session. */
        ESP_LOGE(TAG, "no system prompt; continuing without one");
    }

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
                            "Make yourself louder or quieter BY A RELATIVE AMOUNT, for "
                            "'a bit louder' or 'turn it down'. Negative is quieter, "
                            "positive is louder. A small change is about 10, a big one "
                            "about 30. The scale runs 20 to 100, so once you are told you "
                            "are at a limit, stop trying. If they name a number to land on, "
                            "use set_volume instead.");
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

    cJSON *set_volume = cJSON_CreateObject();
    cJSON_AddStringToObject(set_volume, "name", "set_volume");
    cJSON_AddStringToObject(set_volume, "description",
                            "Set how loud you are TO A SPECIFIC LEVEL, for 'set your "
                            "volume to 50'. The scale runs 20 to 100: 20 is barely "
                            "audible, 100 is maximum. 20 is as quiet as you go and there "
                            "is no mute, so a request below it lands on 20. For 'louder' "
                            "or 'quieter' with no number, use adjust_volume instead.");
    cJSON *sparams = cJSON_AddObjectToObject(set_volume, "parameters");
    cJSON_AddStringToObject(sparams, "type", "object");
    cJSON *sprops = cJSON_AddObjectToObject(sparams, "properties");
    cJSON *level_prop = cJSON_AddObjectToObject(sprops, "level");
    cJSON_AddStringToObject(level_prop, "type", "integer");
    cJSON_AddNumberToObject(level_prop, "minimum", 20);
    cJSON_AddNumberToObject(level_prop, "maximum", 100);
    cJSON *srequired = cJSON_AddArrayToObject(sparams, "required");
    cJSON_AddItemToArray(srequired, cJSON_CreateString("level"));
    cJSON_AddItemToArray(functions, set_volume);

    /*
     * Not Flux-gated either: the display is local and works on either speech
     * stack. The catalog goes in the description for the same reason set_voice
     * does it -- JSON Schema has nowhere to hang a per-enum-value note, and
     * without one the model is choosing between two bare nouns.
     */
    /*
     * PSRAM and ONE buffer, the pattern set_color introduced below and the
     * reason this function's frame no longer grows with the catalog. This was
     * the stack pair the canary note further down describes -- 512 + 700 = 1,212
     * B of the frame for two faces. See .claude/skills/esp-stack-budget/.
     *
     * 377 B in use at two faces. Rounded up because it is PSRAM and free, so a
     * few more faces cannot quietly reach faces_describe()'s truncation path.
     */
    enum { FACE_DESC_LEN = 1024 };
    char *face_desc = heap_caps_malloc(FACE_DESC_LEN, MALLOC_CAP_SPIRAM);
    /* Losing the catalog is survivable -- the enum still constrains the model to
     * valid names. Losing the function is not. */
    const char *face_desc_str = "Change what the device's screen shows.";
    if (face_desc != NULL) {
        int n = snprintf(face_desc, FACE_DESC_LEN,
                         "Change what the device's screen shows. Use when the user asks for a "
                         "different look, mentions the display, or names one of these. "
                         "Faces: ");
        if (n > 0 && (size_t)n < FACE_DESC_LEN) {
            faces_describe(face_desc + n, FACE_DESC_LEN - (size_t)n);
            /* The trailing stop the one-shot snprintf used to supply, kept so
             * this is a move off the stack and not a change to what the model
             * reads. */
            size_t used = strlen(face_desc);
            if (used + 2 <= FACE_DESC_LEN) {
                face_desc[used] = '.';
                face_desc[used + 1] = '\0';
            }
            face_desc_str = face_desc;
        }
    }

    cJSON *set_face = cJSON_CreateObject();
    cJSON_AddStringToObject(set_face, "name", "set_face");
    cJSON_AddStringToObject(set_face, "description", face_desc_str);
    free(face_desc); /* cJSON copied it; free(NULL) is fine */
    cJSON *fparams = cJSON_AddObjectToObject(set_face, "parameters");
    cJSON_AddStringToObject(fparams, "type", "object");
    cJSON *fprops = cJSON_AddObjectToObject(fparams, "properties");
    cJSON *face_prop = cJSON_AddObjectToObject(fprops, "face");
    cJSON_AddStringToObject(face_prop, "type", "string");
    faces_add_enum(face_prop, "enum");
    cJSON *frequired = cJSON_AddArrayToObject(fparams, "required");
    cJSON_AddItemToArray(frequired, cJSON_CreateString("face"));
    cJSON_AddItemToArray(functions, set_face);

    /*
     * Colour, same shape and the same reasoning as the face above: local to the
     * device, so not Flux-gated, and the catalog goes in the description because
     * JSON Schema has nowhere to hang a per-enum-value note.
     *
     * The description names the orb explicitly. The spectrum draws its own
     * palette and ignores the setting, and saying so is what stops the model
     * offering a colour change as the answer to "the bars look wrong".
     */
    /*
     * PSRAM, and ONE buffer rather than a catalog-plus-description pair.
     *
     * NOT A STYLE CHOICE -- MEASURED. This task has task_stack = 6144. This
     * function once gave every described catalog a stack PAIR, a buffer for the
     * catalog and another for the description around it: 1,212 B for faces,
     * 1,668 for voices. Adding a third pair of 1,280 for these colours tripped
     * the stack canary on the first session and put the device in a boot loop,
     * before cJSON's own recursion is even counted. Recovering it needed BOOT
     * held while RESET was tapped -- the board rebooted faster than esptool
     * could sync.
     *
     * So the prefix is written first and the catalog appended into the tail of
     * the same allocation, which costs no stack at all. cJSON copies the string,
     * so it is freed immediately.
     *
     * ALL THREE USE THIS NOW, which is the part worth keeping. The pair pattern
     * was the trap rather than the colour function that sprang it: its cost was
     * O(n) in declared functions with no budget written down anywhere, so it was
     * going to fail for whoever added the third one, whatever it happened to be.
     * Measured either side of moving faces and voices across: this function had
     * a 2,944 B frame and was called from on_ws_event's 192 B one, so the path
     * cost 3,136 B of the 6,144 available before cJSON recursed at all. With the
     * buffers gone it is small enough that the compiler inlines it into
     * on_ws_event outright -- there is no send_settings frame in the image any
     * more, and that caller is still 192 B. 3,136 -> 192.
     *
     * A new described catalog should cost nothing here; if one ever appears to,
     * measure with .claude/skills/esp-stack-budget/ before enlarging anything.
     */
    /* 543 B in use at thirteen colours (201 prefix + 341 catalog + NUL). Rounded
     * up because it is PSRAM and free, so a couple more colours cannot quietly
     * run into orb_colors_describe()'s truncation path. */
    enum { COLOR_DESC_LEN = 1024 };
    char *color_desc = heap_caps_malloc(COLOR_DESC_LEN, MALLOC_CAP_SPIRAM);
    /* Losing the catalog is survivable -- the enum still constrains the model to
     * valid names, it just has less to reason about. Losing the function is not. */
    const char *color_desc_str = "Change the colour of the orb on the device's screen.";
    if (color_desc != NULL) {
        int n = snprintf(color_desc, COLOR_DESC_LEN,
                         "Change the colour of the orb on the device's screen. Use "
                         "when the user asks for a different colour, or to go back "
                         "to normal. Only the orb is affected; the spectrum face "
                         "keeps its own colours. Colours: ");
        if (n > 0 && (size_t)n < COLOR_DESC_LEN) {
            orb_colors_describe(color_desc + n, COLOR_DESC_LEN - (size_t)n);
            color_desc_str = color_desc;
        }
    }

    cJSON *set_color = cJSON_CreateObject();
    cJSON_AddStringToObject(set_color, "name", "set_color");
    cJSON_AddStringToObject(set_color, "description", color_desc_str);
    free(color_desc); /* cJSON copied it; free(NULL) is fine */
    cJSON *cparams = cJSON_AddObjectToObject(set_color, "parameters");
    cJSON_AddStringToObject(cparams, "type", "object");
    cJSON *cprops = cJSON_AddObjectToObject(cparams, "properties");
    cJSON *color_prop = cJSON_AddObjectToObject(cprops, "color");
    cJSON_AddStringToObject(color_prop, "type", "string");
    orb_colors_add_enum(color_prop, "enum");
    cJSON *crequired = cJSON_AddArrayToObject(cparams, "required");
    cJSON_AddItemToArray(crequired, cJSON_CreateString("color"));
    cJSON_AddItemToArray(functions, set_color);

    /*
     * The display test. No parameters, so no enum and no catalog -- which means
     * a plain literal description and none of the buffer juggling above.
     *
     * THE PHRASE HAS TO SURVIVE SPEECH-TO-TEXT, and that is the whole risk in
     * this function. "up up down down left right left right" can arrive merged,
     * hyphenated, as "up-up-down-down", or with a stray "B A" on the end from a
     * user who knows the rest of the code. So this describes the SHAPE of the
     * utterance -- a run of repeated directions -- rather than one exact string,
     * and says to ignore the surrounding wording.
     */
    /*
     * The name. No catalog and no enum -- any name is valid, which is exactly
     * why the DESCRIPTION carries the rules instead: this arrives from
     * speech-to-text, so the model is passing on something it HEARD, and left to
     * itself it will hand over the whole sentence it heard it in.
     */
    cJSON *set_name = cJSON_CreateObject();
    cJSON_AddStringToObject(set_name, "name", "set_name");
    cJSON_AddStringToObject(set_name, "description",
        "Change what you are called, and remember it. Use when the user asks you "
        "to go by something else, gives you a new name, or asks what they should "
        "call you and then names one. Pass ONLY the name itself, spelled as a "
        "name and capitalised, with nothing else around it -- not the sentence "
        "you heard it in, and not a title. If you did not catch it clearly, ask "
        "them to say it again instead of guessing. Do not call this when the "
        "user is telling you THEIR name.");
    cJSON *nparams = cJSON_AddObjectToObject(set_name, "parameters");
    cJSON_AddStringToObject(nparams, "type", "object");
    cJSON *nprops = cJSON_AddObjectToObject(nparams, "properties");
    cJSON *name_prop = cJSON_AddObjectToObject(nprops, "name");
    cJSON_AddStringToObject(name_prop, "type", "string");
    cJSON *nrequired = cJSON_AddArrayToObject(nparams, "required");
    cJSON_AddItemToArray(nrequired, cJSON_CreateString("name"));
    cJSON_AddItemToArray(functions, set_name);

    cJSON *reset_name = cJSON_CreateObject();
    cJSON_AddStringToObject(reset_name, "name", "reset_name");
    cJSON_AddStringToObject(reset_name, "description",
        "Go back to the name this device came with. Use when the user asks you to "
        "reset your name or go back to what you were called before.");
    cJSON *reset_nparams = cJSON_AddObjectToObject(reset_name, "parameters");
    cJSON_AddStringToObject(reset_nparams, "type", "object");
    cJSON_AddObjectToObject(reset_nparams, "properties");
    cJSON_AddItemToArray(functions, reset_name);

    cJSON *set_test = cJSON_CreateObject();
    cJSON_AddStringToObject(set_test, "name", "start_display_test");
    cJSON_AddStringToObject(set_test, "description",
        "Put the device into its display test, which steps through every visual "
        "state of the orb one tap at a time. Call this when the user says a run "
        "of directions in sequence -- the Konami code, \"up up down down left "
        "right left right\" -- however it comes through: merged, hyphenated, "
        "repeated, or with A and B on the end. Match on the sequence of "
        "directions itself and ignore any wording around it. Do not call it for "
        "a single direction, or for a question about the display.");
    cJSON *tparams = cJSON_AddObjectToObject(set_test, "parameters");
    cJSON_AddStringToObject(tparams, "type", "object");
    cJSON_AddObjectToObject(tparams, "properties");
    cJSON_AddItemToArray(functions, set_test);

    /* The catalog goes in the description because JSON Schema has nowhere to
     * hang a per-enum-value note, and without it the model is choosing from
     * bare first names. */
    /*
     * The last of the three stack pairs, and the largest at 768 + 900 = 1,668 B.
     * Same one-buffer PSRAM pattern as the two above.
     *
     * 2048 rather than the 1024 the others use, and sized for growth rather than
     * for today: 705 B is in use at thirteen featured voices, and voices.c keeps
     * the other twenty-three precisely so widening the offer is "a one-flag
     * change". All thirty-six would be 1,743 B, so that flag can be flipped
     * without landing on voices_describe()'s truncation path -- which is the
     * whole point of the catalog not living on the stack any more.
     */
    enum { VOICE_DESC_LEN = 2048 };
    char *description = heap_caps_malloc(VOICE_DESC_LEN, MALLOC_CAP_SPIRAM);
    const char *description_str = "Change the voice you speak in.";
    if (description != NULL) {
        int n = snprintf(description, VOICE_DESC_LEN,
                         "Change the voice you speak in. Use when the user asks you to sound "
                         "different, or asks for a particular accent or gender. Voices: ");
        if (n > 0 && (size_t)n < VOICE_DESC_LEN) {
            voices_describe(description + n, VOICE_DESC_LEN - (size_t)n);
            size_t used = strlen(description);
            if (used + 2 <= VOICE_DESC_LEN) {
                description[used] = '.';
                description[used + 1] = '\0';
            }
            description_str = description;
        }
    }

    cJSON *set_voice = cJSON_CreateObject();
    cJSON_AddStringToObject(set_voice, "name", "set_voice");
    cJSON_AddStringToObject(set_voice, "description", description_str);
    free(description); /* cJSON copied it; free(NULL) is fine */
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

    cJSON *speak_provider = cJSON_AddObjectToObject(
        cJSON_AddObjectToObject(agent, "speak"), "provider");
    cJSON_AddStringToObject(speak_provider, "type", "deepgram");
    /* Same story as listen: "v2" is what picks Flux TTS. Omitting agent.speak
     * entirely would also get Flux with flux-kit-en, but being explicit keeps
     * the voice configurable. */
    cJSON_AddStringToObject(speak_provider, "version", "v2");
    cJSON_AddStringToObject(speak_provider, "model", voices_current_model());
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
        /*
         * Never observed in practice. This was wired to a display state for a
         * while; across every session logged it did not arrive once, so the state
         * was unreachable and has been removed. Still handled here rather than
         * left to the catch-all, so if this stack ever does start sending it the
         * log says so instead of reading as an unknown type.
         */
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
        if (s_test_pending) {
            /* Same deferral: the confirmation is spoken over this session, and
             * the display test closes it. */
            s_test_pending = false;
            ESP_LOGI(TAG, "handing the screen to the display test");
            if (s_cb.on_display_test_required) {
                s_cb.on_display_test_required(s_cb.ctx);
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
        s_json_dropping = false;
    }

    if (!s_json_dropping) {
        if (s_json_len + ev->data_len > JSON_REASSEMBLY_MAX) {
            ESP_LOGW(TAG, "message exceeds %d byte reassembly buffer, dropping",
                     JSON_REASSEMBLY_MAX);
            s_json_dropping = true;
            s_json_len = 0;
        } else {
            memcpy(s_json + s_json_len, ev->data_ptr, ev->data_len);
            s_json_len += ev->data_len;
        }
    }

    /*
     * Complete only when this is the last slice of the last frame: payload_len
     * covers one frame, fin covers the fragment chain.
     *
     * Reached whether or not the message was dropped, and that is the point: the
     * drop is cleared HERE rather than on the next message's first slice, so a
     * following message that never presents an offset-0 TEXT slice still starts
     * clean.
     */
    if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
        if (!s_json_dropping) {
            handle_json(s_json, s_json_len);
        }
        s_json_len = 0;
        s_json_dropping = false;
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
        /*
         * ONE FIELD OF error_handle IS WORTH READING, and it is the one that
         * tells a rejected API key apart from a bad network. That distinction
         * did not matter while the key came from menuconfig; it matters now that
         * someone types it into the setup portal and can mistype it.
         *
         * GATE ON THE STATUS CODE, NOT error_type. This path sets
         * error_type = WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT after filling the
         * status code in just above it (esp_websocket_client.c), so a test for
         * ..._HANDSHAKE would never fire despite this being a handshake failure.
         *
         * AND 401 IS A POSITIVE SIGNAL ONLY. transport_ws.c assigns
         * http_status_code only once a response header has actually been read,
         * and never clears it between attempts, so a later non-HTTP failure can
         * still be carrying an old one. Seeing 401 means a 401 was really read.
         * NOT seeing it proves nothing about the key, so nothing here may treat
         * its absence as the key being good.
         */
        if (ev->error_handle.esp_ws_handshake_status_code == 401) {
            ESP_LOGE(TAG, "the Deepgram API key was rejected (HTTP 401)");
            set_state(DG_AGENT_BAD_KEY);
        } else {
            set_state(DG_AGENT_ERROR);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "socket closed (status %d)", ev->close_status_code);
        s_json_len = 0;
        s_json_dropping = false;
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
/*
 * Drains the queue into the socket, and is the ONLY caller of send_bin.
 *
 * Persistent, like keepalive_task: it outlives any one session, so nothing has
 * to join or delete it on a teardown path. While stopped, s_ready is false and
 * it simply discards what it dequeues -- which also drains anything the capture
 * task queued in the moments before the stop.
 */
static void audio_send_task(void *arg)
{
    while (1) {
        size_t len = 0;
        /* The timeout is a liveness tick, not a deadline: it is what lets this
         * task notice a queue that has been deleted or a session that ended. */
        void *frame = xRingbufferReceive(s_audio_rb, &len, pdMS_TO_TICKS(100));
        if (frame == NULL) {
            continue;
        }

        /*
         * CLAIMED BEFORE THE READINESS TEST, NEVER AFTER IT.
         *
         * dg_agent_stop() clears s_ready and then waits for this flag to fall.
         * Raising it after the test left a window between them: the sender had
         * already decided to send, the stop saw an idle sender, and
         * esp_websocket_client_stop() ran underneath a send that was about to
         * take the client lock -- the exact wedge this task exists to prevent.
         *
         * Flag first, test second, and the two orders cannot both miss. Either
         * the stop observes s_sending and waits, or this observes !s_ready and
         * skips. There is no third outcome, and both tasks are pinned to core 0
         * so preemption is the only interleaving there is to cover.
         *
         * The cost is that the flag is briefly raised for a frame that is then
         * discarded, so a stop can spin one extra 10 ms tick in its quiesce
         * loop. That is the whole price.
         */
        s_sending = true;
        if (s_ready && s_client != NULL) {
            /*
             * Ahead of the send, not after it: a send that blocks is exactly when
             * the keepalive must stay out of the way, and stamping afterwards
             * would leave the clock stale for the whole time it was blocked.
             */
            s_last_audio_ms = (uint32_t)(esp_timer_get_time() / 1000);
            (void)esp_websocket_client_send_bin(s_client, (const char *)frame,
                                                (int)len, AUDIO_SEND_TIMEOUT);

            /*
             * Stack headroom, reported once with real history behind it.
             *
             * Two things this deliberately is NOT. Not on entry -- this task's
             * stack has to cover send_bin down through mbedtls, and a mark taken
             * before the first send measures none of it. And not on the FIRST
             * send either: that lands ~4 s after boot, before a serial capture
             * reliably has the port back (the board re-enumerates after a reset),
             * so the one number worth having was the one that always got lost.
             *
             * 200 frames is ~16 s of session, which is late enough to be
             * capturable and long enough to have met a congested send or two.
             * Not more: CONFIG_SESSION_IDLE_TIMEOUT_S ends a session after 15 s
             * of quiet, so a threshold set for a minute of continuous talking is
             * a threshold that never fires.
             * 4 kB here is INTERNAL RAM, the scarce resource on this board, so
             * this is what says whether 4 kB was the right guess.
             */
            static uint32_t sends;
            if (++sends == 200) {
                ESP_LOGI(TAG, "uplink task stack high water mark: %u B free of 4096"
                              " after %" PRIu32 " frames",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL), sends);
            }
        }
        s_sending = false;
        vRingbufferReturnItem(s_audio_rb, frame);
    }
}

static void keepalive_task(void *arg)
{
    /* A constant string, so there is nothing to build and nothing to free. The
     * task outlives any one session: while stopped the guard below simply fails
     * and it goes back to sleep, which is why nothing has to join or delete it
     * on the teardown path. */
    static const char KEEPALIVE[] = "{\"type\":\"KeepAlive\"}";

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(KEEPALIVE_PERIOD_MS));
        /*
         * Claimed before the readiness test, for the reason audio_send_task()
         * spells out at length: dg_agent_stop() clears s_ready and then waits on
         * this flag, so a claim that comes after the test leaves a window where
         * the stop tears the client down under a send that has already been
         * decided on. This one matters even more than the audio flag -- a
         * keepalive is TEXT, so transport_ws.c's LOCAL PATCH 2 cannot drop it and
         * it blocks in poll_write holding the client lock.
         *
         * Which is why the guard below became a positive condition rather than a
         * pair of `continue`s: the flag has to wrap the whole test, and a
         * `continue` would skip past the lowering.
         */
        s_sending_ka = true;
        if (s_ready && esp_websocket_client_is_connected(s_client)) {
            /*
             * WHEN this frame goes out matters more than whether it does, because it
             * is TEXT and transport_ws cannot drop it when the send queue is full --
             * LOCAL PATCH 2 covers binary only, since Settings must never vanish
             * silently. A congested TEXT send blocks in poll_write holding the client
             * lock, stalls the capture task behind it, and finally times out and takes
             * the session down. Observed twice, as a live session dying with mic=
             * frozen for the full SEND_TIMEOUT and rx=0.
             *
             * So send it only when the uplink is genuinely quiet AND uncongested, and
             * there is one condition that means exactly that: the mic gate is shut
             * because the agent is speaking. Nothing is being pushed upstream then, so
             * the send queue is draining rather than filling, and this is also the one
             * case the keepalive exists for -- a long reply during which the device
             * sends no audio at all and Deepgram's ~10 s idle timer is running.
             *
             * Any other time, either audio is flowing and doing the job already, or
             * the uplink is stalled -- and a stall is precisely when adding a
             * blocking TEXT write is worst. An earlier version keyed on "no audio for
             * 2 s" and did exactly that to itself: the stall aged the clock, the clock
             * fired the keepalive, the keepalive killed the session.
             */
            uint32_t quiet_ms = (uint32_t)(esp_timer_get_time() / 1000) -
                                s_last_audio_ms;
            /* The same rule as the `continue` this replaced, stated the other way
             * round: send iff the mic gate is shut, or the uplink has been quiet
             * long enough that nothing else is holding the session open. */
            if (audio_io_playback_active() || quiet_ms >= KEEPALIVE_QUIET_MS) {
                esp_websocket_client_send_text(s_client, KEEPALIVE,
                                               sizeof(KEEPALIVE) - 1, SEND_TIMEOUT);
            }
        }
        s_sending_ka = false;
    }
}

/* ---------------- public API ---------------- */

esp_err_t dg_agent_init(const dg_agent_callbacks_t *callbacks)
{
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

    /*
     * THE AUTH HEADER IS BUILT AT RUNTIME, because the key is no longer a
     * compile-time constant -- it comes from NVS, written by the setup portal,
     * with CONFIG_DEEPGRAM_API_KEY as a first-boot seed. See api_key.h.
     *
     * Both buffers are heap, not stack, deliberately. This function is 640 B of
     * frame before them and the two together are ~290 B more; the pattern this
     * project settled on for transient buffers of that size is the heap, in
     * PSRAM where it can be -- see .claude/skills/esp-stack-budget/.
     *
     * The client STRDUPS the header (esp_websocket_client.c, cfg->headers), so
     * it does not have to outlive esp_websocket_client_init(). That is the only
     * reason this can be freed a few lines down instead of living forever.
     */
    char *key = heap_caps_malloc(DG_API_KEY_LEN, MALLOC_CAP_SPIRAM);
    char *auth = heap_caps_malloc(sizeof("Authorization: Token \r\n") + DG_API_KEY_LEN,
                                  MALLOC_CAP_SPIRAM);
    if (key == NULL || auth == NULL) {
        free(key);
        free(auth);
        free(s_json);
        s_json = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (!api_key_load(key)) {
        ESP_LOGE(TAG, "no Deepgram API key: set one through the setup portal, "
                      "or seed CONFIG_DEEPGRAM_API_KEY with `idf.py menuconfig`");
        free(key);
        free(auth);
        free(s_json);
        s_json = NULL;
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(auth, sizeof("Authorization: Token \r\n") + DG_API_KEY_LEN,
             "Authorization: Token %s\r\n", key);
    /* Done with the plaintext copy; the header keeps the only one this function
     * needs, and that is freed below too. */
    memset(key, 0, DG_API_KEY_LEN);
    free(key);

    const esp_websocket_client_config_t cfg = {
        .uri = DG_AGENT_URI,
        /* The Agent endpoint authenticates on the upgrade request only. */
        .headers = auth,
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
    /* Copied by now, whether init succeeded or not. */
    memset(auth, 0, strlen(auth));
    free(auth);
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

    /*
     * The uplink queue and its sender. Created here, once, for the life of the
     * process -- see the note on AUDIO_QUEUE_FRAMES.
     *
     * Priority 5, between the capture task (7, must never wait on this) and the
     * keepalive (4). Core 0 with the rest of the network work, deliberately NOT
     * core 1 where the audio tasks live: this one blocks on the socket by design
     * and must not share a core with the task feeding it.
     *
     * 4096 rather than the keepalive's 3072 because this runs send_bin down
     * through mbedtls 12.5 times a second rather than once every five seconds --
     * the same depth, far more often, so the margin is worth 1 kB.
     */
    s_audio_rb = xRingbufferCreateWithCaps(AUDIO_QUEUE_BYTES, RINGBUF_TYPE_NOSPLIT,
                                           MALLOC_CAP_SPIRAM);
    if (s_audio_rb == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the uplink queue");
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(audio_send_task, "dg_uplink", 4096, NULL, 5,
                                &s_send_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "uplink task not created");
        vRingbufferDeleteWithCaps(s_audio_rb);
        s_audio_rb = NULL;
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        free(s_json);
        s_json = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t dg_agent_start(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Stale bytes from a message that was cut off by the previous teardown, and
     * a drop that teardown may have left mid-message. */
    s_json_len = 0;
    s_json_dropping = false;

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
     * QUIESCE THE UPLINK BEFORE TOUCHING THE CLIENT. This is the whole reason
     * the audio send lives on its own task.
     *
     * s_ready is already false, so audio_send_task() will not START another
     * send; what is left is a send that is already in flight, holding the client
     * lock. esp_websocket_client_stop() cannot survive that -- it waits on
     * STOPPED_BIT with portMAX_DELAY -- so waiting here, where the wait is
     * BOUNDED, is what turns a permanent hang into a delay.
     *
     * The bound is AUDIO_SEND_TIMEOUT plus a margin, because that is the deadline
     * the in-flight send is itself running against: it has to return by then,
     * successfully or not. Proceeding anyway on expiry is deliberate -- it leaves
     * us exactly where the old code always was, rather than adding a second way
     * to hang.
     */
    const int64_t quiesce_deadline = esp_timer_get_time() +
                                     (int64_t)pdTICKS_TO_MS(AUDIO_SEND_TIMEOUT) * 1000 + 500000;
    while ((s_sending || s_sending_ka) && esp_timer_get_time() < quiesce_deadline) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_sending || s_sending_ka) {
        ESP_LOGW(TAG, "still sending at the stop deadline (audio=%d keepalive=%d) "
                      "-- stopping anyway", (int)s_sending, (int)s_sending_ka);
    }

    /* Nothing queued is worth sending now, and leaving it would have the sender
     * push stale audio into the next session's socket. */
    if (s_audio_rb != NULL) {
        size_t len = 0;
        void *frame;
        while ((frame = xRingbufferReceive(s_audio_rb, &len, 0)) != NULL) {
            vRingbufferReturnItem(s_audio_rb, frame);
        }
    }

    /*
     * The CLOSE frame is worth sending: Deepgram bills on session duration, and
     * a half-open socket only finalises at the server's idle timeout. But only
     * when connected -- otherwise close() is a guaranteed no-op that just logs
     * an error. Neither call is checked: close() returns ESP_FAIL if the client
     * is already down, and stop() returns ESP_FAIL when a successful close has
     * already stopped the task. Both are expected, not failures.
     *
     * EXCEPT THAT THE CLOSE FRAME IS NOT SENT AT ALL ANY MORE, and the reason is
     * an upstream bug rather than a decision about billing.
     *
     * esp_websocket_client_close() TAKES A TIMEOUT AND IGNORES IT. It forwards
     * portMAX_DELAY to esp_websocket_client_send_close() -- see
     * esp_websocket_client_close_with_optional_body() in the managed component --
     * so the 1000 ms this used to pass never meant anything. On a socket that is
     * not draining, that write never returns; and because it runs BEFORE
     * esp_websocket_client_stop(), it hangs the caller outright. session_ctl's
     * do_stop() stalls with "stopping" on the panel, s_busy latches, and every
     * gesture is refused until the board is physically reset.
     *
     * Measured on hardware 2026-08-27, repeatedly, on an access point that could
     * not drain the uplink.
     *
     * TWO ATTEMPTS AT SENDING IT ONLY WHEN SAFE BOTH FAILED, and the second is
     * why this is now unconditional: the first read the send's return value,
     * which lies by design (transport_ws.c LOCAL PATCH 2), and the second timed
     * the send, which does not lie but is still a guess -- and a guess whose
     * failure mode is a device that needs a reset is not a guess worth making.
     *
     * Skipping it costs a few seconds of billing per session: without a CLOSE the
     * socket is half-open and Deepgram finalises it at its own idle timer instead
     * of immediately. That is the whole price, and it buys a stop bounded by
     * esp_websocket_client_stop() alone -- ~4 s on a congested link, measured,
     * and it completes.
     *
     * The way back, if that billing ever matters more than this did: vendor
     * esp_websocket_client the way components/tcp_transport already vendors its
     * upstream, and make close_with_optional_body() honour its own timeout. Worth
     * knowing before anyone tries -- on a congested socket the frame will not go
     * out even with a correct deadline, so the payoff is failing in 1 s rather
     * than hanging, which is what this line already achieves for free.
     */
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
    if (!s_ready || len == 0 || s_audio_rb == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * NON-BLOCKING, AND THAT IS THE ENTIRE POINT. This runs on the capture task,
     * which also drives the display tap and must come back for the next 80 ms
     * chunk whatever the network is doing. A zero wait means a congested uplink
     * costs one frame here instead of stalling the task that owns the microphone
     * -- and, more importantly, means this task never holds the client lock that
     * dg_agent_stop() needs.
     */
    if (xRingbufferSend(s_audio_rb, pcm, len, 0) != pdTRUE) {
        /* Counted rather than logged: at 12.5 frames/s a log line per drop is
         * its own denial of service, and the count rides the TLM line. */
        s_audio_dropped++;
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

uint32_t dg_agent_audio_dropped(void)
{
    return s_audio_dropped;
}

uint32_t dg_agent_transport_dropped(void)
{
    return transport_ws_local_dropped_frames();
}

