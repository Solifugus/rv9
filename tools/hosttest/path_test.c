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

    /* ---- <text> ---- */

    /* Where the ink actually landed, which is the only honest way to test
       placement: the numbers in the SVG are a claim, the pixels are not. */
    int x0,y0,x1,y1;
    #define INK() do {                                              \
        x0 = PW; y0 = PH; x1 = -1; y1 = -1;                           \
        for (int yy = 0; yy < PH; yy++) {                             \
            for (int xx = 0; xx < PW; xx++) {                         \
                if (!px(xx, yy)) continue;                            \
                if (xx < x0) x0 = xx;                                 \
                if (xx > x1) x1 = xx;                                 \
                if (yy < y0) y0 = yy;                                 \
                if (yy > y1) y1 = yy;                                 \
            }                                                         \
        }                                                             \
    } while (0)

    /* A 40-tall cell, baseline at y=100, so the cell spans 70..110 and an
       'H' -- glyph rows 3..14 of 20 -- lands between 76 and 100. */
    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " fill=\"#ffffff\">H</text></svg>");
    INK();
    CHECK(x1 >= 0, "text drew nothing at all");
    CHECK(x0 >= 100 && x1 <= 120, "glyph outside its cell horizontally");
    CHECK(y0 >= 74 && y0 <= 78, "glyph top is not on the baseline's cap");
    CHECK(y1 >= 97 && y1 <= 102, "glyph does not sit on the baseline");

    /* text-anchor moves the run, not the glyphs within it. */
    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " text-anchor=\"end\" fill=\"#ffffff\">H</text></svg>");
    INK();
    CHECK(x0 >= 80 && x1 <= 100, "text-anchor=end did not shift a cell left");

    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " text-anchor=\"middle\" fill=\"#ffffff\">H</text></svg>");
    INK();
    CHECK(x0 >= 90 && x1 <= 110, "text-anchor=middle did not centre a cell");

    /* Both properties inherit from a group, which is the whole reason for
       putting them there: one axis of labels, one size, one alignment. */
    draw("<svg viewBox=\"0 0 320 172\"><g font-size=\"40\" fill=\"#ffffff\">"
         "<text x=\"100\" y=\"100\">H</text></g></svg>");
    INK();
    CHECK(y1 - y0 >= 20, "font-size did not inherit from the group");
    CHECK(x1 - x0 <= 20, "inherited font-size came out too wide");

    draw("<svg viewBox=\"0 0 320 172\"><g font-size=\"40\" text-anchor=\"end\""
         " fill=\"#ffffff\"><text x=\"100\" y=\"100\">H</text></g></svg>");
    INK();
    CHECK(x1 <= 100, "text-anchor did not inherit from the group");

    /* Three characters advance three cells and no more. */
    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " fill=\"#ffffff\">HHH</text></svg>");
    INK();
    CHECK(x0 >= 100 && x1 <= 160, "three glyphs did not occupy three cells");
    CHECK(x1 > 130, "later glyphs in the run were not advanced");

    /* Whitespace around the content is not part of the text. */
    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " fill=\"#ffffff\">\n      H\n   </text></svg>");
    INK();
    CHECK(x0 >= 100 && x1 <= 120, "leading whitespace shifted the text");

    /* fill=none means no text, not black text. */
    draw("<svg viewBox=\"0 0 320 172\"><text x=\"100\" y=\"100\" font-size=\"40\""
         " fill=\"none\">H</text></svg>");
    INK();
    CHECK(x1 < 0, "text drew despite fill=none");

    printf(fails ? "\n%d check(s) failed\n" : "\nall path checks passed\n", fails);
    return fails!=0;
}
