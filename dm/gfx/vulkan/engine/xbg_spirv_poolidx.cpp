// [ETERNAL] Pool-index slices of translated id Tech 7 shaders, evaluated on the CPU.
//
// The translator (patched Ryujinx GenerateBindlessAccess, research/shaders_xlate.md) indexes descriptor arrays that mirror the
// NVN texture pool: tex[min(h & 0xfffff, N - 1)]. In the live set the handle h is built from the draw's uniform buffers, for
// example (u16 at byte m * 10 + 4 of the material table in c12, m = c4[3].x & 0xffff) | (c5[230].y << 20): per-draw words
// select an entry of a table that is itself a uniform buffer. This file follows the index operand of every OpAccessChain into
// a pool array back through the module (single- and multi-store Function variables, phis, selects, integer / float / bit
// operations, vector construction) to uniform-buffer words and constants, and builds a small program of it. Evaluating the
// program with the draw's bound uniform buffers gives every pool index the draw can sample.
//
// Values are sets of "known-bits" terms (b | any subset of m). Unknown leaves (stage inputs, texture fetches, storage buffers,
// loop-carried variables, calls) are one term with every bit unknown; a phi or a select whose condition is not known is the
// union of its sides; too many terms collapse into one term that covers them. The result is a superset of the indices the GPU
// can compute (flow-insensitive: a Function variable is the union of everything stored into it), or "wide" when it is not
// narrow enough to enumerate.
#include "xbg_spirv_poolidx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {
enum Op : uint8_t {TOP,CONST,UBO,UNION,CONSTRUCT,EXTRACT,SHUFFLE,SELECT,
    IADD,ISUB,IMUL,UDIV,SDIV,UMOD,SREM,SMOD,AND,OR,XOR,SHL,SHR,SAR,UMIN,UMAX,SMIN,SMAX,
    IEQ,INE,ULT,ULE,UGT,UGE,SLT,SLE,SGT,SGE,LOR,LAND,LEQ,LNE,
    FADD,FSUB,FMUL,FDIV,FMIN,FMAX,FOEQ,FONE,FOLT,FOLE,FOGT,FOGE,FUEQ,FUNE,FULT,FULE,FUGT,FUGE,
    NOT,LNOT,SNEG,SABS,F2U,F2S,U2F,S2F,FNEG,FABS,FFLOOR,FTRUNC,ISNAN,
    BFI,BFU,BFS,CLAMP,FMA};
bool binary(uint8_t op){return op>=IADD && op<=FUGE;}
bool unary(uint8_t op){return op>=NOT && op<=ISNAN;}
bool boolean_result(uint8_t op){return (op>=IEQ && op<=LNE) || (op>=FOEQ && op<=FUGE) || op==LNOT || op==ISNAN;}

float f32(uint32_t v){float f;memcpy(&f,&v,4);return f;}
uint32_t u32(float f){uint32_t v;memcpy(&v,&f,4);return v;}
uint32_t f2u(float f){if(!(f>0))return 0;if(f>=4294967296.0f)return 0xffffffffu;return uint32_t(f);}   // Maxwell F2I.U32: saturating, NaN -> 0
uint32_t f2s(float f){if(f!=f)return 0;if(f>=2147483648.0f)return 0x7fffffffu;if(f<=-2147483648.0f)return 0x80000000u;return uint32_t(int32_t(f));}
uint32_t shl(uint32_t a,uint32_t k){return k<32?a<<k:0;}
uint32_t shr(uint32_t a,uint32_t k){return k<32?a>>k:0;}
uint32_t sar(uint32_t a,uint32_t k){return uint32_t(int32_t(a)>>(k<32?k:31));}
uint32_t bits(uint32_t n){return n>=32?0xffffffffu:(1u<<n)-1;}

