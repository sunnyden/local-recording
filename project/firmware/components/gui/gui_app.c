#include "gui.h"
#include "board.h"
#include "recorder.h"
#include "rolling_audio.h"
#include "lan_server.h"
#include "cloud_sync.h"
#include "processing_outbox.h"
#include "voice_client.h"
#include "recorder_provisioning.h"
#include "recorder_network.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
#include "gui_test_input.h"
#endif
#include <stdio.h>
#include <string.h>

static gui_model_t model;
static gui_secret_t secret;
static gui_dirty_t dirty;
static bool display_error, setup_cancel, stop_requested, sensitive_panel, scrub_pending;
static int64_t setup_deadline;
static unsigned catalog_index;
_Static_assert(sizeof(model)+sizeof(secret)+sizeof(dirty)<2048,"GUI state exceeds allowance");
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
static void demo(unsigned id);
#endif
static void full(void) { gui_invalidate(&dirty,(gui_rect_t){0,0,320,240}); }
static void paint(void)
{
    if(display_error) {
        if(scrub_pending&&board_display_scrub()==ESP_OK) {
            scrub_pending=false;
            sensitive_panel=false;
        }
        return;
    }
    esp_err_t status=board_display_poll();
    if(status==ESP_ERR_TIMEOUT) {display_error=true;return;}
    gui_rect_t r;
    uint16_t *pixels=board_display_strip();
    if(!pixels||!gui_next_strip(&dirty,&r))return;
    gui_canvas_t canvas={pixels,r};
    gui_render(&canvas,&model,&secret);
    if(board_display_submit((unsigned)r.x,(unsigned)r.y,(unsigned)r.w,(unsigned)r.h)!=ESP_OK)
        display_error=true;
    else gui_strip_done(&dirty,r);
}
/* Setup is exceptional: presentation must finish before BLE can advertise.
   Poll Back between strips, and never recycle DMA storage after a timeout. */
