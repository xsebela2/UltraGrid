#include <stdlib.h>
#include <stdbool.h>
#include <iostream>
#include <pipewire/pipewire.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <thread>
#include <future>
#include <chrono>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <spa/utils/result.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/format.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <rang.hpp>

#include "utils/synchronized_queue.h"
#include "debug.h"
#include "lib_common.h"
#include "video.h"
#include "video_capture.h"
#include "concurrent_queue/readerwriterqueue.h"

//#define ENABLE_INSTRUMENTATION

#ifdef ENABLE_INSTRUMENTATION
    
    class instrumtation_ScopeStopwatch
    {
        const char *name;
        std::chrono::time_point<std::chrono::high_resolution_clock> begin_time;

    public:    
        instrumtation_ScopeStopwatch (const char *name)
            :name(name)
        {
            begin_time = std::chrono::high_resolution_clock::now();
        }

        ~instrumtation_ScopeStopwatch(){
            auto now = std::chrono::high_resolution_clock::now();
            auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now-begin_time).count();
            LOG(LOG_LEVEL_NOTICE) << "[stopwatch \"" << name << "\"] took " << delta << " us\n";
        }
    };
    #define SCOPE_STOPWATCH(name) instrumtation_ScopeStopwatch scope_stopwatch_##name(#name)

#else
    #define SCOPE_STOPWATCH(name)
#endif

#define MAX_BUFFERS 2
static constexpr int MIN_BUFFERS_PW = 2;
static constexpr int DEFAULT_BUFFERS_PW = 2;
static constexpr int MAX_BUFFERS_PW = 2;
static constexpr int QUEUE_SIZE = 3;
static constexpr int DEFAULT_EXCPETING_FPS = 30;


struct request_path_t {
    std::string token;
    std::string path;

    static request_path_t create(const std::string &name) {
        ++token_counter;

        auto token = std::string("uv") + std::to_string(token_counter);
        request_path_t result = {
            .token = token,
            .path = std::string("/org/freedesktop/portal/desktop/request/") + name + "/" + token
        };

        LOG(LOG_LEVEL_DEBUG) << "new request: '" << result.path << "'\n";
        return result;
    }

private:
    static unsigned int token_counter;
};

unsigned int request_path_t::token_counter = 0;

struct session_path_t {
    std::string token;
    std::string path;

    static session_path_t create(const std::string &name) {
        ++token_counter;

        auto token = std::string("uv") + std::to_string(token_counter);
        return {
            .token = token,
            .path = std::string("/org/freedesktop/portal/desktop/session/") + name + "/" + token
        };
    }

private:
    static unsigned int token_counter;
};

unsigned int session_path_t::token_counter = 0;


template <typename F>
class ScopeExit {
    F func;
public:
    ScopeExit(F&& func) : func(std::forward<F>(func)) {}
    ~ScopeExit() { func(); }
};

using PortalCallCallback = std::function<void(uint32_t response, GVariant *results)>;

class ScreenCastPortal {
private:
    GMainLoop *dbus_loop;
    GDBusConnection *connection;
    GDBusProxy *screencast_proxy;
    std::string unique_name;
    session_path_t session;
public:
    // see https://flatpak.github.io/xdg-desktop-portal/#gdbus-signal-org-freedesktop-portal-Request.Response
    static constexpr uint32_t REQUEST_RESPONSE_OK = 0;
    static constexpr uint32_t REQUEST_RESPONSE_CANCELLED_BY_USER = 1;
    static constexpr uint32_t REQUEST_RESPONSE_OTHER_ERROR = 2;

    ScreenCastPortal() 
    {
        GError *error = nullptr;
        
        dbus_loop = g_main_loop_new(nullptr, false);
        connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        g_assert_no_error(error);
        assert(connection != nullptr);

        unique_name = g_dbus_connection_get_unique_name(connection) + 1;
        std::replace(unique_name.begin(), unique_name.end(), '.', '_');
        screencast_proxy = g_dbus_proxy_new_sync(
                connection, G_DBUS_PROXY_FLAGS_NONE, nullptr,
                "org.freedesktop.portal.Desktop",
                "/org/freedesktop/portal/desktop",
                "org.freedesktop.portal.ScreenCast", nullptr, &error);
        g_assert_no_error(error); 
        assert(screencast_proxy != nullptr);
        
        session = session_path_t::create(unique_name);
        LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: session path: '" << session.path << "'" << " token: '" << session.token << "'\n";
    }
    