uint32_t exact2(uint8_t op,uint32_t a,uint32_t b){
    const int32_t sa=int32_t(a),sb=int32_t(b);const float fa=f32(a),fb=f32(b);
    switch(op){
    case IADD:return a+b;case ISUB:return a-b;case IMUL:return a*b;
    case UDIV:return b?a/b:0xffffffffu;case SDIV:return sb==0?0xffffffffu:(sa==INT32_MIN && sb==-1)?a:uint32_t(sa/sb);
    case UMOD:return b?a%b:a;case SREM:return sb==0||sb==-1?0:uint32_t(sa%sb);case SMOD:{if(sb==0||sb==-1)return 0;int32_t r=sa%sb;if(r && ((r<0)!=(sb<0)))r+=sb;return uint32_t(r);}
    case AND:return a&b;case OR:return a|b;case XOR:return a^b;case SHL:return shl(a,b);case SHR:return shr(a,b);case SAR:return sar(a,b);
    case UMIN:return a<b?a:b;case UMAX:return a>b?a:b;case SMIN:return sa<sb?a:b;case SMAX:return sa>sb?a:b;
    case IEQ:return a==b;case INE:return a!=b;case ULT:return a<b;case ULE:return a<=b;case UGT:return a>b;case UGE:return a>=b;
    case SLT:return sa<sb;case SLE:return sa<=sb;case SGT:return sa>sb;case SGE:return sa>=sb;
    case LOR:return (a|b)&1;case LAND:return (a&b)&1;case LEQ:return ((a^b)&1)^1;case LNE:return (a^b)&1;
    case FADD:return u32(fa+fb);case FSUB:return u32(fa-fb);case FMUL:return u32(fa*fb);case FDIV:return u32(fa/fb);
    case FMIN:return u32(fb<fa?fb:fa);case FMAX:return u32(fa<fb?fb:fa);
    case FOEQ:return fa==fb;case FONE:return fa<fb||fa>fb;case FOLT:return fa<fb;case FOLE:return fa<=fb;case FOGT:return fa>fb;case FOGE:return fa>=fb;
    case FUEQ:return !(fa<fb||fa>fb);case FUNE:return !(fa==fb);case FULT:return !(fa>=fb);case FULE:return !(fa>fb);case FUGT:return !(fa<=fb);case FUGE:return !(fa<fb);
    }
    return 0;
}
uint32_t exact1(uint8_t op,uint32_t a){
    switch(op){case NOT:return ~a;case LNOT:return (a&1)^1;case SNEG:return 0u-a;case SABS:return int32_t(a)<0?0u-a:a;
    case F2U:return f2u(f32(a));case F2S:return f2s(f32(a));case U2F:return u32(float(a));case S2F:return u32(float(int32_t(a)));
    case FNEG:return a^0x80000000u;case FABS:return a&0x7fffffffu;case FFLOOR:return u32(std::floor(f32(a)));case FTRUNC:return u32(std::trunc(f32(a)));
    case ISNAN:return f32(a)!=f32(a);}
    return 0;
}
// Every value of a term, smallest mask first: s walks the subsets of m.
template<class F>void each(XbgPoolIdxTerm t,F f){uint32_t s=0;do{f(t.b|s);s=(s-t.m)&t.m;}while(s);}
int pop(uint32_t m){return __builtin_popcount(m);}

