#include "gui.h"
#include "gui_test_input.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint16_t image[320*240];
static void render(const gui_model_t *m,const gui_secret_t *s)
{
    for(int y=0;y<240;y+=8){
        gui_canvas_t c={image+y*320,{0,y,320,8}};
        gui_render(&c,m,s);
    }
}
static void ppm(unsigned n)
{
    char path[32];snprintf(path,sizeof(path),"screen-%u.ppm",n);
    FILE *f=fopen(path,"wb");assert(f);
    fprintf(f,"P6\n320 240\n255\n");
    for(unsigned i=0;i<320*240;++i){
        unsigned c=image[i];
        unsigned char rgb[]={(unsigned char)((c>>11)*255/31),
            (unsigned char)(((c>>5)&63)*255/63),(unsigned char)((c&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    fclose(f);
}
static ui_input_event_t parse(ui_input_parser_t *p,const char *s)
{
    ui_input_event_t event={0};
    for(;*s;++s)ui_input_feed(p,(unsigned char)*s,&event);
    return event;
}
int main(void)
{
    assert(gui_rgb(0xff0000)==0xf800&&gui_rgb(0x00ff00)==0x07e0&&gui_rgb(0x0000ff)==0x001f);
    gui_rect_t r={-10,-20,20,30};assert(gui_clip(&r,(gui_rect_t){0,0,320,240}));
    assert(r.x==0&&r.y==0&&r.w==10&&r.h==10);
    r=(gui_rect_t){INT_MAX,INT_MAX,INT_MAX,INT_MAX};assert(!gui_clip(&r,(gui_rect_t){0,0,320,240}));
    uint16_t guarded[34];for(unsigned i=0;i<34;++i)guarded[i]=0xface;
    gui_canvas_t c={guarded+1,{3,4,8,4}};
    gui_fill(&c,(gui_rect_t){-20,-20,500,500},123);
    assert(guarded[0]==0xface&&guarded[33]==0xface);
    for(unsigned i=1;i<33;++i)assert(guarded[i]==123);
    gui_dirty_t d={0};gui_invalidate(&d,(gui_rect_t){0,0,320,240});
    unsigned strips=0;
    while(gui_next_strip(&d,&r)){assert(r.h<=8&&r.w*r.h<=2560);gui_strip_done(&d,r);++strips;}
    assert(strips==30&&!d.dirty);
    gui_invalidate(&d,(gui_rect_t){112,33,96,80});
    strips=0;while(gui_next_strip(&d,&r)){assert(r.x==112&&r.w==96);gui_strip_done(&d,r);++strips;}assert(strips==10);
    assert(gui_move(0,5,true)==4&&gui_move(4,5,false)==0&&gui_move(0,0,true)==0);
    gui_secret_t secret;
    assert(gui_secret_set(&secret,"RECORDER_DEMO","USER00000000","000000000000000000000000"));
    assert(gui_text_width(GUI_FONT_BODY,secret.password)<=278);
    gui_secret_clear(&secret);
    for(unsigned i=0;i<sizeof(secret);++i)assert(((unsigned char *)&secret)[i]==0);
    assert(!gui_secret_set(&secret,"RECORDER_DEMO","USER00000000","0000000000000000000000000"));
    gui_secret_set(&secret,"RECORDER_DEMO","USER00000000","000000000000000000000000");
    assert(gui_assets_bytes()<=48*1024&&gui_fonts_bytes()<=24*1024);
    gui_model_t m={.sd_ok=true,.online=true,.time_ok=true,.demo=true,.active=true,.count=3,.seconds=42,
        .total_seconds=117,.progress=36,.level=165,.expires=582};
    strcpy(m.filename,"AudioRecording_20260908_093000.opus");
    strcpy(m.names[0],m.filename);strcpy(m.names[1],"AudioRecording_20260908_091200.opus");
    strcpy(m.names[2],"AudioRecording_UNTIMED_0001.opus");
    strcpy(m.receipts[0],"OPUS / transcript saved");strcpy(m.receipts[1],"OPUS / upload pending");
    strcpy(m.receipts[2],"Local OPUS / clock unavailable");
    strcpy(m.title,"Listening");strcpy(m.detail,"Speak naturally. You can interrupt.");
    for(unsigned i=0;i<7;++i) {
        m.screen=(gui_screen_t)i;
        if(i==1)strcpy(m.title,"Recording");
        if(i==4){strcpy(m.title,"Sync in progress");strcpy(m.detail,"Saving transcript files");}
        if(i==5){strcpy(m.title,"Listening");strcpy(m.detail,"Speak naturally. You can interrupt.");}
        render(&m,&secret);ppm(i);
        /* Partial repaint must produce the same pixels as full-width strips. */
        uint16_t small[96*8];
        for(int y=33;y<113;y+=8) {
            gui_canvas_t roi={small,{112,y,96,8}};gui_render(&roi,&m,&secret);
            for(int row=0;row<8;++row)assert(!memcmp(small+row*96,image+(y+row)*320+112,96*2));
        }
    }
    m.calibration=true;
    render(&m,&secret);ppm(7);
    assert(image[0]==0&&image[239*320+319]==0);
    assert(image[50*320+20]==0xf800&&image[50*320+120]==0x07e0&&image[50*320+220]==0x001f);
    ui_input_parser_t p={0};
    assert(parse(&p,"ui ping\n").kind==UI_INPUT_PING);
    assert(parse(&p,"ui key back\r\n").kind==UI_INPUT_BACK);
    assert(parse(&p,"ui demo setup\n").demo==7);
    assert(parse(&p,"ui demo calibration\n").demo==10);
    assert(parse(&p,"ui memory 0\n").kind==UI_INPUT_INVALID);
    for(unsigned i=0;i<120;++i){ui_input_event_t e;ui_input_feed(&p,'a',&e);}
    assert(parse(&p,"\n").kind==UI_INPUT_INVALID);
    assert(parse(&p,"ui status\n").kind==UI_INPUT_STATUS);
    assert(parse(&p,"ui key\x01""back\n").kind==UI_INPUT_INVALID);
    puts("PASS GUI clipping, strip budget, partial-paint pixels, assets, secrets, keys, bridge; 7 screens + calibration PPM");
    return 0;
}