    void call_with_request(const char* method_name, std::initializer_list<GVariant*> arguments, GVariantBuilder &params_builder, 
                            std::promise<std::string>& error_msg, PortalCallCallback &on_response)
    {
        assert(method_name != nullptr);
        request_path_t request_path = request_path_t::create(sender_name());
        LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: call_with_request: '" << method_name << "' request: '" << request_path.path << "'\n";
        auto response_callback = [](GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                    const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                    gpointer user_data) {
            (void) sender_name;
            (void) interface_name;
            (void) signal_name;
            
            
            uint32_t response;
            GVariant *results;
            g_variant_get(parameters, "(u@a{sv})", &response, &results);
            ScopeExit([&](){g_variant_unref(results); });

            static_cast<const PortalCallCallback *> (user_data)->operator()(response, results);
            g_dbus_connection_call(connection, "org.freedesktop.portal.Desktop",
                    object_path, "org.freedesktop.portal.Request", "Close",
                    nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
        };

        g_dbus_connection_signal_subscribe(connection, "org.freedesktop.portal.Desktop",
                                        "org.freedesktop.portal.Request",
                                        "Response",
                                        request_path.path.c_str(),
                                        nullptr,
                                        G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                        response_callback,
                                        const_cast<void*>(static_cast<const void*>(&on_response)),
                                        nullptr);

        auto call_finished = [](GObject *source_object, GAsyncResult *result, gpointer user_data) {
            auto& error_msg = *static_cast<std::promise<std::string>*>(user_data);
            GError *error = nullptr;
            GVariant *result_finished = g_dbus_proxy_call_finish(G_DBUS_PROXY(source_object), result, &error);
            ScopeExit scope_exit([&](){ g_variant_unref(result_finished); });
            
            if(error != nullptr){
                error_msg.set_value(error->message == nullptr ? "unknown error" : error->message);
                return;
            }

            const char *path = nullptr;
            g_variant_get(result_finished, "(o)", &path);
            LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: call_with_request finished: '" << path << "'\n";
        };


        g_variant_builder_add(&params_builder, "{sv}", "handle_token", g_variant_new_string(request_path.token.c_str()));
        
        GVariantBuilder args_builder;
        g_variant_builder_init(&args_builder, G_VARIANT_TYPE_TUPLE);
        for(GVariant* arg : arguments){
            g_variant_builder_add_value(&args_builder, arg);
        }
        g_variant_builder_add_value(&args_builder, g_variant_builder_end(&params_builder));

        g_dbus_proxy_call(screencast_proxy, method_name, g_variant_builder_end(&args_builder), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, call_finished, &error_msg);     
    }

    void run_loop() {
        g_main_loop_run(dbus_loop);
        LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: finished dbus loop \n";
    }

    void quit_loop() {
        g_main_loop_quit(dbus_loop);
    }

    GDBusProxy *proxy() {
        return screencast_proxy;
    }

    GDBusConnection *dbus_connection() const {
        return connection;
    }

    const std::string& sender_name() const
    {
        return unique_name;
    }

    const std::string& session_path() const
    {
        return session.path;
    }

    const std::string& session_token() const
    {
        return session.token;
    }

    ~ScreenCastPortal() {
        g_dbus_connection_call(dbus_connection(),
                    "org.freedesktop.portal.Desktop",
                    session.path.c_str(),
                    "org.freedesktop.portal.Session",
                    "Close", NULL, NULL,
                    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL,
                    NULL);
        g_main_loop_quit(dbus_loop);
        g_object_unref(screencast_proxy);
        g_object_unref(connection);
        g_main_loop_unref(dbus_loop);
    }
};

//using unique_ptr_video_frame = std::unique_ptr<video_frame, decltype(&vf_free)>;

class video_frame_wrapper
{
private:
    video_frame* frame;
public:
    explicit video_frame_wrapper(video_frame* frame = nullptr)
        :frame(frame)
    {}

    video_frame_wrapper(video_frame_wrapper&) = delete;
    video_frame_wrapper& operator= (video_frame_wrapper&) = delete;
    
    video_frame_wrapper(video_frame_wrapper&& other) 
        : frame(std::exchange(other.frame, nullptr))
    {}

