// [ETERNAL] Pool-index programs (xbg_spirv_poolidx) over the real bundles: builds every bundle that reads a pool array, evaluates
// it with zero / random uniform memory (coverage and timing), and checks one known material lookup by value.
// Build (host): clang++ -std=c++17 -O2 poolidx_test.cpp ../xbg_spirv_poolidx.cpp -o poolidx_test
// Run:          poolidx_test <dir with .xgs bundles>
#include "../xbg_spirv_poolidx.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace {
struct Binding{uint32_t binding,type,count,bytes,source,slot,sampler_slot,flags;};
struct Mem{std::map<uint32_t,std::vector<uint32_t>> slot;std::mt19937 *rng=nullptr;uint32_t fill_mode=0;};
bool read_word(void *ctx,uint32_t slot,uint32_t word,uint32_t *out){
    Mem &m=*static_cast<Mem*>(ctx);if(word>=16384)return false;
    if(m.fill_mode==2){*out=(*m.rng)();return true;}
    auto f=m.slot.find(slot);if(f==m.slot.end() || word>=f->second.size()){*out=0;return true;}
    *out=f->second[word];return true;
}
bool load(const std::string &path,std::vector<Binding> &bs,std::vector<uint32_t> &spv){
    std::ifstream f(path,std::ios::binary);std::vector<char> b((std::istreambuf_iterator<char>(f)),{});if(b.size()<32)return false;
    uint32_t h[8];memcpy(h,b.data(),32);if(h[0]!=0x31534758)return false;
    bs.resize(h[3]);memcpy(bs.data(),b.data()+32,h[3]*32);size_t off=32+size_t(h[3])*32+size_t(h[4])*4+h[5];
    spv.resize(h[6]/4);memcpy(spv.data(),b.data()+off,h[6]);return true;
}
bool program(const std::vector<Binding> &bs,const std::vector<uint32_t> &spv,XbgPoolIdxProgram &p){
    std::vector<XbgPoolIdxArray> arrays;std::vector<XbgPoolIdxUbo> ubos;
    for(auto &b:bs){if(b.source==12){unsigned dim=(b.flags>>8)&7;if(dim==1||dim==2)arrays.push_back({b.binding,uint8_t(dim==1?0:1)});}
        if(b.source==2)ubos.push_back({b.binding,b.slot});}
    return xbg_spirv_poolidx_build(spv.data(),spv.size(),arrays.data(),arrays.size(),ubos.data(),ubos.size(),p);
}
} // namespace

int main(int argc,char **argv){
    if(argc<2){fprintf(stderr,"usage: poolidx_test <bundle dir>\n");return 2;}
    int fails=0;
    // 1. The material lookup of 0102e5f95e1cca30: m = c4[3].x & 0xffff, id = u16 at byte m * 10 + 4 of c12 (slot 9).
    {std::vector<Binding> bs;std::vector<uint32_t> spv;XbgPoolIdxProgram p;
        if(!load(std::string(argv[1])+"/0102e5f95e1cca30.xgs",bs,spv) || !program(bs,spv,p)){printf("FAIL: 0102e5f95e1cca30 not built\n");++fails;}
        else{Mem m;m.slot[1].assign(64,0);m.slot[1][12]=5;m.slot[9].assign(4096,0);m.slot[9][13]=0x1234u<<16;   // byte 54 = word 13, upper half
            XbgPoolIdxScratch s;XbgPoolIdxResult r;xbg_spirv_poolidx_eval(p,read_word,&m,s,r);
            bool ok=r.narrow==p.access.size() && std::count(r.ids[0].begin(),r.ids[0].end(),0x1234u)>0;for(uint32_t id:r.ids[0])ok=ok && (id==0x1234 || id==0);
            printf("0102e5f95e1cca30: nodes=%zu accesses=%zu narrow=%u wide=%u ids=",p.nodes.size(),p.access.size(),r.narrow,r.wide);for(uint32_t id:r.ids[0])printf("%#x ",id);printf("%s\n",ok?"OK":"FAIL");if(!ok)++fails;
            m.slot[1][12]=7;m.slot[9][18]=0x00004321u;   // byte 74 = word 18, lower half
            XbgPoolIdxResult r2;xbg_spirv_poolidx_eval(p,read_word,&m,s,r2);ok=std::count(r2.ids[0].begin(),r2.ids[0].end(),0x4321u)>0;for(uint32_t id:r2.ids[0])ok=ok && (id==0x4321 || id==0);printf("  m=7 -> %s\n",ok?"OK":"FAIL");if(!ok)++fails;}}
    // 2. Every bundle with a pool array.
    size_t bundles=0,built=0,accesses=0,nodes=0,max_nodes=0,truncated=0;uint64_t narrow0=0,wide0=0,narrowR=0,wideR=0,ids0=0,idsR=0,reads=0;double us=0;size_t all_narrow0=0;
    std::map<uint32_t,size_t> dyn_hist;std::mt19937 rng(1);
    for(auto &e:std::filesystem::directory_iterator(argv[1])){if(e.path().extension()!=".xgs")continue;
        std::vector<Binding> bs;std::vector<uint32_t> spv;if(!load(e.path().string(),bs,spv))continue;bool pool=false;for(auto &b:bs)pool|=b.source==12;if(!pool)continue;
        ++bundles;XbgPoolIdxProgram p;if(!program(bs,spv,p)){printf("not built: %s\n",e.path().filename().string().c_str());continue;}
        ++built;accesses+=p.access.size();nodes+=p.nodes.size();if(p.nodes.size()>max_nodes)max_nodes=p.nodes.size();truncated+=p.truncated;
        static XbgPoolIdxScratch s;Mem z;XbgPoolIdxResult r;{XbgPoolIdxResult warm;xbg_spirv_poolidx_eval(p,read_word,&z,s,warm);}   // timed: the second run (scratch warm)
        auto t0=std::chrono::steady_clock::now();xbg_spirv_poolidx_eval(p,read_word,&z,s,r);auto t1=std::chrono::steady_clock::now();
        us+=std::chrono::duration<double,std::micro>(t1-t0).count();narrow0+=r.narrow;wide0+=r.wide;ids0+=r.ids[0].size()+r.ids[1].size();reads+=r.reads;if(!r.wide)++all_narrow0;
        for(auto &a:p.access)if(a.dyn)++dyn_hist[a.dyn];
        Mem rnd;rnd.rng=&rng;rnd.fill_mode=2;XbgPoolIdxResult rr;xbg_spirv_poolidx_eval(p,read_word,&rnd,s,rr);narrowR+=rr.narrow;wideR+=rr.wide;idsR+=rr.ids[0].size()+rr.ids[1].size();}
    printf("bundles with pools %zu, built %zu, accesses %zu, nodes %zu (max %zu, truncated %zu)\n",bundles,built,accesses,nodes,max_nodes,truncated);
    printf("zero memory: narrow %llu wide %llu (bundles fully narrow %zu), ids %llu, reads %llu, eval %.1f us total (%.2f us/bundle)\n",(unsigned long long)narrow0,(unsigned long long)wide0,all_narrow0,
        (unsigned long long)ids0,(unsigned long long)reads,us,built?us/built:0);
    printf("random memory: narrow %llu wide %llu ids %llu\n",(unsigned long long)narrowR,(unsigned long long)wideR,(unsigned long long)idsR);
    printf("dynamic-table slots of accesses:");for(auto &d:dyn_hist)printf(" %#x:%zu",d.first,d.second);printf("\n");
    if(built!=bundles)++fails;
    printf(fails?"FAILED\n":"PASSED\n");return fails?1:0;
}
