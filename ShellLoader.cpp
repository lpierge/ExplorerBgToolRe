/*
* BHO组件加载器
*
* Author: Maple
* date: 2022-1-31 Create
* Copyright winmoes.com
*/

/*
	Corretti bugs e warnings.
	Le modifiche sono marcate/delimitate con il tag //LPI.
	Luca Piergentili, 18/06/2026

	Questo file contiene il codice COM per un'estensione della Shell (BHO - Browser Helper Object).
	Quando viene aperta una cartella, Windows chiama le funzioni custom seguendo il protocollo BHO (Browser Helper Object).
	Le due funzioni OnWindowLoad() e OnDocComplete() fanno da ponte verso la logica principale in dllmain.cpp, in altre
	parole, la DLL non e' semplicemente codice iniettato ma una vera e propria estensione della Shell (ossia l'Explorer).

	Cosa succede quando viene caricata l'estensione:
	- Windows carica dllmain.dll (fase critica, Loader Lock attivo).
	- Il Loader Lock si sblocca.
	- Explorer vede nel registro che esiste l'estensione e chiama DllGetClassObject.
	- Viene restituita la ClassFactory.
	- Explorer dice alla Factory: "Creami un'istanza".
	- La Factory chiama CreateInstance -> esegue new CObjectWithSite().
	- Il costruttore di CObjectWithSite chiama OnWindowLoad().

	L'altra funzione OnDocComplete() viene attivata in questo modo:
	- Nel metodo SetSite, l'oggetto riceve il puntatore IWebBrowser2 di Explorer (come se l'Explorer desse le chiavi di casa...)
	- Viene usato tale puntatore per agganciarsi (tramite IConnectionPoint) agli eventi del browser usando il CIDispatch.
	- Quando l'utente naviga in una cartella e questa ha finito di caricarsi, Explorer invia a tutti i plugin l'evento DISPID_DOCUMENTCOMPLETE.
	- L'Invoke intercetta l'evento con la gestione di DISPID_DOCUMENTCOMPLETE dove estrae il percorso della cartella dalla variabile VARIANT di 
	  COM e lo passa alla logica in dllmain.cpp per farle cambiare lo sfondo della finestra.
*/

//LPI
#include "macro.h"

#include "traceexpr.h"
#define _TRACE_FLAG			_TRFLAG_TRACECONSOLE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
//#define _TRACE_FLAG			_TRFLAG_NOTRACE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
#define _TRACE_FLAG_INFO	_TRACE_FLAG
#define _TRACE_FLAG_WARN	_TRACE_FLAG
#define _TRACE_FLAG_ERR		_TRACE_FLAG

#include "ExplorerBgToolRe.h"
//LPI

#include "ShellLoader.h"

long g_cDllRef = 0;

//LPI istanziato in dllmain.cpp
extern HMODULE global_hModule;

/*如果您修改了代码 请使用VS的GUID工具生成新的GUID！*/
//LPI
//const std::wstring CLSID_SHELL_BHO_STR = L"{ED15A97D-FE3E-4CDE-98FF-BC46B02896B0}";
const std::wstring CLSID_SHELL_BHO_STR = _L(EXPLORERBGTOOL_CLSID);

const CLSID CLSID_SHELL_BHO = { 0xed15a97d, 0xfe3e, 0x4cde, { 0x98, 0xff, 0xbc, 0x46, 0xb0, 0x28, 0x96, 0xb0 } };

#pragma region CObjectWithSite

CObjectWithSite::CObjectWithSite()
{
	OnWindowLoad();
}

CObjectWithSite::~CObjectWithSite()
{
	ReleaseRes();
}

STDMETHODIMP CObjectWithSite::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown)
		*ppv = static_cast<IUnknown*>(this);
	else if (riid == IID_IObjectWithSite)
		*ppv = static_cast<IObjectWithSite*>(this);
	else
		return E_NOINTERFACE;
	AddRef();
	return S_OK;
}

