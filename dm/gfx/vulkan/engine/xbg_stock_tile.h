// [stock-tile] The exact tile rules on Qualcomm's own Adreno driver (port-common/modules/stock-tile, KB PNH-B-01).
// xbg_vendor_policy.h keeps every tile rule off on vendor 0x5143 because Turnip runs our passes with TU_DEBUG=sysmem, where a
// load or a store of an attachment costs nothing. Qualcomm's stock driver (VkDriverId QUALCOMM_PROPRIETARY = 8) reads no
// TU_DEBUG: it bins every render pass through GMEM and pays a load and a store of every attachment per pass, as Mali and PowerVR
// do. Here the rules that cannot change a pixel are on there by default; Turnip and every other driver keep the old defaults.
// Exact means: the same bytes reach every later reader (Vulkan semantics plus the engine's own enforcement):
// - STORE_NONE: STORE_OP_NONE only on aspects no draw / clear of the instance writes, and a write ends the instance first;
// - STORE_DEAD: DONT_CARE only on aspects a full clear provably overwrites before anything can read them (store_scan; a broken
//   proof is caught at run time and turns the rule off);
// - TILE_BARRIER: only the stage masks of barriers narrow (fragment-only readers wait in the fragment stages); every pre-fragment
//   read, every non-draw operation and every pre-fragment storage write keeps the full barrier;
// - LAZY_TWIN_FLUSH / LAZY_KEEP (D3), CLEAR_FOLD (TL2 / NieR), CHUNK_MID_SPLIT=0 (D16): a full clear recorded as the load op of
//   its own pass, a pass not cut mid-submission - the same values in the same order.
// Not here: NOLOAD (first_draw_covers is a proof with a known rare false accept: magenta under DM_GFX_TILE_POISON in DOOM) and
// STORE_HIST (a prediction from earlier frames): heuristics, their own switches (DM_GFX_NOLOAD=1 / DM_GFX_STORE_HIST=1) are the
// separate second step. Each rule's own DM_GFX_* switch still wins over the default this sets.
// Pure functions, no device calls: tests/stock_tile_test.cpp calls them directly.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBG_DRIVER_ID_QUALCOMM_PROPRIETARY 8u   // VK_DRIVER_ID_QUALCOMM_PROPRIETARY
#define XBG_DRIVER_ID_MESA_TURNIP 18u           // VK_DRIVER_ID_MESA_TURNIP

// DM_GFX_STOCK_TILE: 0 = off (every Adreno driver records as before), 1 = on Qualcomm's stock driver only (the default: unset or
// empty), 2 = on every Adreno driver (a test: under Turnip sysmem the rules gain nothing, but the picture and the counters show).
inline int xbg_stock_tile_mode(const char *environment){
    if(!environment || !*environment)return 1;
    const int v=atoi(environment);
    return v<=0?0:v>=2?2:1;
}

// Whether this module turns the exact rules on for this device. Vendor 0x5143 is every Adreno, Turnip included; the driver ID
// tells them apart (never the device name). A driver that reports no driver ID (0) counts as not stock.
inline bool xbg_stock_tile_active(int mode,uint32_t vendor_id,uint32_t driver_id){
    if(mode<=0 || vendor_id!=0x5143)return false;
    return mode>=2 || driver_id==XBG_DRIVER_ID_QUALCOMM_PROPRIETARY;
}

// The default of one exact rule: the family default (Mali / PowerVR) or this module.
inline bool xbg_stock_tile_default(bool family_default,bool active){return family_default || active;}

