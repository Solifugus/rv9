#pragma once
#include "rv9/io.h"
rv9_io_err_t rv9_panel_open(bool landscape, int *w, int *h);
void rv9_panel_size(int *w, int *h);
void rv9_panel_blit(int x0,int y0,int x1,int y1,const uint16_t *px);
bool rv9_panel_take(const void *owner);
void rv9_panel_backlight(uint32_t p);
uint32_t rv9_panel_backlight_get(void);