ULONG __stdcall CObjectWithSite::AddRef()
{
	InterlockedIncrement(&g_cDllRef);
	return InterlockedIncrement(&m_ref);
}

ULONG __stdcall CObjectWithSite::Release()
{
	int tmp = InterlockedDecrement(&m_ref);
	if (tmp == 0)
		delete this;
	InterlockedDecrement(&g_cDllRef);
	return tmp;
}

STDMETHODIMP CObjectWithSite::SetSite(IUnknown* pUnkSite)
{
	ReleaseRes();
	HRESULT hr = pUnkSite->QueryInterface(IID_IWebBrowser2, (void**)&m_web);
	if (FAILED(hr))
		ReleaseRes();

	IConnectionPointContainer* cpoint = nullptr;

	hr = m_web->QueryInterface(IID_IConnectionPointContainer, (void**)&cpoint);
	if (FAILED(hr)) return E_FAIL;

	hr = cpoint->FindConnectionPoint(DIID_DWebBrowserEvents2, &m_cpoint);
	if (FAILED(hr))
	{
		cpoint->Release();
		return E_FAIL;
	}

	m_cpoint->Advise((IUnknown*)new CIDispatch(), &m_cookie);

	return hr;
}

void CObjectWithSite::ReleaseRes()
{
	if (m_web) m_web->Release();
	if (m_cpoint)
	{
		m_cpoint->Unadvise(m_cookie);
		m_cpoint->Release();
	}
	m_web = nullptr;
	m_cpoint = nullptr;
}

#pragma endregion

#pragma region ClassFactory

STDMETHODIMP ClassFactory::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == IID_IClassFactory)
	{ 
		*ppv = this;
		AddRef();
		return S_OK;
	}
	return E_NOINTERFACE;
}

//LPI
/*
	Il codice originale puo' provocare una corruzione dell'heap o un eccezione perche' cerca di eseguire una delete su un oggetto
	(la ClassFactory) dichiarato come statico.
	Nel codice originale AddRef() e Release() gestiscono in modo errato il conteggio dei riferimenti dato che AddRef() incrementa 
	due volte la variabile globale della DLL saltando il contatore della istanza, mentre Release() tenta di distruggere la factory
	chiamando delete this.
	Poiche' ClassFactory e' dichiarata come oggetto statico unico dentro DllGetClassObject(), invocare delete potrebbe generare un 
	crash immediato o la corruzione della memoria globale dell'heap.
	Trattandosi di un oggetto unico e statico che vive per l'intera durata del processo, le funzioni AddRef() e Release() sono state
	blindate rimuovendo il delete this e restituendo un valore > 0 per impedirne l'autodistruzione.
	Sul perche' sia stato originariamente dichiarato statico, suppongo che potrebbe essere per i seguenti motivi:
	- ClassFactory e' l'oggetto che "fabbrica" altre istanze della tua DLL e rendendolo statico ci si assicura che sia accessibile
	  in qualsiasi momento dal DllGetClassObject (la funzione di ingresso di ogni DLL COM) senza dover gestire allocazioni di memoria
	  complesse
	- si evita la creazione di una nuova istanza della ClassFactory ogni volta che Explorer richiede una nuova interfaccia e ci si 
	  assicura che sia il sistema operativo stesso che si occupi di "pulire" la memoria relativa quando la DLL viene scaricata
*/
#if 0 /* originale, buggato */
ULONG __stdcall ClassFactory::AddRef()
{
	InterlockedIncrement(&g_cDllRef);
	return InterlockedIncrement(&g_cDllRef);
}

ULONG __stdcall ClassFactory::Release()
{
	int tmp = InterlockedDecrement(&m_ref);
	if (tmp == 0)
		delete this;
	InterlockedDecrement(&g_cDllRef);
	return tmp;
}
#else /* modificato */
ULONG __stdcall ClassFactory::AddRef()
{
    InterlockedIncrement(&g_cDllRef);
    return 1; // e' statica, ritorna quindi un valore fittizio costante > 0
}