    video_frame_wrapper& operator=(video_frame_wrapper&& other) {
        vf_free(frame);
        frame = std::exchange(other.frame, nullptr);
        return *this;
    }

    ~video_frame_wrapper(){
        vf_free(frame);
    }

    video_frame* get() {
        return frame;
    }

    video_frame* operator->(){
        return get();
    }
};


struct screen_cast_session { 
    // used exlusively by ultragrid thread
    video_frame_wrapper in_flight_frame;

    moodycamel::BlockingReaderWriterQueue<video_frame_wrapper> blank_frames {QUEUE_SIZE};
    moodycamel::BlockingReaderWriterQueue<video_frame_wrapper> sending_frames {QUEUE_SIZE};

    struct {
        bool show_cursor = false;
        std::string restore_file = "";
        uint32_t fps = 0;
        bool crop = true;
    } user_options;

    std::unique_ptr<ScreenCastPortal> portal;

    // empty string if no error occured, or an error message
    std::promise<std::string> init_error;

    struct pw_{
        pw_() {
            pw_init(&uv_argc, &uv_argv);
        }
        
        int fd = -1;
        uint32_t node = -1;
        
        struct pw_thread_loop *loop = nullptr;
        struct pw_core *core = nullptr;
        struct pw_context *context = nullptr;

        struct pw_stream *stream = nullptr;
        struct spa_hook stream_listener = {};
        struct spa_hook core_listener = {};

        struct spa_io_position *position = nullptr;

        struct spa_video_info format = {};

        int width() {
            return format.info.raw.size.width;
        }
        
        int height() {
            return format.info.raw.size.height;
        }

        spa_video_format video_format() {
            return format.info.raw.format;
        }

        video_frame_wrapper allocate_video_frame()
        {
            struct video_frame *frame = vf_alloc(1);
            frame->color_spec = RGBA;
            frame->interlacing = PROGRESSIVE;
            frame->fps = expecting_fps;
            frame->callbacks.data_deleter = vf_data_deleter;
            
            struct tile* tile = vf_get_tile(frame, 0);
            assert(tile != nullptr);
            tile->width = width();
            tile->height = height();
            tile->data_len = vc_get_linesize(tile->width, frame->color_spec) * tile->height;
            tile->data = (char *) malloc(tile->data_len);
            return video_frame_wrapper(frame);
        }

        int frame_count = 0;
        uint64_t frame_counter_begin_time = time_since_epoch_in_ms();
        uint64_t expecting_fps = DEFAULT_EXCPETING_FPS;

        ~pw_() {
            if(loop != nullptr){
                pw_thread_loop_stop(loop);
                if(stream != nullptr)
                    pw_stream_destroy(stream);
                if(context != nullptr)
                    pw_context_destroy(context);
                if(loop != nullptr)
                    pw_thread_loop_destroy(loop);
            }
            if( fd > 0)
                close(fd);
        }
    }pw;
};

static void on_stream_state_changed(void *session_ptr, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void) session_ptr;
    LOG(LOG_LEVEL_INFO) << "[screen_pw] stream state changed \"" << pw_stream_state_as_string(old) 
                        << "\" -> \""<<pw_stream_state_as_string(state)<<"\"\n";
    
    if (error != nullptr) {
        LOG(LOG_LEVEL_ERROR) << "[screen_pw] stream error: '"<< error << "'\n";
    }
}


static void on_stream_param_changed(void *session_ptr, uint32_t id, const struct spa_pod *param) {
    auto &session = *static_cast<screen_cast_session*>(session_ptr);
    (void) id;
    LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: [cap_pipewire] param changed:\n";
    spa_debug_format(2, nullptr, param);

    if(id == SPA_PARAM_Invalid)
    {
        assert(false && "invalid params");
    }

    // from example code, not sure what this is
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    int parse_format_ret = spa_format_parse(param, &session.pw.format.media_type, &session.pw.format.media_subtype);
    assert(parse_format_ret > 0);

    assert(session.pw.format.media_type == SPA_MEDIA_TYPE_video);
    assert(session.pw.format.media_subtype == SPA_MEDIA_SUBTYPE_raw);

    spa_format_video_raw_parse(param, &session.pw.format.info.raw);
    LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: size: " << session.pw.width() << " x " << session.pw.height() << "\n";

    int linesize = vc_get_linesize(session.pw.width(), RGBA);
    int32_t size = linesize * session.pw.height();

    uint8_t params_buffer[1024];

    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    
    const struct spa_pod *params[3];
    int n_params = 0;
    params[n_params++] = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, 10), //FIXME
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(linesize),
        SPA_PARAM_BUFFERS_dataType,
        SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr)))
    );
    
    if(session.user_options.crop) {
        params[n_params++] = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size,
            SPA_POD_Int(sizeof(struct spa_meta_region)))
        );
    }
    
    pw_stream_update_params(session.pw.stream, params, n_params);

    for(int i = 0; i < QUEUE_SIZE; ++i)
        session.blank_frames.enqueue(session.pw.allocate_video_frame());

    session.init_error.set_value("");
}


