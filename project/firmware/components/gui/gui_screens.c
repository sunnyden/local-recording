#include "gui.h"
#include <stdio.h>
#include <string.h>

#define INK gui_rgb(0x17243a)
#define MUTED gui_rgb(0x59677c)
#define BLUE gui_rgb(0x0f6cbd)
#define PALE gui_rgb(0xeaf3ff)
#define LINE gui_rgb(0xd9e1eb)
#define PAPER gui_rgb(0xf6f8fc)
#define WHITE 0xffff
#define RED gui_rgb(0xbd2946)
#define GREEN gui_rgb(0x107c41)
static void text(gui_canvas_t *c,int x,int y,int w,const char *s)
{ gui_text(c,x,y,w,GUI_FONT_BODY,s,INK); }
static void hint(gui_canvas_t *c,int x,int y,int w,const char *s)
{ gui_text(c,x,y,w,GUI_FONT_HINT,s,MUTED); }
static void center(gui_canvas_t *c,int y,gui_font_id_t f,const char *s,uint16_t color)
{ gui_text(c,(320-gui_text_width(f,s))/2,y,308,f,s,color); }
static int icon_top(gui_icon_id_t icon,unsigned size,int top,int height)
{
    const gui_bitmap_t *bitmap=gui_asset_get(icon,size);
    int actual=bitmap?bitmap->height:(int)size;
    return top+(height-actual)/2;
}
static void time_text(char *s,size_t n,uint32_t seconds)
{ snprintf(s,n,"%02lu:%02lu",(unsigned long)(seconds/60),(unsigned long)(seconds%60)); }
static void button(gui_canvas_t *c,int y,const char *s,uint16_t color)
{
    int w=gui_text_width(GUI_FONT_BODY,s)+42,x=(320-w)/2;
    gui_fill(c,(gui_rect_t){x,y,w,30},color);
    gui_icon(c,x+8,y+7,GUI_ICON_STOP,16,WHITE);
    gui_text(c,x+30,y+7,w-34,GUI_FONT_BODY,s,WHITE);
}
static void bars(gui_canvas_t *c,int x,int y,unsigned level,uint16_t color)
{
    static const uint8_t shape[]={3,5,8,12,9,15,11,7,13,9,6,4,2};
    for(unsigned i=0;i<13;++i) {
        int h=2+(int)(shape[i]*level/255);
        gui_fill(c,(gui_rect_t){x+(int)i*6,y+(18-h)/2,3,h},color);
    }
}
static void filename_label(const char *name,char *out,size_t capacity)
{
    const char *stamp=strstr(name,"AudioRecording_");
    if(strstr(name,"UNTIMED")) snprintf(out,capacity,"Untimed recording");
    else if(stamp && strlen(stamp)>=30) {
        const char *p=stamp+15;
        snprintf(out,capacity,"%.4s-%.2s-%.2s / %.2s:%.2s",p,p+4,p+6,p+9,p+11);
    } else snprintf(out,capacity,"Legacy / %s",name);
}
static void calibration(gui_canvas_t *c)
{
    static const uint16_t colors[]={0xf800,0x07e0,0x001f,0xdefb,0x8410,0x3186};
    static const char *labels[]={"Red F800","Green 07E0","Blue 001F","Light gray","Mid gray","Dark gray"};
    gui_fill(c,c->clip,WHITE);
    gui_outline(c,(gui_rect_t){0,0,320,240},0);
    for(unsigned corner=0;corner<4;++corner) {
        int x=corner&1?307:3,y=corner&2?227:3;
        gui_fill(c,(gui_rect_t){x,y,10,2},0);
        gui_fill(c,(gui_rect_t){x,y,2,10},0);
    }
    gui_text(c,16,14,244,GUI_FONT_TITLE,"RGB565 calibration",INK);
    gui_text(c,263,17,49,GUI_FONT_HINT,"DEMO",RED);
    for(unsigned i=0;i<6;++i) {
        int x=16+(int)(i%3)*100,y=45+(int)(i/3)*80;
        gui_fill(c,(gui_rect_t){x,y,88,54},colors[i]);
        gui_text(c,x,y+57,91,GUI_FONT_HINT,labels[i],INK);
    }
    hint(c,16,216,288,"320 x 240 / 1px border / 2 Back");
}
void gui_render(gui_canvas_t *c,const gui_model_t *m,const gui_secret_t *secret)
{
    if(m->demo&&m->calibration){calibration(c);return;}
    static const char *titles[]={"Recorder","Record","Recordings","Playback","OneDrive Sync","AI Conversation","Setup"};
    static const gui_icon_id_t icons[]={GUI_ICON_MIC,GUI_ICON_MIC,GUI_ICON_EXPLORER,GUI_ICON_SPEAKER,
        GUI_ICON_ONEDRIVE,GUI_ICON_COPILOT,GUI_ICON_SETTINGS};
    char s[80];
    gui_fill(c,c->clip,PAPER);
    gui_fill(c,(gui_rect_t){0,0,320,28},WHITE);
    gui_fill(c,(gui_rect_t){0,218,320,22},WHITE);
    gui_fill(c,(gui_rect_t){0,27,320,1},LINE);
    gui_fill(c,(gui_rect_t){0,218,320,1},LINE);
    gui_icon(c,10,icon_top(icons[m->screen],20,0,28),icons[m->screen],20,BLUE);
    gui_text(c,36,5,200,GUI_FONT_TITLE,titles[m->screen],INK);
    if(m->demo) gui_text(c,m->screen==GUI_SETUP?158:263,8,55,GUI_FONT_HINT,"DEMO",RED);
    else if(m->screen==GUI_SETUP)gui_icon(c,220,5,GUI_ICON_MICROSOFT,88,INK);
    else {
        gui_icon(c,291,6,m->online?GUI_ICON_WIFI:GUI_ICON_WIFI_OFF,16,MUTED);
        gui_icon(c,263,6,m->sd_ok?GUI_ICON_CHECK:GUI_ICON_WARNING,16,
                 m->sd_ok?GREEN:RED);
    }
    const char *footer="3 / 1 Move      0 Open";
    switch(m->screen) {
    case GUI_HOME: {
        static const char *labels[]={"Record","Recordings","Sync","AI chat","Setup"};
        static const gui_icon_id_t tile[]={GUI_ICON_MIC_COLOR,GUI_ICON_EXPLORER,GUI_ICON_ONEDRIVE,GUI_ICON_COPILOT,GUI_ICON_SETTINGS};
        for(unsigned i=0;i<5;++i) {
            int x=12+(int)(i%2)*152,y=38+(int)(i/2)*59;
            gui_rect_t r={x,y,144,52};
            gui_fill(c,r,m->selected==i?PALE:WHITE);
            gui_outline(c,r,m->selected==i?BLUE:LINE);
            if(m->selected==i)gui_outline(c,(gui_rect_t){x+1,y+1,142,50},BLUE);
            gui_icon(c,x+8,icon_top(tile[i],32,y,52),tile[i],32,BLUE);
            text(c,x+44,y+18,96,labels[i]);
        }
        gui_rect_t status={164,156,144,52};
        gui_fill(c,status,WHITE);
        gui_outline(c,status,m->sd_ok?GREEN:RED);
        gui_icon(c,172,icon_top(m->sd_ok?GUI_ICON_CHECK:GUI_ICON_WARNING,24,
                 status.y,status.h),m->sd_ok?GUI_ICON_CHECK:GUI_ICON_WARNING,24,
                 m->sd_ok?GREEN:RED);
        gui_text(c,208,status.y+17,92,GUI_FONT_TITLE,m->sd_ok?"Ready":"Insert SD",
                 m->sd_ok?GREEN:RED);
        if(m->note[0]) {
            gui_fill(c,(gui_rect_t){12,204,296,14},PAPER);
            hint(c,12,204,296,m->note);
        }
        break;
    }
    case GUI_RECORDING:
        center(c,37,GUI_FONT_BODY,m->title,RED);
        time_text(s,sizeof(s),m->seconds);
        center(c,61,GUI_FONT_TIMER,s,INK);
        bars(c,123,108,m->active?m->level:0,RED);
        button(c,153,m->active?"Save & stop":"Saved on SD",RED);
        hint(c,12,193,296,m->filename);
        footer="0 / 2 Save & stop";
        break;
    case GUI_RECORDINGS:
        snprintf(s,sizeof(s),"%u of %u",m->count?m->page_start+m->page_selected+1:0,m->count);
        hint(c,12,33,200,"On this device"); hint(c,254,33,60,s);
        if(!m->count)text(c,22,96,276,m->sd_ok?"No recordings yet":"SD unavailable");
        for(unsigned i=0;i<3 && m->page_start+i<m->count;++i) {
            int y=52+(int)i*47;
            if(i==m->page_selected){
                gui_fill(c,(gui_rect_t){12,y,296,45},PALE);
                gui_outline(c,(gui_rect_t){12,y,296,45},BLUE);
                gui_outline(c,(gui_rect_t){13,y+1,294,43},BLUE);
            }
            gui_icon(c,20,icon_top(GUI_ICON_DOCUMENT,20,y,45),GUI_ICON_DOCUMENT,20,BLUE);
            filename_label(m->names[i],s,sizeof(s));text(c,49,y+8,249,s);
            hint(c,49,y+24,249,m->receipts[i]);
        }
        hint(c,12,197,296,m->count?m->names[m->page_selected]:"Original local files are retained");
        footer="3 / 1 Move     0 Play     2 Back";
        break;
    case GUI_PLAYBACK:
        filename_label(m->filename,s,sizeof(s));text(c,12,41,296,s);
        hint(c,12,67,296,m->filename);
        gui_fill(c,(gui_rect_t){12,103,296,5},LINE);
        gui_fill(c,(gui_rect_t){12,103,(int)(296*m->progress/100),5},BLUE);
        time_text(s,sizeof(s),m->seconds);text(c,12,115,115,s);
        time_text(s,sizeof(s),m->total_seconds);text(c,254,115,60,s);
        button(c,147,m->active?"Stop":"Finished",BLUE);
        center(c,187,GUI_FONT_HINT,m->active?"Playing":"Stopped",MUTED);
        footer="0 / 2 Stop playback";
        break;
    case GUI_SYNC:
        text(c,12,40,296,m->title);
        snprintf(s,sizeof(s),"%u recordings",m->count);
        hint(c,12,66,296,s);
        gui_icon(c,17,93,m->error?GUI_ICON_WARNING:GUI_ICON_SYNC,20,m->error?RED:BLUE);
        text(c,48,93,258,m->detail);
        if(m->total_seconds) {
            gui_fill(c,(gui_rect_t){48,122,256,5},LINE);
            gui_fill(c,(gui_rect_t){48,122,(int)(256*m->progress/100),5},BLUE);
        }
        hint(c,12,145,296,m->note);
        footer="2 Cancel / keep pending";
        break;
    case GUI_AI:
        gui_outline(c,(gui_rect_t){130,35,60,58},m->level?BLUE:LINE);
        gui_icon(c,136,40,GUI_ICON_COPILOT,48,BLUE);
        bars(c,123,95,m->level,BLUE);
        center(c,125,GUI_FONT_TITLE,m->title,INK);
        center(c,152,GUI_FONT_HINT,m->detail,MUTED);
        gui_icon(c,140,icon_top(GUI_ICON_ONEDRIVE,16,180,17),GUI_ICON_ONEDRIVE,16,BLUE);
        gui_icon(c,164,icon_top(GUI_ICON_CHECK,16,180,17),GUI_ICON_CHECK,16,GREEN);
        footer="2 End conversation";
        break;
    case GUI_SETUP:
        text(c,12,34,296,"Connect using the companion");
        hint(c,12,55,296,"Microsoft sign-in happens in your browser.");
        if(m->demo)gui_icon(c,218,5,GUI_ICON_MICROSOFT,88,INK);
        gui_fill(c,(gui_rect_t){12,74,296,49},WHITE);
        gui_outline(c,(gui_rect_t){12,74,296,49},LINE);
        hint(c,20,77,278,"Setup username");
        text(c,20,96,278,secret&&secret->active?secret->username:"Setup inactive");
        gui_fill(c,(gui_rect_t){12,129,296,49},WHITE);
        gui_outline(c,(gui_rect_t){12,129,296,49},LINE);
        hint(c,20,132,278,"Setup password");
        text(c,20,151,278,secret&&secret->active?secret->password:"Credentials cleared");
        if(secret&&secret->active)snprintf(s,sizeof(s),"Bluetooth: %s",secret->service);
        else snprintf(s,sizeof(s),"Bluetooth setup stopped");
        hint(c,12,183,296,s);
        snprintf(s,sizeof(s),"Setup expires in %02u:%02u",m->expires/60,m->expires%60);
        hint(c,12,201,296,s);
        footer="2 Exit & clear credentials";
        break;
    }
    if(m->error) {
        gui_fill(c,(gui_rect_t){10,153,300,63},WHITE);
        gui_outline(c,(gui_rect_t){10,153,300,63},RED);
        gui_icon(c,18,161,GUI_ICON_WARNING,20,RED);
        text(c,45,159,257,m->title);
        hint(c,18,185,282,m->detail);
        footer="2 Back / acknowledge error";
    }
    hint(c,12,223,296,footer);
    if(m->demo)gui_text(c,267,223,49,GUI_FONT_HINT,"DEMO",RED);
}
