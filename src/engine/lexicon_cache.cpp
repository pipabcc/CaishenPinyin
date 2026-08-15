#include "lexicon_cache.h"

#include "../common/com_utils.h"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#pragma comment(lib, "bcrypt.lib")

namespace shuru {
namespace {
constexpr char kMagic[8] = {'F','C','P','Y','L','E','X','1'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kMaxRows = 2000000;
constexpr std::uint32_t kMaxField = 1u << 20;
#pragma pack(push, 1)
struct Header { char magic[8]; std::uint32_t version; std::uint32_t count; unsigned char source[32]; unsigned char payload[32]; };
#pragma pack(pop)

bool ReadAll(const std::wstring& path, std::vector<unsigned char>* data) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end); const auto n=in.tellg();
    if (n < 0 || n > static_cast<std::streamoff>(512ull<<20)) return false;
    in.seekg(0); data->resize(static_cast<size_t>(n));
    return data->empty() || !!in.read(reinterpret_cast<char*>(data->data()), n);
}
bool Sha256(const unsigned char* data, size_t size, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objlen=0, cb=0;
    if (BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return false;
    if (BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objlen),sizeof(objlen),&cb,0)<0) { BCryptCloseAlgorithmProvider(alg,0); return false; }
    std::vector<unsigned char> obj(objlen);
    bool ok=BCryptCreateHash(alg,&hash,obj.data(),objlen,nullptr,0,0)>=0 &&
      (size==0 || BCryptHashData(hash,const_cast<PUCHAR>(data),static_cast<ULONG>(size),0)>=0) &&
      BCryptFinishHash(hash,out,32,0)>=0;
    if(hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0); return ok;
}
bool Sha256File(const std::wstring& path, unsigned char out[32]) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return false;
    BCRYPT_ALG_HANDLE alg=nullptr; BCRYPT_HASH_HANDLE hash=nullptr; DWORD objlen=0, cb=0;
    if (BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) return false;
    if (BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objlen),sizeof(objlen),&cb,0)<0) { BCryptCloseAlgorithmProvider(alg,0); return false; }
    std::vector<unsigned char> obj(objlen); std::array<unsigned char,64*1024> block{};
    bool ok=BCryptCreateHash(alg,&hash,obj.data(),objlen,nullptr,0,0)>=0;
    while(ok && in) { in.read(reinterpret_cast<char*>(block.data()),block.size()); const auto n=in.gcount(); if(n>0) ok=BCryptHashData(hash,block.data(),static_cast<ULONG>(n),0)>=0; }
    ok=ok && in.eof() && BCryptFinishHash(hash,out,32,0)>=0;
    if(hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(alg,0); return ok;
}
template<class T> void Put(std::vector<unsigned char>* b,const T& v){ auto p=reinterpret_cast<const unsigned char*>(&v); b->insert(b->end(),p,p+sizeof(v)); }
bool Get(const unsigned char*& p,const unsigned char* e,void* out,size_t n){ if(n>size_t(e-p))return false; std::memcpy(out,p,n);p+=n;return true; }
}

bool BuildLexiconCache(const std::wstring& source_path, const std::wstring& cache_path) {
    std::vector<unsigned char> source; if(!ReadAll(source_path,&source)) return false;
    std::istringstream in(std::string(reinterpret_cast<const char*>(source.data()),source.size()));
    std::vector<unsigned char> payload; std::string line; std::uint32_t count=0;
    while(std::getline(in,line)) {
        if(line.empty()||line[0]=='#'||line[0]==';') continue;
        std::istringstream ss(line); std::string py,word,freq;
        if(!std::getline(ss,py,'\t')||!std::getline(ss,word,'\t')||py.empty()||word.empty()) continue;
        std::int32_t f=1; if(std::getline(ss,freq,'\t')) try { f=std::stoi(freq); } catch(...) { continue; }
        if(py.size()>kMaxField||word.size()>kMaxField||count>=kMaxRows) return false;
        std::uint32_t a=static_cast<std::uint32_t>(py.size()),b=static_cast<std::uint32_t>(word.size()); Put(&payload,a);Put(&payload,b);Put(&payload,f);payload.insert(payload.end(),py.begin(),py.end());payload.insert(payload.end(),word.begin(),word.end());++count;
    }
    Header h{};std::memcpy(h.magic,kMagic,8);h.version=kVersion;h.count=count;
    if(!Sha256(source.data(),source.size(),h.source)||!Sha256(payload.data(),payload.size(),h.payload))return false;
    const std::filesystem::path target(cache_path);
    const std::filesystem::path temp(target.wstring()+L".tmp."+std::to_wstring(GetCurrentProcessId()));
    std::ofstream out(temp,std::ios::binary|std::ios::trunc); if(!out)return false; out.write(reinterpret_cast<char*>(&h),sizeof(h)); if(!payload.empty())out.write(reinterpret_cast<char*>(payload.data()),payload.size());out.close();
    if(!out||!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(temp.c_str());return false;} return true;
}

bool VisitLexiconCache(const std::wstring& source_path, const std::wstring& cache_path,
                       const LexiconRowVisitor& visitor, size_t* row_count) {
    if (!visitor) return false;
    std::vector<unsigned char> cache_data;
    if (!ReadAll(cache_path, &cache_data) || cache_data.size() < sizeof(Header)) {
        return false;
    }
    const auto* h = reinterpret_cast<const Header*>(cache_data.data());
    if (std::memcmp(h->magic, kMagic, 8) != 0 || h->version != kVersion || h->count > kMaxRows) {
        return false;
    }

    unsigned char digest[32];
    if (!Sha256File(source_path, digest) || std::memcmp(digest, h->source, 32) != 0) {
        return false;
    }
    const size_t payload_size = cache_data.size() - sizeof(Header);
    if (!Sha256(cache_data.data() + sizeof(Header), payload_size, digest) ||
        std::memcmp(digest, h->payload, 32) != 0) {
        return false;
    }

    const unsigned char* p = cache_data.data() + sizeof(Header);
    const unsigned char* end = cache_data.data() + cache_data.size();
    size_t count = 0;
    for (std::uint32_t i = 0; i < h->count; ++i) {
        std::uint32_t a = 0, b = 0;
        std::int32_t f = 0;
        if (!Get(p, end, &a, sizeof(a)) || !Get(p, end, &b, sizeof(b)) ||
            !Get(p, end, &f, sizeof(f)) || a > kMaxField || b > kMaxField) {
            return false;
        }
        if (p + a + b > end) {
            return false;
        }
        CachedLexiconLine row;
        row.pinyin.assign(reinterpret_cast<const char*>(p), a);
        p += a;
        row.word_utf8.assign(reinterpret_cast<const char*>(p), b);
        p += b;
        row.frequency = f;
        if (!visitor(std::move(row))) {
            return false;
        }
        ++count;
    }
    if (p != end) return false;
    if (row_count) *row_count = count;
    return true;
}

bool LoadLexiconCache(const std::wstring& source_path, const std::wstring& cache_path, std::vector<CachedLexiconLine>* rows) {
    if(!rows)return false;std::vector<CachedLexiconLine> parsed;
    const bool ok=VisitLexiconCache(source_path,cache_path,[&](CachedLexiconLine&& row){parsed.push_back(std::move(row));return true;});
    if(ok)*rows=std::move(parsed);return ok;
}
} // namespace shuru