static void copy_frame_impl_cropped(bool swap_red_blue, char *dest, char *src, 
    int actual_width, int actual_height, int x_begin, int y_begin, int crop_width, int crop_height) 
{
    UNUSED(actual_height);
    SCOPE_STOPWATCH(copy_frame_impl_cropped);
    
    for(int y = y_begin; y < crop_height; ++y){
        int line_offset_src = 4 * y * actual_width;
        int line_offset_dest = 4 * y * crop_width;     
        if (swap_red_blue) {
            for(int x = x_begin; x < crop_width; ++x) {
                int x_offset = 4 * x;
                // rgba <- bgra    
                dest[line_offset_dest + x_offset    ] = src[line_offset_src + x_offset + 2];
                dest[line_offset_dest + x_offset + 1] = src[line_offset_src + x_offset + 1];
                dest[line_offset_dest + x_offset + 2] = src[line_offset_src + x_offset    ];
                dest[line_offset_dest + x_offset + 3] = src[line_offset_src + x_offset + 3];
            }
        } else {
            memcpy(dest+line_offset_dest, src + line_offset_src + 4 * x_begin, crop_width * 4);
        }
    }
}

static void copy_frame_impl(bool swap_red_blue, char *dest, char *src, int width, int height)
{
    SCOPE_STOPWATCH(copy_frame_impl);
    int linesize = vc_get_linesize(width, RGBA);
    if (swap_red_blue) {
        for (int line_offset = 0; line_offset < height * linesize; line_offset += linesize) {
            for(int x = 0; x < linesize; x += 4) {
                // rgba <- bgra
                dest[line_offset + x    ] = src[line_offset + x + 2];
                dest[line_offset + x + 1] = src[line_offset + x + 1];
                dest[line_offset + x + 2] = src[line_offset + x    ];
                dest[line_offset + x + 3] = src[line_offset + x + 3];
            }
        }
    } else {
        memcpy(dest, src, height * linesize);
    }
}

static void copy_frame(bool swap_red_blue, spa_buffer *buffer, video_frame_wrapper& output_frame, int session_width, int session_height, spa_region *crop_region = nullptr){
    if (crop_region != nullptr) {
        copy_frame_impl_cropped(swap_red_blue, output_frame->tiles[0].data, static_cast<char*>(buffer->datas[0].data), session_width, session_height,
            crop_region->position.x, crop_region->position.y, crop_region->size.width, crop_region->size.height);
    } else {
        copy_frame_impl(swap_red_blue, output_frame->tiles[0].data, static_cast<char*>(buffer->datas[0].data), session_width, session_height);
    }
    
    struct tile *tile = vf_get_tile(output_frame.get(), 0);
    assert(tile != nullptr);
    if (crop_region != nullptr){
        tile->width = crop_region->size.width;
        tile->height = crop_region->size.height;
    }else{
        tile->width = session_width;
        tile->height = session_height;
    }

    tile->data_len = vc_get_linesize(tile->width, RGBA) * tile->height;
}

