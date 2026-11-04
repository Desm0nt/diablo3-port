// Offline census of D3 SPIR-V bundles (docs/d3/D3_PERF_CANDIDATES.md): runs the runtime rewrite chain (nan_minmax with NAN_SAT ->
// ldc_fold -> forward temps -> ldc_canon -> halfflag -> f2i -> texcoord unguard, psize strip for VS), counts hits and patterns,
// writes per_module.tsv in the current directory and, with a second argument, the rewritten modules (<key>.spv) for spirv-val.
// Linux / WSL, from the repository root: E=dm/gfx/vulkan/engine; g++ -O2 -std=c++17 -I$E tools/d3_spv_census.cpp
//        $E/xbg_spirv_ldc.cpp $E/xbg_spirv_temps.cpp $E/xbg_spirv_peep.cpp -o d3_spv_census && ./d3_spv_census work/shaders-vk [outdir]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <dirent.h>
#include "xbg_spirv_ldc.h"
#include "xbg_spirv_temps.h"
#include "xbg_spirv_peep.h"

static size_t nan_minmax_swap(std::vector<uint32_t> &w,bool sat,size_t &sat_kept,size_t &fclamp_all){
    uint32_t glsl=0;size_t k=0;std::unordered_map<uint32_t,uint32_t> fconst;
    for(size_t i=5;i<w.size();){
        uint32_t op=w[i]&0xFFFFu,n=w[i]>>16;if(!n||i+n>w.size())break;
        if(op==43&&n==4)fconst[w[i+2]]=w[i+3];
        if(op==11&&n>=5&&!memcmp(&w[i+2],"GLSL.std.450",13))glsl=w[i+1];
        else if(op==12&&n>=5&&glsl&&w[i+3]==glsl){uint32_t &e=w[i+4];
            if(e==43&&n>=8){++fclamp_all;auto lo=fconst.find(w[i+6]),hi=fconst.find(w[i+7]);
                if(lo!=fconst.end()&&hi!=fconst.end()&&lo->second==0u&&hi->second==0x3f800000u){++sat_kept;if(sat){i+=n;continue;}}}
            if(e==37){e=79;++k;}else if(e==40){e=80;++k;}else if(e==43){e=81;++k;}}
        i+=n;
    }
    return k;
}
struct Census{size_t insts=0,kill=0,demote=0,fragdepth=0,img_write=0,ssbo_store=0,atomics=0,subgroup=0,trunc=0,isnan=0,samples=0,fetch=0,gather=0,
    unpackhalf=0,packhalf=0,select=0,deriv=0,ldc_shr=0,glsl_ext=0,nmin=0,nmax=0,fmin=0,fmax=0,fclamp=0,nclamp=0,bitcast=0,convert=0,psize_store=0,loops=0,branches=0;};