static bool present_setup(bool cancellable)
{
    int64_t end=esp_timer_get_time()+2000000;
    while(!display_error&&(dirty.dirty||board_display_poll()!=ESP_OK)) {
        board_key_t key=KEY_NONE;
        if(cancellable&&board_key_read(&key)==ESP_OK&&key==KEY_BACK) {
            setup_cancel=true;return false;
        }
        paint();
        if(esp_timer_get_time()>=end){display_error=true;return false;}
        vTaskDelay(1);
    }
    return !display_error;
}
static void setup_clear(void *ctx)
{
    (void)ctx;
    gui_secret_clear(&secret);setup_deadline=0;
    model.active=false;model.expires=0;model.screen=GUI_HOME;
    full();
    if(present_setup(false)) sensitive_panel=false;
    else {
        (void)board_display_hide();
        scrub_pending=true;
        if(board_display_scrub()==ESP_OK) {
            scrub_pending=false;
            sensitive_panel=false;
        }
    }
}
static esp_err_t setup_show(void *ctx,const char *service,const char *user,const char *password,unsigned seconds)
{
    (void)ctx;
    if(!board_display_available()||display_error)return ESP_ERR_INVALID_STATE;
    if(!gui_secret_set(&secret,service,user,password))return ESP_ERR_INVALID_SIZE;
    model.screen=GUI_SETUP;model.error=false;model.active=true;model.expires=seconds;
    sensitive_panel=true;
    setup_deadline=esp_timer_get_time()+(int64_t)seconds*1000000;
    setup_cancel=false;full();
    if(!present_setup(true)){
        setup_clear(NULL);
        return setup_cancel?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
static void error_message(esp_err_t err)
{
    model.error=true;model.active=false;model.level=0;
    snprintf(model.title,sizeof(model.title),"Operation unavailable");
    snprintf(model.detail,sizeof(model.detail),"%s",esp_err_to_name(err));
    full();
}
static void catalog_load(void)
{
    size_t count=0;
    char name[65];
    esp_err_t err=storage_catalog(catalog_index,name,sizeof(name),&count);
    model.count=(unsigned)count;
    if(count&&catalog_index>=count)catalog_index=(unsigned)count-1;
    model.page_start=catalog_index/3*3;model.page_selected=catalog_index%3;
    memset(model.names,0,sizeof(model.names));memset(model.receipts,0,sizeof(model.receipts));
    if(err!=ESP_OK&&err!=ESP_ERR_NOT_FOUND){error_message(err);return;}
    for(unsigned i=0;i<3&&model.page_start+i<count;++i) {
        if(storage_catalog(model.page_start+i,model.names[i],65,&count)!=ESP_OK)break;
        processing_job_t job;
        const char *label="Not synced";
        if(processing_outbox_load(model.names[i],&job)==ESP_OK) {
            switch(job.state) {
            case PROCESS_COMPLETED:label="Transcript ready";break;
            case PROCESS_PENDING:label="Processing";break;
            case PROCESS_TOO_LONG:label="Audio only";break;
            case PROCESS_REMOTE_MISSING:label="Cloud file removed";break;
            }
        } else if(strstr(model.names[i],"UNTIMED"))label="Time unavailable";
        snprintf(model.receipts[i],sizeof(model.receipts[i]),"%s",label);
    }
    full();
}
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
static bool busy(local_status_t l,sync_status_t s,voice_status_t v)
{return l.mode!=LOCAL_IDLE||s.active||v.active||recorder_setup_active();}
#endif
static void key_route(board_key_t key,local_status_t l,sync_status_t s,voice_status_t v,int sd,int audio)
{
    if(key==KEY_NONE)return;
    if(lan_server_pause()!=ESP_OK){error_message(ESP_ERR_TIMEOUT);return;}
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
    if(model.demo) {
        if(model.calibration&&key!=KEY_BACK)return;
        if(key==KEY_BACK||((model.screen==GUI_RECORDING||model.screen==GUI_PLAYBACK)&&key==KEY_ENTER)) {
            demo(0);
        } else if(model.screen==GUI_HOME&&(key==KEY_UP||key==KEY_DOWN))
            model.selected=gui_move(model.selected,5,key==KEY_UP);
        else if(model.screen==GUI_HOME&&key==KEY_ENTER) {
            static const unsigned demos[]={1,2,4,5,7};
            demo(demos[model.selected]);
        } else if(model.screen==GUI_RECORDINGS&&(key==KEY_UP||key==KEY_DOWN))
            model.page_selected=gui_move(model.page_selected,3,key==KEY_UP);
        else if(model.screen==GUI_RECORDINGS&&key==KEY_ENTER)model.screen=GUI_PLAYBACK;
        full();return;
    }
#endif
    if(v.active) {if(key==KEY_BACK){voice_client_stop();stop_requested=true;}return;}
    if(s.active) {if(key==KEY_BACK){cloud_sync_cancel();stop_requested=true;}return;}
    if(recorder_setup_active()) {if(key==KEY_BACK)recorder_setup_stop();return;}
    if(l.mode!=LOCAL_IDLE) {
        if(key==KEY_BACK||key==KEY_ENTER){local_stop();stop_requested=true;}
        return;
    }
    if(key==KEY_BACK){
        gui_secret_clear(&secret);model.screen=GUI_HOME;model.error=false;full();return;
    }
    if(model.error&&key!=KEY_ENTER)return;
    if(key==KEY_UP||key==KEY_DOWN) {
        if(model.screen==GUI_RECORDINGS) {
            catalog_index=gui_move(catalog_index,model.count,key==KEY_UP);
            if(catalog_index/3*3!=model.page_start)catalog_load();
            else {model.page_selected=catalog_index%3;full();}
        } else if(model.screen==GUI_HOME){model.selected=gui_move(model.selected,5,key==KEY_UP);full();}
        return;
    }
    if(key!=KEY_ENTER)return;
    model.error=false;stop_requested=false;
    esp_err_t err=ESP_OK;
    if(model.screen==GUI_RECORDINGS) {
        rolling_audio_set_enabled(false);
        if(model.count&&model.names[model.page_selected][0])
            err=local_play_start(model.names[model.page_selected]);
        else err=ESP_ERR_NOT_FOUND;
    } else if(model.screen==GUI_HOME) {
        model.note[0]=0;
        switch(model.selected) {
        case 0:
            if(sd==ESP_OK&&audio==ESP_OK) {
                rolling_snapshot_t snapshot;
                err=rolling_audio_take(&snapshot);
                if(err==ESP_OK)err=local_record_start_with_preroll(&snapshot);
                else if(err==ESP_ERR_NO_MEM)err=local_record_start();
                rolling_snapshot_release(&snapshot);
            } else err=ESP_ERR_INVALID_STATE;
            break;
        case 1:model.screen=GUI_RECORDINGS;catalog_index=0;catalog_load();break;
        case 2:rolling_audio_set_enabled(false);err=sd==ESP_OK?cloud_sync_start():ESP_ERR_INVALID_STATE;break;
        case 3:
            if(audio==ESP_OK) {
                rolling_snapshot_t snapshot;
                err=rolling_audio_take(&snapshot);
                if(err==ESP_OK)err=voice_client_start_with_context(&snapshot);
                rolling_snapshot_release(&snapshot);
            } else err=ESP_ERR_INVALID_STATE;
            break;
        case 4:rolling_audio_set_enabled(false);err=recorder_setup_start();break;
        }
    } else {model.screen=GUI_HOME;full();}
    if(err!=ESP_OK)error_message(err);
}
static const char *voice_label(const char *state)
{
    if(!state)return "Stopped";
    if(strstr(state,"CONNECT"))return "Connecting";
    if(strstr(state,"LISTEN"))return "Listening";
    if(strstr(state,"SPEAK"))return "Speaking";
    if(strstr(state,"STOP"))return "Stopping";
    if(strstr(state,"ERROR"))return "Error";
    return "Stopped";
}
static void update(local_status_t l,sync_status_t s,voice_status_t v,int64_t now)
{
    gui_model_t before=model;
    model.online=recorder_network_ready();model.time_ok=recorder_time_valid();
    bool was_active=model.active;
    rolling_status_t rolling=rolling_audio_status();
    model.rolling_active=rolling.active;
    model.rolling_seconds=rolling.samples/PCM_RATE;
    uint32_t old_generation=model.generation;
    model.pressure=heap_caps_get_free_size(MALLOC_CAP_INTERNAL)<48000||
        l.overruns>0||l.queue_peak>8;
    if(v.active) {
        model.screen=GUI_AI;model.active=true;model.generation=v.generation;
        const char *label=stop_requested?"Stopping":voice_label(v.state);
        snprintf(model.title,sizeof(model.title),"%s",label);
        const char *detail=!strcmp(label,"Listening")?"Speak naturally. You can interrupt.":
            !strcmp(label,"Speaking")?"Playing the assistant response.":
            !strcmp(label,"Connecting")?"Opening the voice connection.":"Ending the conversation safely.";
        snprintf(model.detail,sizeof(model.detail),"%s",detail);
        unsigned phase=(unsigned)(now/100000)%12;
        uint8_t connecting=(uint8_t)(24+(phase<6?phase:11-phase)*28);
        uint8_t level=!strcmp(label,"Listening")?v.microphone_level:
            !strcmp(label,"Speaking")&&v.playback_active?v.speaker_level:
            !strcmp(label,"Connecting")&&!model.pressure?connecting:0;
        model.level=old_generation==v.generation&&!stop_requested?level:0;
    } else if(s.active) {
        model.screen=GUI_SYNC;model.active=true;model.level=0;model.count=s.files_done;
        snprintf(model.title,sizeof(model.title),"%s",stop_requested?"Cancelling safely":"Sync in progress");
        if(s.phase==SYNC_UPLOADING&&!s.total_bytes)
            snprintf(model.detail,sizeof(model.detail),"%s",cloud_sync_phase_name(s.phase));
        else cloud_sync_format_status(s,model.detail,sizeof(model.detail));
        model.total_seconds=0;model.progress=0;
        uint32_t done=0,total=0;
        if(s.phase==SYNC_UPLOADING){done=s.confirmed_bytes;total=s.total_bytes;}
        else if(s.processing_bytes_known){done=s.processing_bytes;total=s.processing_total;}
        if(total){model.total_seconds=total;model.progress=(unsigned)((uint64_t)done*100/total);if(model.progress>100)model.progress=100;}
        if(s.processing_pending&&(s.processing_result!=PROCESS_OK||!model.note[0]))
            snprintf(model.note,sizeof(model.note),"Pending: %s",s.processing_result==PROCESS_OK?
                "transcript confirmation required":cloud_sync_result_name(s.processing_result));
        else if(s.processing_missing)snprintf(model.note,sizeof(model.note),"Remote deleted / local copies retained");
    } else if(recorder_setup_active()) {
        model.screen=GUI_SETUP;model.active=true;model.level=0;
        model.expires=setup_deadline>now?(unsigned)((setup_deadline-now+999999)/1000000):0;
    } else if(l.mode!=LOCAL_IDLE) {
        if(l.mode==LOCAL_RECORD)model.screen=GUI_RECORDING;
        else if(l.mode==LOCAL_PLAY)model.screen=GUI_PLAYBACK;
        model.active=true;model.generation=l.generation;
        model.seconds=l.samples/PCM_RATE;model.total_seconds=l.total_samples/PCM_RATE;
        model.pre_roll_seconds=l.pre_roll_samples/PCM_RATE;
        model.progress=l.total_samples?(unsigned)((uint64_t)l.samples*100/l.total_samples):0;
        if(model.progress>100)model.progress=100;
        model.level=old_generation==l.generation&&l.mode==LOCAL_RECORD&&!stop_requested?l.activity_level:0;
        snprintf(model.filename,sizeof(model.filename),"%s",l.filename);
        snprintf(model.title,sizeof(model.title),"%s",l.mode==LOCAL_STOPPING||stop_requested?"Saving safely":"Recording");
    } else if(was_active) {
        model.active=false;model.level=0;stop_requested=false;
        if(model.screen==GUI_AI) {
            snprintf(model.title,sizeof(model.title),"Conversation ended");
            snprintf(model.detail,sizeof(model.detail),"%s",v.error==ESP_OK?"Stopped / press Back":esp_err_to_name(v.error));
            model.error=v.error!=ESP_OK;
        } else if(model.screen==GUI_SYNC) {
            snprintf(model.title,sizeof(model.title),"%s",cloud_sync_summary(s));
            snprintf(model.detail,sizeof(model.detail),"%s",cloud_sync_result_name(s.processing_result));
            model.error=s.error!=ESP_OK;
        } else if(model.screen==GUI_RECORDING||model.screen==GUI_PLAYBACK) {
            model.seconds=l.samples/PCM_RATE;
            snprintf(model.title,sizeof(model.title),"%s",model.screen==GUI_RECORDING?"Saved on SD":"Playback finished");
            if(l.error!=ESP_OK)error_message(l.error);
        }
    }
#ifdef CONFIG_RECORDER_GUI_REDUCED_MOTION
    model.level=0;
#endif
    uint8_t level=before.level;
    uint32_t seconds=before.seconds,total=before.total_seconds;
    unsigned progress=before.progress,expires=before.expires;
    before.level=model.level;before.seconds=model.seconds;before.progress=model.progress;
    before.total_seconds=model.total_seconds;before.expires=model.expires;
    if(memcmp(&before,&model,sizeof(model)))full();
    else {
        if(level!=model.level)gui_invalidate(&dirty,model.screen==GUI_AI?
            (gui_rect_t){112,33,96,80}:(gui_rect_t){112,104,96,26});
        if(seconds!=model.seconds||total!=model.total_seconds||progress!=model.progress)
            gui_invalidate(&dirty,(gui_rect_t){12,60,296,80});
        if(expires!=model.expires)gui_invalidate(&dirty,(gui_rect_t){12,200,296,17});
    }
}
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
static void demo(unsigned id)
{
    unsigned selected=model.selected;
    gui_secret_clear(&secret);memset(&model,0,sizeof(model));
    model.demo=id!=9;model.selected=selected;model.sd_ok=true;model.online=true;model.time_ok=true;
    static const gui_screen_t screens[]={GUI_HOME,GUI_RECORDING,GUI_RECORDINGS,GUI_PLAYBACK,
        GUI_SYNC,GUI_AI,GUI_AI,GUI_SETUP,GUI_HOME,GUI_HOME,GUI_HOME};
    model.screen=screens[id];model.active=id!=0&&id!=2&&id!=8&&id!=9;
    if(id==10){model.calibration=true;model.active=false;full();return;}
    if(id==9){full();return;}
    model.seconds=42;model.total_seconds=117;model.progress=36;model.level=160;model.expires=582;
    model.count=3;snprintf(model.filename,sizeof(model.filename),"AudioRecording_20260908_093000.opus");
    strcpy(model.names[0],"AudioRecording_20260908_093000.opus");
    strcpy(model.names[1],"AudioRecording_20260908_091200.opus");
    strcpy(model.names[2],"AudioRecording_UNTIMED_0001.opus");
    strcpy(model.receipts[0],"OPUS / transcript saved");
    strcpy(model.receipts[1],"OPUS / upload pending");
    strcpy(model.receipts[2],"Local OPUS / clock unavailable");
    strcpy(model.title,id==6?"Speaking":id==5?"Listening":id==4?"Sync in progress":"Recording");
    strcpy(model.detail,id==4?"Saving transcript files":id==6?"Playing the assistant response.":"Speak naturally. You can interrupt.");
    if(id==4)strcpy(model.note,"Pending");
    if(id==7)gui_secret_set(&secret,"RECORDER_DEMO","USER00000000","000000000000000000000000");
    if(id==8){model.error=true;strcpy(model.title,"SD unavailable");strcpy(model.detail,"Local storage needs attention");}
    full();
}
static board_key_t console_key(local_status_t l,sync_status_t s,voice_status_t v)
{
    ui_input_event_t event;
    if(!gui_console_poll(&event))return KEY_NONE;
    switch(event.kind) {
    case UI_INPUT_PING:gui_console_reply("pong");break;
    case UI_INPUT_STATUS:
        {
        rolling_status_t rolling=rolling_audio_status();
        printf("UI_TEST {\"v\":1,\"event\":\"status\",\"screen\":\"%s\",\"mode\":%u,\"error\":%d,\"selected\":%u,\"demo\":%s,\"sensitive_setup\":%s,\"samples\":%lu,\"total_samples\":%lu,\"pre_roll_samples\":%lu,\"rolling_active\":%s,\"rolling_samples\":%lu,\"rolling_bytes\":%lu,\"rolling_error\":%d,\"heap_free\":%u,\"heap_min\":%u,\"dma_largest\":%u,\"overruns\":%lu,\"display_fault\":%s,\"gui_ram_bytes\":%u,\"asset_bytes\":%u,\"font_bytes\":%u,\"display_dma_bytes\":10240}\n",
            model.calibration?"calibration":gui_screen_name(model.screen),
            (unsigned)l.mode,(int)l.error,model.selected,model.demo?"true":"false",
            sensitive_panel||(secret.active&&!model.demo)?"true":"false",
            (unsigned long)l.samples,(unsigned long)l.total_samples,
            (unsigned long)l.pre_roll_samples,rolling.active?"true":"false",
            (unsigned long)rolling.samples,(unsigned long)rolling.bytes,
            (int)rolling.error,heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
            heap_caps_get_largest_free_block(MALLOC_CAP_DMA),(unsigned long)l.overruns,display_error?"true":"false",
            (unsigned)(sizeof(model)+sizeof(secret)+sizeof(dirty)+sizeof(setup_deadline)+sizeof(catalog_index)+
                sizeof(display_error)+sizeof(setup_cancel)+sizeof(stop_requested)+sizeof(sensitive_panel)+
                sizeof(scrub_pending)+gui_console_storage_bytes()),
            (unsigned)gui_assets_bytes(),(unsigned)gui_fonts_bytes());
        }
        break;
    case UI_INPUT_DEMO:
        if(busy(l,s,v)||sensitive_panel||(secret.active&&!model.demo))gui_console_reply("error");
        else {demo(event.demo);gui_console_reply("demo");}
        break;
    case UI_INPUT_UP:case UI_INPUT_DOWN:case UI_INPUT_ENTER:case UI_INPUT_BACK:
        gui_console_reply("key");return (board_key_t)(KEY_UP+event.kind-UI_INPUT_UP);
    default:gui_console_reply("error");break;
    }
    return KEY_NONE;
}
#endif
void gui_app_run(int sd,int audio,unsigned repaired,unsigned failed)
{
    model.sd_ok=sd==ESP_OK;model.screen=GUI_HOME;
    if(failed) snprintf(model.note,sizeof(model.note),"Recovery needs attention");
    else if(repaired) snprintf(model.note,sizeof(model.note),"Recovered %u recording%s",
                               repaired,repaired==1?"":"s");
    const recorder_setup_display_t callbacks={.show=setup_show,.clear=setup_clear};
    if(recorder_setup_set_display(&callbacks)!=ESP_OK)error_message(ESP_ERR_INVALID_STATE);
    if(!board_display_available())display_error=true;
    full();
    int64_t last_update=0;
    for(;;) {
        /* Input and cancellation always precede snapshots and bounded painting. */
        board_key_t key=KEY_NONE;
        esp_err_t key_error=board_key_read(&key);
        local_status_t l=local_status();
        sync_status_t s=cloud_sync_status();
        voice_status_t v=voice_client_status();
        bool rolling_should_run=model.screen==GUI_HOME&&l.mode==LOCAL_IDLE&&
            !s.active&&!v.active&&!recorder_setup_active()&&!model.demo&&
            !display_error&&!model.error;
        rolling_audio_set_enabled(rolling_should_run);
        lan_server_set_available(rolling_should_run&&sd==ESP_OK&&
                                 recorder_network_ready());
#ifdef CONFIG_RECORDER_GUI_TEST_INPUT
        board_key_t injected=console_key(l,s,v);
        if(key==KEY_NONE)key=injected;
#endif
        if(key_error!=ESP_OK&&!model.error)error_message(key_error);
        key_route(key,l,s,v,sd,audio);
        if(!model.demo)recorder_setup_tick();
        int64_t now=esp_timer_get_time();
        int64_t interval=model.pressure?400000:100000;
        if(!model.demo&&!dirty.dirty&&(key!=KEY_NONE||now-last_update>=interval)){
            update(local_status(),cloud_sync_status(),voice_client_status(),now);last_update=now;
        } else if(model.demo&&!dirty.dirty&&now-last_update>=100000&&
                  (model.screen==GUI_AI||model.screen==GUI_RECORDING)) {
            unsigned phase=(unsigned)(now/100000)%12;
            model.level=(uint8_t)(40+(phase<6?phase:11-phase)*36);
            gui_invalidate(&dirty,model.screen==GUI_AI?
                (gui_rect_t){112,33,96,80}:(gui_rect_t){112,104,96,26});
            last_update=now;
        }
        paint();
        vTaskDelay(1);
    }
}
