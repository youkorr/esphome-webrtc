"""ESPHome external component: WebRTC peer-to-peer calling on the ESP32-P4,
built directly on Espressif's clean, registry-published `esp_peer`
(PeerConnection: ICE/STUN/TURN, DTLS-SRTP, SCTP data channel).

esp_peer only handles TRANSPORT; this component adds a small AppRTC signaling
client (signaling.cpp) and, in later phases, plugs esp_peer into youkorr's own
media components (esp_video + esp_h264 capture/encode, the ip-camera-viewer
edge264 decoder -> LVGL canvas, fdaudio) so it can share a firmware with LVGL.

ESP32-P4 + ESP-IDF only.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_URL

from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
    only_on_variant,
)
from esphome.components.esp32.const import VARIANT_ESP32P4

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]

CONF_ROOM_ID = "room_id"
CONF_VIDEO_CODEC = "video_codec"
CONF_AUDIO_CODEC = "audio_codec"
CONF_VIDEO_DIRECTION = "video_direction"
CONF_AUDIO_DIRECTION = "audio_direction"
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

ConnectedTrigger = webrtc_ns.class_("ConnectedTrigger", automation.Trigger.template())
DisconnectedTrigger = webrtc_ns.class_(
    "DisconnectedTrigger", automation.Trigger.template()
)
PairedTrigger = webrtc_ns.class_("PairedTrigger", automation.Trigger.template())
ConnectFailedTrigger = webrtc_ns.class_(
    "ConnectFailedTrigger", automation.Trigger.template()
)

StartAction = webrtc_ns.class_("StartAction", automation.Action)
StopAction = webrtc_ns.class_("StopAction", automation.Action)
SendDataAction = webrtc_ns.class_("SendDataAction", automation.Action)

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

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebRTCComponent),
            cv.Optional(CONF_ROOM_ID, default="esphome_room"): cv.string,
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
            cv.Optional(CONF_VIDEO_WIDTH, default=640): cv.uint16_t,
            cv.Optional(CONF_VIDEO_HEIGHT, default=480): cv.uint16_t,
            cv.Optional(CONF_FPS, default=15): cv.int_range(min=1, max=60),
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
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_room_id(config[CONF_ROOM_ID]))
    cg.add(var.set_video_codec(config[CONF_VIDEO_CODEC]))
    cg.add(var.set_audio_codec(config[CONF_AUDIO_CODEC]))
    cg.add(var.set_video_direction(config[CONF_VIDEO_DIRECTION]))
    cg.add(var.set_audio_direction(config[CONF_AUDIO_DIRECTION]))
    cg.add(
        var.set_video_resolution(
            config[CONF_VIDEO_WIDTH], config[CONF_VIDEO_HEIGHT], config[CONF_FPS]
        )
    )
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

    # --- esp_peer (registry) : the WebRTC transport ---
    add_idf_component(name="espressif/esp_peer", ref="~1.5")
    # AppRTC signaling client uses a WebSocket + HTTP + cJSON.
    add_idf_component(name="espressif/esp_websocket_client", ref="~1.4")
    # cJSON (json) and esp_http_client are built-in IDF components; ESPHome
    # 2026.2+ drops unused built-ins, so pull them in explicitly.
    include_builtin_idf_component("json")
    include_builtin_idf_component("esp_http_client")

    # DTLS-SRTP is mandatory for WebRTC media encryption.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_PROTO_DTLS", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_DTLS_SRTP", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CREATE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC", True)
    # WebRTC DTLS uses an ECDSA self-signed cert + ECDHE-ECDSA-AES-GCM
    # ciphersuites. Plain ESP-IDF has these on by default (the upstream demo
    # omits them) but ESPHome trims ciphersuites its own client TLS doesn't use,
    # which yields -0x7080 (MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE) in the DTLS
    # handshake. Force the pieces on.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECDH_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECDSA_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_GCM_C", True)
    # Cert bundle for HTTPS/WSS to webrtc.espressif.com.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    # PSRAM for the media/jitter buffers; keep small allocations internal (the
    # upstream demo does this -- helps mbedTLS/DTLS).
    add_idf_sdkconfig_option("CONFIG_SPIRAM", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL", 256)
    # esp_peer's ICE transport references struct sockaddr_in6 (IPv6).
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
    # ICE candidate gathering opens many concurrent UDP sockets.
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_UDP_PCBS", 1024)
    add_idf_sdkconfig_option("CONFIG_LWIP_UDP_RECVMBOX_SIZE", 64)
    add_idf_sdkconfig_option("CONFIG_LWIP_TCPIP_RECVMBOX_SIZE", 64)

    cg.add_define("USE_ESP_WEBRTC")


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
