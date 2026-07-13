/*
* BHO组件加载器
*
* Author: Maple
* date: 2022-1-31 Create
* Copyright winmoes.com
*/

/*
	Corretti bugs e warnings (nel .cpp).
	Le modifiche sono marcate/delimitate con il tag //LPI.
	Luca Piergentili, 18/06/2026

	Questo file e' un header COM per un'estensione della Shell (BHO - Browser Helper Object).
	Definizioni delle interfacce:
	- ClassFactory per la creazione
	- CObjectWithSite che funge da "aggancio" per ricevere l'istanza del browser (Explorer)
	- CIDispatch che gestisce gli eventi
	e riferimento alle due funzioni che fanno da ponte verso la logica principale in dllmain.cpp:
	OnWindowLoad() e OnDocComplete()
*/

//LPI
#ifndef _SHELLLOADER_H
#define _SHELLLOADER_H 1

#include <sdkddkver.h>
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>


#include <Unknwn.h>
#include <exdisp.h>
#include <exdispid.h>
#include <mshtml.h>
#include <mshtmdid.h>
#include <string>

extern const std::wstring CLSID_SHELL_BHO_STR;
extern const CLSID CLSID_SHELL_BHO;

extern void OnWindowLoad();
extern void OnDocComplete(std::wstring,DWORD);

/*
* IDispatch 接口实现 浏览器事件
*/
class CIDispatch : public IDispatch
{
public:
	CIDispatch() = default;
	~CIDispatch();

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();

	//IDispatch
	STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) { return E_NOTIMPL; }
	STDMETHODIMP GetTypeInfo(UINT iTInfo, LCID lcid, ITypeInfo** ppTInfo) { return E_NOTIMPL; }
	STDMETHODIMP GetIDsOfNames(REFIID riid, LPOLESTR* rgszNames, UINT cNames, LCID lcid, DISPID* rgDispId) { return E_NOTIMPL; }
	STDMETHODIMP Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr);

protected:
	long m_ref = 0;
	std::wstring m_lastpath;
};

/*
* 一个基本的COM浏览器类 只实现基本接口
*/
class CObjectWithSite : public IObjectWithSite
{
public:
	CObjectWithSite();
	virtual ~CObjectWithSite();

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();

	//IObjectWithSite
	STDMETHODIMP SetSite(IUnknown* pUnkSite);
	STDMETHODIMP GetSite(REFIID riid, void** ppvSite) { return E_NOINTERFACE; }

protected:
	long m_ref = 0;
	IWebBrowser2* m_web = nullptr;
	IConnectionPoint* m_cpoint = nullptr;
	DWORD m_cookie = 0;
	void ReleaseRes();
};

/*
* 类工厂 用来处理浏览器类的创建
*/
class ClassFactory : public IClassFactory
{
public:

	//IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
	ULONG STDMETHODCALLTYPE AddRef();
	ULONG STDMETHODCALLTYPE Release();

	//IClassFactory
	IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv);
	IFACEMETHODIMP LockServer(BOOL fLock);

private:
	long m_ref = 0;
};

//LPI
#endif // _SHELLLOADER_H