static void on_process(void *session_ptr) {
    using namespace std::chrono_literals;
    SCOPE_STOPWATCH(on_process);

    screen_cast_session &session = *static_cast<screen_cast_session*>(session_ptr);
    pw_buffer *buffer;
    int n_buffers_from_pw = 0;
    while((buffer = pw_stream_dequeue_buffer(session.pw.stream)) != nullptr){    
        ++n_buffers_from_pw;

        video_frame_wrapper next_frame;
        
        assert(buffer->buffer != nullptr);
        assert(buffer->buffer->datas != nullptr);
        assert(buffer->buffer->n_datas == 1);
        assert(buffer->buffer->datas[0].data != nullptr);

        if(buffer->buffer->datas[0].chunk == nullptr || buffer->buffer->datas[0].chunk->size == 0) {
            LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: dropping - empty pw frame " << "\n";
            pw_stream_queue_buffer(session.pw.stream, buffer);
            continue;
        }

        if(!session.blank_frames.wait_dequeue_timed(next_frame, 1000ms / session.pw.expecting_fps)) {
            LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: dropping frame (blank frame dequeue timed out)\n";
            pw_stream_queue_buffer(session.pw.stream, buffer);
            continue;
        }

        spa_region *crop_region = nullptr;
        if (session.user_options.crop) {
            spa_meta_region *meta_crop_region = static_cast<spa_meta_region*>(spa_buffer_find_meta_data(buffer->buffer, SPA_META_VideoCrop, sizeof(*meta_crop_region)));
            if (meta_crop_region != nullptr && spa_meta_region_is_valid(meta_crop_region))
               crop_region = &meta_crop_region->region;
        }

        copy_frame(true, buffer->buffer,next_frame, session.pw.width(), session.pw.height(), crop_region);
    
        session.sending_frames.enqueue(std::move(next_frame));
        
        pw_stream_queue_buffer(session.pw.stream, buffer);
        
        ++session.pw.frame_count;
        uint64_t time_now = time_since_epoch_in_ms();

        uint64_t delta = time_now - session.pw.frame_counter_begin_time;
        if(delta >= 5000) {
            double average_fps = session.pw.frame_count / (delta / 1000.0);
            LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: on process: average fps in last 5 seconds: " << average_fps << "\n";
            session.pw.expecting_fps = average_fps;
            if(session.pw.expecting_fps == 0)
                session.pw.expecting_fps = 1;
            session.pw.frame_count = 0;
            session.pw.frame_counter_begin_time = time_since_epoch_in_ms();
        }
    }
    
    LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: from pw: "<< n_buffers_from_pw << "\t sending: "<<session.sending_frames.size_approx() << "\t blank: " << session.blank_frames.size_approx() << "\n";
    
}

static void on_drained(void*)
{
    LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: pipewire: drained\n";
}

static void on_add_buffer(void *session_ptr, struct pw_buffer *)
{
    (void) session_ptr;

    LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: pipewire: add_buffer\n";
}

static void on_remove_buffer(void *session_ptr, struct pw_buffer *)
{
    (void) session_ptr;
    LOG(LOG_LEVEL_VERBOSE) << "[screen_pw]: pipewire: remove_buffer\n";
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .destroy = nullptr,
        .state_changed = on_stream_state_changed,
        .control_info = nullptr,
        .io_changed = nullptr,
        .param_changed = on_stream_param_changed,
        .add_buffer = on_add_buffer,
        .remove_buffer = on_remove_buffer,
        .process = on_process,
        .drained = on_drained,
        .command = nullptr,
        .trigger_done = nullptr,
};