static void census(const std::vector<uint32_t> &w,Census &c){
    uint32_t glsl=0;std::map<uint32_t,uint32_t> builtin;
    for(size_t i=5;i<w.size();){uint32_t op=w[i]&0xFFFFu,n=w[i]>>16;if(!n||i+n>w.size())break;
        if(op==11&&n>=5&&!memcmp(&w[i+2],"GLSL.std.450",13))glsl=w[i+1];
        if(op==71&&n>=4&&w[i+2]==11)builtin[w[i+1]]=w[i+3];
        if(op>=54)++c.insts;
        switch(op){
        case 252:++c.kill;break;case 5380:++c.demote;break;
        case 99:++c.img_write;break;
        case 227:case 228:case 229:case 230:case 231:case 232:case 233:case 234:case 235:case 236:case 237:case 238:case 239:case 240:case 241:case 242:++c.atomics;break;
        case 87:case 88:case 89:case 90:case 91:case 92:case 93:case 94:++c.samples;break;
        case 95+0x1000:break;
        case 169:++c.select;break;
        case 207:case 208:case 209:case 210:case 211:case 212:case 213:case 214:case 215:++c.deriv;break;
        case 124:++c.bitcast;break;
        case 109:case 110:case 111:case 112:++c.convert;break;
        case 246:++c.loops;break;case 250:++c.branches;break;
        case 156:++c.isnan;break;
        case 12:if(n>=5&&w[i+3]==glsl){uint32_t e=w[i+4];if(e==3)++c.trunc;if(e==62)++c.unpackhalf;if(e==58)++c.packhalf;if(e==79)++c.nmin;if(e==80)++c.nmax;if(e==37)++c.fmin;if(e==40)++c.fmax;if(e==43)++c.fclamp;if(e==81)++c.nclamp;++c.glsl_ext;}break;
        default:if(op>=333&&op<=366)++c.subgroup;break;
        }
        if(op==95)++c.fetch;if(op==96||op==97)++c.gather;
        i+=n;}
    (void)builtin;
}
int main(int argc,char**argv){
    const char *dir=argc>1?argv[1]:".";DIR *d=opendir(dir);if(!d){perror(dir);return 1;}
    std::vector<std::string> files;for(dirent *e;(e=readdir(d));){std::string s=e->d_name;if(s.size()>4&&s.substr(s.size()-4)==".xgs")files.push_back(s);}closedir(d);
    size_t mods[6]={},fold=0,fwd=0,canon=0,canon_m=0,half=0,f2i=0,psize=0,texnan=0,minmax_all=0,sat_kept=0,fclamp_all=0;
    size_t mod_canon=0,mod_half=0,mod_f2i=0,mod_texnan=0,mod_sat=0,mod_fold=0;
    Census cs[6];size_t words_before=0,words_after=0;
    FILE *per=fopen("per_module.tsv","w");fprintf(per,"key\tstage\tinsts\tsat\tnminmax\tcanon\thalf\tf2i\ttexnan\tpsize\tkill\tsamples\tfetch\tunpackhalf\tselect\tisnan\tconvert\tbitcast\n");
    for(auto &f:files){
        std::string p=std::string(dir)+"/"+f;FILE *fp=fopen(p.c_str(),"rb");if(!fp)continue;fseek(fp,0,SEEK_END);long n=ftell(fp);rewind(fp);std::vector<uint8_t> b(n);fread(b.data(),1,n,fp);fclose(fp);
        uint32_t h[8];memcpy(h,b.data(),32);if(h[0]!=0x31534758)continue;uint32_t stage=h[2],nb=h[3],ni=h[4],c1=h[5],spv=h[6];
        size_t off=32+size_t(nb)*32+size_t(ni)*4+c1;if(off+spv>b.size())continue;
        std::vector<uint32_t> w(spv/4);memcpy(w.data(),b.data()+off,spv);if(stage<6)++mods[stage];words_before+=w.size();
        size_t sk=0,fa=0;size_t k=nan_minmax_swap(w,true,sk,fa);minmax_all+=k;sat_kept+=sk;fclamp_all+=fa;if(sk)++mod_sat;
        std::vector<uint32_t> out;
        size_t a=xbg_spirv_ldc_fold(w.data(),w.size(),out);if(a){w.swap(out);fold+=a;++mod_fold;}
        a=xbg_spirv_forward_temps(w.data(),w.size(),out);if(a){w.swap(out);fwd+=a;}
        Census c;census(w,c);
        size_t m=0;size_t kc=xbg_spirv_ldc_canon(w.data(),w.size(),out,&m);if(kc){w.swap(out);canon+=kc;canon_m+=m;++mod_canon;}
        size_t kh=xbg_spirv_halfflag_fold(w.data(),w.size(),out);if(kh){w.swap(out);half+=kh;++mod_half;}
        size_t kf=xbg_spirv_f2i_notrunc(w.data(),w.size(),out);if(kf){w.swap(out);f2i+=kf;++mod_f2i;}
        size_t kt=xbg_spirv_texcoord_unguard(w.data(),w.size(),out);if(kt){texnan+=kt;++mod_texnan;w.swap(out);}
        size_t kp=0;if(stage==0){kp=xbg_spirv_strip_pointsize(w.data(),w.size(),out);if(kp)++psize;}
        words_after+=w.size();
        if(argc>2){std::string o=std::string(argv[2])+"/"+f.substr(0,16)+".spv";FILE*q=fopen(o.c_str(),"wb");fwrite(w.data(),4,w.size(),q);fclose(q);}
        Census &t=cs[stage<6?stage:5];
        #define ADD(x) t.x+=c.x;
        ADD(insts)ADD(kill)ADD(demote)ADD(img_write)ADD(atomics)ADD(subgroup)ADD(trunc)ADD(isnan)ADD(samples)ADD(fetch)ADD(gather)ADD(unpackhalf)ADD(packhalf)ADD(select)ADD(deriv)ADD(nmin)ADD(nmax)ADD(fmin)ADD(fmax)ADD(fclamp)ADD(nclamp)ADD(bitcast)ADD(convert)ADD(loops)ADD(branches)
        fprintf(per,"%s\t%u\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\t%zu\n",f.substr(0,16).c_str(),stage,c.insts,sk,k,kc,kh,kf,kt,kp,c.kill,c.samples,c.fetch,c.unpackhalf,c.select,c.isnan,c.convert,c.bitcast);
    }
    fclose(per);
    printf("modules vs=%zu ps=%zu gs=%zu cs=%zu\n",mods[0],mods[4],mods[3],mods[5]);
    printf("nan_minmax swaps (NAN_SAT=1 keeps sat)=%zu  FClamp total=%zu  FClamp(x,0,1) saturates=%zu in %zu modules\n",minmax_all,fclamp_all,sat_kept,mod_sat);
    printf("ldc_fold=%zu (%zu modules) forward_temps=%zu\n",fold,mod_fold,fwd);
    printf("ldc_canon=%zu merges=%zu modules=%zu | halfflag=%zu modules=%zu | f2i_notrunc=%zu modules=%zu | texcoord guards=%zu modules=%zu | psize VS=%zu\n",canon,canon_m,mod_canon,half,mod_half,f2i,mod_f2i,texnan,mod_texnan,psize);
    printf("words before=%zu after=%zu\n",words_before,words_after);
    const char *sn[6]={"vs","hs","ds","gs","ps","cs"};
    for(int s=0;s<6;s++){if(!mods[s])continue;Census &c=cs[s];
        printf("[%s] insts=%zu kill=%zu demote=%zu img_write=%zu atomics=%zu subgroup=%zu trunc=%zu isnan=%zu samples=%zu fetch=%zu gather=%zu unpackhalf=%zu packhalf=%zu select=%zu deriv=%zu NMin=%zu NMax=%zu FMin=%zu FMax=%zu FClamp=%zu NClamp=%zu bitcast=%zu convert=%zu loops=%zu branches=%zu\n",
            sn[s],c.insts,c.kill,c.demote,c.img_write,c.atomics,c.subgroup,c.trunc,c.isnan,c.samples,c.fetch,c.gather,c.unpackhalf,c.packhalf,c.select,c.deriv,c.nmin,c.nmax,c.fmin,c.fmax,c.fclamp,c.nclamp,c.bitcast,c.convert,c.loops,c.branches);}
    return 0;
}