ULONG __stdcall ClassFactory::Release()
{
    InterlockedDecrement(&g_cDllRef);
    return 1; // e' statica, non va MAI distrutta, ritorna > 0
}
#endif
//LPI

STDMETHODIMP ClassFactory::LockServer(BOOL fLock)
{ 
	if (fLock)
		InterlockedIncrement(&g_cDllRef);
	else 
		InterlockedDecrement(&g_cDllRef);
	return S_OK;
}

STDMETHODIMP ClassFactory::CreateInstance(LPUNKNOWN pUnkOuter, REFIID riid, LPVOID* ppvObj)
{ 
	*ppvObj = NULL;
	if (pUnkOuter) 
		return CLASS_E_NOAGGREGATION;
	CObjectWithSite* bho = new CObjectWithSite();
	bho->AddRef();
	HRESULT hr = bho->QueryInterface(riid, ppvObj);
	bho->Release();
	return hr; 
}

#pragma endregion

#pragma region Reg

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppvOut)
{
	static ClassFactory factory;
	*ppvOut = NULL;
	if (rclsid == CLSID_SHELL_BHO) { 
		return factory.QueryInterface(riid, ppvOut);
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void)
{
	return (g_cDllRef > 0) ? S_FALSE : S_OK;
}

STDAPI DllRegisterServer()
{
	HKEY hkey = 0;

	//LPI come che TCHAR??
	//TCHAR dllpath[MAX_PATH];
	wchar_t dllpath[MAX_PATH+1] = {0};

	//LPI
	//GetModuleFileNameW(g_hModule, dllpath, MAX_PATH);
	GetModuleFileNameW(global_hModule, dllpath, MAX_PATH);

	//创建CLSID
	std::wstring regpath = L"CLSID\\" + CLSID_SHELL_BHO_STR;
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT,regpath.c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL) != ERROR_SUCCESS)
		return SELFREG_E_CLASS;

	//设置COM组件名称
	//LPI
	// commentato e corretto l'originale: va usato sizeof, non numeri hardcoded, oltre al fatto che essendo caratteri wide il totale e' 26, non 24
	//RegSetValueEx(hkey, NULL, 0, REG_SZ, (const BYTE*)L"ExplorerTool", 24 * sizeof(TCHAR));
	RegSetValueExW(hkey, NULL, 0, REG_SZ, (const BYTE*)L"ExplorerTool", sizeof(L"ExplorerTool"));
	RegCloseKey(hkey);

	//创建InProcServer32
	if (RegCreateKeyExW(HKEY_CLASSES_ROOT, (regpath + L"\\InProcServer32").c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL) != ERROR_SUCCESS)
		return SELFREG_E_CLASS;

	//设置dll位置

	//LPI
	// giusto per il warning del compilatore
	//RegSetValueExW(hkey, NULL, 0, REG_SZ, (const BYTE*)dllpath, (wcslen(dllpath) + 1) * sizeof(wchar_t));
	RegSetValueExW(hkey, NULL, 0, REG_SZ, (const BYTE*)dllpath, (DWORD)((wcslen(dllpath) + 1) * sizeof(wchar_t)));

	// Set the ThreadingModel to Apartment
	//LPI
	// commentato e corretto l'originale: va usato sizeof, non numeri hardcoded
	//RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", 10 * sizeof(wchar_t));
	RegSetValueExW(hkey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Apartment", sizeof(L"Apartment"));
	RegCloseKey(hkey);

	//注册BHO组件
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects\)"
		+ CLSID_SHELL_BHO_STR).c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL) != ERROR_SUCCESS)
		return SELFREG_E_CLASS;

	//禁止IE浏览器加载本组件
	DWORD value = 1;
	RegSetValueExW(hkey, L"NoInternetExplorer", 0, REG_DWORD, (const BYTE*)&value, sizeof(DWORD));
	RegCloseKey(hkey);

	//注册文件对话框
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, (LR"(SOFTWARE\Classes\Drive\shellex\FolderExtensions\)"
		+ CLSID_SHELL_BHO_STR).c_str(), 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hkey, NULL) != ERROR_SUCCESS)
		return SELFREG_E_CLASS;

	value = 255;
	RegSetValueExW(hkey, L"DriveMask", 0, REG_DWORD, (const BYTE*)&value, sizeof(DWORD));
	RegCloseKey(hkey);
	return S_OK;
}