static int start_pipewire(screen_cast_session &session)
{    
    const struct spa_pod *params[2] = {};
    uint8_t params_buffer[1024];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    session.pw.loop = pw_thread_loop_new("pipewire_thread_loop", nullptr);
    assert(session.pw.loop != nullptr);
    pw_thread_loop_lock(session.pw.loop);
    session.pw.context = pw_context_new(pw_thread_loop_get_loop(session.pw.loop), nullptr, 0);
    assert(session.pw.context != nullptr);

    if (pw_thread_loop_start(session.pw.loop) != 0) {
        assert(false && "error starting pipewire thread loop");
    }

    pw_core *core = pw_context_connect_fd(session.pw.context, session.pw.fd, nullptr, 0); 
    assert(core != nullptr);

    session.pw.stream = pw_stream_new(core, "my_screencast", pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Screen",
            nullptr));
    assert(session.pw.stream != nullptr);
    pw_stream_add_listener(session.pw.stream, &session.pw.stream_listener, &stream_events, &session);

    auto size_rect_def = SPA_RECTANGLE(640, 480);
    auto size_rect_min = SPA_RECTANGLE(1, 1);
    auto size_rect_max = SPA_RECTANGLE(3840,
                                       2160);

    auto framerate_def = SPA_FRACTION(session.user_options.fps > 0 ? session.user_options.fps : DEFAULT_EXCPETING_FPS, 1);
    auto framerate_min = SPA_FRACTION(0, 1);
    auto framerate_max = SPA_FRACTION(600, 1);

    const int n_params = 1;
    params[0] = static_cast<spa_pod *> (spa_pod_builder_add_object(
            &pod_builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format,
            SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA,
			SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx),
            SPA_FORMAT_VIDEO_size,
            SPA_POD_CHOICE_RANGE_Rectangle(
                    &size_rect_def,
                    &size_rect_min,
                    &size_rect_max),
            SPA_FORMAT_VIDEO_framerate,
            SPA_POD_CHOICE_RANGE_Fraction(
                    &framerate_def,
                    &framerate_min,
                    &framerate_max)
    ));


    if (int res; (res = pw_stream_connect(session.pw.stream,
                                          PW_DIRECTION_INPUT,
                                          session.pw.node,
                                          static_cast<pw_stream_flags>(
                                                  PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                          params, n_params)) < 0) {
        fprintf(stderr, "can't connect: %s\n", spa_strerror(res));
        return -1;
    }

    pw_thread_loop_unlock(session.pw.loop);
    return 0;
}

static void on_portal_session_closed(GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                                    const gchar *interface_name, const gchar *signal_name, GVariant *parameters, gpointer user_data)
{
    (void) connection;
    (void) sender_name;
    (void) object_path;
    (void) interface_name;
    (void) signal_name;
    (void) parameters;
    auto &session = *static_cast<screen_cast_session*>(user_data);
    //TODO: check if this is fired by newer Gnome 
    LOG(LOG_LEVEL_INFO) << "[screen_pw] session closed by compositor\n";
    pw_thread_loop_stop(session.pw.loop);
}

