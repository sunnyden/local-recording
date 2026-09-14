#include "gui_test_input.h"
#include <string.h>
bool ui_input_feed(ui_input_parser_t *p,unsigned char ch,ui_input_event_t *e)
{
    if(ch=='\r')return false;
    if(ch!='\n') {
        if(ch<32||ch>126||p->used>=sizeof(p->line)-1)p->discard=true;
        if(!p->discard)p->line[p->used++]=(char)ch;
        return false;
    }
    p->line[p->used]=0; e->kind=UI_INPUT_INVALID;e->demo=0;
    if(!p->discard) {
        static const char *commands[]={"ui ping","ui status","ui key up","ui key down","ui key enter","ui key back"};
        for(unsigned i=0;i<6;++i)if(!strcmp(p->line,commands[i]))e->kind=(ui_input_kind_t)(UI_INPUT_PING+i);
        static const char *demos[]={"home","recording","recordings","playback","sync","ai-listening","ai-speaking","setup","error","off","calibration"};
        if(!strncmp(p->line,"ui demo ",8))
            for(unsigned i=0;i<11;++i)if(!strcmp(p->line+8,demos[i])){e->kind=UI_INPUT_DEMO;e->demo=i;}
    }
    memset(p,0,sizeof(*p));return true;
}
