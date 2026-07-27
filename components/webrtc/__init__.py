"""ESPHome external component: WebRTC peer-to-peer calling on the ESP32-P4,
built directly on Espressif's clean, registry-published `esp_peer`
(PeerConnection: ICE/STUN/TURN, DTLS-SRTP, SCTP data channel).

esp_peer only handles TRANSPORT; this component adds a small AppRTC signaling
client (signaling.cpp), a G.711 audio bridge that shares fdaudio's mic/speaker
(via the ESPHome microphone + speaker platforms, so it coexists with
voice_assistant), and — in later phases — the esp_video/esp_h264 capture path
and the ip-camera-viewer edge264 decoder -> LVGL canvas.

ESP32-P4 + ESP-IDF only.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_TRIGGER_ID, CONF_URL, CONF_SAMPLE_RATE

from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
    only_on_variant,
)
from esphome.components.esp32.const import VARIANT_ESP32P4

CODEOWNERS = ["@youkorr"]
DEPENDENCIES = ["esp32", "network"]
# So webrtc.cpp can include the mic/speaker headers for the fdaudio audio bridge.
AUTO_LOAD = ["microphone", "speaker"]

CONF_ROOM_ID = "room_id"
CONF_ROLE = "role"
CONF_SIGNALING_URL = "signaling_url"
CONF_SIGNALING_TOKEN = "signaling_token"
CONF_VIDEO_CODEC = "video_codec"
CONF_AUDIO_CODEC = "audio_codec"
CONF_VIDEO_DIRECTION = "video_direction"
CONF_AUDIO_DIRECTION = "audio_direction"
CONF_VIDEO_WIDTH = "video_width"
CONF_VIDEO_HEIGHT = "video_height"
CONF_FPS = "fps"
CONF_JPEG_QUALITY = "jpeg_quality"
CONF_VIDEO_BITRATE = "video_bitrate"
CONF_ENABLE_DATA_CHANNEL = "enable_data_channel"
CONF_AUTO_START = "auto_start"
CONF_ICE_SERVERS = "ice_servers"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_MICROPHONE_ID = "microphone_id"
CONF_SPEAKER_ID = "speaker_id"
CONF_CAMERA_ID = "camera_id"
CONF_DRIVE_CAMERA = "drive_camera"
CONF_VIDEO = "video"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_ON_PAIRED = "on_paired"
CONF_ON_CONNECT_FAILED = "on_connect_failed"

webrtc_ns = cg.esphome_ns.namespace("webrtc")
WebRTCComponent = webrtc_ns.class_("WebRTCComponent", cg.Component)

# The camera (from the esp_cam_sensor external component). Declared by C++ type
# name so we can use_id() it without importing that package's Python module;
# webrtc shares its RGB565 frames (LVGL preview keeps working) and H.264-encodes
# them for the call. Optional: video only runs when camera_id is set.
esp_cam_sensor_ns = cg.esphome_ns.namespace("esp_cam_sensor")
MipiDSICamComponent = esp_cam_sensor_ns.class_("MipiDSICamComponent", cg.Component)

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

PeerRole = webrtc_ns.enum("PeerRole")
PEER_ROLE = {
    "auto": PeerRole.ROLE_AUTO,      # role from AppRTC join order (P4<->browser)
    "caller": PeerRole.ROLE_CALLER,  # always the offerer (pin one P4 to this)
    "callee": PeerRole.ROLE_CALLEE,  # always the answerer (pin the other P4)
}

ICE_SERVER_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_URL): cv.string,
        cv.Optional(CONF_USERNAME): cv.string,
        cv.Optional(CONF_PASSWORD): cv.string,
    }
)


def _validate_bridge(config):
    # The audio bridge needs BOTH the mic and the speaker (full-duplex).
    if (CONF_MICROPHONE_ID in config) != (CONF_SPEAKER_ID in config):
        raise cv.Invalid(
            "microphone_id and speaker_id must be set together "
            "(the fdaudio audio bridge needs both)"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebRTCComponent),
            cv.Optional(CONF_ROOM_ID, default="esphome_room"): cv.string,
            cv.Optional(CONF_ROLE, default="auto"): cv.enum(PEER_ROLE, lower=True),
            # Self-hosted signaling (signaling-server/). When set, the P4 talks to
            # your own server (HTTP + token) instead of webrtc.espressif.com.
            cv.Optional(CONF_SIGNALING_URL, default=""): cv.string,
            cv.Optional(CONF_SIGNALING_TOKEN, default=""): cv.string,
            cv.Optional(CONF_VIDEO_CODEC, default="h264"): cv.enum(
                VIDEO_CODEC, lower=True
            ),
            cv.Optional(CONF_AUDIO_CODEC, default="g711a"): cv.enum(
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
            # MJPEG encode quality (P4<->P4 sends JPEG over the SCTP data channel).
            # Lower = smaller frames. Big frames (~30 KB) overflow the data channel
            # and crash the link; keep this low enough that frames stay well under
            # ~24 KB at your chosen resolution/fps (~40 works for 640x480).
            cv.Optional(CONF_JPEG_QUALITY, default=40): cv.int_range(min=10, max=90),
            # H.264 target bitrate in bits/s. 0 = auto (w*h*fps/6). Raise for more
            # quality/débit until the ESP-Hosted C6 link chokes (SCTP "No buffer
            # for TSN" / resets), then back off. e.g. 600000 = 600 kbps.
            cv.Optional(CONF_VIDEO_BITRATE, default=0): cv.int_range(min=0, max=4000000),
            cv.Optional(CONF_ENABLE_DATA_CHANNEL, default=True): cv.boolean,
            cv.Optional(CONF_AUTO_START, default=False): cv.boolean,
            # fdaudio audio bridge: share fdaudio's mic + speaker (via the ESPHome
            # microphone + speaker platforms) so webrtc coexists with fdaudio +
            # voice_assistant in one firmware. Set BOTH. PCM is mono 16-bit at
            # `sample_rate`; the bridge decimates it to G.711's 8 kHz.
            cv.Optional(CONF_MICROPHONE_ID): cv.use_id(microphone.Microphone),
            cv.Optional(CONF_SPEAKER_ID): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(
                min=8000, max=48000
            ),
            # Video: share the esp_cam_sensor camera's RGB565 frames, PPA->YUV420,
            # H.264-encode (esp_h264 HW on P4), send to the peer. Only active when
            # camera_id is set. The camera keeps streaming for LVGL in parallel.
            cv.Optional(CONF_CAMERA_ID): cv.use_id(MipiDSICamComponent),
            # true (default): webrtc drives the camera capture (sole consumer,
            # one-canvas UI). Set false when lvgl_camera_display also uses the
            # camera (self-view) so only one task does the V4L2 dequeue.
            cv.Optional(CONF_DRIVE_CAMERA, default=True): cv.boolean,
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
    _validate_bridge,
    only_on_variant(supported=[VARIANT_ESP32P4]),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_room_id(config[CONF_ROOM_ID]))
    cg.add(var.set_role(config[CONF_ROLE]))
    cg.add(var.set_signaling_url(config[CONF_SIGNALING_URL]))
    cg.add(var.set_signaling_token(config[CONF_SIGNALING_TOKEN]))
    cg.add(var.set_video_codec(config[CONF_VIDEO_CODEC]))
    cg.add(var.set_audio_codec(config[CONF_AUDIO_CODEC]))
    cg.add(var.set_video_direction(config[CONF_VIDEO_DIRECTION]))
    cg.add(var.set_audio_direction(config[CONF_AUDIO_DIRECTION]))
    cg.add(
        var.set_video_resolution(
            config[CONF_VIDEO_WIDTH], config[CONF_VIDEO_HEIGHT], config[CONF_FPS]
        )
    )
    cg.add(var.set_jpeg_quality(config[CONF_JPEG_QUALITY]))
    cg.add(var.set_video_bitrate(config[CONF_VIDEO_BITRATE]))
    cg.add(var.set_enable_data_channel(config[CONF_ENABLE_DATA_CHANNEL]))
    cg.add(var.set_auto_start(config[CONF_AUTO_START]))

    if CONF_MICROPHONE_ID in config and CONF_SPEAKER_ID in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE_ID])
        spk = await cg.get_variable(config[CONF_SPEAKER_ID])
        cg.add(var.set_microphone(mic))
        cg.add(var.set_speaker(spk))
        cg.add(var.set_audio_sample_rate(config[CONF_SAMPLE_RATE]))
        # Opus codec (esp_audio_codec) for higher-quality audio than G.711.
        # Compiled in whenever the audio bridge is used; selected at runtime by
        # audio_codec: opus (G.711 stays the default). esp_peer only transports,
        # so the actual Opus encode/decode lives in webrtc.cpp.
        add_idf_component(name="espressif/esp_audio_codec", ref=">=2.0.0")
        cg.add_define("USE_ESP_WEBRTC_OPUS")

    if CONF_CAMERA_ID in config:
        cam = await cg.get_variable(config[CONF_CAMERA_ID])
        cg.add(var.set_camera(cam))
        cg.add(var.set_drive_camera(config[CONF_DRIVE_CAMERA]))
        # P4 hardware H.264 encoder. 1.1.x adds ESP_H264_RAW_FMT_RGB565_LE (the
        # P4 HW encoder accepts RGB565 directly); 1.0.4's enum lacked it.
        add_idf_component(name="espressif/esp_h264", ref="~1.1")
        # Gates the video path (esp_h264 + PPA + JPEG includes) in webrtc.cpp.
        cg.add_define("USE_ESP_WEBRTC_VIDEO")
        # P4 hardware Motion-JPEG codec (encode + decode). Used for ESP<->ESP
        # video ("video_codec: mjpeg"), since the P4 has a HW JPEG decoder but
        # NO HW H.264 decoder. Built into ESP-IDF (esp_driver_jpeg).
        include_builtin_idf_component("esp_driver_jpeg")

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
    add_idf_component(name="espressif/esp_websocket_client", ref="~1.4")
    include_builtin_idf_component("json")
    include_builtin_idf_component("esp_http_client")

    # === mbedTLS for WebRTC DTLS-SRTP ===
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_PROTO_DTLS", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_DTLS_SRTP", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CREATE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC", True)
    # The ESP32 is the DTLS SERVER; ESPHome may trim mbedTLS to client-only.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_PROTO_TLS1_2", True)
    # WebRTC DTLS uses ECDSA secp256r1 cert + ECDHE-ECDSA-AES-GCM. -0x7080
    # (FEATURE_UNAVAILABLE) means these are trimmed. NEEDS A CLEAN BUILD.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECDH_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECDSA_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_GCM_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)

    # === PSRAM / lwIP ===
    add_idf_sdkconfig_option("CONFIG_SPIRAM", True)
    add_idf_sdkconfig_option("CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP", True)
    # 16384 (NOT 256): must match h264_hp's value. h264_hp enables
    # CONFIG_SPIRAM_USE_MALLOC (edge264 needs malloc to reach PSRAM); with that
    # active, a 256-byte threshold pushes EVERY plain malloc >256 B into PSRAM —
    # including esp_driver_jpeg's internal structs/DMA descriptors. The 2D-DMA
    # engine then reads descriptors from contended PSRAM (camera DMA + WiFi) and
    # corrupts them -> the JPEG encoder crashed on a garbage function pointer
    # full of pixel data. 16 KB keeps driver-sized allocations in internal DRAM.
    add_idf_sdkconfig_option("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL", 16384)
    add_idf_sdkconfig_option("CONFIG_LWIP_IPV6", True)
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