static void run_screencast(screen_cast_session *session_ptr) {
    auto& session = *session_ptr;
    session.portal = std::make_unique<ScreenCastPortal>();

    session.pw.fd = -1;
    session.pw.node = UINT32_MAX;
    
    g_dbus_connection_signal_subscribe(session.portal->dbus_connection(), 
                                       nullptr, // sender
                                       "org.freedesktop.portal.Session", // interface_name
                                       "closed", //signal name
                                       session.portal->session_path().c_str(), // object path
                                       nullptr, // arg0
                                       G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                       on_portal_session_closed,
                                       session_ptr,
                                       nullptr);

    auto pipewire_opened = [](GObject *source, GAsyncResult *res, void *user_data) {
        auto session = static_cast<screen_cast_session*>(user_data);
        GError *error = nullptr;
        GUnixFDList *fd_list = nullptr;

        GVariant *result = g_dbus_proxy_call_with_unix_fd_list_finish(G_DBUS_PROXY(source), &fd_list, res, &error);
        g_assert_no_error(error);
        ScopeExit([&](){g_variant_unref(result); });

        gint32 handle;
        g_variant_get(result, "(h)", &handle);
        assert(handle == 0); //it should always be the first index

        session->pw.fd = g_unix_fd_list_get(fd_list, handle, &error);
        g_assert_no_error(error);

        assert(session->pw.fd != -1);
        assert(session->pw.node != UINT32_MAX);
        
        LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: starting pipewire\n";
        start_pipewire(*session);
    };

    PortalCallCallback started = [&](uint32_t response, GVariant *results) {
        LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: started: " << g_variant_print(results, true) << "\n";
        
        if(response == ScreenCastPortal::REQUEST_RESPONSE_CANCELLED_BY_USER) {
            session.init_error.set_value("failed to start (dialog cancelled by user)");
            return;
        } else if(response != ScreenCastPortal::REQUEST_RESPONSE_OK) {
            session.init_error.set_value("failed to start (unknown reason)");
            return;
        }

        const char *restore_token = nullptr;
        if (g_variant_lookup(results, "restore_token", "s", &restore_token)){
            if(session.user_options.restore_file.empty()){
                LOG(LOG_LEVEL_WARNING) << "[screen_pw]: got unexpected restore_token from ScreenCast portal, ignoring it\n";
            }else{
                std::ofstream file(session.user_options.restore_file);
                file<<restore_token;
            }
        }
        
        GVariant *streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE_ARRAY);
        GVariant *stream_properties;
        GVariantIter iter;
        g_variant_iter_init(&iter, streams);
        assert(g_variant_iter_n_children(&iter) == 1);
        bool got_item = g_variant_iter_loop(&iter, "(u@a{sv})", &session.pw.node, &stream_properties);
        assert(got_item);
        uint32_t capture_type;
        g_variant_lookup(stream_properties, "source_type", "u", &capture_type);
        if(capture_type == 1) {
            // user selected a whole screen inside Portal dialog, that means crop can be disabled without changing anything
            session.user_options.crop = false;
        }

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
        g_dbus_proxy_call_with_unix_fd_list(session.portal->proxy(), "OpenPipeWireRemote",
                                            g_variant_new("(oa{sv})", session.portal->session_path().c_str(), &builder),
                                            G_DBUS_CALL_FLAGS_NONE, -1,
                                            nullptr, nullptr, pipewire_opened, &session);
        g_variant_builder_clear(&builder);
    };

    PortalCallCallback sources_selected = [&](uint32_t response, GVariant *results) {
        gchar *pretty = g_variant_print(results, true);
        LOG(LOG_LEVEL_INFO) << "[screen_pw]: selected sources: " << pretty << "\n";
        g_free((gpointer) pretty);

        if(response != ScreenCastPortal::REQUEST_RESPONSE_OK) {
            session.init_error.set_value("Failed to select sources");
            return;
        }

        {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            session.portal->call_with_request("Start", {g_variant_new_object_path(session.portal->session_path().c_str()),  /*parent window: */ g_variant_new_string("")}, options, session.init_error, started);
        }
    };

    PortalCallCallback session_created = [&](uint32_t response, GVariant *results) {
        if(response != ScreenCastPortal::REQUEST_RESPONSE_OK) {
            session.init_error.set_value("Failed to create session");
            return;
        }
        
        const char *session_handle = nullptr;
        g_variant_lookup(results, "session_handle", "s", &session_handle);
        
        LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: session created with handle: " << session_handle << "\n";
        assert(session.portal->session_path() == session_handle);
        
        {
            GVariantBuilder params;
            g_variant_builder_init(&params, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&params, "{sv}", "types", g_variant_new_uint32(3)); // 1 full screen, 2 - a window, 3 - both
            g_variant_builder_add(&params, "{sv}", "multiple", g_variant_new_boolean(false));
            if(session.user_options.show_cursor)
                g_variant_builder_add(&params, "{sv}", "cursor_mode", g_variant_new_uint32(2));
            
            if(!session.user_options.restore_file.empty()){
                std::string token;
                std::ifstream file(session.user_options.restore_file);

                if(file.is_open()) {
                    std::ostringstream ss;
                    ss << file.rdbuf();
                    token = ss.str();
                }
                
                //  0: Do not persist (default), 1: Permissions persist as long as the application is running, 2: Permissions persist until explicitly revoked
                g_variant_builder_add(&params, "{sv}", "persist_mode", g_variant_new_uint32(2)); 
                if(!token.empty())
                    g_variant_builder_add(&params, "{sv}", "restore_token", g_variant_new_string(token.c_str())); 
            }

            session.portal->call_with_request("SelectSources", {g_variant_new_object_path(session.portal->session_path().c_str())}, params, session.init_error, sources_selected);
        }
    };


    {
        GVariantBuilder params;
        g_variant_builder_init(&params, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&params, "{sv}", "session_handle_token", g_variant_new_string(session.portal->session_token().c_str()));
        
        session.portal->call_with_request("CreateSession", {}, params, session.init_error, session_created);
    }
    
    session.portal->run_loop();
}

static struct vidcap_type * vidcap_screen_pw_probe(bool verbose, void (**deleter)(void *))
{
    UNUSED(verbose);
    
    struct vidcap_type* vt;
    *deleter = free;

    vt = (struct vidcap_type *) calloc(1, sizeof(struct vidcap_type));
    if (vt == nullptr) {
        return nullptr;
    }

    vt->name = "screen_pw";
    vt->description = "Grabbing screen using PipeWire";
    return vt;
}

