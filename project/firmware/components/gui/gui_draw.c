#include "gui.h"
#include <limits.h>
#include <string.h>

uint16_t gui_rgb(unsigned c) { return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x7e0) | ((c >> 3) & 31)); }
bool gui_clip(gui_rect_t *r, gui_rect_t b)
{
    if (r->w <= 0 || r->h <= 0 || b.w <= 0 || b.h <= 0) return false;
    int64_t right = (int64_t)r->x + r->w, bottom = (int64_t)r->y + r->h;
    int64_t br = (int64_t)b.x + b.w, bb = (int64_t)b.y + b.h;
    if (r->x < b.x) r->x = b.x;
    if (r->y < b.y) r->y = b.y;
    if (right > br) right = br;
    if (bottom > bb) bottom = bb;
    if (right <= r->x || bottom <= r->y) return false;
    r->w = (int)(right - r->x); r->h = (int)(bottom - r->y);
    return true;
}
void gui_fill(gui_canvas_t *c, gui_rect_t r, uint16_t color)
{
    if (!gui_clip(&r, c->clip)) return;
    for (int y = r.y; y < r.y + r.h; ++y)
        for (int x = r.x; x < r.x + r.w; ++x)
            c->pixels[(y - c->clip.y) * c->clip.w + x - c->clip.x] = color;
}
void gui_outline(gui_canvas_t *c, gui_rect_t r, uint16_t color)
{
    gui_fill(c, (gui_rect_t){r.x,r.y,r.w,1}, color);
    gui_fill(c, (gui_rect_t){r.x,r.y+r.h-1,r.w,1}, color);
    gui_fill(c, (gui_rect_t){r.x,r.y,1,r.h}, color);
    gui_fill(c, (gui_rect_t){r.x+r.w-1,r.y,1,r.h}, color);
}
static void blend(gui_canvas_t *c, int x, int y, uint16_t color, unsigned a)
{
    if (!a || x < c->clip.x || y < c->clip.y || x >= c->clip.x+c->clip.w || y >= c->clip.y+c->clip.h) return;
    uint16_t *p = &c->pixels[(y-c->clip.y)*c->clip.w+x-c->clip.x], bg = *p;
    unsigned r = (((color>>11)*a + (bg>>11)*(15-a)+7)/15);
    unsigned g = ((((color>>5)&63)*a + ((bg>>5)&63)*(15-a)+7)/15);
    unsigned b = (((color&31)*a + (bg&31)*(15-a)+7)/15);
    *p = (uint16_t)((r<<11)|(g<<5)|b);
}
static unsigned alpha(const uint8_t *data, unsigned i)
{
    return data ? ((data[i/2] >> ((i&1) ? 0 : 4)) & 15) : 15;
}
int gui_text_width(gui_font_id_t id, const char *text)
{
    const gui_font_t *f = gui_font_get(id);
    int n = 0;
    if (!f || !text) return 0;
    for (; *text; ++text) {
        const gui_glyph_t *g = gui_font_glyph(f, (unsigned char)*text);
        if (g && n <= INT_MAX - g->advance) n += g->advance;
    }
    return n;
}
void gui_text(gui_canvas_t *c, int x, int y, int width, gui_font_id_t id, const char *text, uint16_t color)
{
    const gui_font_t *f = gui_font_get(id);
    if (!f || !text || width <= 0) return;
    gui_rect_t old = c->clip, limit = {x,y,width,f->height+3};
    if (!gui_clip(&limit, old)) return;
    /* Keep original stride: clipping is checked per glyph pixel. */
    int edge = x + width;
    bool elide = gui_text_width(id,text) > width;
    int dots = gui_text_width(id,"...");
    for (; *text; ++text) {
        const gui_glyph_t *g = gui_font_glyph(f,(unsigned char)*text);
        if (!g) continue;
        if (x + g->advance > edge - (elide ? dots : 0)) break;
        for (unsigned j=0; j<g->height; ++j) for (unsigned i=0; i<g->width; ++i) {
            int px=x+g->offset_x+(int)i, py=y+f->baseline+g->offset_y+(int)j;
            if (px < edge && py >= y && py < y+f->height+3)
                blend(c,px,py,color,alpha(g->alpha4,j*g->width+i));
        }
        x += g->advance;
    }
    if (elide && dots <= width) gui_text(c,x,y,edge-x,id,"...",color);
}
void gui_icon(gui_canvas_t *c, int x, int y, gui_icon_id_t id, unsigned size, uint16_t tint)
{
    const gui_bitmap_t *b = gui_asset_get(id,size);
    if (!b) return;
    gui_rect_t r={x,y,b->width,b->height};
    if (!gui_clip(&r,c->clip)) return;
    for (int py=r.y;py<r.y+r.h;++py) for(int px=r.x;px<r.x+r.w;++px) {
        unsigned i=(py-y)*b->width+px-x;
        blend(c,px,py,b->rgb565?b->rgb565[i]:tint,alpha(b->alpha4,i));
    }
}
void gui_invalidate(gui_dirty_t *d, gui_rect_t r)
{
    if (!gui_clip(&r,(gui_rect_t){0,0,320,240})) return;
    if (d->dirty) {
        int x=d->rect.x<r.x?d->rect.x:r.x, y=d->next_y<r.y?d->next_y:r.y;
        int right=d->rect.x+d->rect.w>r.x+r.w?d->rect.x+d->rect.w:r.x+r.w;
        int bottom=d->rect.y+d->rect.h>r.y+r.h?d->rect.y+d->rect.h:r.y+r.h;
        r=(gui_rect_t){x,y,right-x,bottom-y};
    }
    d->rect=r; d->next_y=r.y; d->dirty=true;
}
bool gui_next_strip(const gui_dirty_t *d, gui_rect_t *r)
{
    if (!d->dirty) return false;
    *r=d->rect; r->y=d->next_y; r->h=d->rect.y+d->rect.h-d->next_y;
    if(r->h>8)r->h=8;
    return true;
}
void gui_strip_done(gui_dirty_t *d, gui_rect_t r)
{
    d->next_y=r.y+r.h;
    if(d->next_y>=d->rect.y+d->rect.h)d->dirty=false;
}
void gui_secret_clear(gui_secret_t *s)
{
    volatile unsigned char *p=(volatile unsigned char *)s;
    for(size_t i=0;i<sizeof(*s);++i)p[i]=0;
}
bool gui_secret_set(gui_secret_t *s,const char *service,const char *user,const char *pass)
{
    gui_secret_clear(s);
    if(!service||!user||!pass)return false;
    const char *values[]={service,user,pass};
    const size_t sizes[]={sizeof(s->service),sizeof(s->username),sizeof(s->password)};
    for(unsigned i=0;i<3;++i) {
        size_t n=0;
        while(n<sizes[i]&&values[i][n]) {
            if((unsigned char)values[i][n]<32||(unsigned char)values[i][n]>126)return false;
            ++n;
        }
        if(!n||n>=sizes[i])return false;
    }
    if(gui_text_width(GUI_FONT_BODY,pass)>278 || gui_text_width(GUI_FONT_BODY,user)>278)return false;
    strcpy(s->service,service);strcpy(s->username,user);strcpy(s->password,pass);s->active=true;
    return true;
}
unsigned gui_move(unsigned s,unsigned count,bool up)
{ return count ? (up ? (s+count-1)%count : (s+1)%count) : 0; }
const char *gui_screen_name(gui_screen_t s)
{
    static const char *names[]={"home","recording","recordings","playback","sync","ai","setup"};
    return (unsigned)s<7?names[s]:"home";
}