// Word holding the result id of an instruction: 0 none, 1 (types, labels, imports, strings), 2 (type at word 1).
int result_pos(uint32_t op){
    switch(op){
    case 0:case 2:case 3:case 4:case 5:case 6:case 8:case 10:case 14:case 15:case 16:case 17:case 56:case 62:case 63:case 64:case 71:case 72:case 74:case 75:
    case 99:case 218:case 219:case 220:case 221:case 224:case 225:case 228:case 246:case 247:case 249:case 250:case 251:case 252:case 253:case 254:case 255:
    case 256:case 257:case 317:case 330:case 331:case 332:case 4416:case 5364:case 5365:case 5380:case 5632:case 5633:return 0;
    case 7:case 11:case 73:case 248:return 1;}
    if(op>=19 && op<=39)return 1;   // types
    return 2;
}
// ---- building ----
struct Builder{
    const uint32_t *w;size_t n;uint32_t bound;
    std::vector<uint32_t> at;          // id -> word offset of its defining instruction (0: none)
    std::vector<int32_t> memo;         // id -> node (-1: not built, -2: being built)
    std::vector<uint8_t> lanes_of,storage_of,complex_var;std::vector<uint32_t> binding_of;
    std::unordered_map<uint32_t,std::vector<uint32_t>> stores;   // Function variable -> values stored into it
    std::unordered_map<uint32_t,uint32_t> ubo_slot;              // Uniform variable -> NVN uniform slot
    uint32_t glsl=0;XbgPoolIdxProgram &p;
    explicit Builder(XbgPoolIdxProgram &out):p(out){}
    uint16_t op_of(uint32_t id)const{return id<bound && at[id]?uint16_t(w[at[id]]&0xffffu):0;}
    int32_t add(XbgPoolIdxNode nd){if(p.nodes.size()>=8192){p.truncated=true;return 0;}p.nodes.push_back(nd);return int32_t(p.nodes.size()-1);}
    int32_t top(){++p.top_leaves;return 0;}
    uint8_t lanes(uint32_t type)const{return type<bound?lanes_of[type]:0;}
    uint32_t type_of(uint32_t id)const{return id<bound && at[id] && result_pos(w[at[id]]&0xffffu)==2?w[at[id]+1]:0;}
    bool is_const(int32_t node)const{return node>=0 && p.nodes[size_t(node)].op==CONST;}
    uint16_t dyn(int32_t node)const{return node>=0?p.nodes[size_t(node)].dyn:0;}
    int32_t list_node(uint8_t op,uint8_t ln,const std::vector<int32_t> &kids){
        XbgPoolIdxNode nd;nd.op=op;nd.lanes=ln;nd.list_off=uint32_t(p.lists.size());nd.list_n=uint32_t(kids.size());
        for(int32_t k:kids){p.lists.push_back(k);nd.dyn|=dyn(k);}return add(nd);}
    int32_t node(uint8_t op,uint8_t ln,int32_t a,int32_t b=-1,int32_t c=-1,int32_t d=-1){
        XbgPoolIdxNode nd;nd.op=op;nd.lanes=ln;nd.a=a;nd.b=b;nd.c=c;nd.d=d;nd.dyn=uint16_t(dyn(a)|dyn(b)|dyn(c)|dyn(d));return add(nd);}
    int32_t build(uint32_t id,int depth){
        if(id>=bound)return top();
        if(memo[id]>=0)return memo[id];
        if(memo[id]==-2){++p.cycles;return top();}   // loop-carried (or self-referencing) value
        if(depth>600 || p.truncated)return top();
        memo[id]=-2;const int32_t r=make(id,depth+1);memo[id]=r;return r;
    }
    int32_t make(uint32_t id,int depth){
        const uint32_t o=at[id];if(!o)return top();
        const uint32_t c=w[o]>>16,op=w[o]&0xffffu;const uint32_t *a=w+o;
        auto res_lanes=[&]{uint8_t l=lanes(a[1]);return l;};
        switch(op){
        case 43:{if(lanes(a[1])!=1 || c!=4)return top();XbgPoolIdxNode nd;nd.op=CONST;nd.imm[0]=a[3];return add(nd);}     // OpConstant (32-bit)
        case 41:case 42:{XbgPoolIdxNode nd;nd.op=CONST;nd.imm[0]=op==41;return add(nd);}                                   // OpConstantTrue / False
        case 46:{uint8_t l=res_lanes();if(!l)return top();XbgPoolIdxNode nd;nd.op=CONST;nd.lanes=l;return add(nd);}         // OpConstantNull
        case 44:case 80:{uint8_t l=res_lanes();if(!l || c-3!=l)return top();std::vector<int32_t> k;                          // OpConstantComposite / OpCompositeConstruct
            for(uint32_t i=3;i<c;i++){if(lanes(type_of(a[i]))!=1)return top();k.push_back(build(a[i],depth));}
            return list_node(CONSTRUCT,l,k);}
        case 1:return top();                                                                                                    // OpUndef
        case 61:{uint8_t l=res_lanes();if(!l || c<4)return top();const uint32_t ptr=a[3];if(ptr>=bound)return top();            // OpLoad
            const uint16_t po=op_of(ptr);
            if(po==59){if(storage_of[ptr]!=7 || complex_var[ptr])return top();auto f=stores.find(ptr);if(f==stores.end() || f->second.empty())return top();
                if(f->second.size()==1)return build(f->second[0],depth);
                std::vector<int32_t> k;for(uint32_t v:f->second)k.push_back(build(v,depth));return list_node(UNION,l,k);}
            if(po==65 || po==66){const uint32_t *q=w+at[ptr];const uint32_t qc=q[0]>>16,base=q[3];
                auto s=ubo_slot.find(base);if(s==ubo_slot.end() || (qc!=6 && qc!=7))return top();
                int32_t m0=build(q[4],depth);if(!is_const(m0) || p.nodes[size_t(m0)].imm[0]!=0)return top();
                if((qc==7)!=(l==1) || (qc==6 && l!=4))return top();
                int32_t e=build(q[5],depth),cmp=qc==7?build(q[6],depth):-1;
                XbgPoolIdxNode nd;nd.op=UBO;nd.lanes=l;nd.a=e;nd.b=cmp;nd.imm[0]=s->second;nd.dyn=uint16_t(dyn(e)|dyn(cmp)|(is_const(e)?0:(1u<<(s->second&15))));
                ++p.ubo_nodes;return add(nd);}
            return top();}
        case 245:{uint8_t l=res_lanes();if(!l)return top();std::vector<int32_t> k;for(uint32_t i=3;i+1<c;i+=2)k.push_back(build(a[i],depth));   // OpPhi
            return list_node(UNION,l,k);}
        case 169:{uint8_t l=res_lanes();if(!l || c!=6)return top();int32_t cnd=build(a[3],depth),x=build(a[4],depth),y=build(a[5],depth);return node(SELECT,l,cnd,x,y);}
        case 83:case 124:case 113:case 114:{uint8_t l=res_lanes();if(!l || c!=4)return top();                                  // copy, bitcast, 32-bit int conversions
            if(lanes(type_of(a[3]))!=l)return top();return build(a[3],depth);}
        case 81:{if(c!=5 || res_lanes()!=1)return top();int32_t s=build(a[3],depth);XbgPoolIdxNode nd;nd.op=EXTRACT;nd.a=s;nd.imm[0]=a[4]&3;nd.dyn=dyn(s);return add(nd);}
        case 79:{uint8_t l=res_lanes();if(!l || c!=5u+l)return top();const uint8_t la=lanes(type_of(a[3]));if(!la)return top();
            int32_t x=build(a[3],depth),y=build(a[4],depth);XbgPoolIdxNode nd;nd.op=SHUFFLE;nd.lanes=l;nd.a=x;nd.b=y;nd.c=la;nd.dyn=uint16_t(dyn(x)|dyn(y));
            for(uint8_t i=0;i<l;i++)nd.imm[i]=a[5+i]==0xffffffffu?0:a[5+i];return add(nd);}
        case 12:{uint8_t l=res_lanes();if(!l || a[3]!=glsl || c<6)return top();const uint32_t inst=a[4];const uint32_t na=c-5;const uint32_t *x=a+5;
            auto b1=[&](uint8_t o2){return na==1?node(o2,l,build(x[0],depth)):top();};
            auto b2=[&](uint8_t o2){return na==2?node(o2,l,build(x[0],depth),build(x[1],depth)):top();};
            switch(inst){
            case 37:return b2(FMIN);case 40:return b2(FMAX);case 38:return b2(UMIN);case 41:return b2(UMAX);case 39:return b2(SMIN);case 42:return b2(SMAX);
            case 4:return b1(FABS);case 5:return b1(SABS);case 8:return b1(FFLOOR);case 3:return b1(FTRUNC);
            case 43:case 44:case 45:return na==3?node(CLAMP,l,build(x[0],depth),build(x[1],depth),build(x[2],depth)):top();
            case 50:return na==3?node(FMA,l,build(x[0],depth),build(x[1],depth),build(x[2],depth)):top();}
            return top();}
        case 201:{uint8_t l=res_lanes();if(!l || c!=7)return top();return node(BFI,l,build(a[3],depth),build(a[4],depth),build(a[5],depth),build(a[6],depth));}
        case 202:case 203:{uint8_t l=res_lanes();if(!l || c!=6)return top();return node(op==203?BFU:BFS,l,build(a[3],depth),build(a[4],depth),build(a[5],depth));}
        }
        static const struct{uint16_t spv;uint8_t op;}two[]={{128,IADD},{130,ISUB},{132,IMUL},{134,UDIV},{135,SDIV},{137,UMOD},{138,SREM},{139,SMOD},
            {199,AND},{197,OR},{198,XOR},{196,SHL},{194,SHR},{195,SAR},{170,IEQ},{171,INE},{176,ULT},{178,ULE},{172,UGT},{174,UGE},{177,SLT},{179,SLE},{173,SGT},{175,SGE},
            {166,LOR},{167,LAND},{164,LEQ},{165,LNE},{129,FADD},{131,FSUB},{133,FMUL},{136,FDIV},
            {180,FOEQ},{182,FONE},{184,FOLT},{188,FOLE},{186,FOGT},{190,FOGE},{181,FUEQ},{183,FUNE},{185,FULT},{189,FULE},{187,FUGT},{191,FUGE}};
        static const struct{uint16_t spv;uint8_t op;}one[]={{200,NOT},{168,LNOT},{126,SNEG},{109,F2U},{110,F2S},{112,U2F},{111,S2F},{127,FNEG},{156,ISNAN}};
        for(auto &e:two)if(e.spv==op){uint8_t l=res_lanes();if(!l || c!=5)return top();return node(e.op,l,build(a[3],depth),build(a[4],depth));}
        for(auto &e:one)if(e.spv==op){uint8_t l=res_lanes();if(!l || c!=4)return top();return node(e.op,l,build(a[3],depth));}
        return top();   // inputs, texture results, storage buffers, calls, function parameters, unknown operations
    }
};
} // namespace

