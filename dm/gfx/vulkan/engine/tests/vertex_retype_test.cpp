// Vertex input retype (xbg_vertex_retype.h) on a synthetic vertex module: a float vec4 input read per component and whole becomes a
// uvec4 / ivec4 input whose loads are bitcast back under their original ids; uses it does not understand leave the module alone.
// With arguments: rewrite each given SPIR-V file (all inputs to uint, then all to sint) and write <file>.u.spv / <file>.s.spv for
// spirv-val.
// [vtxscaled] Conversion variants (want 3 / 4 = a *_SSCALED / *_USCALED attribute bound as *_SINT / *_UINT): inputs of 1..4 components
// declared float, sint or uint become ivec / uvec inputs whose loads go through OpConvertSToF / OpConvertUToF. `--emit <dir>` writes
// every synthetic module and its variants there for spirv-val --target-env vulkan1.1 (tests/vertex_scaled_test.py runs both).
#include "../xbg_vertex_retype.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
using Module=std::vector<uint32_t>;
void require(bool ok,const char *why){if(!ok)throw std::runtime_error(why);}
void op(Module &m,uint32_t opcode,std::initializer_list<uint32_t> args){m.push_back((uint32_t(args.size()+1)<<16)|opcode);m.insert(m.end(),args);}
enum : uint32_t {tVoid=1,tFn=2,tFloat=3,tVec4=4,tInt=5,tBlock=6,tPtrInV4=7,tPtrInF=8,tPtrOutBlock=9,tPtrOutV4=10,tUint=11,
    i0=20,i1=21,inA=30,inB=31,outPV=32,fMain=40,lEntry=41,ldX=50,ldAll=51,cX=52,ldB=53,cvt=54,pos=55,acc=56};
// inA (location 2) is read as one component (bitcast to uint, converted) and whole; inB (location 0) is the position.
Module vertex(bool escape){
    Module m{0x07230203,0x00010300,0,100,0};
    op(m,17,{1});op(m,14,{0,1});
    op(m,15,{0,fMain,0x6e69616d,0,inA,inB,outPV});
    op(m,71,{inA,30,2});op(m,71,{inB,30,0});
    op(m,71,{tBlock,2});op(m,72,{tBlock,0,11,0});
    op(m,19,{tVoid});op(m,33,{tFn,tVoid});op(m,22,{tFloat,32});op(m,23,{tVec4,tFloat,4});op(m,21,{tInt,32,1});op(m,21,{tUint,32,0});
    op(m,30,{tBlock,tVec4});
    op(m,32,{tPtrInV4,1,tVec4});op(m,32,{tPtrInF,1,tFloat});op(m,32,{tPtrOutBlock,3,tBlock});op(m,32,{tPtrOutV4,3,tVec4});
    op(m,43,{tInt,i0,0});op(m,43,{tInt,i1,1});
    op(m,59,{tPtrInV4,inA,1});op(m,59,{tPtrInV4,inB,1});op(m,59,{tPtrOutBlock,outPV,3});
    op(m,54,{tVoid,fMain,0,tFn});op(m,248,{lEntry});
    op(m,65,{tPtrInF,acc,inA,i1});op(m,61,{tFloat,ldX,acc});op(m,124,{tUint,cX,ldX});op(m,112,{tFloat,cvt,cX});
    op(m,61,{tVec4,ldAll,inA});op(m,61,{tVec4,ldB,inB});
    if(escape)op(m,62,{inA,ldB});   // OpStore into an input: not a pattern the rewrite knows
    op(m,65,{tPtrOutV4,pos,outPV,i0});op(m,62,{pos,ldB});
    op(m,253,{});op(m,56,{});
    return m;
}
size_t find(const Module &m,uint32_t opcode,uint32_t word,uint32_t value,size_t from=5){
    for(size_t p=from;p<m.size();p+=m[p]>>16)if((m[p]&65535)==opcode && m[p+word]==value)return p;return 0;}
