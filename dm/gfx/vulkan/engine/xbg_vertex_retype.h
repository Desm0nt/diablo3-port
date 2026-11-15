#pragma once
// [nonadreno] Vertex input retype (DM_GFX_VTX_RETYPE). NVN vertex attributes are untyped registers: an integer format (UINT / SINT)
// puts the integer bits there, a float / norm format the float bits, and the Maxwell shader reinterprets them as it likes. The
// translator declares each input with one scalar type (xbg_vertex_input_classes); D3's full-screen passes declare float vec4 inputs
// that hold R*_UINT data and read them through OpBitcast. Vulkan leaves such a mismatch between the attribute format and the input
// type undefined: Adreno hands over the raw bits, Mali converts the value into the declared type (Mali-G57 0.2.3-debug: the UVs of
// every full-screen pass became garbage, the level was black under the HUD). The fix declares the input with the format's type and
// bitcasts every load back to the type the shader computes with, so the shader sees the same bits on every GPU.
// [vtxscaled] Conversion kind (want 3 / 4): the attribute is a *_SSCALED / *_USCALED format the device cannot fetch (AMD-style GPUs:
// stock Xclipse, RADV report no VERTEX_BUFFER for them), so the pipeline binds the same bytes as the *_SINT / *_UINT format of the
// same layout (xbg_vertex_scaled_int) and the shader does what the scaled fetch does in hardware: the input is declared ivec / uvec
// and every load is followed by OpConvertSToF / OpConvertUToF (then an OpBitcast when the shader declared the input as integer).
// Header-only so tests/vertex_retype_test.cpp checks it on the host.
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Scalar class of a vertex attribute format as the Vulkan spec wants the input declared: 0 = float, 1 = sint, 2 = uint.
inline uint32_t xbg_vertex_format_class(int vk_format){
    switch(vk_format){
    case 13:case 20:case 27:case 34:case 41:case 48:case 55:case 62:case 68:case 74:case 81:case 88:case 95:case 98:case 101:case 104:case 107:return 2;
    case 14:case 21:case 28:case 35:case 42:case 49:case 56:case 63:case 69:case 75:case 82:case 89:case 96:case 99:case 102:case 105:case 108:return 1;
    default:return 0;}
}

// The integer format with the layout of a scaled vertex format (Vulkan numbers every such group UNORM, SNORM, USCALED, SSCALED, UINT,
// SINT), or 0 (VK_FORMAT_UNDEFINED) for any other format. *kind: 1 = SSCALED (sint -> float), 2 = USCALED (uint -> float).
inline int xbg_vertex_scaled_int(int vk_format,uint32_t *kind=nullptr){
    switch(vk_format){
    case 11:case 18:case 25:case 32:case 39:case 46:case 53:case 60:case 66:case 72:case 79:case 86:case 93:if(kind)*kind=2;return vk_format+2;
    case 12:case 19:case 26:case 33:case 40:case 47:case 54:case 61:case 67:case 73:case 80:case 87:case 94:if(kind)*kind=1;return vk_format+2;
    default:if(kind)*kind=0;return 0;}
}
enum : uint32_t {XBG_VTX_WANT_FLOAT=0,XBG_VTX_WANT_SINT=1,XBG_VTX_WANT_UINT=2,XBG_VTX_WANT_SINT_TO_FLOAT=3,XBG_VTX_WANT_UINT_TO_FLOAT=4};

