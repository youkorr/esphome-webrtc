"""ESPHome external component: Espressif ESP-WebRTC port for the ESP32-P4.

Wraps Espressif's `esp_webrtc` / `esp_peer` stack so an ESP32-P4 can do
real-time peer-to-peer audio/video calling from an ESPHome configuration.

NOTE: Only the ESP32-P4 is supported (hardware H.264/MJPEG + the camera/codec
pipeline the upstream demos rely on), and only the ESP-IDF framework.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_URL

from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    only_on_variant,
)
from esphome.components.esp32.const import VARIANT_ESP32P4

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]

CONF_SIGNALING = "signaling"
CONF_ROOM_ID = "room_id"
CONF_SIGNAL_URL = "signal_url"
CONF_BOARD = "board"
CONF_VIDEO_CODEC = "video_codec"
CONF_AUDIO_CODEC = "audio_codec"
CONF_VIDEO_DIRECTION = "video_direction"
CONF_AUDIO_DIRECTION = "audio_direction"
CONF_VIDEO_BITRATE = "video_bitrate"
CONF_AUDIO_BITRATE = "audio_bitrate"
CONF_VIDEO_WIDTH = "video_width"
CONF_VIDEO_HEIGHT = "video_height"
CONF_FPS = "fps"
CONF_ENABLE_DATA_CHANNEL = "enable_data_channel"
CONF_AUTO_START = "auto_start"
CONF_ICE_SERVERS = "ice_servers"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_PAIRED = "on_paired"
CONF_ON_CONNECT_FAILED = "on_connect_failed"

webrtc_ns = cg.esphome_ns.namespace("webrtc")
WebRTCComponent = webrtc_ns.class_("WebRTCComponent", cg.Component)

# Triggers
ConnectedTrigger = webrtc_ns.class_("ConnectedTrigger", automation.Trigger.template())
DisconnectedTrigger = webrtc_ns.class_(
    "DisconnectedTrigger", automation.Trigger.template()
)
PairedTrigger = webrtc_ns.class_("PairedTrigger", automation.Trigger.template())
ConnectFailedTrigger = webrtc_ns.class_(
    "ConnectFailedTrigger", automation.Trigger.template()
)

# Actions
StartAction = webrtc_ns.class_("StartAction", automation.Action)
StopAction = webrtc_ns.class_("StopAction", automation.Action)
SendDataAction = webrtc_ns.class_("SendDataAction", automation.Action)

# Enums shared with C++ (see webrtc.h)
Signaling = webrtc_ns.enum("Signaling")
# janus is not available at esp-webrtc-solution v1.0.0 (no impl/janus_signal),
# so it is not offered here. apprtc + whip only.
SIGNALING = {
    "apprtc": Signaling.SIGNALING_APPRTC,
    "whip": Signaling.SIGNALING_WHIP,
}

VideoCodec = webrtc_ns.enum("VideoCodec")
VIDEO_CODEC = {
    "none": VideoCodec.VIDEO_CODEC_NONE,
    "h264": VideoCodec.VIDEO_CODEC_H264,
    "mjpeg": VideoCodec.VIDEO_CODEC_MJPEG,
}

AudioCodec = webrtc_ns.enum("AudioCodec")
AUDIO_CODEC = {
    "none": AudioCodec.AUDIO_CODEC_NONE,
    "g711a": AudioCodec.AUDIO_CODEC_G711A,
    "g711u": AudioCodec.AUDIO_CODEC_G711U,
    "opus": AudioCodec.AUDIO_CODEC_OPUS,
}

MediaDir = webrtc_ns.enum("MediaDir")
MEDIA_DIR = {
    "none": MediaDir.MEDIA_DIR_NONE,
    "send_only": MediaDir.MEDIA_DIR_SEND_ONLY,
    "recv_only": MediaDir.MEDIA_DIR_RECV_ONLY,
    "sendrecv": MediaDir.MEDIA_DIR_SEND_RECV,
}

ICE_SERVER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_URL): cv.string,
        cv.Optional(CONF_USERNAME): cv.string,
        cv.Optional(CONF_PASSWORD): cv.string,
    }
)


def _validate(config):
    # ESP-IDF only: enforced by only_on_variant(ESP32P4) below -- the P4 has no
    # Arduino support, so requiring the P4 variant already implies ESP-IDF.
    sig = config[CONF_SIGNALING]
    if sig == "apprtc":
        if CONF_ROOM_ID not in config:
            raise cv.Invalid("'room_id' is required when signaling: apprtc")
    else:
        if CONF_SIGNAL_URL not in config:
            raise cv.Invalid(f"'signal_url' is required when signaling: {sig}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebRTCComponent),
            cv.Optional(CONF_SIGNALING, default="apprtc"): cv.enum(
                SIGNALING, lower=True
            ),
            cv.Optional(CONF_ROOM_ID): cv.string,
            cv.Optional(CONF_SIGNAL_URL): cv.string,
            cv.Optional(CONF_BOARD, default="ESP32_P4_DEV_V14"): cv.string,
            cv.Optional(CONF_VIDEO_CODEC, default="h264"): cv.enum(
                VIDEO_CODEC, lower=True
            ),
            cv.Optional(CONF_AUDIO_CODEC, default="opus"): cv.enum(
                AUDIO_CODEC, lower=True
            ),
            cv.Optional(CONF_VIDEO_DIRECTION, default="sendrecv"): cv.enum(
                MEDIA_DIR, lower=True
            ),
            cv.Optional(CONF_AUDIO_DIRECTION, default="sendrecv"): cv.enum(
                MEDIA_DIR, lower=True
            ),
            cv.Optional(CONF_VIDEO_BITRATE, default=0): cv.uint32_t,
            cv.Optional(CONF_AUDIO_BITRATE, default=0): cv.uint32_t,
            cv.Optional(CONF_VIDEO_WIDTH, default=1024): cv.uint16_t,
            cv.Optional(CONF_VIDEO_HEIGHT, default=600): cv.uint16_t,
            cv.Optional(CONF_FPS, default=10): cv.int_range(min=1, max=60),
            cv.Optional(CONF_ENABLE_DATA_CHANNEL, default=True): cv.boolean,
            cv.Optional(CONF_AUTO_START, default=False): cv.boolean,
            cv.Optional(CONF_ICE_SERVERS): cv.ensure_list(ICE_SERVER_SCHEMA),
            cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConnectedTrigger)}
            ),
            cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DisconnectedTrigger)}
            ),
            cv.Optional(CONF_ON_PAIRED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PairedTrigger)}
            ),
            cv.Optional(CONF_ON_CONNECT_FAILED): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ConnectFailedTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    only_on_variant(supported=[VARIANT_ESP32P4]),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_signaling(config[CONF_SIGNALING]))
    if CONF_ROOM_ID in config:
        cg.add(var.set_room_id(config[CONF_ROOM_ID]))
    if CONF_SIGNAL_URL in config:
        cg.add(var.set_signal_url(config[CONF_SIGNAL_URL]))
    cg.add(var.set_board(config[CONF_BOARD]))
    cg.add(var.set_video_codec(config[CONF_VIDEO_CODEC]))
    cg.add(var.set_audio_codec(config[CONF_AUDIO_CODEC]))
    cg.add(var.set_video_direction(config[CONF_VIDEO_DIRECTION]))
    cg.add(var.set_audio_direction(config[CONF_AUDIO_DIRECTION]))
    cg.add(var.set_video_bitrate(config[CONF_VIDEO_BITRATE]))
    cg.add(var.set_audio_bitrate(config[CONF_AUDIO_BITRATE]))
    cg.add(var.set_video_width(config[CONF_VIDEO_WIDTH]))
    cg.add(var.set_video_height(config[CONF_VIDEO_HEIGHT]))
    cg.add(var.set_video_fps(config[CONF_FPS]))
    cg.add(var.set_enable_data_channel(config[CONF_ENABLE_DATA_CHANNEL]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))

    for srv in config.get(CONF_ICE_SERVERS, []):
        cg.add(
            var.add_ice_server(
                srv[CONF_URL],
                srv.get(CONF_USERNAME, ""),
                srv.get(CONF_PASSWORD, ""),
            )
        )

    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_DISCONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_PAIRED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_CONNECT_FAILED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    # --- esp-webrtc-solution components (git, NOT registry) ---
    # esp_webrtc, codec_board and the impls are not published to the ESP
    # Component Registry: they live only in the esp-webrtc-solution repo, and
    # the upstream demos pull them in via EXTRA_COMPONENT_DIRS (local paths).
    # ESPHome can't do that, so we fetch them by git subpath. The core
    # dependencies they need (esp_peer, tempotian/av_render,
    # tempotian/media_lib_utils, esp_capture, esp_codec_dev, nghttp,
    # esp_websocket_client) ARE in the registry and resolve automatically.
    #
    # Pinned to v1.0.0 (the C++ targets the ~0.9.1 API; v1.0.0 is the closest
    # stable tag). The component layout below matches the v1.0.0 videocall_demo
    # -- it differs from newer refs (e.g. janus_signal only exists later).
    webrtc_repo = "https://github.com/espressif/esp-webrtc-solution.git"
    webrtc_ref = "v1.0.0"

    def webrtc_git(name, path):
        add_idf_component(name=name, repo=webrtc_repo, ref=webrtc_ref, path=path)

    # Core components (not in the registry).
    webrtc_git("esp_webrtc", "components/esp_webrtc")
    webrtc_git("codec_board", "components/codec_board")

    # esp_webrtc impls (v1.0.0 layout). peer_default provides
    # esp_peer_get_default_impl(); apprtc/whip provide the signaling backends
    # (janus does not exist at this tag).
    webrtc_git("peer_default", "components/esp_webrtc/impl/peer_default")
    webrtc_git("apprtc_signal", "components/esp_webrtc/impl/apprtc_signal")
    webrtc_git("whip_signal", "components/esp_webrtc/impl/whip_signal")

    # esp_capture source + encoder impls used by media_sys.c (camera V4L2 src,
    # mic src, audio/video encoders). The esp_capture core itself comes from
    # the registry via esp_webrtc's manifest.
    webrtc_git("capture_audio_src", "components/esp_capture/src/impl/capture_audio_src")
    webrtc_git("capture_video_src", "components/esp_capture/src/impl/capture_video_src")
    webrtc_git("capture_audio_enc", "components/esp_capture/src/impl/capture_audio_enc")
    webrtc_git("capture_video_enc", "components/esp_capture/src/impl/capture_video_enc")

    # av_render renderer impls (I2S speaker + LCD) used by media_sys.c. The
    # av_render core comes from the registry (tempotian/av_render).
    webrtc_git("render_impl", "components/av_render/render_impl")

    # Registry: hardware H.264 (version matching the v1.0.0 demo) + P4 camera.
    add_idf_component(name="espressif/esp_h264", ref="1.0.4")
    add_idf_component(name="espressif/esp_video", ref="~1.0")

    # --- sdkconfig required by ESP-WebRTC on the ESP32-P4 ---
    # DTLS-SRTP is mandatory for WebRTC media encryption.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_PROTO_DTLS", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_DTLS_SRTP", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CREATE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC", True)
    # PSRAM is required to hold the media buffers.
    add_idf_sdkconfig_option("CONFIG_SPIRAM", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP", True)
    # New I2C master driver (codec_board expects it).
    add_idf_sdkconfig_option("CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE", False)
    # I2S channel ISR must live in internal RAM. On firmwares that enable
    # CONFIG_GDMA_ISR_IRAM_SAFE (the P4 video/LCD stack does), GDMA refuses an
    # I2S channel whose context is in PSRAM ("user context not in internal
    # RAM"), which aborts audio init regardless of free RAM. This pairing is
    # the decisive fix for the boot-time audio crash -- keep both set wherever
    # this component does I2S via codec_board.
    add_idf_sdkconfig_option("CONFIG_I2S_ISR_IRAM_SAFE", True)
    # Experimental features used by the esp_video/ISP pipeline on the P4.
    add_idf_sdkconfig_option("CONFIG_IDF_EXPERIMENTAL_FEATURES", True)
    add_idf_sdkconfig_option(
        "CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER", True
    )
    # Bidirectional A/V + ICE candidate gathering opens many concurrent UDP
    # sockets; the IDF defaults are too low (matches upstream videocall_demo).
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_UDP_PCBS", 1024)
    add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", 64)
    add_idf_sdkconfig_option("CONFIG_LWIP_TCPIP_RECVMBOX_SIZE", 64)

    cg.add_define("USE_ESP_WEBRTC")


# --- Actions -----------------------------------------------------------------

# maybe_simple_id lets `webrtc.start: rtc` work as shorthand for
# `webrtc.start: {id: rtc}` (the plain dict schema rejected the bare id).
WEBRTC_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(WebRTCComponent)}
)


@automation.register_action(
    "webrtc.start", StartAction, WEBRTC_ACTION_SCHEMA, synchronous=True
)
async def webrtc_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "webrtc.stop", StopAction, WEBRTC_ACTION_SCHEMA, synchronous=True
)
async def webrtc_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


CONF_DATA = "data"
SEND_DATA_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(WebRTCComponent),
        cv.Required(CONF_DATA): cv.templatable(cv.string),
    }
)


@automation.register_action(
    "webrtc.send_data", SendDataAction, SEND_DATA_ACTION_SCHEMA, synchronous=True
)
async def webrtc_send_data_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    template_ = await cg.templatable(config[CONF_DATA], args, cg.std_string)
    cg.add(var.set_data(template_))
    return var