bool xbg_spirv_poolidx_build(const uint32_t *w,size_t n,const XbgPoolIdxArray *arrays,size_t narrays,const XbgPoolIdxUbo *ubos,size_t nubos,XbgPoolIdxProgram &out){
    out=XbgPoolIdxProgram{};if(n<5 || w[0]!=0x07230203u || !narrays)return false;
    const uint32_t bound=w[3];if(!bound || bound>(1u<<22))return false;
    Builder b(out);b.w=w;b.n=n;b.bound=bound;b.at.assign(bound,0);b.memo.assign(bound,-1);b.lanes_of.assign(bound,0);b.storage_of.assign(bound,0);b.complex_var.assign(bound,0);b.binding_of.assign(bound,0xffffffffu);
    std::vector<uint32_t> chains;   // OpAccessChain instructions
    for(size_t i=5;i<n;){const uint32_t c=w[i]>>16,op=w[i]&0xffffu;if(!c || i+c>n)return false;const uint32_t *a=w+i;
        const int rp=result_pos(op);
        if(rp && c>uint32_t(rp) && a[rp]<bound)b.at[a[rp]]=uint32_t(i);
        switch(op){
        case 21:case 22:if(c>=3 && a[1]<bound)b.lanes_of[a[1]]=a[2]==32?1:0;break;                                     // OpTypeInt / OpTypeFloat
        case 20:if(c>=2 && a[1]<bound)b.lanes_of[a[1]]=1;break;                                                      // OpTypeBool
        case 23:if(c>=4 && a[1]<bound && a[2]<bound)b.lanes_of[a[1]]=b.lanes_of[a[2]]==1 && a[3]>=2 && a[3]<=4?uint8_t(a[3]):0;break;   // OpTypeVector
        case 71:if(c>=4 && a[1]<bound && a[2]==33)b.binding_of[a[1]]=a[3];break;                                     // OpDecorate Binding
        case 11:if(c>=3 && a[1]<bound){const char *str=reinterpret_cast<const char*>(a+2);if(!strncmp(str,"GLSL.std.450",12))b.glsl=a[1];}break;
        case 59:if(c>=4 && a[2]<bound){b.storage_of[a[2]]=uint8_t(a[3]);if(c>=5 && a[3]==7)b.stores[a[2]].push_back(a[4]);}break;   // initializer = a store
        case 65:case 66:if(c>=5)chains.push_back(uint32_t(i));break;
        case 62:if(c>=3 && a[1]<bound){const uint32_t ptr=a[1];const uint16_t po=b.op_of(ptr);
                if(po==59)b.stores[ptr].push_back(a[2]);
                else if(po==65 || po==66){const uint32_t base=w[b.at[ptr]+3];if(base<bound)b.complex_var[base]=1;}}break;   // partial store: not tracked
        case 63:case 64:case 57:   // OpCopyMemory(Sized), OpFunctionCall: pointer operands may be written
            for(uint32_t k=op==57?4:1;k<(op==57?c:3u);k++)if(a[k]<bound){uint32_t v=a[k];const uint16_t po=b.op_of(v);if(po==65 || po==66)v=w[b.at[v]+3];if(v<bound && b.op_of(v)==59)b.complex_var[v]=1;}
            break;
        default:break;}
        i+=c;}
    // Types of variables / pointers are not needed beyond the storage class; uniform blocks by binding.
    for(uint32_t id=1;id<bound;id++)if(b.op_of(id)==59 && b.binding_of[id]!=0xffffffffu){
        if(b.storage_of[id]==2)for(size_t k=0;k<nubos;k++)if(ubos[k].binding==b.binding_of[id] && ubos[k].slot<16){b.ubo_slot[id]=ubos[k].slot;break;}}
    XbgPoolIdxNode t;t.op=TOP;t.lanes=4;out.nodes.push_back(t);   // node 0: any value
    for(uint32_t ci:chains){const uint32_t *a=w+ci;const uint32_t base=a[3];if(base>=bound || b.op_of(base)!=59 || b.storage_of[base]!=0)continue;   // UniformConstant arrays
        const uint32_t bind=b.binding_of[base];if(bind==0xffffffffu)continue;
        for(size_t k=0;k<narrays;k++)if(arrays[k].binding==bind){const int32_t nd=b.build(a[4],0);out.access.push_back({nd,arrays[k].kind,out.nodes[size_t(nd)].dyn});break;}}
    for(const XbgPoolIdxNode &nd:out.nodes)if(nd.op==UBO){const bool ce=b.is_const(nd.a),cc=nd.b<0 || b.is_const(nd.b);
        if(ce && cc){const uint32_t e=out.nodes[size_t(nd.a)].imm[0];const uint32_t c0=nd.b<0?0:(out.nodes[size_t(nd.b)].imm[0]&3);
            for(uint32_t l=0;l<(nd.b<0?4u:1u);l++)if(e<0x10000000u)out.key_words.push_back(nd.imm[0]<<24 | ((e*4+c0+l)&0xffffffu));}
        else out.table_slots|=uint16_t(1u<<(nd.imm[0]&15));}
    std::sort(out.key_words.begin(),out.key_words.end());out.key_words.erase(std::unique(out.key_words.begin(),out.key_words.end()),out.key_words.end());
    return !out.access.empty();
}

