#include "ime/edit_sessions.h"
#include <Windows.h>
#include <msctf.h>
#include <cstdio>
#include <string>

class Sink final : public ITfCompositionSink {
  LONG refs_=1;
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**out) override {if(!out)return E_INVALIDARG;*out=nullptr;if(id==IID_IUnknown||id==IID_ITfCompositionSink){*out=this;AddRef();return S_OK;}return E_NOINTERFACE;}
  ULONG STDMETHODCALLTYPE AddRef() override{return InterlockedIncrement(&refs_);} ULONG STDMETHODCALLTYPE Release() override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}
  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie,ITfComposition*) override{return S_OK;}
};
class Read final : public ITfEditSession {LONG refs_=1;ITfContext*c_;std::wstring*out_;public:Read(ITfContext*c,std::wstring*o):c_(c),out_(o){c_->AddRef();}~Read(){c_->Release();}HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void**p)override{if(!p)return E_INVALIDARG;*p=nullptr;if(id==IID_IUnknown||id==IID_ITfEditSession){*p=this;AddRef();return S_OK;}return E_NOINTERFACE;}ULONG STDMETHODCALLTYPE AddRef()override{return InterlockedIncrement(&refs_);}ULONG STDMETHODCALLTYPE Release()override{auto n=InterlockedDecrement(&refs_);if(!n)delete this;return n;}HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie ec)override{ITfRange*r=nullptr;HRESULT h=c_->GetStart(ec,&r);if(FAILED(h))return h;LONG moved=0;r->ShiftEnd(ec,512,&moved,nullptr);wchar_t b[512]{};ULONG n=0;h=r->GetText(ec,0,b,511,&n);r->Release();if(SUCCEEDED(h))out_->assign(b,n);return h;}};
static bool Run(ITfContext*c,TfClientId id,ITfEditSession*s,DWORD flags=TF_ES_SYNC|TF_ES_READWRITE){HRESULT result=E_FAIL;HRESULT h=c->RequestEditSession(id,s,flags,&result);s->Release();return SUCCEEDED(h)&&SUCCEEDED(result);}
int wmain(){CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);ITfThreadMgr*tm=nullptr;ITfDocumentMgr*doc=nullptr;ITfContext*ctx=nullptr;TfClientId id=0;TfEditCookie ec=0;ITfComposition*comp=nullptr;Sink*sink=new Sink;int rc=1;
 if(FAILED(CoCreateInstance(CLSID_TF_ThreadMgr,nullptr,CLSCTX_INPROC_SERVER,IID_ITfThreadMgr,(void**)&tm))||FAILED(tm->Activate(&id))||FAILED(tm->CreateDocumentMgr(&doc))||FAILED(doc->CreateContext(id,0,nullptr,&ctx,&ec))||FAILED(doc->Push(ctx))||FAILED(tm->SetFocus(doc)))goto done;
 if(!Run(ctx,id,new shuru::StartCompositionEditSession(ctx,id,sink,&comp))||!Run(ctx,id,new shuru::SetCompositionEditSession(ctx,id,&comp,L"suixinshuru"))||!Run(ctx,id,new shuru::InsertTextEditSession(ctx,id,&comp,L"随心")))goto done;
 // Partial coverage: committed prefix remains and the tail becomes a fresh real TSF composition.
 if(!Run(ctx,id,new shuru::StartCompositionEditSession(ctx,id,sink,&comp))||!Run(ctx,id,new shuru::SetCompositionEditSession(ctx,id,&comp,L"shuru"))||!Run(ctx,id,new shuru::InsertTextEditSession(ctx,id,&comp,L"输入")))goto done;
 // Number/space submission through the same production insertion edit session.
 if(!Run(ctx,id,new shuru::InsertTextEditSession(ctx,id,&comp,L"1 ")))goto done;
 {std::wstring text;if(!Run(ctx,id,new Read(ctx,&text),TF_ES_SYNC|TF_ES_READ)||text!=L"随心输入1 "){std::fwprintf(stderr,L"unexpected real TSF text: %ls\n",text.c_str());goto done;}}
 std::wprintf(L"real ITfThreadMgr/document/context composition/partial/submit flow passed\n");rc=0;
done:if(comp)comp->Release();sink->Release();if(doc)doc->Pop(TF_POPF_ALL);if(ctx)ctx->Release();if(doc)doc->Release();if(tm){tm->Deactivate();tm->Release();}CoUninitialize();return rc;}
