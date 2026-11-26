// [stock-tile] Host test of xbg_stock_tile.h (port-common/modules/stock-tile, PNH-B-01): the DM_GFX_STOCK_TILE modes, which
// drivers get the exact tile rules, that the family defaults are never turned off, and the boot line (content, truncation).
// Build (WSL or any host; no Vulkan headers needed):
//   g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -o stock_tile_test stock_tile_test.cpp && ./stock_tile_test
#include "../xbg_stock_tile.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(expression) do { if(!(expression)){printf("FAIL line %d: %s\n",__LINE__,#expression);++failures;} } while(0)
static bool has(const char *s,const char *w){return s && strstr(s,w)!=nullptr;}

int main(){
    // Modes: unset / empty = 1 (default on for the stock driver), "0" off, "2" any Adreno driver, junk = off, out of range clamps.
    CHECK(xbg_stock_tile_mode(nullptr)==1);CHECK(xbg_stock_tile_mode("")==1);CHECK(xbg_stock_tile_mode("1")==1);
    CHECK(xbg_stock_tile_mode("0")==0);CHECK(xbg_stock_tile_mode("2")==2);CHECK(xbg_stock_tile_mode("7")==2);
    CHECK(xbg_stock_tile_mode("-1")==0);CHECK(xbg_stock_tile_mode("off")==0);

    // Who gets the rules: Qualcomm stock (driverID 8) on vendor 0x5143 by default; never Turnip (18), an unknown ID (0) or a
    // non-Adreno vendor; mode 2 = every Adreno driver; mode 0 = nobody.
    CHECK(xbg_stock_tile_active(1,0x5143,8));
    CHECK(!xbg_stock_tile_active(1,0x5143,18));
    CHECK(!xbg_stock_tile_active(1,0x5143,0));
    CHECK(!xbg_stock_tile_active(1,0x13B5,8));   // a Mali claiming ID 8 (impossible) still is not this module's business
    CHECK(!xbg_stock_tile_active(1,0x13B5,9));
    CHECK(xbg_stock_tile_active(2,0x5143,18));CHECK(xbg_stock_tile_active(2,0x5143,0));
    CHECK(!xbg_stock_tile_active(2,0x13B5,9));CHECK(!xbg_stock_tile_active(2,0x1010,7));
    CHECK(!xbg_stock_tile_active(0,0x5143,8));

    // Defaults: the module only adds; a tiler family keeps its rules whatever the module says.
    CHECK(xbg_stock_tile_default(true,false));CHECK(xbg_stock_tile_default(true,true));
    CHECK(xbg_stock_tile_default(false,true));CHECK(!xbg_stock_tile_default(false,false));

    // Names.
    char n[128];
    xbg_stock_tile_names(n,sizeof n,0);CHECK(!strcmp(n,"none"));
    xbg_stock_tile_names(n,sizeof n,XBG_STK_TILE_BARRIER|XBG_STK_STORE_DEAD);CHECK(!strcmp(n,"tile_barrier store_dead"));
    xbg_stock_tile_names(n,sizeof n,(1u<<XBG_STK_RULES)-1);CHECK(has(n,"chunk_mid_split=0"));CHECK(has(n,"clear_fold"));
    {char t[8];int k=xbg_stock_tile_names(t,sizeof t,(1u<<XBG_STK_RULES)-1);CHECK(k==7);CHECK(strlen(t)==7);}   // truncated, terminated
    {char t[1];xbg_stock_tile_names(t,sizeof t,XBG_STK_STORE_NONE);CHECK(t[0]==0);}
    CHECK(xbg_stock_tile_names(nullptr,10,1)==0);

    // The boot line.
    const uint32_t d3=XBG_STK_TILE_BARRIER|XBG_STK_STORE_NONE|XBG_STK_STORE_DEAD|XBG_STK_LAZY_TWIN_FLUSH|XBG_STK_LAZY_KEEP;
    char line[600];
    xbg_stock_tile_line(line,sizeof line,1,true,0x5143,8,"Qualcomm Technologies Inc. Adreno Vulkan Driver",d3,d3,0,false);
    CHECK(has(line,"Qualcomm stock driver"));CHECK(has(line,"driverID 8"));CHECK(has(line,"GMEM"));
    CHECK(has(line,"exact rules turned on: tile_barrier store_none store_dead lazy_twin_flush lazy_keep"));
    CHECK(has(line,"noload stays off"));CHECK(has(line,"DM_GFX_STOCK_TILE=0"));CHECK(!has(line,"own switch"));CHECK(!has(line,"store_none off"));
    // own switch wins and no extension
    xbg_stock_tile_line(line,sizeof line,1,true,0x5143,8,"Q",d3,d3&~(XBG_STK_STORE_NONE|XBG_STK_TILE_BARRIER),XBG_STK_TILE_BARRIER,true);
    CHECK(has(line,"in effect: store_dead lazy_twin_flush lazy_keep"));CHECK(has(line,"own switch set: tile_barrier"));CHECK(has(line,"store_none off: no VK_EXT"));
    // skipped lines
    xbg_stock_tile_line(line,sizeof line,1,false,0x5143,18,"turnip Mesa driver",0,0,0,false);CHECK(has(line,"Turnip"));CHECK(has(line,"sysmem"));
    xbg_stock_tile_line(line,sizeof line,0,false,0x5143,8,"Q",0,0,0,false);CHECK(has(line,"off (DM_GFX_STOCK_TILE=0)"));
    xbg_stock_tile_line(line,sizeof line,1,false,0x13B5,9,"ARM",0,0,0,false);CHECK(has(line,"not an Adreno"));
    xbg_stock_tile_line(line,sizeof line,1,false,0x5143,0,nullptr,0,0,0,false);CHECK(has(line,"driverID 0"));CHECK(has(line,"\"?\""));
    xbg_stock_tile_line(line,sizeof line,2,true,0x5143,18,"turnip",d3,d3,0,false);CHECK(has(line,"DM_GFX_STOCK_TILE=2"));CHECK(has(line,"forced"));
    // truncation at every size keeps the buffer terminated (ASan catches an overrun)
    for(size_t b=1;b<=sizeof line;b++){char *s=new char[b];xbg_stock_tile_line(s,b,1,true,0x5143,8,"Qualcomm Technologies Inc. Adreno Vulkan Driver",d3,d3,d3,true);CHECK(strlen(s)<b);delete[] s;}
    CHECK(xbg_stock_tile_line(nullptr,0,1,true,0x5143,8,"Q",d3,d3,0,false)==0);

    if(failures){printf("stock_tile_test: %d failures\n",failures);return 1;}
    printf("stock_tile_test: all checks passed\n");return 0;
}
