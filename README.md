# ESPHome WebRTC (ESP32-P4)

Real-time **peer-to-peer audio/video calling** for ESPHome, powered by
Espressif's [ESP-WebRTC](https://github.com/espressif/esp-webrtc-solution)
stack (`esp_webrtc` / `esp_peer`) running natively on the **ESP32-P4**.

This is an ESPHome **external component** that wraps the upstream ESP-WebRTC
solution: it brings up the camera + audio capture pipeline, the LCD + speaker
renderer, and the WebRTC peer connection, and exposes a small YAML API
(`webrtc:` + actions/triggers) to start/stop calls and react to call events.

> ⚠️ **Status:** functional scaffold built against the verified upstream API
> (esp_webrtc `0.9.1`, esp_peer `1.5.0`). It targets the ESP32-P4 + ESP-IDF and
> must be **validated on real hardware** — the media bring-up (`media_sys.c`)
> depends on the `codec_board` pinmap for your specific board. See
> [Hardware & build](#hardware--build).

---

## Features

- **Peer-to-peer WebRTC** media (RTP/SRTP) + data channel (SCTP)
- **Video** H.264 (hardware) or MJPEG; **audio** OPUS or G.711 (A/µ-law)
- **Signaling**: AppRTC (Espressif's public bridge), WHIP, or Janus
- **YAML actions**: `webrtc.start`, `webrtc.stop`, `webrtc.send_data`
- **YAML triggers**: `on_connected`, `on_disconnected`, `on_paired`,
  `on_connect_failed`
- Optional STUN/TURN ICE servers

> ⚠️ **Hardware:** ESP32-P4 only (hardware H.264/MJPEG + the camera/codec
> pipeline upstream relies on), ESP-IDF framework only. The P4 has no native
> Wi-Fi — use an ESP-Hosted co-processor (e.g. ESP32-C6) or Ethernet.

---

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/youkorr/esphome-webrtc
      ref: main
    components: [webrtc]
    refresh: always
```

The component automatically declares its ESP Component Registry dependencies
(`espressif/esp_webrtc`, `espressif/esp_h264`, `espressif/esp_video`,
`espressif/codec_board`) and the required `sdkconfig` options (DTLS-SRTP, PSRAM,
ISP pipeline, …) — you do not add those by hand.

---

## Configuration

```yaml
webrtc:
  id: rtc
  signaling: apprtc            # apprtc | whip | janus
  room_id: "my_esphome_room"   # required for apprtc (both peers use the same)
  # signal_url: "https://..."  # required for whip / janus instead of room_id
  board: "ESP32_P4_DEV_V14"    # codec_board pinmap name for your hardware
  video_codec: h264            # h264 | mjpeg | none
  audio_codec: opus            # opus | g711a | g711u | none
  video_direction: sendrecv    # sendrecv | send_only | recv_only | none
  audio_direction: sendrecv
  enable_data_channel: true
  auto_start: false            # start a call automatically on boot
  ice_servers:
    - url: "stun:stun.l.google.com:19302"
  on_connected:
    - logger.log: "Call connected"
  on_disconnected:
    - logger.log: "Call ended"
```

### Options

| Option | Type | Default | Description |
|---|---|---|---|
| `signaling` | enum | `apprtc` | Signaling backend: `apprtc`, `whip`, `janus`. |
| `room_id` | string | — | Room to join (AppRTC). **Required** for `apprtc`. |
| `signal_url` | string | — | Signaling endpoint URL. **Required** for `whip`/`janus`. |
| `board` | string | `ESP32_P4_DEV_V14` | `codec_board` board name (camera/codec/LCD pinmap). |
| `video_codec` | enum | `h264` | `h264`, `mjpeg`, or `none`. |
| `audio_codec` | enum | `opus` | `opus`, `g711a`, `g711u`, or `none`. |
| `video_direction` | enum | `sendrecv` | `sendrecv`, `send_only`, `recv_only`, `none`. |
| `audio_direction` | enum | `sendrecv` | direction for audio. |
| `video_bitrate` | uint | `0` | Target video bitrate in bps (`0` = codec default). |
| `audio_bitrate` | uint | `0` | Target audio bitrate in bps (`0` = codec default). |
| `enable_data_channel` | bool | `true` | Enable the SCTP data channel (for `webrtc.send_data`). |
| `auto_start` | bool | `false` | Begin a call on boot. |
| `ice_servers` | list | — | STUN/TURN servers (`url`, optional `username`/`password`). |

---

## Actions

```yaml
button:
  - platform: template
    name: "Call"
    on_press:
      - webrtc.start: rtc
  - platform: template
    name: "Hang up"
    on_press:
      - webrtc.stop: rtc
  - platform: template
    name: "Ping peer"
    on_press:
      - webrtc.send_data:
          id: rtc
          data: "hello from ESPHome"
```

| Action | Parameters | Description |
|---|---|---|
| `webrtc.start` | — | Open the peer connection and join / start the call. |
| `webrtc.stop` | — | Stop the current call. |
| `webrtc.send_data` | `data` (templatable string) | Send a message over the data channel. |

## Triggers

| Trigger | Fires when |
|---|---|
| `on_connected` | The WebRTC media connection is established. |
| `on_paired` | The two peers have been paired via signaling. |
| `on_disconnected` | The call ended / the peer disconnected. |
| `on_connect_failed` | The connection attempt failed. |

---

## How it works

1. **Media bring-up** (`media_sys.c`, adapted from upstream `videocall_demo`):
   selects the board via `codec_board`, registers the H.264/MJPEG/OPUS/G.711
   encoders & decoders, builds the **capture** system (camera over V4L2 +
   microphone) and the **player** (LCD + I2S speaker).
2. **Peer connection**: `esp_webrtc_open()` is configured with the chosen
   signaling impl (`esp_signaling_get_apprtc_impl()` / whip / janus), the
   default peer impl, codecs and directions, then the capture/player are
   attached via `esp_webrtc_set_media_provider()`.
3. **Calling**: `webrtc.start` calls `esp_webrtc_start()`; the component polls
   `esp_webrtc_query()` ~1 Hz to drive the state machine and forwards events to
   your triggers.

---

## Hardware & build

- **ESP32-P4** with **PSRAM** (required for media buffers).
- A **camera** supported by `esp_video` (e.g. SC2336 over MIPI-CSI) and an
  audio codec (e.g. ES8311 + ES7210) — described by a `codec_board` entry whose
  name you pass as `board:`.
- **Networking**: the P4 has no Wi-Fi radio; use ESP-Hosted (ESP32-C6) or
  Ethernet.
- **ESP-IDF** framework (the component refuses to build on Arduino / non-P4).

If your board isn't a known `codec_board` type, you'll need to add its pinmap
to the `codec_board` component (camera pins, I2C bus, codec record/playback
handles, LCD handle) — that's the main porting step for new hardware.

---

## License

The ESPHome integration code in this repo is **MIT** — see [`LICENSE`](LICENSE).

**Important:** the underlying ESP-WebRTC stack it pulls in (`esp_peer`,
`esp_webrtc`, `av_render`) is distributed under the **Espressif Modified MIT
License** (`LicenseRef-Espressif-Modified-MIT`), which restricts use to
**Espressif hardware only**, and `esp_peer` ships partly as a **precompiled
binary**. `media_sys.c` is derived from Espressif's demo and keeps those terms.

You can freely use and redistribute this component **for ESP32-P4 targets**, but
you cannot relicense the Espressif parts as plain MIT or use them on
non-Espressif silicon. See [`LICENSE`](LICENSE) for details. *(Not legal advice.)*

## Credits

- [Espressif ESP-WebRTC](https://github.com/espressif/esp-webrtc-solution) —
  the WebRTC stack, media pipeline, and reference demos this component wraps.
- [ESPHome](https://esphome.io) — the firmware framework.