// Rule bits for the boot line.
enum : uint32_t {
    XBG_STK_TILE_BARRIER   =1u<<0,
    XBG_STK_STORE_NONE     =1u<<1,
    XBG_STK_STORE_DEAD     =1u<<2,
    XBG_STK_LAZY_TWIN_FLUSH=1u<<3,
    XBG_STK_LAZY_KEEP      =1u<<4,
    XBG_STK_CLEAR_FOLD     =1u<<5,
    XBG_STK_CHUNK_NO_SPLIT =1u<<6,
    XBG_STK_RULES          =7
};
inline const char *xbg_stock_tile_rule_name(unsigned bit){
    static const char *names[XBG_STK_RULES]={"tile_barrier","store_none","store_dead","lazy_twin_flush","lazy_keep","clear_fold","chunk_mid_split=0"};
    return bit<XBG_STK_RULES?names[bit]:"?";
}
// "a b c" for the bits of mask, "none" when empty. Returns the length written (snprintf rules: truncated, always terminated).
inline int xbg_stock_tile_names(char *out,size_t bytes,uint32_t mask){
    if(!out || !bytes)return 0;
    out[0]=0;int at=0;
    for(unsigned b=0;b<XBG_STK_RULES;b++)if(mask&(1u<<b)){
        int n=snprintf(out+at,bytes-size_t(at),"%s%s",at?" ":"",xbg_stock_tile_rule_name(b));
        if(n<0)break;
        at+=n;
        if(size_t(at)>=bytes){at=int(bytes)-1;break;}
    }
    if(!at)at=snprintf(out,bytes,"none");
    return at;
}

// The one boot line (also the [pc] report): what is on and why.
//   mode / active: xbg_stock_tile_mode / xbg_stock_tile_active; vendor_id, driver_id, driver_name: the device's;
//   wanted: the rules this module turned on (family default off, module on); effective: the rules in effect after their own
//   switches; own: rules whose own DM_GFX_* switch was set; no_store_none: STORE_NONE wanted but the device has no
//   load_store_op_none (STORE_OP_NONE does not exist there, so the rule stays off).
inline int xbg_stock_tile_line(char *out,size_t bytes,int mode,bool active,uint32_t vendor_id,uint32_t driver_id,const char *driver_name,
                               uint32_t wanted,uint32_t effective,uint32_t own,bool no_store_none){
    if(!out || !bytes)return 0;
    const char *dn=driver_name && *driver_name?driver_name:"?";
    if(!active){
        if(mode<=0)return snprintf(out,bytes,"off (DM_GFX_STOCK_TILE=0): every Adreno driver keeps the old tile defaults; PNH-B-01");
        if(vendor_id!=0x5143)return snprintf(out,bytes,"not an Adreno (vendor %#x, driverID %u): the GPU family policy decides the tile rules; PNH-B-01",vendor_id,driver_id);
        if(driver_id==XBG_DRIVER_ID_MESA_TURNIP)
            return snprintf(out,bytes,"Turnip (driverID 18 \"%s\"): it runs our passes in sysmem (TU_DEBUG=sysmem), loads / stores cost nothing there, tile defaults as before (DM_GFX_STOCK_TILE=2 tries them); PNH-B-01",dn);
        return snprintf(out,bytes,"Adreno driverID %u \"%s\" is not Qualcomm's stock driver (8): tile defaults as before (DM_GFX_STOCK_TILE=2 tries them); PNH-B-01",driver_id,dn);
    }
    char w[128],e[128],o[128];xbg_stock_tile_names(w,sizeof w,wanted);xbg_stock_tile_names(e,sizeof e,effective);
    int at=snprintf(out,bytes,"%s (driverID %u \"%s\"): %s; exact rules turned on: %s; in effect: %s",
                    mode>=2?"DM_GFX_STOCK_TILE=2 (test, any Adreno driver)":"Qualcomm stock driver",driver_id,dn,
                    driver_id==XBG_DRIVER_ID_QUALCOMM_PROPRIETARY?"it bins every pass through GMEM, each attachment load / store costs":"forced",w,e);
    if(at<0 || size_t(at)>=bytes)return at;
    if(own){
        xbg_stock_tile_names(o,sizeof o,own);
        const int n=snprintf(out+at,bytes-size_t(at),"; own switch set: %s",o);
        if(n<0)return at;
        at+=n;
        if(size_t(at)>=bytes)return at;
    }
    if(no_store_none){
        const int n=snprintf(out+at,bytes-size_t(at),"; store_none off: no VK_EXT / KHR_load_store_op_none / QCOM_render_pass_store_ops");
        if(n<0)return at;
        at+=n;
        if(size_t(at)>=bytes)return at;
    }
    int n=snprintf(out+at,bytes-size_t(at),"; noload stays off (heuristic: DM_GFX_NOLOAD=1 = second step); DM_GFX_STOCK_TILE=0 = old path; PNH-B-01");
    return n<0?at:at+n;
}
