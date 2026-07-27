// media_sys.c -- intentionally empty.
//
// The component was rewritten to drive esp_peer directly (see webrtc.cpp), so
// it no longer uses Espressif's esp_capture / av_render media pipeline. This
// file is kept (empty) only because it was part of the component; the media
// capture/render is now provided by youkorr's own components (esp_video +
// esp_h264 for send, the ip-camera-viewer edge264 decoder + fdaudio for recv).