void check_rewrite(uint32_t cls){
    const Module in=vertex(false);Module out;std::string why;
    const uint32_t loc[2]={2,0},want[2]={cls,0};
    require(xbg_vertex_retype(in,loc,2,want,out,&why),"rewrite refused");
    require(out[3]>in[3],"bound not raised");
    const size_t var=find(out,59,2,inA);require(var>0,"input variable missing");
    const uint32_t ptype=out[var+1];const size_t pt=find(out,32,1,ptype);require(pt>0 && out[pt+2]==1,"input pointer type missing");
    const size_t vt=find(out,23,1,out[pt+3]);require(vt>0 && out[vt+3]==4,"input is not a 4-vector");
    const uint32_t comp=out[vt+2];require(comp==(cls==2?tUint:tInt),"input component type is not the format's");
    require(var>find(out,23,1,out[pt+3]) && var<find(out,54,2,fMain),"variable not between its types and the function");
    // the component load reads the new type and is bitcast to float under the old id
    const size_t ac=find(out,65,2,acc);require(ac>0 && out[ac+1]!=tPtrInF,"access chain keeps the float pointer");
    const size_t lx=find(out,124,2,ldX);require(lx>0 && out[lx+1]==tFloat,"component load not bitcast back to float");
    const size_t lt=find(out,61,2,out[lx+3]);require(lt>0 && out[lt+1]==comp && out[lt+3]==acc,"component load has the wrong type");
    const size_t la=find(out,124,2,ldAll);require(la>0 && out[la+1]==tVec4,"vector load not bitcast back to vec4");
    require(find(out,61,2,ldB)>0 && find(out,61,1,tVec4)>0,"untouched input changed");
    require(find(out,59,2,inB)>0 && out[find(out,59,2,inB)+1]==tPtrInV4,"position input retyped");
}
void check_noop_and_refusal(){
    const Module in=vertex(false);Module out;
    const uint32_t loc[2]={2,0},same[2]={0,0},keep[2]={255,255};
    require(xbg_vertex_retype(in,loc,2,same,out) && out==in,"matching classes changed the module");
    require(xbg_vertex_retype(in,loc,2,keep,out) && out==in,"keep changed the module");
    const Module esc=vertex(true);std::string why;const uint32_t want[2]={2,0};Module untouched{1,2,3};
    require(!xbg_vertex_retype(esc,loc,2,want,untouched,&why) && !why.empty(),"store into an input accepted");
    const uint32_t missing[1]={7},w1[1]={2};require(!xbg_vertex_retype(in,missing,1,w1,untouched),"missing location accepted");
}
// [vtxscaled] One input at location 3 with `count` components declared with scalar class `cls` (0 float, 1 sint, 2 uint), read whole
// and (vectors) per component; the position is built from the first component, bitcast to float when the shader declared an integer
// (what the translator emits for a register it reads as integer). A second float vec4 input at location 1 stays untouched.
enum : uint32_t {sVoid=1,sFn=2,sFloat=3,sVec4=4,sInt=5,sUint=6,sBlock=7,sIn=8,sInPtr=9,sInCompPtr=10,sPtrOutBlock=11,sPtrOutV4=12,sPtrInV4=13,
    sI0=20,sIn0=30,sIn1=31,sOut=32,sMain=40,sEntry=41,sLdAll=50,sAcc=51,sLdX=52,sF=53,sPos=54,sOutPtr=55,sLdB=56,sSum=57};
