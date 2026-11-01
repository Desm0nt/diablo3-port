// [ETERNAL] Which texture-pool ids a draw can index: the slice of a translated shader that computes a pool-array index, run on the CPU
// over the draw's uniform buffers (see xbg_spirv_poolidx.cpp and the bindless namespace of xbg_game.cpp).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// A pool array the shader indexes (by its binding number in the bundle, before bindless::rewrite) and which mirror it belongs to.
struct XbgPoolIdxArray{uint32_t binding;uint8_t kind;};
// A uniform block of the shader (source 2): its binding and NVN uniform slot (Maxwell bank - 3).
struct XbgPoolIdxUbo{uint32_t binding,slot;};
struct XbgPoolIdxNode{uint8_t op=0,lanes=1;uint16_t dyn=0;int32_t a=-1,b=-1,c=-1,d=-1;uint32_t imm[4]={0,0,0,0};uint32_t list_off=0,list_n=0;};
// One OpAccessChain into a pool array: the node of its index, the mirror, and the uniform slots read with a non-constant element
// index somewhere in its slice (the tables a bounded scan falls back to when the index is not narrow at run time).
struct XbgPoolIdxAccess{int32_t node;uint8_t kind;uint16_t dyn;};
struct XbgPoolIdxProgram{std::vector<XbgPoolIdxNode> nodes;std::vector<int32_t> lists;std::vector<XbgPoolIdxAccess> access;
    uint32_t ubo_nodes=0,top_leaves=0,cycles=0;bool truncated=false;
    // The inputs, for a caller-side cache: uniform words read at constant addresses (slot << 24 | word, sorted) and the slots read
    // at computed addresses (tables: the caller keys them by their bound range, their contents are assumed to change only with it).
    std::vector<uint32_t> key_words;uint16_t table_slots=0;};
// Builds the program from a module's words. False: no access into a listed pool array (nothing to evaluate) or unparsable words.
// Values the CPU cannot know (inputs, texture fetches, storage buffers, loop-carried variables, calls) become "any value" leaves.
bool xbg_spirv_poolidx_build(const uint32_t *words,size_t count,const XbgPoolIdxArray *arrays,size_t narrays,const XbgPoolIdxUbo *ubos,size_t nubos,XbgPoolIdxProgram &out);

// Reads one 32-bit word of uniform slot `slot`; false when unbound or out of range (the shader reads 0 there: robust access).
typedef bool (*XbgPoolIdxRead)(void *ctx,uint32_t slot,uint32_t word,uint32_t *out);
struct XbgPoolIdxTerm{uint32_t b,m;};   // the values b | s for every subset s of the bits m (b & m == 0)
struct XbgPoolIdxScratch{std::vector<XbgPoolIdxTerm> terms,tmp;struct Set{uint32_t off;uint16_t n;};std::vector<Set> sets;std::vector<uint32_t> vals,vals2;};
struct XbgPoolIdxResult{
    std::vector<uint32_t> ids[2];   // per mirror kind: every index the accesses can take (may repeat; ids >= the pool length are left in)
    uint16_t scan[2]={0,0};         // per kind: uniform slots to scan for the accesses whose index set was too wide
    uint32_t narrow=0,wide=0,reads=0;
};
// Evaluates every access of the program; appends to r (the caller clears it). `max_ids` caps the ids one access may add.
void xbg_spirv_poolidx_eval(const XbgPoolIdxProgram &p,XbgPoolIdxRead read,void *ctx,XbgPoolIdxScratch &s,XbgPoolIdxResult &r,uint32_t max_ids=256);