// Rewrite the vertex inputs at loc[i] to want[i]: 0 float, 1 sint, 2 uint = declare with that scalar class, bits kept; 3 / 4 =
// declare sint / uint and convert the value to float (scaled formats bound as integer); anything else = keep. Every load of a
// retyped input (whole vector or one component through OpAccessChain) is followed by an OpBitcast (classes 0-2) or an
// OpConvertSToF / OpConvertUToF (3 / 4; plus an OpBitcast when the shader's own type is not float) to the type the shader used,
// under the load's original result id. Returns false (out untouched, *why set) for anything it does not understand; the caller
// then keeps the original module.
inline bool xbg_vertex_retype(const std::vector<uint32_t> &w,const uint32_t *loc,size_t inputs,const uint32_t *want,
                              std::vector<uint32_t> &out,std::string *why=nullptr){
    auto fail=[&](const char *m){if(why)*why=m;return false;};
    if(w.size()<5 || w[0]!=0x07230203u)return fail("not SPIR-V");
    for(size_t p=5;p<w.size();){size_t z=w[p]>>16;if(!z || z>w.size()-p)return fail("bad instruction length");p+=z;}
    struct Num{uint32_t kind=0,width=0,sign=0;};   // kind 1 = int, 2 = float
    std::unordered_map<uint32_t,Num> scalar;
    std::unordered_map<uint32_t,std::pair<uint32_t,uint32_t>> vec,ptr;   // vector: component, count; pointer: storage class, pointee
    std::unordered_map<uint32_t,uint32_t> loc_of,var_type;   // Location decoration; Input OpVariable -> its pointer type
    size_t first_function=0;
    for(size_t p=5;p<w.size();p+=w[p]>>16){
        const uint32_t op=w[p]&65535,z=w[p]>>16;
        if(op==21 && z==4)scalar[w[p+1]]={1,w[p+2],w[p+3]};
        else if(op==22 && z==3)scalar[w[p+1]]={2,w[p+2],0};
        else if(op==23 && z==4)vec[w[p+1]]={w[p+2],w[p+3]};
        else if(op==32 && z==4)ptr[w[p+1]]={w[p+2],w[p+3]};
        else if(op==71 && z==4 && w[p+2]==30)loc_of[w[p+1]]=w[p+3];
        else if(op==59 && z>=4 && w[p+3]==1 && !first_function)var_type[w[p+2]]=w[p+1];
        else if(op==54 && !first_function)first_function=p;
    }
    if(!first_function)return fail("no function");
    uint32_t bound=w[3];
    std::vector<uint32_t> decl;   // new type declarations, emitted before the first function
    auto find_scalar=[&](uint32_t cls)->uint32_t{
        for(auto &s:scalar)if(s.second.width==32 && (cls==0?s.second.kind==2:s.second.kind==1 && s.second.sign==(cls==1?1u:0u)))return s.first;
        const uint32_t id=bound++;
        if(cls==0){decl.insert(decl.end(),{(3u<<16)|22u,id,32u});scalar[id]={2,32,0};}
        else{decl.insert(decl.end(),{(4u<<16)|21u,id,32u,cls==1?1u:0u});scalar[id]={1,32,cls==1?1u:0u};}
        return id;};
    auto find_vec=[&](uint32_t comp,uint32_t count)->uint32_t{
        for(auto &v:vec)if(v.second.first==comp && v.second.second==count)return v.first;
        const uint32_t id=bound++;decl.insert(decl.end(),{(4u<<16)|23u,id,comp,count});vec[id]={comp,count};return id;};
    auto new_ptr=[&](uint32_t pointee)->uint32_t{const uint32_t id=bound++;decl.insert(decl.end(),{(4u<<16)|32u,id,1u,pointee});return id;};
    struct Retype{uint32_t ptr,comp_ptr,scalar_old,scalar_new,value_old,value_new,conv,scalar_float,value_float;};   // conv: 0 = bitcast, else the convert opcode
    std::unordered_map<uint32_t,Retype> vars;   // retyped Input variables
    for(size_t i=0;i<inputs;i++){
        if(want[i]>4)continue;
        const uint32_t conv=want[i]==3?111u:want[i]==4?112u:0u,want_cls=want[i]==3?1u:want[i]==4?2u:want[i];   // OpConvertSToF / OpConvertUToF
        uint32_t var=0;for(auto &v:var_type){auto l=loc_of.find(v.first);if(l!=loc_of.end() && l->second==loc[i]){if(var)return fail("two inputs at one location");var=v.first;}}
        if(!var)return fail("input location not found");
        auto pt=ptr.find(var_type[var]);if(pt==ptr.end() || pt->second.first!=1)return fail("input is not an Input pointer");
        const uint32_t value=pt->second.second;uint32_t comp=value,count=0;
        auto vv=vec.find(value);if(vv!=vec.end()){comp=vv->second.first;count=vv->second.second;}
        auto sc=scalar.find(comp);if(sc==scalar.end() || sc->second.width!=32)return fail("input is not 32-bit scalar / vector");
        const uint32_t cls=sc->second.kind==2?0:sc->second.sign?1:2;
        if(cls==want_cls && !conv)continue;
        const uint32_t ncomp=find_scalar(want_cls),nvalue=count?find_vec(ncomp,count):ncomp;
        // a convert of an integer-declared input goes through float (the register holds the float's bits, as with a real scaled fetch)
        const uint32_t fcomp=conv && cls?find_scalar(0):comp,fvalue=conv && cls?(count?find_vec(fcomp,count):fcomp):value;
        Retype r{new_ptr(nvalue),count?new_ptr(ncomp):0,comp,ncomp,value,nvalue,conv,fcomp,fvalue};
        vars[var]=r;
    }
    if(vars.empty()){out=w;return true;}
    // Pass 1: access chains into retyped inputs (result: a pointer to one component).
    std::unordered_map<uint32_t,uint32_t> chain;   // access chain id -> its variable
    for(size_t p=first_function;p<w.size();p+=w[p]>>16){
        const uint32_t op=w[p]&65535,z=w[p]>>16;
        if((op==65 || op==66) && z>=5 && vars.count(w[p+3])){
            const Retype &r=vars[w[p+3]];
            if(!r.comp_ptr || z!=5)return fail("access chain into a scalar input or with several indices");
            auto pt=ptr.find(w[p+1]);if(pt==ptr.end() || pt->second.second!=r.scalar_old)return fail("access chain of an unexpected type");
            chain[w[p+2]]=w[p+3];
        }
    }
    // Every other use of the variables or chains is unexpected: refuse (operands only; literals of the opcodes below are skipped).
    std::unordered_set<uint32_t> watched;for(auto &v:vars)watched.insert(v.first);for(auto &c:chain)watched.insert(c.first);
    for(size_t p=first_function;p<w.size();p+=w[p]>>16){
        const uint32_t op=w[p]&65535,z=w[p]>>16;
        if(op==61 || op==65 || op==66)continue;   // OpLoad / access chains: handled
        size_t from=1,to=z;
        if(op==12)from=5;                            // OpExtInst: type, id, set, instruction number, then operands
        else if(op==79 || op==81 || op==82 || op==251 || op==8 || op==5 || op==6)continue;   // shuffle / extract / insert / switch / line / names: no pointer operands
        for(size_t k=from;k<to;k++)if(watched.count(w[p+k]))return fail("retyped input used outside OpLoad / OpAccessChain");
    }
    for(size_t p=first_function;p<w.size();p+=w[p]>>16)
        if((w[p]&65535)==61 && (w[p]>>16)>=4){
            const uint32_t src=w[p+3];
            if(chain.count(src)){if(w[p+1]!=vars[chain[src]].scalar_old)return fail("component load of an unexpected type");}
            else if(vars.count(src)){if(w[p+1]!=vars[src].value_old)return fail("load of an unexpected type");}
        }
    // Pass 2: emit. Retyped variables move to just before the first function, after the new declarations.
    out.clear();out.reserve(w.size()+decl.size()+64);
    out.insert(out.end(),w.begin(),w.begin()+5);
    for(size_t p=5;p<w.size();p+=w[p]>>16){
        const uint32_t op=w[p]&65535,z=w[p]>>16;
        if(p==first_function){
            out.insert(out.end(),decl.begin(),decl.end());
            for(auto &v:vars)out.insert(out.end(),{(4u<<16)|59u,v.second.ptr,v.first,1u});
        }
        if(op==59 && p<first_function && vars.count(w[p+2]))continue;
        if((op==65 || op==66) && chain.count(w[p+2])){
            out.insert(out.end(),w.begin()+p,w.begin()+p+z);out[out.size()-z+1]=vars[chain[w[p+2]]].comp_ptr;continue;}
        if(op==61 && z>=4 && (chain.count(w[p+3]) || vars.count(w[p+3]))){
            const Retype &r=chain.count(w[p+3])?vars[chain[w[p+3]]]:vars[w[p+3]];const bool component=chain.count(w[p+3])!=0;
            const uint32_t type_old=w[p+1],id=w[p+2],tmp=bound++;
            out.insert(out.end(),w.begin()+p,w.begin()+p+z);out[out.size()-z+1]=component?r.scalar_new:r.value_new;out[out.size()-z+2]=tmp;
            if(!r.conv)out.insert(out.end(),{(4u<<16)|124u,type_old,id,tmp});   // OpBitcast back to the shader's type, under the load's id
            else{const uint32_t ftype=component?r.scalar_float:r.value_float;   // the scaled fetch: integer value -> float
                if(ftype==type_old)out.insert(out.end(),{(4u<<16)|r.conv,type_old,id,tmp});
                else{const uint32_t f=bound++;out.insert(out.end(),{(4u<<16)|r.conv,ftype,f,tmp,(4u<<16)|124u,type_old,id,f});}}
            continue;}
        out.insert(out.end(),w.begin()+p,w.begin()+p+z);
    }
    out[3]=bound;
    return true;
}
