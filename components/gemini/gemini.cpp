#include "gemini.h"
#include "gemini_protocol.h"
#include "websocket.h"
#include "esp_log.h"
namespace { static const char *TAG="GEMINI"; static bool s_initialized=false,s_started=false;
void on_connected(void *){gemini_protocol_on_connected();}
void on_disconnected(void *){ESP_LOGW(TAG,"Gemini transport disconnected");}
void on_text(const char *d,size_t n,void *){gemini_protocol_on_text(d,n);}
void on_binary(const uint8_t *d,size_t n,void *){gemini_protocol_on_binary(d,n);}
void on_error(void *){ESP_LOGE(TAG,"Gemini transport error");}}
extern "C" bool gemini_init(const char *uri){if(!uri||!*uri)return false;gemini_protocol_init();websocket_callbacks_t cb={};cb.on_connected=on_connected;cb.on_disconnected=on_disconnected;cb.on_text=on_text;cb.on_binary=on_binary;cb.on_error=on_error;s_initialized=websocket_init(uri,&cb);return s_initialized;}
extern "C" bool gemini_start(void){if(!s_initialized)return false;s_started=websocket_start();return s_started;}
extern "C" void gemini_stop(void){websocket_stop();gemini_protocol_reset();s_started=false;}
extern "C" bool gemini_is_connected(void){return s_started&&websocket_is_connected();}
extern "C" bool gemini_send_audio(const uint8_t *d,size_t n){return s_started&&gemini_protocol_send_audio(d,n);}
extern "C" bool gemini_send_text(const char *d,size_t n){return s_started&&gemini_protocol_send_text(d,n);}
