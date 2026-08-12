#include "engine/dictionary.h"
#include "engine/lexicon_cache.h"
#include "engine/pinyin_engine.h"
#include "engine/pinyin_lattice.h"
#include "ime/ui/ime_ui_logic.h"
#include "common/com_utils.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
bool Has(const shuru::EngineQueryResult& r,const std::wstring& w,size_t max=99){for(size_t i=0;i<r.candidates.size()&&i<max;++i)if(r.candidates[i].text==w)return true;return false;}
long long Ms(std::chrono::steady_clock::time_point a){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-a).count();}
}
int wmain(int argc,wchar_t**argv){
 using namespace shuru; namespace fs=std::filesystem; const fs::path lex=argc>1?argv[1]:L"data/lexicon"; const fs::path temp=fs::temp_directory_path()/(L"facai-p1-test-"+std::to_wstring(GetCurrentProcessId()));fs::remove_all(temp);fs::create_directories(temp);const fs::path local_app_data=temp/L"local-app-data";fs::create_directories(local_app_data);if(!SetEnvironmentVariableW(L"LOCALAPPDATA",local_app_data.c_str()))return 31;
 Dictionary d;if(!d.LoadFromUtf8Lines({"old\t旧\t7","bad","new\t新\t20\t8\t1700000000","broken\t坏\t2\tx\t1"},true))return 1;auto e=d.SnapshotUserEntries();if(e.size()!=2)return 2;
 const auto score_now=Dictionary::ComputeLearningScore(8,1700000000,1700000000);const auto score_old=Dictionary::ComputeLearningScore(8,1700000000,1700000000+180*86400ll);if(score_now<=score_old||Dictionary::ComputeLearningScore(1,1700000000,1700000000)>=score_now)return 3;
 d.IncreaseUserWord("undo",L"撤销",20,0,1700000000);if(!d.DecreaseUserWord("undo",L"撤销",20))return 4;
 const auto user=temp/L"user.txt";if(!d.SaveUserToFile(user.wstring()))return 5;Dictionary migrated;if(!migrated.LoadFromFile(user.wstring(),true))return 6;auto me=migrated.SnapshotUserEntries();if(me.size()!=2)return 7;
 const auto source=(lex/L"base_dict.txt").wstring(),cache=(temp/L"base.bin").wstring();auto t=std::chrono::steady_clock::now();Dictionary text;DeleteFileW(cache.c_str());if(!text.LoadFromFile(source))return 8;const auto text_ms=Ms(t);if(!BuildLexiconCache(source,cache))return 9;std::vector<CachedLexiconLine> rows;t=std::chrono::steady_clock::now();if(!LoadLexiconCache(source,cache,&rows))return 10;const auto cache_ms=Ms(t);{std::fstream f(cache,std::ios::binary|std::ios::in|std::ios::out);f.seekp(20);char x=0x55;f.write(&x,1);}if(LoadLexiconCache(source,cache,&rows))return 11;
 auto mohu_exact=text.LookupExact("mohu");if(std::none_of(mohu_exact.begin(),mohu_exact.end(),[](const Candidate&c){return c.text==L"模糊";}))return 29;
 PinyinEngine engine;if(!engine.Initialize(lex.wstring()))return 12;auto mhu_debug=engine.Query("mhu",9),sunguo_debug=engine.Query("sunguo",9);QueryOptions strict_mhu;strict_mhu.fuzzy_enabled=false;auto mhu_strict=engine.Query("mhu",9,strict_mhu);if(!Has(mhu_debug,L"模糊",5)||Has(mhu_strict,L"模糊",9)||!Has(sunguo_debug,L"孙国",9)){std::cerr<<"mhu count="<<mhu_debug.candidates.size()<<" normalized="<<PinyinEngine::NormalizeInput("mhu")<<" strict_has_mohu="<<Has(mhu_strict,L"模糊",9)<<"\n";for(const auto&c:mhu_debug.candidates)std::cerr<<"  text="<<shuru::WideToUtf8(c.text)<<" pinyin="<<c.pinyin<<" cost="<<c.match_cost<<" score="<<c.ranking_score<<"\n";std::cerr<<"sunguo count="<<sunguo_debug.candidates.size()<<"\n";for(const auto&c:sunguo_debug.candidates)std::cerr<<"  text="<<shuru::WideToUtf8(c.text)<<" pinyin="<<c.pinyin<<" cost="<<c.match_cost<<" score="<<c.ranking_score<<"\n";return 13;}
 auto full=engine.Query("suixinshuru",512);if(!Has(full,L"随心输入",512)||full.matched_pinyin_len!=11){std::wcerr<<L"suixinshuru coverage="<<full.matched_pinyin_len<<L" count="<<full.candidates.size()<<L"\n";return 16;}
 auto multi=engine.Query("womenzhidao",90);if(!Has(multi,L"我们知道",90)||multi.matched_pinyin_len!=11){std::wcerr<<L"womenzhidao coverage="<<multi.matched_pinyin_len<<L" count="<<multi.candidates.size()<<L"\n";return 17;}
 auto xian=engine.Query("xian",90),xi_an=engine.Query("xi'an",90);if(Has(xi_an,L"先",90)||!Has(xian,L"先",90))return 19;
 auto lattice=shuru::pinyin_data::BuildSyllableLattice("xian");bool ambiguous=false;for(const auto&p:lattice)if(p.edges.size()==2&&p.complete)ambiguous=true;if(!ambiguous)return 20;
 auto tail=shuru::pinyin_data::BuildSyllableLattice("niha");bool partial_syllable=false;for(const auto&p:tail)if(!p.edges.empty()&&p.edges.back().partial)partial_syllable=true;if(!partial_syllable)return 21;
 auto mixed=engine.Query("nihr",90);if(!Has(mixed,L"你好人",90)&&!Has(mixed,L"你好",90))return 22;
 auto partial_for_plan=engine.Query("suixinzzz",90);auto it=std::find_if(partial_for_plan.candidates.begin(),partial_for_plan.candidates.end(),[](const Candidate&c){return c.text==L"随心";});if(it==partial_for_plan.candidates.end()||it->covered_input_len!=6)return 23;auto plan=PlanCandidateCommit("suixinzzz",*it);if(plan.remaining!="zzz"||plan.learned_input!="suixin")return 24;
 Candidate zero;zero.text=L"零";auto zero_plan=PlanCandidateCommit("zzz",zero);if(zero_plan.has_coverage)return 25;
 auto full_plan_result=engine.Query("suixinshuru",512);auto fit=std::find_if(full_plan_result.candidates.begin(),full_plan_result.candidates.end(),[](const Candidate&c){return c.text==L"随心输入";});if(fit==full_plan_result.candidates.end()||!PlanCandidateCommit("suixinshuru",*fit).remaining.empty())return 26;
 auto stable1=engine.Query("womenzhidaosuixinshuru",20),stable2=engine.Query("womenzhidaosuixinshuru",20);if(stable1.candidates.size()!=stable2.candidates.size())return 27;for(size_t i=0;i<stable1.candidates.size();++i)if(stable1.candidates[i].text!=stable2.candidates[i].text||stable1.candidates[i].covered_input_len!=stable2.candidates[i].covered_input_len)return 28;
 auto partial=engine.Query("suixinzzz",90);if(!Has(partial,L"随心",90)||partial.matched_pinyin_len<6){std::wcerr<<L"suixinzzz coverage="<<partial.matched_pinyin_len<<L" count="<<partial.candidates.size()<<L"\n";return 18;}
 engine.Learn("nihao",L"拟好");if(!Has(engine.Query("nihao",9),L"你好",5)||!engine.UndoLastLearning())return 14;
 const std::vector<std::string> typing_sequence={"r","re","ren","renz","renzh","renzhe","renzhen"};
 std::vector<long long> typing_query_us;for(int round=0;round<20;++round)for(const auto&input:typing_sequence){auto q0=std::chrono::steady_clock::now();auto typed=engine.Query(input,90);typing_query_us.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-q0).count());if(input=="renzhen"&&!Has(typed,L"认真",9))return 30;}
 std::sort(typing_query_us.begin(),typing_query_us.end());auto typing_pct=[&](double p){return typing_query_us[static_cast<size_t>(p*(typing_query_us.size()-1))];};
 std::vector<long long> q;for(int i=0;i<300;++i){auto q0=std::chrono::steady_clock::now();engine.Query(i%2?"nihao":"mhu",9);q.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-q0).count());}std::sort(q.begin(),q.end());auto pct=[&](double p){return q[static_cast<size_t>(p*(q.size()-1))];};
 std::cout<<"P1 benchmark text_load_ms="<<text_ms<<" cache_validate_ms="<<cache_ms<<" cache_bytes="<<fs::file_size(cache)<<" query_us P50="<<pct(.50)<<" P95="<<pct(.95)<<" P99="<<pct(.99)<<" typing_query_us P95="<<typing_pct(.95)<<" P99="<<typing_pct(.99)<<"\n";if(pct(.99)>200000||typing_pct(.99)>50000||cache_ms>text_ms*5+500)return 15;fs::remove_all(temp);return 0;
}
