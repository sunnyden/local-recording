#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
const char *esp_err_to_name(int err);
static char console_output[768];
static int capture_printf(const char *format,...)
{
    va_list args;va_start(args,format);int n=vsnprintf(console_output,sizeof(console_output),format,args);
    va_end(args);return n;
}
#define printf capture_printf
#include "../../components/gui/gui_app.c"
#undef printf

static local_status_t local;
static sync_status_t sync_state;
static voice_status_t voice;
static unsigned record_calls,play_calls,sync_calls,voice_calls,setup_calls,stop_calls,cancel_calls,voice_stops;
static unsigned draw_calls;
static bool setup_active,display_available=true;
static int64_t clock_us=1000000;
static uint16_t pixels[2560];
static board_key_t next_key;
static ui_input_event_t next_input;
static bool has_input;
static const recorder_setup_display_t *registered;
const char *esp_err_to_name(int err){return err?"TEST_ERROR":"ESP_OK";}
local_status_t local_status(void){return local;}
sync_status_t cloud_sync_status(void){return sync_state;}
voice_status_t voice_client_status(void){return voice;}
esp_err_t local_record_start(void){++record_calls;local.mode=LOCAL_RECORD;return ESP_OK;}
esp_err_t local_record_start_with_preroll(rolling_snapshot_t *snapshot)
{assert(snapshot);++record_calls;local.mode=LOCAL_RECORD;memset(snapshot,0,sizeof(*snapshot));return ESP_OK;}
esp_err_t local_play_start(const char *name){assert(name&&name[0]);++play_calls;local.mode=LOCAL_PLAY;return ESP_OK;}
void local_stop(void){++stop_calls;}
esp_err_t cloud_sync_start(void){++sync_calls;sync_state.active=true;return ESP_OK;}
void cloud_sync_cancel(void){++cancel_calls;}
esp_err_t voice_client_start(void){++voice_calls;voice.active=true;return ESP_OK;}
esp_err_t voice_client_start_with_context(rolling_snapshot_t *snapshot)
{assert(snapshot);memset(snapshot,0,sizeof(*snapshot));++voice_calls;voice.active=true;return ESP_OK;}
void voice_client_stop(void){++voice_stops;}
esp_err_t rolling_audio_set_enabled(bool enabled){(void)enabled;return ESP_OK;}
esp_err_t rolling_audio_take(rolling_snapshot_t *snapshot)
{memset(snapshot,0,sizeof(*snapshot));return ESP_OK;}
void rolling_snapshot_release(rolling_snapshot_t *snapshot){memset(snapshot,0,sizeof(*snapshot));}
rolling_status_t rolling_audio_status(void){return (rolling_status_t){0};}
void lan_server_set_available(bool enabled){(void)enabled;}
esp_err_t lan_server_pause(void){return ESP_OK;}
bool recorder_setup_active(void){return setup_active;}
esp_err_t recorder_setup_start(void){++setup_calls;setup_active=true;return ESP_OK;}
void recorder_setup_stop(void){setup_active=false;setup_clear(NULL);}
void recorder_setup_tick(void){}
esp_err_t recorder_setup_set_display(const recorder_setup_display_t *p){registered=p;return ESP_OK;}
bool recorder_network_ready(void){return true;}
bool recorder_time_valid(void){return true;}
unsigned heap_caps_get_free_size(unsigned caps){(void)caps;return 96000;}
unsigned heap_caps_get_minimum_free_size(unsigned caps){(void)caps;return 64000;}
unsigned heap_caps_get_largest_free_block(unsigned caps){(void)caps;return 32000;}
int64_t esp_timer_get_time(void){return clock_us;}
void vTaskDelay(TickType_t t){clock_us+=(int64_t)t*1000;}
TickType_t xTaskGetTickCount(void){return (TickType_t)(clock_us/1000);}
bool board_display_available(void){return display_available;}
bool board_display_faulted(void){return false;}
esp_err_t board_display_hide(void){return ESP_OK;}
esp_err_t board_display_scrub(void){memset(pixels,0,sizeof(pixels));return ESP_OK;}
esp_err_t board_display_poll(void){return ESP_OK;}
uint16_t *board_display_strip(void){return pixels;}
esp_err_t board_display_submit(unsigned x,unsigned y,unsigned w,unsigned h)
{assert(x+w<=320&&y+h<=240&&h<=8);++draw_calls;return ESP_OK;}
esp_err_t board_key_read(board_key_t *k){*k=next_key;next_key=KEY_NONE;return ESP_OK;}
bool gui_console_poll(ui_input_event_t *e){if(!has_input)return false;*e=next_input;has_input=false;return true;}
void gui_console_reply(const char *s){assert(!strcmp(s,"key")||!strcmp(s,"demo")||!strcmp(s,"pong")||!strcmp(s,"error"));}
size_t gui_console_storage_bytes(void){return sizeof(ui_input_parser_t)+3*sizeof(TickType_t)+5*sizeof(bool);}
esp_err_t storage_catalog(size_t i,char *name,size_t capacity,size_t *count)
{*count=8;if(i>=8)return ESP_ERR_NOT_FOUND;snprintf(name,capacity,"legacy_%u.opus",(unsigned)i);return ESP_OK;}
esp_err_t processing_outbox_load(const char *name,processing_job_t *job)
{(void)name;(void)job;return ESP_ERR_NOT_FOUND;}
static void reset(void)
{
    memset(&model,0,sizeof(model));memset(&dirty,0,sizeof(dirty));gui_secret_clear(&secret);
    memset(&local,0,sizeof(local));memset(&sync_state,0,sizeof(sync_state));memset(&voice,0,sizeof(voice));
    display_error=false;setup_active=false;stop_requested=false;sensitive_panel=false;model.sd_ok=true;
}
static void key(board_key_t k){key_route(k,local,sync_state,voice,ESP_OK,ESP_OK);}
int main(void)
{
    reset();key(KEY_UP);assert(model.selected==4);key(KEY_DOWN);assert(model.selected==0);
    key(KEY_ENTER);assert(record_calls==1);
    key(KEY_ENTER);key(KEY_BACK);assert(stop_calls==2&&record_calls==1);
    reset();model.selected=1;key(KEY_ENTER);assert(model.screen==GUI_RECORDINGS&&model.count==8);
    key(KEY_DOWN);key(KEY_DOWN);key(KEY_DOWN);assert(model.page_start==3&&model.page_selected==0);
    key(KEY_UP);assert(model.page_start==0&&model.page_selected==2);
    key(KEY_ENTER);assert(play_calls==1);
    reset();model.selected=2;key(KEY_ENTER);key(KEY_BACK);assert(sync_calls==1&&cancel_calls==1);
    reset();model.selected=3;key(KEY_ENTER);key(KEY_BACK);assert(voice_calls==1&&voice_stops==1);
    reset();model.selected=4;key(KEY_ENTER);assert(setup_calls==1);key(KEY_BACK);assert(!setup_active);
    reset();voice=(voice_status_t){.active=true,.state="LISTENING",.generation=1,.microphone_level=200};
    update(local,sync_state,voice,clock_us);assert(model.screen==GUI_AI&&model.level==0);
    dirty.dirty=false;update(local,sync_state,voice,clock_us);assert(model.level==200);
    assert(dirty.rect.x==112&&dirty.rect.y==33&&dirty.rect.w==96&&dirty.rect.h==80);
    voice.state="SPEAKING";voice.speaker_level=250;voice.playback_active=false;
    update(local,sync_state,voice,clock_us);assert(model.level==0);
    voice.playback_active=true;update(local,sync_state,voice,clock_us);assert(model.level==250);
    voice.generation=2;update(local,sync_state,voice,clock_us);assert(model.level==0);
    voice.active=false;voice.error=ESP_FAIL;update(local,sync_state,voice,clock_us);
    assert(model.error&&!model.active&&model.level==0);
    update(local,sync_state,voice,clock_us);assert(model.error);
    key(KEY_BACK);assert(model.screen==GUI_HOME&&!model.error);
    reset();full();unsigned before=draw_calls;paint();assert(draw_calls==before+1&&dirty.dirty);
    reset();display_available=false;
    assert(setup_show(NULL,"RECORDER_DEMO","USER00000000","000000000000000000000000",600)!=ESP_OK);
    assert(!secret.active);display_available=true;
    assert(setup_show(NULL,"RECORDER_DEMO","USER00000000","000000000000000000000000",600)==ESP_OK);
    assert(secret.active&&model.screen==GUI_SETUP&&!dirty.dirty);
    setup_clear(NULL);assert(!secret.active&&model.screen==GUI_HOME);
    for(unsigned i=0;i<sizeof(secret);++i)assert(((unsigned char *)&secret)[i]==0);
    next_key=KEY_BACK;
    assert(setup_show(NULL,"RECORDER_DEMO","USER00000000","000000000000000000000000",600)!=ESP_OK);
    assert(!secret.active);
    reset();
    unsigned calls=record_calls+play_calls+sync_calls+voice_calls+setup_calls;
    for(unsigned i=0;i<10;++i) {
        demo(i);assert(model.demo==(i!=9));
        if(i!=9){key(KEY_ENTER);key(KEY_UP);key(KEY_DOWN);key(KEY_BACK);}
    }
    assert(calls==record_calls+play_calls+sync_calls+voice_calls+setup_calls);
    demo(10);assert(model.demo&&model.calibration&&!model.active&&!secret.active);
    key(KEY_ENTER);assert(model.calibration);
    next_input=(ui_input_event_t){UI_INPUT_STATUS,0};has_input=true;
    console_key(local,sync_state,voice);
    assert(strstr(console_output,"\"screen\":\"calibration\"")&&strstr(console_output,"\"sensitive_setup\":false"));
    key(KEY_BACK);assert(!model.calibration&&model.demo&&model.screen==GUI_HOME);
    assert(calls==record_calls+play_calls+sync_calls+voice_calls+setup_calls);
    reset();next_input=(ui_input_event_t){UI_INPUT_BACK,0};has_input=true;
    assert(console_key(local,sync_state,voice)==KEY_BACK);
    voice.active=true;next_input=(ui_input_event_t){UI_INPUT_DEMO,7};has_input=true;
    console_key(local,sync_state,voice);assert(!model.demo&&!secret.active);
    reset();assert(gui_secret_set(&secret,"SYNTHETIC_SERVICE","SYNTHETIC_USER","SYNTHETIC_PASSWORD"));
    next_input=(ui_input_event_t){UI_INPUT_STATUS,0};has_input=true;
    console_key(local,sync_state,voice);
    assert(strstr(console_output,"UI_TEST {\"v\":1,")&&strstr(console_output,"\"sensitive_setup\":true"));
    assert(!strstr(console_output,"SYNTHETIC")&&!strstr(console_output,"password")&&!strstr(console_output,"filename"));
    reset();sync_state.active=true;sync_state.phase=SYNC_UPLOADING;
    update(local,sync_state,voice,clock_us);assert(!strchr(model.detail,'%'));
    sync_state.processing_pending=1;sync_state.processing_result=PROCESS_AUTHENTICATION;
    update(local,sync_state,voice,clock_us);assert(strstr(model.note,"authentication"));
    sync_state.processing_result=PROCESS_OK;
    update(local,sync_state,voice,clock_us);assert(strstr(model.note,"authentication"));
    printf("PASS GUI controller: all modes, exclusive actions, cancellation, paging, sticky errors, private setup, stale activity, demo isolation\n");
    return 0;
}