STDAPI DllUnregisterServer()
{
	//删除BHO组件注册
	//LPI
	// commentato e corretto l'originale: il doppio backslash alla fine (...Objects\\) unito alla stringa del CLSID (che non inizia con un backslash), 
	// crea un percorso non valido nel registro, impedendo la rimozione pulita del BHO
	//RegDeleteKey(HKEY_LOCAL_MACHINE, (LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects\\)" + CLSID_SHELL_BHO_STR).c_str());
	RegDeleteKey(HKEY_LOCAL_MACHINE, (LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\Browser Helper Objects\)" + CLSID_SHELL_BHO_STR).c_str());
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, (LR"(SOFTWARE\Classes\Drive\shellex\FolderExtensions\)" + CLSID_SHELL_BHO_STR).c_str());

	//删除COM组件注册
	RegDeleteKey(HKEY_CLASSES_ROOT, (L"CLSID\\" + CLSID_SHELL_BHO_STR + L"\\InProcServer32").c_str());
	RegDeleteKey(HKEY_CLASSES_ROOT, (L"CLSID\\" + CLSID_SHELL_BHO_STR).c_str());

	return S_OK;
}

#pragma endregion

#pragma region IDispatch

CIDispatch::~CIDispatch()
= default;

STDMETHODIMP CIDispatch::QueryInterface(REFIID riid, void** ppv)
{
	if (riid == IID_IUnknown || riid == DIID_DWebBrowserEvents2)
		*ppv = static_cast<CIDispatch*>(this);
	else if (riid == IID_IDispatch)
		*ppv = static_cast<IDispatch*>(this);
	else
		return E_NOINTERFACE;
	AddRef();
	return S_OK;
}

ULONG __stdcall CIDispatch::AddRef()
{
	InterlockedIncrement(&g_cDllRef);
	return InterlockedIncrement(&m_ref);
}

ULONG __stdcall CIDispatch::Release()
{
	int tmp = InterlockedDecrement(&m_ref);
	if (tmp == 0)
		delete this;
	InterlockedDecrement(&g_cDllRef);
	return tmp;
}

STDMETHODIMP CIDispatch::Invoke(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS* pDispParams, VARIANT* pVarResult, EXCEPINFO* pExcepInfo, UINT* puArgErr)
{
//LPI
	//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"Invoke(): BHO Invoke received DISPID: %d\n",dispIdMember));

	if(!pDispParams)
		return(E_INVALIDARG);

	switch(dispIdMember)
	{
		case DISPID_DOCUMENTCOMPLETE:
		{
			//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"Invoke(): DISPID_DOCUMENTCOMPLETE\n"));

			std::wstring path = (wchar_t*)pDispParams->rgvarg->pvarVal->bstrVal;
			if(path!=m_lastpath)
			{
				m_lastpath = path;
				OnDocComplete(path, GetCurrentThreadId());
			}

			break;
		}

		case DISPID_BEFORENAVIGATE2:
		case DISPID_NAVIGATECOMPLETE2:
		case DISPID_DOWNLOADBEGIN:
		case DISPID_DOWNLOADCOMPLETE:
		case DISPID_NEWWINDOW2:
		case DISPID_WINDOWREGISTERED:
		case DISPID_ONQUIT:
		default:
			break;
	}

	return(S_OK);
//LPI
}

#pragma endregion