Module scaled_vertex(uint32_t count,uint32_t cls){
    Module m{0x07230203,0x00010300,0,100,0};
    const uint32_t comp=cls==0?sFloat:cls==1?sInt:sUint,value=count>1?sIn:comp;
    op(m,17,{1});op(m,14,{0,1});
    op(m,15,{0,sMain,0x6e69616d,0,sIn0,sIn1,sOut});
    op(m,71,{sIn0,30,3});op(m,71,{sIn1,30,1});
    op(m,71,{sBlock,2});op(m,72,{sBlock,0,11,0});
    op(m,19,{sVoid});op(m,33,{sFn,sVoid});op(m,22,{sFloat,32});op(m,23,{sVec4,sFloat,4});op(m,21,{sInt,32,1});op(m,21,{sUint,32,0});
    if(count>1 && !(count==4 && cls==0))op(m,23,{sIn,comp,count});
    const uint32_t in_type=count==4 && cls==0?uint32_t(sVec4):value;
    op(m,30,{sBlock,sVec4});
    op(m,32,{sInPtr,1,in_type});if(count>1)op(m,32,{sInCompPtr,1,comp});
    op(m,32,{sPtrOutBlock,3,sBlock});op(m,32,{sPtrOutV4,3,sVec4});op(m,32,{sPtrInV4,1,sVec4});
    op(m,43,{sInt,sI0,0});
    op(m,59,{sInPtr,sIn0,1});op(m,59,{sPtrInV4,sIn1,1});op(m,59,{sPtrOutBlock,sOut,3});
    op(m,54,{sVoid,sMain,0,sFn});op(m,248,{sEntry});
    op(m,61,{in_type,sLdAll,sIn0});
    uint32_t x=sLdAll;if(count>1){op(m,65,{sInCompPtr,sAcc,sIn0,sI0});op(m,61,{comp,sLdX,sAcc});x=sLdX;}
    uint32_t f=x;if(cls){op(m,124,{sFloat,sF,x});f=sF;}
    op(m,61,{sVec4,sLdB,sIn1});op(m,142,{sVec4,sSum,sLdB,f});   // OpVectorTimesScalar
    op(m,65,{sPtrOutV4,sOutPtr,sOut,sI0});op(m,62,{sOutPtr,sSum});
    op(m,253,{});op(m,56,{});
    return m;
}
size_t count_ops(const Module &m,uint32_t opcode){size_t n=0;for(size_t p=5;p<m.size();p+=m[p]>>16)if((m[p]&65535)==opcode)++n;return n;}
// kind 1 = sint -> float (SSCALED), 2 = uint -> float (USCALED)
Module check_convert(uint32_t count,uint32_t cls,uint32_t kind){
    const Module in=scaled_vertex(count,cls);Module out;std::string why;
    const uint32_t loc[2]={3,1},want[2]={2+kind,255};const uint32_t conv=kind==1?111u:112u,other=kind==1?112u:111u;
    require(xbg_vertex_retype(in,loc,2,want,out,&why),"conversion refused");
    for(size_t p=5;p<out.size();){const size_t z=out[p]>>16;require(z && z<=out.size()-p,"bad instruction length in the output");p+=z;}
    // the input is an integer scalar / vector of the format's signedness and the shader's component count
    const size_t var=find(out,59,2,sIn0);require(var>0 && var<find(out,54,2,sMain),"input variable missing or inside the function");
    const size_t pt=find(out,32,1,out[var+1]);require(pt>0 && out[pt+2]==1,"input pointer type missing");
    uint32_t comp=out[pt+3];if(count>1){const size_t vt=find(out,23,1,comp);require(vt>0 && out[vt+3]==count,"component count changed");comp=out[vt+2];}
    const size_t it=find(out,21,1,comp);require(it>0 && out[it+2]==32 && out[it+3]==(kind==1?1u:0u),"input is not a 32-bit integer of the format's signedness");
    // every load converts: the whole value and (vectors) the component
    const size_t loads=count>1?2:1;
    require(count_ops(out,conv)==loads && count_ops(out,other)==0,"wrong number of conversions");
    require(count_ops(out,124)==(cls?loads+1:0),"wrong number of bitcasts");   // + the shader's own bitcast of an integer input
    const uint32_t in_type=count==4 && cls==0?uint32_t(sVec4):count>1?uint32_t(sIn):cls==0?uint32_t(sFloat):cls==1?uint32_t(sInt):uint32_t(sUint);
    const uint32_t comp_type=cls==0?sFloat:cls==1?sInt:sUint;
    auto scalar_of=[&](uint32_t type){const size_t v=find(out,23,1,type);return v?out[v+2]:type;};
    for(uint32_t id:{uint32_t(sLdAll),uint32_t(count>1?sLdX:sLdAll)}){
        const uint32_t type=id==sLdAll?in_type:comp_type;
        size_t c=find(out,cls?124u:conv,2,id);require(c>0 && out[c+1]==type,"the load's id does not carry the converted value in the shader's type");
        if(cls){c=find(out,conv,2,out[c+3]);require(c>0,"bitcast operand is not the conversion");}
        require(find(out,22,1,scalar_of(out[c+1]))>0,"conversion result is not float");
        const size_t ld=find(out,61,2,out[c+3]);require(ld>0,"conversion operand is not a load");
        require(scalar_of(out[ld+1])==comp,"load is not a load of the integer input type");
    }
    require(find(out,59,2,sIn1)>0 && out[find(out,59,2,sIn1)+1]==sPtrInV4 && find(out,61,2,sLdB)>0,"untouched input changed");
    return out;
}
void check_mixed(){   // one call: location 2 bitcast-retyped to uint, location 0 converted from uint -> both rules in one module
    const Module in=vertex(false);Module out;std::string why;const uint32_t loc[2]={2,0},want[2]={2,4};
    require(xbg_vertex_retype(in,loc,2,want,out,&why),"mixed rewrite refused");
    require(count_ops(out,112)==2 && count_ops(out,124)==3,"mixed rewrite: one conversion (+ the shader's own OpConvertUToF) and two bitcasts (+ the shader's own) expected");
    const size_t cb=find(out,112,2,ldB);require(cb>0 && out[cb+1]==tVec4,"position load not converted under its id");
}
void write_words(const std::string &path,const Module &m){std::ofstream(path,std::ios::binary).write(reinterpret_cast<const char*>(m.data()),m.size()*4);}
std::vector<uint32_t> read_words(const char *path){
    std::ifstream f(path,std::ios::binary);std::vector<char> b((std::istreambuf_iterator<char>(f)),std::istreambuf_iterator<char>());
    std::vector<uint32_t> w(b.size()/4);memcpy(w.data(),b.data(),w.size()*4);return w;}
}