static void show_help() {
    auto param = [](const char* name) -> std::ostream& {
        std::cout << rang::style::bold << "  " <<name << rang::style::reset << " - ";
        return std::cout;
    };

    std::cout << "Screen capture using PipeWire and ScreenCast freedesktop portal API\n";
    std::cout << "Usage: -t screen_pw[:cursor|:nocrop|:fps=<fps>|:restore=<token_file>]]\n";
    param("cursor") << "make the cursor visible (default hidden)\n";
    param("nocrop") << "when capturing a window do not crop out the empty background\n";
    param("<fps>") << "prefered FPS passed to PipeWire (PipeWire may ignore it)\n";
    param("<token_file>") << "restore the selected window/display from a file.\n\t\tIf not possible, display the selection dialog and save the token to the file specified.\n";
}


static int parse_params(struct vidcap_params *params, screen_cast_session &session) {
    if(const char *fmt = vidcap_params_get_fmt(params)) {        
        std::istringstream params_stream(fmt);
        
        std::string param;
        while (std::getline(params_stream, param, ':')) {
            if (param == "help") {
                    show_help();
                    return VIDCAP_INIT_NOERR;
            } else if (param == "cursor") {
                session.user_options.show_cursor = true;
            } else if (param == "nocrop") {
                session.user_options.crop = false;
            } else {
                auto split_index = param.find('=');
                if(split_index != std::string::npos && split_index != 0){
                    std::string name = param.substr(0, split_index);
                    std::string value = param.substr(split_index + 1);

                    if (name == "fps" || name == "FPS"){
                        std::istringstream is(value);
                        is >> session.user_options.fps;
                        continue;
                    }else if(name == "restore"){
                        session.user_options.restore_file = value;
                        continue;
                    }
                }

                LOG(LOG_LEVEL_ERROR) << "[screen_pw] invalid option: \"" << param << "\"\n";
                return VIDCAP_INIT_FAIL;
            }
        }
    }
    return VIDCAP_INIT_OK;
}

static int vidcap_screen_pw_init(struct vidcap_params *params, void **state)
{
    if (vidcap_params_get_flags(params) & VIDCAP_FLAG_AUDIO_ANY) {
        return VIDCAP_INIT_AUDIO_NOT_SUPPOTED;
    }

    screen_cast_session &session = *new screen_cast_session();
    *state = &session;

    LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: [cap_pipewire] init\n";
    
    int params_ok = parse_params(params, session);
    if(params_ok != VIDCAP_INIT_OK)
        return params_ok;

    std::future<std::string> future_error = session.init_error.get_future();
    std::thread dbus_thread(run_screencast, &session);
    future_error.wait();
    
    if (std::string error_msg = future_error.get(); !error_msg.empty()) {
        LOG(LOG_LEVEL_FATAL) << "[screen_pw]: " << error_msg << "\n";
        dbus_thread.join();
        session.portal->quit_loop();
        return VIDCAP_INIT_FAIL;
    }

    dbus_thread.detach();
    LOG(LOG_LEVEL_DEBUG) << "[screen_pw]: init ok\n";
    return VIDCAP_INIT_OK;
}

static void vidcap_screen_pw_done(void *session_ptr)
{
    LOG(LOG_LEVEL_DEBUG) <<"[cap_pipewire] done\n";   
    delete static_cast<screen_cast_session*>(session_ptr);
}

static struct video_frame *vidcap_screen_pw_grab(void *session_ptr, struct audio_frame **audio)
{    
    SCOPE_STOPWATCH(vidcap_screen_pw_grab);

    assert(session_ptr != nullptr);
    auto &session = *static_cast<screen_cast_session*>(session_ptr);
    *audio = nullptr;
   
    if(session.in_flight_frame.get() != nullptr){
        session.blank_frames.enqueue(std::move(session.in_flight_frame));
    }
    
    using namespace std::chrono_literals;
    session.sending_frames.wait_dequeue_timed(session.in_flight_frame, 500ms);
    return session.in_flight_frame.get();
}

static const struct video_capture_info vidcap_screen_pw_info = {
    vidcap_screen_pw_probe,
    vidcap_screen_pw_init,
    vidcap_screen_pw_done,
    vidcap_screen_pw_grab,
    true,
};

REGISTER_MODULE(screen_pw, &vidcap_screen_pw_info, LIBRARY_CLASS_VIDEO_CAPTURE, VIDEO_CAPTURE_ABI_VERSION);