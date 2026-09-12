#include "raster.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define W 64
#define H 48
static uint16_t img[W*H];
static uint16_t cov[W];

static void band_render(const int32_t *pts, int n, bool eo, uint16_t col,
                        int stroke_w)
{
    for (int y = 0; y < H; y += 8) {
        rband_t b = { .y0=y, .w=W, .rows=8, .px=&img[y*W], .cov=cov };
        if (stroke_w) rv9_raster_stroke(&b, pts, n, true, stroke_w, col);
        else          rv9_raster_fill(&b, pts, n, eo, col);
    }
}

static int at(int x, int y) { return img[y*W+x]; }

int main(void)
{
    int fails = 0;
    #define CHECK(c,msg) do{ if(!(c)){printf("FAIL: %s\n",msg); fails++;} }while(0)

    /* 1. A solid rectangle, exactly on pixel bounds. */
    for (int i=0;i<W*H;i++) img[i]=0;
    int32_t rect[8] = { RV9_FIX(10),RV9_FIX(10), RV9_FIX(30),RV9_FIX(10),
                        RV9_FIX(30),RV9_FIX(20), RV9_FIX(10),RV9_FIX(20) };
    band_render(rect, 4, false, 0xFFFF, 0);
    CHECK(at(20,15)==0xFFFF, "rect interior not filled");
    CHECK(at(9,15)==0,       "rect bled left");
    CHECK(at(30,15)==0,      "rect bled right (half-open x)");
    CHECK(at(20,9)==0,       "rect bled above");
    CHECK(at(20,20)==0,      "rect bled below (half-open y)");
    CHECK(at(10,10)==0xFFFF, "rect top-left corner missing");
    CHECK(at(29,19)==0xFFFF, "rect bottom-right corner missing");

    /* 2. Half-pixel offset should give partial coverage, not nothing. */
    for (int i=0;i<W*H;i++) img[i]=0;
    int32_t half[8] = { RV9_FIX(10)+128,RV9_FIX(10), RV9_FIX(30),RV9_FIX(10),
                        RV9_FIX(30),RV9_FIX(20), RV9_FIX(10)+128,RV9_FIX(20) };
    band_render(half, 4, false, 0xFFFF, 0);
    CHECK(at(10,15)!=0 && at(10,15)!=0xFFFF, "no horizontal antialiasing");

    /* 3. A triangle: diagonal edge must be antialiased, interior solid. */
    for (int i=0;i<W*H;i++) img[i]=0;
    int32_t tri[6] = { RV9_FIX(5),RV9_FIX(5), RV9_FIX(45),RV9_FIX(5),
                       RV9_FIX(5),RV9_FIX(45) };
    band_render(tri, 3, false, 0xFFFF, 0);
    CHECK(at(7,7)==0xFFFF, "triangle interior not filled");
    int aa = 0;
    for (int y=6;y<40;y++) for (int x=0;x<W;x++) {
        int v = at(x,y); if (v!=0 && v!=0xFFFF) aa++;
    }
    CHECK(aa > 20, "diagonal edge shows no antialiasing");

    /* 4. Spans must not run off the right edge (clipping). */
    for (int i=0;i<W*H;i++) img[i]=0;
    int32_t wide[8] = { RV9_FIX(-20),RV9_FIX(4), RV9_FIX(200),RV9_FIX(4),
                        RV9_FIX(200),RV9_FIX(8), RV9_FIX(-20),RV9_FIX(8) };
    band_render(wide, 4, false, 0x1234, 0);
    CHECK(at(0,5)==0x1234 && at(W-1,5)==0x1234, "wide shape did not clip cleanly");

    /* 5. A five-pointed star: even-odd empties the middle, nonzero fills
       it. Two subpaths would be the other way to test this, and this
       renderer has no subpaths -- one point list, implicitly closed. */
    int32_t star[10] = {
        RV9_FIX(24),RV9_FIX(6),
        (int32_t)(34.58*256),(int32_t)(38.56*256),
        (int32_t)(6.88*256), (int32_t)(18.44*256),
        (int32_t)(41.12*256),(int32_t)(18.44*256),
        (int32_t)(13.42*256),(int32_t)(38.56*256),
    };
    for (int i=0;i<W*H;i++) img[i]=0;
    band_render(star, 5, true, 0xFFFF, 0);
    CHECK(at(24,24)==0, "even-odd did not empty the star's middle");
    CHECK(at(24,12)==0xFFFF, "even-odd lost the star's top point");

    for (int i=0;i<W*H;i++) img[i]=0;
    band_render(star, 5, false, 0xFFFF, 0);
    CHECK(at(24,24)==0xFFFF, "nonzero did not fill the star's middle");

    /* 6. A stroked square leaves its middle alone. */
    for (int i=0;i<W*H;i++) img[i]=0;
    band_render(rect, 4, false, 0xF800, RV9_FIX(2));
    CHECK(at(20,15)==0, "stroke filled the interior");
    CHECK(at(20,10)!=0, "stroke missed the top edge");
    CHECK(at(10,15)!=0, "stroke missed the left edge");

    /* 7. Byte swap on the way to the panel. */
    img[0] = 0x1234;
    rband_t b = { .y0=0, .w=1, .rows=1, .px=img, .cov=cov };
    rv9_raster_to_panel(&b);
    CHECK(img[0]==0x3412, "panel byte swap wrong");

    printf(fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", fails);
    return fails != 0;
}