// ---- evaluation ----
namespace {
constexpr size_t K=24;   // terms per value before they collapse into one
struct Eval{
    const XbgPoolIdxProgram &p;XbgPoolIdxScratch &s;XbgPoolIdxRead read;void *ctx;uint32_t reads=0;
    std::vector<XbgPoolIdxTerm> &tmp;
    Eval(const XbgPoolIdxProgram &pp,XbgPoolIdxScratch &ss,XbgPoolIdxRead r,void *c):p(pp),s(ss),read(r),ctx(c),tmp(ss.tmp){}
    XbgPoolIdxScratch::Set get(int32_t node,uint32_t lane)const{if(node<0)return {0,0};const XbgPoolIdxNode &nd=p.nodes[size_t(node)];return s.sets[size_t(node)*4+(lane<nd.lanes?lane:nd.lanes-1u)];}
    static void push(std::vector<XbgPoolIdxTerm> &v,XbgPoolIdxTerm t){t.b&=~t.m;for(auto &x:v)if(x.b==t.b && x.m==t.m)return;v.push_back(t);}
    // Stores tmp as a set (collapsed to one covering term when it has more than K terms).
    XbgPoolIdxScratch::Set store(){
        if(tmp.empty())tmp.push_back({0,0xffffffffu});
        if(tmp.size()>K){uint32_t all=0xffffffffu,any=0;for(auto &t:tmp){all&=t.b;any|=t.b|t.m;}tmp.clear();tmp.push_back({all,any&~all});}
        XbgPoolIdxScratch::Set r{uint32_t(s.terms.size()),uint16_t(tmp.size())};s.terms.insert(s.terms.end(),tmp.begin(),tmp.end());return r;}
    void add_set(XbgPoolIdxScratch::Set x){for(uint32_t i=0;i<x.n;i++)push(tmp,s.terms[x.off+i]);}
    bool single(XbgPoolIdxScratch::Set x,uint32_t &v)const{if(x.n!=1 || s.terms[x.off].m)return false;v=s.terms[x.off].b;return true;}
    void apply2(uint8_t op,XbgPoolIdxTerm a,XbgPoolIdxTerm b){
        if(!a.m && !b.m){push(tmp,{exact2(op,a.b,b.b),0});return;}
        switch(op){
        case AND:{uint32_t bb=a.b&b.b;push(tmp,{bb,((a.b|a.m)&(b.b|b.m))&~bb});return;}
        case LAND:{uint32_t bb=a.b&b.b&1;push(tmp,{bb,((a.b|a.m)&(b.b|b.m))&1&~bb});return;}
        case OR:case LOR:{uint32_t bb=a.b|b.b,mm=(a.m|b.m)&~bb;if(op==LOR){bb&=1;mm&=1;}push(tmp,{bb,mm});return;}
        case XOR:case LNE:case LEQ:{uint32_t mm=a.m|b.m,bb=(a.b^b.b)&~mm;if(op!=XOR){mm&=1;bb=(bb&1)^(op==LEQ?1:0);bb&=~mm;}push(tmp,{bb,mm});return;}
        case SHL:case SHR:case SAR:case UDIV:case UMOD:case IMUL:
            if(!b.m && (op==SHL||op==SHR||op==SAR)){const uint32_t k=b.b;if(op==SHL)push(tmp,{shl(a.b,k),shl(a.m,k)});else if(op==SHR)push(tmp,{shr(a.b,k),shr(a.m,k)});
                else{uint32_t mm=sar(a.m,k);push(tmp,{sar(a.b,k)&~mm,mm});}return;}
            if(!b.m && b.b && !(b.b&(b.b-1))){const uint32_t k=uint32_t(__builtin_ctz(b.b));
                if(op==UDIV){push(tmp,{a.b>>k,a.m>>k});return;}if(op==UMOD){push(tmp,{a.b&(b.b-1),a.m&(b.b-1)});return;}if(op==IMUL){push(tmp,{shl(a.b,k),shl(a.m,k)});return;}}
            break;
        case UMIN:case UMAX:case SMIN:case SMAX:case FMIN:case FMAX:push(tmp,a);push(tmp,b);return;   // the result is one of the operands
        default:break;}
        if(pop(a.m)+pop(b.m)<=6){each(a,[&](uint32_t x){each(b,[&](uint32_t y){push(tmp,{exact2(op,x,y),0});});});return;}
        if(op==IADD || op==ISUB){const uint32_t low=bits(uint32_t(__builtin_ctz(a.m|b.m)));push(tmp,{exact2(op,a.b,b.b)&low,~low});return;}   // carries only move up
        push(tmp,{0,boolean_result(op)?1u:0xffffffffu});
    }
    void apply1(uint8_t op,XbgPoolIdxTerm a){
        if(!a.m){push(tmp,{exact1(op,a.b),0});return;}
        if(op==NOT){push(tmp,{~a.b&~a.m,a.m});return;}
        if(op==LNOT){push(tmp,{(a.b&1)^1,a.m&1});return;}
        if(pop(a.m)<=6){each(a,[&](uint32_t x){push(tmp,{exact1(op,x),0});});return;}
        push(tmp,{0,boolean_result(op)?1u:0xffffffffu});
    }
    // Every value of a set when it has at most `cap` of them.
    bool values(XbgPoolIdxScratch::Set x,std::vector<uint32_t> &out,size_t cap){
        out.clear();for(uint32_t i=0;i<x.n;i++){const XbgPoolIdxTerm t=s.terms[x.off+i];if(pop(t.m)>12)return false;
            bool over=false;each(t,[&](uint32_t v){if(out.size()>=cap){over=true;return;}out.push_back(v);});if(over)return false;}
        return true;}
    void run(){
        s.terms.clear();s.sets.assign(p.nodes.size()*4,{0,0});
        for(size_t ni=0;ni<p.nodes.size();ni++){const XbgPoolIdxNode &nd=p.nodes[ni];
            for(uint32_t j=0;j<nd.lanes;j++){tmp.clear();
                switch(nd.op){
                case TOP:break;
                case CONST:s.sets[ni*4+j]={uint32_t(s.terms.size()),1};s.terms.push_back({nd.imm[j],0});continue;
                case UBO:{
                    // The element (vec4) index set, then each word; a too wide index set reads "anything".
                    if(!values(get(nd.a,0),s.vals,64))break;
                    s.vals2.clear();
                    if(nd.b>=0){if(!values(get(nd.b,0),s.vals2,4)){s.vals2={0,1,2,3};}}else s.vals2.push_back(j);
                    for(uint32_t e:s.vals)for(uint32_t c:s.vals2){uint32_t v=0;++reads;
                        if(e<0x10000000u && read && !read(ctx,nd.imm[0],e*4+(c&3),&v))v=0;push(tmp,{v,0});}
                    break;}
                case UNION:for(uint32_t k=0;k<nd.list_n;k++)add_set(get(p.lists[nd.list_off+k],j));break;
                case CONSTRUCT:if(j<nd.list_n){s.sets[ni*4+j]=get(p.lists[nd.list_off+j],0);continue;}break;
                case EXTRACT:s.sets[ni*4+j]=get(nd.a,nd.imm[0]);continue;
                case SHUFFLE:{const uint32_t c=nd.imm[j],la=uint32_t(nd.c);s.sets[ni*4+j]=c<la?get(nd.a,c):get(nd.b,c-la);continue;}
                case SELECT:{const XbgPoolIdxScratch::Set cs=get(nd.a,j);uint32_t cv;
                    if(single(cs,cv)){s.sets[ni*4+j]=get(cv&1?nd.b:nd.c,j);continue;}else{add_set(get(nd.b,j));add_set(get(nd.c,j));}break;}
                case CLAMP:add_set(get(nd.a,j));add_set(get(nd.b,j));add_set(get(nd.c,j));break;   // one of the three
                case BFI:case BFU:case BFS:{
                    uint32_t off,cnt;const int32_t on=nd.op==BFI?nd.c:nd.b,cn=nd.op==BFI?nd.d:nd.c;
                    if(!single(get(on,j),off) || !single(get(cn,j),cnt)){break;}
                    off&=31;const uint32_t mk=cnt>=32?0xffffffffu:bits(cnt);
                    if(nd.op==BFI){const uint32_t fm=shl(mk,off);const XbgPoolIdxScratch::Set A=get(nd.a,j),B=get(nd.b,j);
                        for(uint32_t x=0;x<A.n;x++)for(uint32_t y=0;y<B.n;y++){const XbgPoolIdxTerm ta=s.terms[A.off+x],tb=s.terms[B.off+y];
                            push(tmp,{(ta.b&~fm)|(shl(tb.b,off)&fm),(ta.m&~fm)|(shl(tb.m,off)&fm)});}}
                    else{const XbgPoolIdxScratch::Set A=get(nd.a,j);
                        for(uint32_t x=0;x<A.n;x++){const XbgPoolIdxTerm ta=s.terms[A.off+x];XbgPoolIdxTerm r{(ta.b>>off)&mk,(ta.m>>off)&mk};
                            if(nd.op==BFS && cnt && cnt<32){const uint32_t sign=1u<<(cnt-1);
                                if(r.m&sign)r={r.b,r.m|~mk};else if(r.b&sign)r.b|=~mk;}
                            push(tmp,r);}}
                    break;}
                case FMA:{const XbgPoolIdxScratch::Set A=get(nd.a,j),B=get(nd.b,j),C=get(nd.c,j);uint32_t x,y,z;
                    if(single(A,x) && single(B,y) && single(C,z))tmp.push_back({u32(std::fma(f32(x),f32(y),f32(z))),0});break;}
                default:
                    if(binary(nd.op)){const XbgPoolIdxScratch::Set A=get(nd.a,j),B=get(nd.b,j);uint32_t x,y;
                        if(single(A,x) && single(B,y)){s.sets[ni*4+j]={uint32_t(s.terms.size()),1};s.terms.push_back({exact2(nd.op,x,y),0});continue;}   // the common case
                        for(uint32_t x=0;x<A.n && tmp.size()<=4*K;x++)for(uint32_t y=0;y<B.n;y++)apply2(nd.op,s.terms[A.off+x],s.terms[B.off+y]);}
                    else if(unary(nd.op)){const XbgPoolIdxScratch::Set A=get(nd.a,j);uint32_t x;
                        if(single(A,x)){s.sets[ni*4+j]={uint32_t(s.terms.size()),1};s.terms.push_back({exact1(nd.op,x),0});continue;}for(uint32_t x=0;x<A.n;x++)apply1(nd.op,s.terms[A.off+x]);}
                    break;}
                if(nd.op!=TOP && tmp.size()>4*K){uint32_t all=0xffffffffu,any=0;for(auto &t:tmp){all&=t.b;any|=t.b|t.m;}tmp.clear();tmp.push_back({all,any&~all});}
                if(nd.op==TOP)tmp.clear();
                s.sets[ni*4+j]=store();}}
    }
};
} // namespace

void xbg_spirv_poolidx_eval(const XbgPoolIdxProgram &p,XbgPoolIdxRead read,void *ctx,XbgPoolIdxScratch &s,XbgPoolIdxResult &r,uint32_t max_ids){
    if(p.access.empty())return;
    Eval e(p,s,read,ctx);e.run();r.reads+=e.reads;
    for(const XbgPoolIdxAccess &a:p.access){const uint32_t k=a.kind&1;
        if(e.values(e.get(a.node,0),s.vals,max_ids)){++r.narrow;r.ids[k].insert(r.ids[k].end(),s.vals.begin(),s.vals.end());}
        else{++r.wide;r.scan[k]|=a.dyn;}}
}