int main(int argc,char **argv){
    try{
        if(argc>2 && !strcmp(argv[1],"--emit")){   // [vtxscaled] synthetic modules and their variants for spirv-val
            const std::string dir=argv[2];int n=0;static const char *const cn[3]={"f","i","u"};
            for(uint32_t count=1;count<=4;count++)for(uint32_t cls=0;cls<3;cls++){
                const std::string base=dir+"/scaled_"+cn[cls]+std::to_string(count);
                write_words(base+".spv",scaled_vertex(count,cls));++n;
                write_words(base+".s2f.spv",check_convert(count,cls,1));++n;write_words(base+".u2f.spv",check_convert(count,cls,2));++n;}
            {const Module in=vertex(false);Module out;const uint32_t loc[2]={2,0},want[2]={2,4};require(xbg_vertex_retype(in,loc,2,want,out),"mixed rewrite refused");
             write_words(dir+"/mixed.spv",in);write_words(dir+"/mixed.out.spv",out);n+=2;}
            std::cout<<"vertex_retype emit: "<<n<<" modules\n";return 0;
        }
        if(argc>1){
            int ok=0,refused=0;
            for(int a=1;a<argc;a++){
                const auto w=read_words(argv[a]);std::vector<uint32_t> locs;
                for(size_t p=5;p+3<w.size() && (w[p]>>16);p+=w[p]>>16)if((w[p]&65535)==71 && (w[p]>>16)==4 && w[p+2]==30)locs.push_back(w[p+3]);
                for(uint32_t cls:{2u,1u}){
                    std::vector<uint32_t> loc,want;std::vector<uint32_t> out;std::string why;
                    for(uint32_t l:locs){bool dup=false;for(uint32_t x:loc)dup|=x==l;if(!dup){loc.push_back(l);want.push_back(cls);}}
                    // outputs share location numbers: try each location alone so an Output one does not refuse the module
                    std::vector<uint32_t> in_loc,in_want;
                    for(size_t i=0;i<loc.size();i++){std::vector<uint32_t> o;if(xbg_vertex_retype(w,&loc[i],1,&want[i],o,&why)){in_loc.push_back(loc[i]);in_want.push_back(cls);}}
                    if(!xbg_vertex_retype(w,in_loc.data(),in_loc.size(),in_want.data(),out,&why)){++refused;std::cerr<<argv[a]<<": "<<why<<"\n";continue;}
                    std::ofstream(std::string(argv[a])+(cls==2?".u.spv":".s.spv"),std::ios::binary).write(reinterpret_cast<const char*>(out.data()),out.size()*4);++ok;
                }
            }
            std::cout<<"vertex_retype files: written="<<ok<<" refused="<<refused<<"\n";return 0;
        }
        check_rewrite(2);check_rewrite(1);check_noop_and_refusal();
        for(uint32_t count=1;count<=4;count++)for(uint32_t cls=0;cls<3;cls++)for(uint32_t kind=1;kind<=2;kind++)check_convert(count,cls,kind);
        check_mixed();
        {uint32_t k=0;require(xbg_vertex_scaled_int(66,&k)==68 && k==2 && xbg_vertex_scaled_int(67,&k)==69 && k==1 && xbg_vertex_scaled_int(68,&k)==0 && k==0 && xbg_vertex_scaled_int(90)==0,"scaled -> integer format");}
        require(xbg_vertex_format_class(41)==2 && xbg_vertex_format_class(42)==1 && xbg_vertex_format_class(37)==0 && xbg_vertex_format_class(109)==0,"format class");
        std::cout<<"vertex_retype_test: ok\n";return 0;
    }catch(const std::exception &e){std::cerr<<"vertex_retype_test: "<<e.what()<<"\n";return 1;}
}
