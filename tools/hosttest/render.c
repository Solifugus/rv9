#include "drv_svgwin.c"      /* statics and all: this is a test of the inside */
#include <stdio.h>

#define PW 320
#define PH 172
static uint16_t screen[PW*PH];

rv9_io_err_t rv9_panel_open(bool l, int *w, int *h)
{ (void)l; if(w)*w=PW; if(h)*h=PH; return RV9_IO_OK; }
void rv9_panel_size(int *w,int *h){ if(w)*w=PW; if(h)*h=PH; }
void rv9_panel_backlight(uint32_t p){(void)p;}
uint32_t rv9_panel_backlight_get(void){return 100;}
bool rv9_panel_take(const void *o){(void)o;return false;}

void rv9_panel_blit(int x0,int y0,int x1,int y1,const uint16_t *px)
{
    for (int y=y0;y<y1;y++)
        for (int x=x0;x<x1;x++)
            screen[y*PW+x] = px[(y-y0)*(x1-x0)+(x-x0)];
}

static void rgb(uint16_t v,int *r,int *g,int *b)
{   /* stored swapped by rv9_raster_to_panel */
    uint16_t n = (uint16_t)((v>>8)|(v<<8));
    *r = ((n>>11)&0x1F)*255/31; *g = ((n>>5)&0x3F)*255/63; *b = (n&0x1F)*255/31;
}

int main(int argc, char **argv)
{
    rv9_dev_t dev; memset(&dev,0,sizeof(dev));
    dev.opt[OPT_ROTATE]=1; dev.opt[OPT_BG]=0x0000;

    if (svgwin_init(&dev)!=RV9_IO_OK) { puts("init failed"); return 1; }
    if (svgwin_open(&dev,3)!=RV9_IO_OK) { puts("open failed"); return 1; }

    FILE *f = fopen(argv[1],"rb");
    if(!f){puts("no svg");return 1;}
    static char buf[8192]; size_t n=fread(buf,1,sizeof buf,f); fclose(f);

    size_t done=0;
    svgwin_write(&dev, buf, n, &done);

    FILE *o=fopen("out.ppm","wb");
    fprintf(o,"P6\n%d %d\n255\n",PW,PH);
    for(int i=0;i<PW*PH;i++){int r,g,b;rgb(screen[i],&r,&g,&b);
        fputc(r,o);fputc(g,o);fputc(b,o);}
    fclose(o);

    if (argc < 3) { puts("rendered"); return 0; }
    /* Spot checks against what the picture says it should be. */
    int r,g,b; int fails=0;
    #define AT(x,y) rgb(screen[(y)*PW+(x)],&r,&g,&b)
    #define NEAR(v,w) (abs((v)-(w))<24)
    AT(10,10);   if(!(NEAR(r,0x10)&&NEAR(g,0x20)&&NEAR(b,0x40))){printf("sky wrong: %d %d %d\n",r,g,b);fails++;}
    AT(300,165); if(!(NEAR(r,0x1d)&&NEAR(g,0x5c)&&NEAR(b,0x2e))){printf("ground wrong: %d %d %d\n",r,g,b);fails++;}
    AT(258,40);  if(!(NEAR(r,0xff)&&NEAR(g,0xcc)&&NEAR(b,0x33))){printf("sun wrong: %d %d %d\n",r,g,b);fails++;}
    AT(210,10);  if(!(NEAR(r,0x10)&&NEAR(g,0x20)&&NEAR(b,0x40))){printf("above sun should be sky: %d %d %d\n",r,g,b);fails++;}
    AT(70,80);   if(!(NEAR(r,0x2a)&&NEAR(g,0x6b)&&NEAR(b,0x3f))){printf("hill wrong: %d %d %d\n",r,g,b);fails++;}
    AT(45,110);  if(!(NEAR(r,0xc8)&&NEAR(g,0xb2)&&NEAR(b,0x8a))){printf("house wall wrong: %d %d %d\n",r,g,b);fails++;}
    printf(fails?"%d spot check(s) failed\n":"all spot checks passed\n",fails);
    return fails!=0;
}
