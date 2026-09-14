#include "gui_test_input.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
bool gui_console_poll(ui_input_event_t *event)
{
    static ui_input_parser_t parser;
    static bool initialized, enabled;
    static TickType_t last_command[3];
    static bool rate_started[3];
    if(!initialized) {
        int flags=fcntl(STDIN_FILENO,F_GETFL,0);
        enabled=flags>=0&&fcntl(STDIN_FILENO,F_SETFL,flags|O_NONBLOCK)>=0;
        initialized=true;
    }
    if(!enabled)return false;
    for(unsigned i=0;i<96;++i) {
        unsigned char ch;
        if(read(STDIN_FILENO,&ch,1)!=1)break;
        if(ui_input_feed(&parser,ch,event)) {
            if(event->kind>=UI_INPUT_STATUS&&event->kind<=UI_INPUT_DEMO) {
                TickType_t now=xTaskGetTickCount();
                unsigned category=event->kind==UI_INPUT_STATUS?0:event->kind==UI_INPUT_DEMO?1:2;
                if(rate_started[category]&&(TickType_t)(now-last_command[category])<pdMS_TO_TICKS(80))
                    event->kind=UI_INPUT_INVALID;
                else {last_command[category]=now;rate_started[category]=true;}
            }
            return true;
        }
    }
    return false;
}
void gui_console_reply(const char *result)
{
    if(!strcmp(result,"error"))
        printf("UI_TEST {\"v\":1,\"event\":\"error\",\"code\":\"invalid_command\"}\n");
    else printf("UI_TEST {\"v\":1,\"event\":\"%s\"}\n",result);
}
size_t gui_console_storage_bytes(void)
{
    return sizeof(ui_input_parser_t)+3*sizeof(TickType_t)+5*sizeof(bool);
}
