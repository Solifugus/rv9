#include "drv_svgwin.c"      /* statics and all: this is a test of the inside */
#include <stdio.h>
#include <stdlib.h>

#define PW 320
#define PH 172
static uint16_t screen[PW*PH];

rv9_io_err_t rv9_panel_open(bool l,int *w,int *h){(void)l;if(w)*w=PW;if(h)*h=PH;return RV9_IO_OK;}
void rv9_panel_size(int *w,int *h){if(w)*w=PW;if(h)*h=PH;}
void rv9_panel_backlight(uint32_t p){(void)p;}
uint32_t rv9_panel_backlight_get(void){return 100;}
bool rv9_panel_take(const void *o){(void)o;return false;}
void rv9_panel_blit(int x0,int y0,int x1,int y1,const uint16_t *px)
{ for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++) screen[y*PW+x]=px[(y-y0)*(x1-x0)+(x-x0)]; }

static rv9_dev_t dev;
static int fails;

static void draw(const char *svg)
{
    memset(screen,0,sizeof screen);
    size_t done=0;
    svgwin_write(&dev, svg, strlen(svg), &done);
}
/* stored byte-swapped by rv9_raster_to_panel */
static int px(int x,int y){ uint16_t v=screen[y*PW+x]; return (uint16_t)((v>>8)|(v<<8)); }
#define CHECK(c,m) do{ if(!(c)){ printf("FAIL: %s\n",m); fails++; } }while(0)

int main(void)
{
    memset(&dev,0,sizeof dev);
    dev.opt[OPT_ROTATE]=1; dev.opt[OPT_BG]=0x0000;
    svgwin_init(&dev); svgwin_open(&dev,3);

    const uint16_t W = 0xFFFF;

    /* M/L/Z closes and fills a triangle. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 10 L100 10 L10 100 Z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(20,20)==W, "M/L/Z did not fill");
    CHECK(px(90,90)==0, "M/L/Z filled outside the triangle");

    /* Lowercase is relative. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 10 l90 0 l0 90 z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(95,50)==W, "relative l did not reach");
    CHECK(px(20,90)==0, "relative l filled the wrong side");

    /* H and V. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 10 H110 V110 H10 Z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(60,60)==W, "H/V box not filled");
    CHECK(px(120,60)==0, "H/V box too wide");

    /* An implicit lineto after a moveto: "M a b c d" is M then L. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 10 110 10 110 110 10 110 Z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(60,60)==W, "implicit lineto after moveto ignored");

    /* A cubic bulges away from the chord. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 100 C 10 10, 110 10, 110 100 Z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(60,40)==W, "cubic did not bulge upward");
    CHECK(px(60,120)==0, "cubic spilled below its chord");

    /* A quadratic likewise. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 100 Q 60 10 110 100 Z\" fill=\"#ffffff\"/></svg>");
    CHECK(px(60,75)==W, "quadratic did not bulge");

    /* Two subpaths with even-odd: a real hole. */
    draw("<svg viewBox=\"0 0 320 172\"><path fill-rule=\"evenodd\" fill=\"#ffffff\""
         " d=\"M10 10 H110 V110 H10 Z M40 40 H80 V80 H40 Z\"/></svg>");
    CHECK(px(20,60)==W, "outer ring lost");
    CHECK(px(60,60)==0, "even-odd did not punch the hole");

    /* Same shape, nonzero winding: both wound the same way, so no hole. */
    draw("<svg viewBox=\"0 0 320 172\"><path fill=\"#ffffff\""
         " d=\"M10 10 H110 V110 H10 Z M40 40 H80 V80 H40 Z\"/></svg>");
    CHECK(px(60,60)==W, "nonzero should not have made a hole");

    /* Arcs: a circle from two half-arcs, and it must be solid and round. */
    draw("<svg viewBox=\"0 0 320 172\"><path fill=\"#ffffff\""
         " d=\"M160 86 m -40 0 a 40 40 0 1 0 80 0 a 40 40 0 1 0 -80 0 Z\"/></svg>");
    CHECK(px(160,86)==W, "arc circle hollow in the middle");
    CHECK(px(160,50)==W, "arc circle missing at the top");
    CHECK(px(160,120)==W, "arc circle missing at the bottom");
    CHECK(px(122,86)==W, "arc circle missing at the left");
    CHECK(px(197,86)==W, "arc circle missing at the right");
    CHECK(px(160,40)==0, "arc circle too tall");
    CHECK(px(112,86)==0, "arc circle too wide");

    /* A ring: outer and inner circles, even-odd. This is the one that came
       out with a wedge cut to the centre when a one-point subpath left a
       stray point in the shared array. */
    draw("<svg viewBox=\"0 0 320 172\"><path fill=\"#ffffff\" fill-rule=\"evenodd\""
         " d=\"M160 86 m -40 0 a 40 40 0 1 0 80 0 a 40 40 0 1 0 -80 0 Z"
         "    M160 86 m -18 0 a 18 18 0 1 1 36 0 a 18 18 0 1 1 -36 0 Z\"/></svg>");
    CHECK(px(160,86)==0, "ring has no hole");
    CHECK(px(160,55)==W, "ring missing at the top");
    CHECK(px(128,86)==W, "ring missing at the left");
    CHECK(px(192,86)==W, "ring missing at the right");
    CHECK(px(160,117)==W, "ring missing at the bottom");
    int wedge=0;
    for (int x=122;x<142;x++) if (px(x,86)==0) wedge++;
    CHECK(wedge==0, "a wedge is cut out of the ring's left side");

    /* An unknown command stops the path rather than misreading numbers. */
    draw("<svg viewBox=\"0 0 320 172\"><path d=\"M10 10 H110 V110 H10 Z K 5 5\" fill=\"#ffffff\"/></svg>");
    CHECK(px(60,60)==W, "path before an unknown command was discarded");

    printf(fails ? "\n%d check(s) failed\n" : "\nall path checks passed\n", fails);
    return fails!=0;
}
