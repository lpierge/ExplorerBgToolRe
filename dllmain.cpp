/*
	dllmain.cpp

	ExplorerBgToolRe(dux)
	Il progetto e' una refattorizzazione/riscrittura del progetto originale ExploreBgTool:

		copyright originale:
		文件资源管理器背景工具扩展 (estensione per lo sfondo di Explorer)
		Author: Maple
		date: 2021-7-13 Create
		Copyright winmoes.com
		(reperibile su: https://github.com/Maplespe/explorerTool)

	Innanzitutto, riguardo al tipo di codice qui in esame, questa non e' solo una DLL iniettata "brutalmente" a basso
	livello, ma e' un'estensione della Shell COM. Al caricamento, Explorer interroga il Registro di Sistema, scopre la 
	DLL (come estensione) e chiede a Windows di usarla.
	Windows chiama la ClassFactory, che a sua volta fa nascere l'oggetto CObjectWithSite, il cui costruttore a sua volta
	chiama la OnWindowLoad(). A tal punto il codice e' in esecuzione in un thread dell'interfaccia utente di Explorer.

	Il codice ed il disegno dell'architettura in generale sono abbastanza complessi, quindi per facilitare le cose a chi 
	dovesse metterci le mani (io stesso!) ho cercato di documentare e spiegare il piu' possibile i punti che mi sembrano
	importanti. Non sono masochista, quindi i commenti vanno tutti nella mia lingua originale, se non li capite, potete
	usare il traduttore di Google che oggigiorno funziona abbastanza bene.

	E' stato cambiato il disegno originale e sono stati corretti vari bugs, sopratutto relativi a problemi di concorrenza
	e di memoria (leaks ed incongruenze varie), cercando di ottimizzare al meglio.

	Il cambio di maggior rilievo e' stata la rimozione della mappa dei dati grafici per i vari threads e la sostituzione 
	con il meccanismo Thread Local Storage (TLS) nativo Win32, grazie al quale ogni thread ha il suo proprio blocco di 
	dati grafici, independente e senza bisogno di lock per l'accesso.

	I lock della mappa originale non erano congruenti, o assenti come nel caso dell'operatore [], potendo generare gravi 
	problemi di sincronizzazione/corruzione della memoria. Inizialmente si 'e provato ad introdurre dei mutex globali per
	gestire i lock, ma il cambio ha prodotto effetti nefasti sul "timing" dei threads di Explorer.
	Un lock vero e robusto costringe i thread ad aspettarsi l'un l'altro, il che fa emergere il problema architetturale
	latente: ossia che Explorer, durante la chiusura, uccide i thread in modo violento. Con il codice originale i thread
	passano attraverso le funzioni senza bloccarsi quasi mai, perche' i lock morivano subito, quindi al momento della 
	chiusura della DLL non c'erano threads "in attesa". Ma con i lock corretti, i thread hanno iniziato ad aspettarsi,
	rimanendo congelati nei momenti meno opportuni, provocando crash e riavvi dell'Explorer.

	Usando la tecnica del Thread Local Storage (TLS) si e' eliminata la necessita' di scrivere classi di sincronizzazione
	complesse (come l'originale, inefficace, o la nuova, fin troppo effettiva) perche' ogni thread e' stato dotato della
	sua propria memoria (privata).

	L'unico punto in cui i threads potrebbero pestarsi i piedi ora e' durante il caricamento della lista con le immagini,
	per questo, invece di usare un mutex che potrebbe causare i problemi di cui sopra, viene usato un oggetto INIT_ONCE.
	Tenere sempre presente che i mutex di Windows sono oggetti del kernel ed ogni volta che se ne usa uno la CPU deve 
	passare da User-Mode a Kernel-Mode, operazione pesante e che in un hook globale puo' causare un collo di bottiglia o 
	un deadlock inter-processo.

	Ulteriori implementazioni ex-novo:
	- subclassing della finestra dell'Explorer, per poter cambiare il valore per la trasparenza della finestra, vedi i campi
	  "backgroundalpha" e "foregroundalpha" in config.ini
	- separazione delle immagini per gli special folders dalle immagini della directory "Image" (il "calderone"), con la
	  possibilita' di specificare il pathname completo per tali immagini
	- implementazione della wildcard * nei nomi di sezione per gli special folder, in modo da poter estendere l'uso di una
	  determinata immagine a tutte le (sub)directory che matchano 
	- aggiunta del campo "graphicsformats" in config.ini per poter specificare i formati grafici delle immagini da caricare
	  invece di inchiodarli nel codice

	La rifattorizzazione vera e propria viene effettuata sull'intero contenuto dei files dllmain.* (questo) e WinAPI.*,
	il file ShellLoader.cpp viene corretto in punti specifici, mentre altre correzioni minori nel resto del codice vengono 
	marcate con il tag //LPI.

	Luca Piergentili, 18/06/2026
*/
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <comdef.h>
#include <shlobj.h>
#pragma comment(lib,"shell32.lib")
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#include <gdiplus.h>
#pragma comment(lib, "GdiPlus.lib")
#pragma comment(lib,"Msimg32.lib")  
#include "MinHook.h"
#include "ShellLoader.h"
#include "WinAPI.h"
#include "HookDef.h"
#include "fastrand.h"
#include <string>
#include <utility>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include "L:/ExplorerBgToolRe/version.h"

#include "traceexpr.h"
#define _TRACE_FLAG			_TRFLAG_TRACECONSOLE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
//#define _TRACE_FLAG			_TRFLAG_NOTRACE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
#define _TRACE_FLAG_INFO	_TRACE_FLAG
#define _TRACE_FLAG_WARN	_TRACE_FLAG
#define _TRACE_FLAG_ERR		_TRACE_FLAG

HMODULE		global_hModule		= NULL;
bool		global_bHooked		= false;
bool		global_bInitFailed	= false;
ULONG_PTR	global_GdiPlusToken = NULL;
DWORD		global_dwTlsIndex	= TLS_OUT_OF_INDEXES;	// indice TLS globale per memorizzare il puntatore alla struttura MyData privata di ogni thread
INIT_ONCE	global_InitOnceImg	= INIT_ONCE_STATIC_INIT;
SRWLOCK		global_SRWLock		= SRWLOCK_INIT;			// occhio: SRWLock NON e' rientrante

struct MyData
{
	// per poter separare le immagini per gli special folders da quelle del calderone, usa
	// una seconda lista ed imposta un puntatore diretto al bitmap invece di usare l'indice
	// in pratica:
	// - MyCreateWindowExW (per i folder normali) sceglie random un'immagine dal "calderone" ed imposta il puntatore pActiveBmp
	// - OnDocComplete (per i folder speciali) cerca l'immagine nella lista speciale e sovrascrive pActiveBmp con il nuovo puntatore
	// - MyFillRect semplicemente prende pActiveBmp e lo disegna senza sapere se il puntatore provenga dal calderone o dalla lista speciale
	// in tal modo disaccoppia totalmente la logica di disegno dalla gestione delle liste senza aggiungere nessun overhead
	struct data
	{
		HDC hdc = 0;
		int imgIndex = 0;					// l'indice con cui accedere alle immagini del calderone - da eliminare
		BitmapGDI* pActiveBmp = nullptr;	// il puntatore diretto al bitmap attivo (calderone o speciale) - nuova logica
	};

	SIZE size = {0,0};

	// mappa HWND di DirectUIHWND -> dati del disegno (imgIndex, ecc.)
	std::unordered_map<HWND,data> duiList;

	// mappa HWND di CabinetWClass -> WNDPROC originale della cornice
	// il subclassing della finestra dell'Explorer verra' effettuato per ogni sua nuova finestra che
	// venga aperta, quindi ognuna di esse dovra' avere il proprio puntatore alla WndProc relativa
	std::unordered_map<HWND,WNDPROC> parentList;
};

struct imgInfo
{
    std::wstring fileName;
    DWORD fileSize;
    BitmapGDI* bmp;
};

struct iniData
{
	std::wstring section;
	std::wstring value;
};

struct Config
{
	// [load]
	std::wstring	whitelist = L"";			// lista programmi abilitati al caricamento della DLL
	bool			filedialog = false;			// flag per dialogo apri/salva
	std::wstring	filedialoglist = L"";		// lista programmi abilitati al dialogo apri/salva
    bool			showerrors = true;			// flag per visualizzare o meno gli errori

	// [explorer]
	int				backgroundalpha = 128;		// trasparenza per Explorer in secondo piano
	int				foregroundalpha = 255;		// trasparenza per Explorer in primo piano

	// [image]
	int				maximages = 64;				// limite di default per numero immagini da caricare
    int				displaymode = 0;			// 0=top-left, 1=top-right, 2=bottom-left, 3=bottom-right, 4=center, 5=zoom, 6=fill
    BYTE			alphablend = 255;			// trasparenza di default
    bool			random = true;				// modo random/fissa
    std::wstring	customfolder = L"";			// directory per immagini invece di Image
    bool			specialfolders = false;		// se usare o meno gli special folders
	std::wstring	graphicsformats = L"";		// elenco formati grafici da caricare

	// dati interni, non del config.ini
    std::vector<imgInfo> imageList;				// lista immagini per il calderone
	std::vector<imgInfo> specialFoldersImageList;// lista immagini per gli special folders
	std::vector<iniData> dirSkeleton;			// lista (interna) per nomi sezioni per gli special folder con wildcard
} global_stConfig;

#define IMAGES_MAX_ALLOWED			128			// anche se usa liste dinamiche, limita il numero di immagini per non far fare il botto a Explorer
#define IMAGES_MAX_DEFAULT			64			// numero max immagini da caricare di default

#define IMAGES_MODE_TOP_LEFT		0			// modalita' visualizzazione immagine nella finestra dell'Explorer
#define IMAGES_MODE_TOP_RIGHT		1
#define IMAGES_MODE_BOTTOM_LEFT		2
#define IMAGES_MODE_BOTTOM_RIGHT	3
#define IMAGES_MODE_CENTER			4
#define IMAGES_MODE_ZOOM			5
#define IMAGES_MODE_FILL			6

#define IMAGES_MODE_MIN				0
#define IMAGES_MODE_MAX				6

#define ALPHABLEND_MIN				0			// valori trasparenza (0=invisibile, 255=completamente solido)
#define ALPHABLEND_DEFAULT			128
#define ALPHABLEND_MAX				255

// prototipi
BOOL APIENTRY		DllMain						(HMODULE hModule,DWORD dwReason,LPVOID lpReserved);
void APIENTRY		DLLVersion					(int& major,int& minor,int& patch);
bool				ShouldLoad					(void);
bool				IsProcessInList				(const std::wstring& processList,const std::wstring& processName);
std::wstring		GetCurDllDir				(void);

void				LoadSettings				(void);
void				LoadImages					(void);
BOOL CALLBACK		LoadImagesCallback			(PINIT_ONCE InitOnce,PVOID Parameter,PVOID* Context);
int					DoesPathMatchData			(const std::wstring& path,const std::vector<iniData>& dirSkeleton);

MyData*				GetLocalData				(bool bCreateOnDemand);
HWND				GetHWNDFromDC				(HDC hdc);
HWND				GetHWNDFromDUI				(HDC dc,MyData::data& outData);

void				OnWindowLoad				(void);
void				OnDocComplete				(std::wstring path,DWORD threadID);

LRESULT CALLBACK	ExplorerSubclassProc		(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

HWND				MyCreateWindowExW			(DWORD dwExStyle,LPCWSTR lpClassName,LPCWSTR lpWindowName,DWORD dwStyle,int X,int Y,int nWidth,int nHeight,HWND hWndParent,HMENU hMenu,HINSTANCE hInstance,LPVOID lpParam);
BOOL				MyDestroyWindow				(HWND hWnd);
HDC					MyBeginPaint				(HWND hWnd,LPPAINTSTRUCT lpPaint);
int					MyFillRect					(HDC hDC,const RECT* lprc,HBRUSH hbr);
HDC					MyCreateCompatibleDC		(HDC hDC);

/*
	DllMain()

	Il punto d'entrata (e uscita) della DLL.

	Secondo la stessa Microsoft, bisogna prestare attenzione al codice che si esegue durante l'inizializzazione.
	All'interno di DllMain() ci si trova nel "Loader Lock" del Kernel, un mutex interno del sistema operativo che
	serializza il caricamento/scaricamento delle DLL.
	Qualsiasi cosa che prova ad acquisire lo stesso lock, o che blocca il thread, provoca quindi un deadlock o un
	comportamento indefinito (un crash silenzioso).
	La lista dei "proibiti" include LoadLibrary(), FreeLibrary(), CreateThread(), ExitThread(), inizializzare COM,
	WaitForSingleObject(), funzioni CRT che allocano sull'heap, GetStringType(), ShellExecute() e RegOpenKey().
	In parole povere, il thread del sistema operativo usa un lock a livello globale durante il caricamento della 
	DLL e qualsiasi cosa che possa interferire, puo' provocare un deadlock od un crash.
*/
BOOL APIENTRY DllMain(HMODULE hModule,DWORD dwReason,LPVOID lpReserved)
{
	if(dwReason==DLL_PROCESS_ATTACH && !global_hModule)
	{
		TRACE_INIT(_TRACE_FLAG_INFO);
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): DLL_PROCESS_ATTACH\n"));

		global_hModule = hModule;
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): hModule: %p\n",(void*)global_hModule));
        
		// alloca l'indice TLS globale per il processo
		global_dwTlsIndex = TlsAlloc();
		if(global_dwTlsIndex==TLS_OUT_OF_INDEXES)
		{
			TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"DllMain(): FATAL ERROR: TLS_OUT_OF_INDEXES\n"));
			return(FALSE);
		}

		// la chiamata a DisableThreadLibraryCalls() impedisce ai thread secondari di Explorer di 
		// transitare in DllMain(), eliminando alla radice i conflitti di Loader Lock/Heap Lock, ma...
		// ...COMMENTATO! vedi piu' sotto le note in DLL_THREAD_DETACH
		//DisableThreadLibraryCalls(hModule);

		// decide se il processo puo' caricarla o meno        
		return(ShouldLoad());
	}
	else if(dwReason==DLL_PROCESS_DETACH)
	{
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): DLL_PROCESS_DETACH\n"));

		// quando lpReserved e' diverso da NULL, il processo sta terminando: gli altri thread sono 
		// gia' morti e l'ambiente e' instabile, non si deve liberare nulla
		// quando invece lpReserved e' NULL, si tratta di un unmap manuale (FreeLibrary), allora si
		// deve ripulire
		if(lpReserved==NULL)
		{
			// nel casi in cui le immagini del calderone vengano usate anche per i folder speciali,
			// deve evitare di liberare lo stesso puntatore due volte (causerebbe un crash), raccoglie
			// quindi tutti i puntatori grafici unici per evitare il double-free
			std::vector<BitmapGDI*> uniqueBitmaps;
			for(auto& info : global_stConfig.imageList)
				uniqueBitmaps.push_back(info.bmp);
			
			for(auto& info : global_stConfig.specialFoldersImageList)
			{
				if(std::find(uniqueBitmaps.begin(),uniqueBitmaps.end(),info.bmp)==uniqueBitmaps.end())
					uniqueBitmaps.push_back(info.bmp);
			}

			// dealloca ogni bitmap una sola volta
			for(auto* bmp : uniqueBitmaps)
				delete bmp;
			
			global_stConfig.imageList.clear();
			global_stConfig.specialFoldersImageList.clear();

			if(global_GdiPlusToken!=NULL)
			{
				Gdiplus::GdiplusShutdown(global_GdiPlusToken);
				global_GdiPlusToken = NULL;
			}
            
			if(global_dwTlsIndex!=TLS_OUT_OF_INDEXES)
			{
				TlsFree(global_dwTlsIndex);
				global_dwTlsIndex = TLS_OUT_OF_INDEXES;
			}
		}
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): lpReserved: %s\n",lpReserved ? L"process termination" : L"explicit unload"));

		TRACE_TERM(_TRACE_FLAG_INFO);
	}
	else if(dwReason==DLL_THREAD_ATTACH)
	{
		//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): thread ATTACH\n"));
	}
	else if(dwReason==DLL_THREAD_DETACH)
	{
		//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): thread DETACH\n"));

		// occhio al memory leak invisibile del TLS
		// l'allocazione on-demand viene gestita in GetLocalData() e la distruzione in MyDestroyWindow(), il che
		// funziona perfettamente finche' il thread chiude educatamente le sue finestre, ma Explorer suele creare
		// thread di servizio che aprono finestre temporanee o che vengono chiusi improvvisamente (AC/DC) e se un
		// thread alloca un oggetto MyData tramite GetLocalData(true), ma non passa mai per MyDestroyWindow() o se
		// Explorer lo termina prima, quel puntatore rimane orfano e la memoria di MyData non verra' mai liberata
		// per risolvere bisogna quindi intercettare il momento esatto in cui un thread muore, a prescindere se ha
		// chiuso le sue finestre o meno, per questo bisogna gestire DLL_THREAD_DETACH e disattivare la chiamata a
		// DisableThreadLibraryCalls(), vedi sopra

		// ogni volta che un thread di Explorer muore, pulisce i suoi dati privati
		if(global_dwTlsIndex!=TLS_OUT_OF_INDEXES)
		{
			MyData* pData = (MyData*)TlsGetValue(global_dwTlsIndex);
			if(pData!=nullptr)
			{
				delete pData;
				TlsSetValue(global_dwTlsIndex,NULL);
				TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"DllMain(): TLS cleaning for the dying thread\n"));
			}
		}
	}

	return(TRUE);
}

/*
	DllVersion()

	Restituisce il numero di versione corrente.
*/
extern "C" __declspec(dllexport) HRESULT APIENTRY DllVersion(int& major,int& minor,int& patch)
{
	major = MAJOR_VERSION;
	minor = MINOR_VERSION;
	patch = PATCH_VERSION;
	
	return(S_OK);
}

/*
	ShouldLoad()

	Controlla, in base alla whitelist, se il programma e' abilitato al caricamento della DLL.

	Dato che gli hooks vengono impostati su funzioni basiche (come la CreateWindow) a livello globale, deve controllare
	che solo i programmi abilitati possano caricare la DLL, in caso contrario il codice per il disegno dell'immagine di
	fondo verrebbe applicato a qualsiasi programma con conseguenze imprevedibili (crash, etc. vedi qui sotto a proposito
	della finestra di TRACE).

	Occhio: ricordarsi che per visualizzare la finestra di TRACE di processi terzi che non siano Explorer, ovviamente
	bisogna inserire il nome di tale processo nella whitelist, ma mentre per processi relativamente semplici, come 
	Notepad, non c'e' problema, per processi complessi come Chrome, etc., il codice relativo implementa meccanismi di
	controllo e protezione che impediscono iniettare la DLL.
*/
bool ShouldLoad(void)
{
	bool bAllowed = false;

	// ricava il nome del processo che sta cercando di caricarla
	wchar_t pName[MAX_PATH+1] = {0};
	::GetModuleFileNameW(NULL,pName,MAX_PATH);
	TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): GetModuleFileNameW(): %s\n",pName));
	std::wstring path = std::wstring(pName);
	std::wstring name = path;
	size_t pos = path.find_last_of(L"\\");
	if(pos!=std::wstring::npos)
		name = path.substr(pos+1);
	std::transform(name.begin(),name.end(),name.begin(),::tolower);

	// carica la configurazione
	LoadSettings();

	/*
	ATTENZIONE: se il TRACE e' configurato su console, la stampa di "ShouldLoad(): process is NOT in the whitelist\n"
	in realta' non avverra' mai. Il codice verra' eseguito nella memoria del processo bannato, ma fallira' in silenzio
	perche' il processo (stiamo parlando di programmi Windows "normali") e' un programma grafico (GUI) e non ha una 
	console a cui inviare l'output.
	La DLL, vedendo che il processo non sta nella whitelist, restituisce subito FALSE e si scarica immediatamente. Non 
	fa quindi in tempo ad allocare una nuova finestra di console solo per dire che sta uscendo (e non avrebbe senso).
	Per "visualizzare" tali messaggi bisognerebbe quindi cambiare al TRACE su file, dato che il file system (C:\...) e'
	globale ed accessibile da qualsiasi processo.
	In altre parole, la finestra di console di TRACE visualizzera' sempre e solo i dati relativi all'Explorer, o per
	meglio dire, i dati dei processi esplicitamente presenti nella whitelist (a parte dell'Explorer che ovviamente e'
	abilitato per default).
	*/

	// se il processo NON e' l'Explorer (ovviamente abilitato by design), allora controlla nella whitelist
	if(name==L"explorer.exe")
	{
		bAllowed = true;
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): process %s is allowed by design\n",name.c_str()));
	}
	else
	{
		// controlla la whitelist della configurazione o ripiega sull'elenco di fallback
		std::wstring whitelist = global_stConfig.whitelist;
		if(whitelist.empty())
			whitelist = L"regsvr32.exe,ebtl.exe";
		else
			std::transform(whitelist.begin(),whitelist.end(),whitelist.begin(),::tolower);

		if(IsProcessInList(whitelist,name))
		{
			bAllowed = true;
			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): process %s is in the whitelist\n",name.c_str()));
		}
		else
		{
			bAllowed = false;
			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): process %s is NOT in the whitelist\n",name.c_str()));
		}
	}

	// idem come sopra, pero per i processi abilitati per il disegno dell'immagine di fondo nei dialoghi File Apri/Salva
	std::wstring filedialoglist = global_stConfig.filedialoglist;
	if(!filedialoglist.empty())
	{
		std::transform(filedialoglist.begin(),filedialoglist.end(),filedialoglist.begin(),::tolower);

		if(IsProcessInList(filedialoglist,name))
		{
			bAllowed = true;
			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): process %s is in the filedialoglist\n",name.c_str()));
		}
		else
		{
			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"ShouldLoad(): process %s is NOT in the filedialoglist\n",name.c_str()));
		}
	}

	return(bAllowed);
}

/*
	IsProcessInList()

	Verifica se il processo si trova nella lista di quelli abilitati al caricamento della DLL.

	Usata per il disegno dell'immagine sullo sfondo della vista files (explorer.exe) o per il 
	disegno dello sfondo nei dialoghi File Apri/Salva.
*/
bool IsProcessInList(const std::wstring& processList,const std::wstring& processName)
{
	if(processList.empty() || processName.empty())
		return(false);

	size_t pos = processList.find(processName);
	while(pos!=std::wstring::npos)
	{
		bool startMatch = (pos==0 || processList[pos - 1]==L',');
		bool endMatch = (pos + processName.length()==processList.length() || processList[pos + processName.length()]==L',');

		if(startMatch && endMatch)
			return(true);

		pos = processList.find(processName,pos + 1);
	}

	return(false);
}

/*
	GetCurDllDir()

	Ricava la directory corrente per la DLL.
*/
std::wstring GetCurDllDir(void)
{
	wchar_t wzPath[MAX_PATH+1] = {0};

	// GetModuleFileNameW() restituisce il numero di caratteri effettivamente copiati
	int nLen = ::GetModuleFileNameW(global_hModule,wzPath,MAX_PATH);

	// costruisce la std::wstring passando la lunghezza gia' nota
	// questo evita al C++ di dover calcolare la lunghezza con wcslen() internamente
	std::wstring dllPath(wzPath,nLen);
	dllPath = dllPath.substr(0,dllPath.rfind(L"\\"));

	return(dllPath);
}

/*
	LoadSettings()

	Carica la configurazione corrente dal file relativo (config.ini).

	Il file deve risiedere nella stessa directory della DLL.

	Qui si occupa di caricare i vari flags (eccetuando le immagini), in modo tale che eventuali modifiche al
	file di configurazione si riflettano al volo. La funzione viene infatti chiamata nella OnWindowLoad(),
	ossia per ogni nuova finestra aperta (dall'Explorer) su cui disegnare.
	Il codice deve essere quindi mantenuto il piu' leggero e rapido possibile. La logica che usa e' quella di
	leggere il file ed usare un lock solo nel momento esatto in cui aggiorna i valori di configurazione globali
	(usati da ogni nuovo thread dell'Explorer) con quelli appena letti.

	Le immagini, al contrario, vengono caricare a parte SOLO una volta, al primo avvio in assoluto, quindi per
	riflettere eventuali cambi nella directory relativa ("Image"), bisogna scaricare e ricaricare la DLL con il
	loader (ebtl.exe) o riavviare Windows.
*/
void LoadSettings(void)
{
	//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadSettings()\n"));

	// legge dal file a pelo, senza nessun lock
	Config stConfig;
	std::wstring str;
	std::wstring cfgPath = GetCurDllDir() + L"\\config.ini";

	stConfig.whitelist		= GetIniString(cfgPath.c_str(),L"load",L"whitelist");
	stConfig.filedialog		= GetIniString(cfgPath.c_str(),L"load",L"filedialog")==L"true" ? true : false;
	stConfig.filedialoglist	= GetIniString(cfgPath.c_str(),L"load",L"filedialoglist");
	stConfig.showerrors		= GetIniString(cfgPath.c_str(),L"load",L"showerrors")==L"true" ? true : false;

	str = GetIniString(cfgPath.c_str(),L"explorer",L"backgroundalpha");
	if(str.empty())
		stConfig.backgroundalpha = ALPHABLEND_DEFAULT;
	else
	{
		stConfig.backgroundalpha = std::stoi(str);
		if(stConfig.backgroundalpha < ALPHABLEND_MIN || stConfig.backgroundalpha > ALPHABLEND_MAX)
			stConfig.backgroundalpha = ALPHABLEND_MAX;
	}
	str = GetIniString(cfgPath.c_str(),L"explorer",L"foregroundalpha");
	if(str.empty())
		stConfig.foregroundalpha = ALPHABLEND_MAX;
	else
	{
		stConfig.foregroundalpha = std::stoi(str);
		if(stConfig.foregroundalpha < ALPHABLEND_MIN || stConfig.foregroundalpha > ALPHABLEND_MAX)
			stConfig.foregroundalpha = ALPHABLEND_MAX;
	}

	str = GetIniString(cfgPath.c_str(),L"image",L"maximages");
	if(str.empty())
		stConfig.maximages	= IMAGES_MAX_DEFAULT;
	else
	{
		stConfig.maximages = std::stoi(str);
		if(stConfig.maximages < 1 || stConfig.maximages > IMAGES_MAX_ALLOWED)
			stConfig.maximages = IMAGES_MAX_DEFAULT;
	}
	str = GetIniString(cfgPath.c_str(),L"image",L"displaymode");
	if(str.empty())
		str = L"0";
	stConfig.displaymode = std::stoi(str);
	if(stConfig.displaymode < IMAGES_MODE_MIN || stConfig.displaymode > IMAGES_MODE_MAX)
		stConfig.displaymode = IMAGES_MODE_BOTTOM_RIGHT;
	str = GetIniString(cfgPath.c_str(),L"image",L"alphablend");
	if(str.empty())
		stConfig.alphablend = ALPHABLEND_MAX;
	else
	{
		int alpha = std::stoi(str);
		if(alpha > ALPHABLEND_MAX)
			alpha = ALPHABLEND_MAX;
		if(alpha < ALPHABLEND_MIN)
			alpha = ALPHABLEND_MIN;
		stConfig.alphablend = (BYTE)alpha;
	}
	stConfig.random			= GetIniString(cfgPath.c_str(),L"image",L"random")==L"true" ? true : false;
	stConfig.customfolder	= GetIniString(cfgPath.c_str(),L"image",L"customfolder");
	stConfig.specialfolders	= GetIniString(cfgPath.c_str(),L"image",L"specialfolders")==L"true" ? true : false;
	stConfig.graphicsformats= GetIniString(cfgPath.c_str(),L"image",L"graphicsformats");

	// lock "mordi e fuggi" per non scasinare con i vari lock di sistema
	AcquireSRWLockExclusive(&global_SRWLock);
	global_stConfig.whitelist		= stConfig.whitelist;
	global_stConfig.filedialog		= stConfig.filedialog;
	global_stConfig.filedialoglist	= stConfig.filedialoglist;
	global_stConfig.showerrors		= stConfig.showerrors;
	global_stConfig.backgroundalpha	= stConfig.backgroundalpha;
	global_stConfig.foregroundalpha	= stConfig.foregroundalpha;
	global_stConfig.maximages		= stConfig.maximages;
	global_stConfig.displaymode		= stConfig.displaymode;
	global_stConfig.alphablend		= stConfig.alphablend;
	global_stConfig.random			= stConfig.random;
	global_stConfig.customfolder	= stConfig.customfolder;
	global_stConfig.specialfolders	= stConfig.specialfolders;
	global_stConfig.graphicsformats	= stConfig.graphicsformats;
	ReleaseSRWLockExclusive(&global_SRWLock);
}

/*
	LoadImages()

	Carica la lista con le immagini da usare come sfondo per la finestra dell'Explorer.

	Le immagini devono risiedere nella sub-directory "Image" della directory in cui si trova la DLL
	o in quella specificata nel campo "customfolder" del file .ini.
	Notare che le immagini vengono caricate SOLO un volta al primo avvio in assoluto, quindi per 
	riflettere eventuali cambi nella directory relativa, bisogna scaricare e ricaricare la DLL con 
	il loader (ebtl.exe) o riavviare Windows.

	A differenza del caricamento del resto dei valori di configurazione, qui si deve usare un lock 
	ed obbligare i vari threads ad aspettare che il primo che arriva termini il caricamento della
	lista delle immagini.

	Per evitare che un mutex possa scasinare con i lock di sistema ed i threads dell'Explorer, qui
	usa il meccanismo provvisto dall'API InitOnceExecuteOnce().
*/
void LoadImages(void)
{
	TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadImages()\n"));

	// garantisce l'atomicita' assoluta, gestita direttamente dal kernel di Windows
	// se 10 thread arrivano qui insieme, solo uno eseguira' la LoadImagesCallback()
	// gli altri 9 dovranno aspettare la fine del caricamento prima di proseguire
	// NON interferisce con i mutex del Kernel
	InitOnceExecuteOnce(&global_InitOnceImg,LoadImagesCallback,NULL,NULL);
}

/*
	LoadImagesCallback()

	Carica la lista delle immagini.

	Vedi le note precedenti nelle funzioni di caricamento della configurazione.

	Notare che, a differenza dell'originale, stabilisce un limite al numero di immagini che possono
	essere caricate (la directory potrebbe contenere un milione di files) e carica, sempre entro il
	limite massimo, tutte le immagini, in modo che si possa poi cambiare al volo la modalita' per la
	selezione delle immagini (la prima o una random) ogni volta che cambia il valore nel file .ini.

	In seguito alle ultime modifiche, carica le immagini per gli special folders in una lista a parte.
	
	Tenere presente che il totale di memoria allocata derivante dal caricamento dei file puo' essere
	critico, sopratutto quando si tratta di file che in realta' sono compressi, come i PNG, JPG, etc.
*/
BOOL CALLBACK LoadImagesCallback(PINIT_ONCE InitOnce,PVOID Parameter,PVOID* Context)
{
	std::wstring imgPath = global_stConfig.customfolder;
	if(imgPath.empty())
		imgPath = GetCurDllDir() + L"\\Image";
            
	TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadImagesCallback(): loading images from: %s\n",imgPath.c_str()));

	// controlla se la directory esiste
	if(DoesDirectoryExist(imgPath))
	{
		// formati da caricare
		std::wstring graphicsFormats = global_stConfig.graphicsformats;
		if(graphicsFormats.empty())
			graphicsFormats = L"*.png,*.jpg,*.jpeg,*.jpe,*.jfif,*.gif";
		std::wstring ext;
		std::vector<std::wstring> fileList;
		while(!graphicsFormats.empty())
		{
			ext = ExtractToken(graphicsFormats);
			EnumFiles(imgPath.c_str(),ext.c_str(),fileList);
		}

		TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"LoadImagesCallback(): %s (%d)\n",fileList.empty() ? L"ERROR: empty folder" : L"images found",fileList.size()));

		// nessun file presente nella directory
		if(fileList.empty() && global_stConfig.showerrors)
		{
			wchar_t wzBuffer[1024] = {0};
			swprintf_s(wzBuffer,1024-1,L"ERROR: the folder <%s> for Explorer background images is empty.",imgPath.c_str());
			MessageBoxW(NULL,wzBuffer,L"ExplorerBgToolRe",MB_ICONERROR|MB_SYSTEMMODAL|MB_OK);
			return(TRUE);
		}

#ifdef _DEBUG
		// elenca i formati supportati da GDI, giusto per vedere che combina piu' sotto con il caricamento
		// il supporto per WEBP in teoria dipende dai codecs installati, ma a me non funziona ne ho intenzione
		// di approfondire per il momento
		UINT num = 0;
		UINT size = 0;

		Gdiplus::GetImageDecodersSize(&num,&size);
		Gdiplus::ImageCodecInfo* pInfo = (Gdiplus::ImageCodecInfo*)malloc(size);
		Gdiplus::GetImageDecoders(num,size,pInfo);

		TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"LoadImagesCallback(): GDI supported formats:\n"));
		for(UINT i=0; i < num; i++)
			TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"LoadImagesCallback(): %s\n",pInfo[i].MimeType));

		free(pInfo);
#endif

		// per contabilizzare le immagini caricate (NON puo' superare il limite <n>)
		size_t loadedCount = 0;

		// notare che carica tutte le immagini (fino al limite <n>), solo poi decidira' se usare la prima o selezionare una
		// in modo random, per poter cambiare dinamicamente a seconda del valore corrente nel file .ini di configurazione
		for(auto& i : fileList)
		{
			// carica solo fino a <n> immagini
			if(loadedCount > (size_t)global_stConfig.maximages)
				break;

			// carica nella lista il nome del file, la dimensione ed il bitmap relativo
			BitmapGDI* bmp = new BitmapGDI(i.c_str());
			if(bmp->src)
			{
				global_stConfig.imageList.push_back({GetFileName(i.c_str()),GetFileSize(i.c_str()),bmp});
				loadedCount++;
				TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadImagesCallback(): loading %s\n",GetFileName(i.c_str()).c_str()));
			}
			else
			{
				delete bmp;
			}
		}

		// inizio caricamento immagini specifiche per gli special folders
		std::wstring cfgPath = GetCurDllDir() + L"\\config.ini";
		wchar_t wzSections[4096] = {0};
		DWORD dwSectionLen = ::GetPrivateProfileSectionNamesW(wzSections,4096,cfgPath.c_str());
		if(dwSectionLen > 0)
		{
			wchar_t* pwzSection = wzSections;
			while(*pwzSection)
			{
				std::wstring wzSectionName(pwzSection);
				
				// salta le sezioni di configurazione generale, deve leggere solo quelle per i folders
				// speciali, di cui non conosce il nome a priori
				if(wzSectionName!=L"load" && wzSectionName!=L"explorer" && wzSectionName!=L"image")
				{
					// ha beccato un folder speciale

					// ricava il campo "img" (controllando il limite <n>)
					std::wstring specialImgName = GetIniString(cfgPath.c_str(),pwzSection,L"img");
					if(!specialImgName.empty() && loadedCount <= (size_t)global_stConfig.maximages)
					{
						// se il nome di sezione contiene la wildcard, lo aggiunge alla lista relativa
						if(wcschr(wzSectionName.c_str(),L'*'))
						{
							iniData data = {wzSectionName,specialImgName};
							global_stConfig.dirSkeleton.push_back(data);
						}

						bool bAlreadyLoaded = false;
						BitmapGDI* existingBmp = nullptr;
						
						// verifica se l'immagine e' gia' stata caricata nella lista del calderone
						for(auto& img : global_stConfig.imageList)
						{
							if(img.fileName==specialImgName)
							{
								bAlreadyLoaded = true;
								existingBmp = img.bmp;
								break;
							}
						}
						
						// verifica se e' gia' stata inserita nella lista per gli special folders da un'altra sezione
						// (ossia se usa la stessa immagine per piu' special folders)
						for(auto& img : global_stConfig.specialFoldersImageList)
						{
							if(img.fileName==specialImgName)
							{
								bAlreadyLoaded = true;
								break;
							}
						}

						// la carica nella lista
						if(!bAlreadyLoaded)
						{
							// percorso completo del file
							std::wstring fullImgPath = imgPath + L"\\" + specialImgName;
							if(::GetFileAttributesW(fullImgPath.c_str())==INVALID_FILE_ATTRIBUTES)
							{
								// prova con il percorso assoluto o con il relativo alla working directory
								fullImgPath = specialImgName;
								if(::GetFileAttributesW(fullImgPath.c_str())==INVALID_FILE_ATTRIBUTES)
									fullImgPath = GetCurDllDir() + L"\\" + specialImgName;
							}
							if(::GetFileAttributesW(fullImgPath.c_str())!=INVALID_FILE_ATTRIBUTES)
							{
								BitmapGDI* bmp = new BitmapGDI(fullImgPath.c_str());
								if(bmp->src)
								{
									global_stConfig.specialFoldersImageList.push_back({specialImgName,GetFileSize(fullImgPath.c_str()),bmp});
									loadedCount++;
									TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadImagesCallback(): loading %s for the special folder %s\n",specialImgName.c_str(),wzSectionName.c_str()));
								}
								else
								{
									delete bmp;
									TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"LoadImagesCallback(): ERROR: unable to load %s\n",fullImgPath.c_str()));
								}
							}
						}
						else if(existingBmp!=nullptr)
						{
							// se gia' esiste, crea un alias riutilizzando lo stesso puntatore
							global_stConfig.specialFoldersImageList.push_back({specialImgName,0,existingBmp});
							TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"LoadImagesCallback(): %s has been aliased because already exists\n",specialImgName.c_str()));
						}
					}
				}

				pwzSection += wzSectionName.length() + 1;
			}
		}
		// fine caricamento
	}
	else
	{
		TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"LoadImagesCallback(): ERROR: no such folder\n"));
		if(global_stConfig.showerrors)
		{
			wchar_t wzBuffer[1024] = {0};
			swprintf_s(wzBuffer,1024-1,L"ERROR: the folder <%s> for Explorer background images does not exist.",imgPath.c_str());
			MessageBoxW(NULL,wzBuffer,L"ExplorerBgToolRe",MB_ICONERROR|MB_SYSTEMMODAL|MB_OK);
		}
	}

	return(TRUE);
}

/*
	DoesPathMatchData()

	Controlla se il path ricevuto soddisfa le logiche di prefisso degli skeleton
	contenuti nel vettore.
*/
int DoesPathMatchData(const std::wstring& path,const std::vector<iniData>& dirSkeleton)
{
	size_t pathLen = path.length();

	for(int i=0; i < (int)dirSkeleton.size(); ++i) 
	{
		const std::wstring& skeleton = dirSkeleton[i].section;
		size_t skelLen = skeleton.length();

		// verifica se lo skeleton termina con l'asterisco '*'
		if(skelLen > 0 && skeleton[skelLen-1]==L'*') 
		{
			size_t prefixLen = skelLen - 1; // lunghezza del prefisso (senza l'asterisco)

			// caso speciale: lo skeleton termina con "\*" (es. "C:\TMP\*")
			if(skelLen >= 2 && skeleton[skelLen-2]==L'\\') 
			{
				// deve matchare la directory stessa (es. "C:\TMP")
				// controlla se la lunghezza del path corrisponde allo skeleton meno "\*"
				if(pathLen==prefixLen-1 && _wcsnicmp(path.c_str(),skeleton.c_str(),prefixLen-1)==0) 
					return(i);
			}

			// regola generale del prefisso:
			// per "C:\TMP*", il prefisso e' "C:\TMP" (matcha C:\TMP, C:\TMPDIR, C:\TMP\sub)
			// per "C:\TMP\*", il prefisso e' "C:\TMP\" (matcha C:\TMP\sub, ma NON C:\TMPDIR)
			if(pathLen >= prefixLen && _wcsnicmp(path.c_str(),skeleton.c_str(),prefixLen)==0) 
				return(i);
		} 
		else 
		{
			// se lo skeleton NON ha l'asterisco, e' necessario un match esatto (case-insensitive)
			if(pathLen==skelLen && _wcsicmp(path.c_str(),skeleton.c_str())==0)
				return(i);
		}
	}

	return(-1);
}

/*
	GetLocalData()

	Ottiene (false) o alloca on-demand (true) la struttura dati del thread corrente.
*/
MyData* GetLocalData(bool bCreateOnDemand)
{
	if(global_dwTlsIndex==TLS_OUT_OF_INDEXES)
		return(nullptr);
    
	MyData* pData = (MyData*)TlsGetValue(global_dwTlsIndex);
	if(!pData && bCreateOnDemand)
	{
		pData = new MyData();
		TlsSetValue(global_dwTlsIndex,pData);
	}

	return(pData);
}

/*
	GetHWNDFromDC()

    Recupera l'HWND della finestra associata al DC.
*/
HWND GetHWNDFromDC(HDC hdc)
{
	HWND hWnd = ::WindowFromDC(hdc);
	return(hWnd);
}

/*
	GetHWNDFromDUI()
	
	Per estrarre l'HWND associato ad un DC.
*/
HWND GetHWNDFromDUI(HDC dc,MyData::data& outData)
{
	MyData* pLocalData = GetLocalData(false);
	if(pLocalData)
	{
		for(auto& pair : pLocalData->duiList)
		{
			if(pair.second.hdc==dc)
			{
				outData = pair.second;
				return(pair.first);
			}
		}
	}
	
	return(0);
}

/*
	OnWindowLoad()

	Viene chiamata per ogni finestra nuova che viene aperta dall'Explorer.

	Approfitta la prima chiamata in assoluto per inizializzare, caricare le immagini ed impostare gli hooks,
	altrimenti si limita a (ri)caricare la configurazione dal .ini per applicare al volo gli eventuali cambi.
*/
void OnWindowLoad(void)
{
	// prima chiamata in assoluto, inizializza, crea gli hooks e carica la configurazione/immagini
	if(!global_bHooked)
	{
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnWindowLoad(): still NOT hooked\n"));

		global_bHooked = TRUE;

		// carica la configurazione
		LoadSettings();

		// deve caricare GDI prima di caricare le immagini
		Gdiplus::GdiplusStartupInput StartupInput;
		Gdiplus::Status ret = GdiplusStartup(&global_GdiPlusToken,&StartupInput,NULL);
		if(ret!=Gdiplus::Ok)
		{
			TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"OnWindowLoad(): FATAL ERROR initializing GDI\n"));
			global_bInitFailed = TRUE;
			if(global_stConfig.showerrors)
			{
				MessageBoxW(NULL,L"FATAL ERROR: Unable to initialize GDI+.",L"ExplorerBgToolRe",MB_ICONERROR|MB_SYSTEMMODAL|MB_OK);
				return;
			}
		}

		// carica le immagini
		LoadImages();

		/*
			crea gli hooks
			il codice non "intercetta" i messaggi (tipo WM_PAINT etc.) ma "dirotta" le funzioni specificate
			ad es., quando l'Explorer riceve un WM_PAINT, per disegnarsi deve chiamare la BeginPaint() e la
			FillRect(), ma qui la DLL, grazie alla libreria MinHook, intercetta tali chiamate e le dirotta 
			alle funzioni proprie
			basicamente, la macro CreateMHook() prende la funzione originale di Windows (es. FillRect()), ci 
			piazza un salto (JMP) in memoria verso la funzione della DLL (MyFillRect()) e salva il vecchio 
			indirizzo in _FillRect_ per poter chiamare poi il codice originale
			notare che messaggi come WM_SIZE e WM_WINDOWPOSCHANGED sono puri messaggi inviati alla finestra e 
			non funzioni API globali che possano essere intercettati con MinHook, quindi per catturarli bisogna 
			fare il subclassing della finestra CreateWindowExW()
		*/
		MH_STATUS ms = MH_Initialize();
		if(ms==MH_OK)
		{
			CreateMHook(CreateWindowExW,	MyCreateWindowExW,		_CreateWindowExW_,		1);
			CreateMHook(DestroyWindow,		MyDestroyWindow,		_DestroyWindow_,		2);
			CreateMHook(BeginPaint,			MyBeginPaint,			_BeginPaint_,			3);
			CreateMHook(FillRect,			MyFillRect,				_FillRect_,				4);
			CreateMHook(CreateCompatibleDC,	MyCreateCompatibleDC,	_CreateCompatibleDC_,	5);

			MH_EnableHook(MH_ALL_HOOKS);

			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnWindowLoad(): hooks set correctly\n"));
		}
		else
		{
			TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"OnWindowLoad(): FATAL ERROR setting hooks\n"));
			global_bInitFailed = TRUE;
			if(global_stConfig.showerrors)
			{
				wchar_t wzError[_MAX_PATH+1] = {L"Unknown fatal error."};
				switch(ms)
				{
					case MH_ERROR_ALREADY_INITIALIZED:	wcscpy_s(wzError,_MAX_PATH,L"ERROR: MinHook is already initialized.");																			break;
					case MH_ERROR_NOT_INITIALIZED:		wcscpy_s(wzError,_MAX_PATH,L"ERROR: MinHook is not initialized yet, or already uninitialized.");												break;
					case MH_ERROR_ALREADY_CREATED:		wcscpy_s(wzError,_MAX_PATH,L"ERROR: The hook for the specified target function is already created.");											break;
					case MH_ERROR_NOT_CREATED:			wcscpy_s(wzError,_MAX_PATH,L"ERROR: The hook for the specified target function is not created yet.");											break;
					case MH_ERROR_ENABLED:				wcscpy_s(wzError,_MAX_PATH,L"ERROR: The hook for the specified target function is already enabled.");											break;
					case MH_ERROR_DISABLED:				wcscpy_s(wzError,_MAX_PATH,L"ERROR: The hook for the specified target function is not enabled yet, or already disabled.");						break;
					case MH_ERROR_NOT_EXECUTABLE:		wcscpy_s(wzError,_MAX_PATH,L"ERROR: The specified pointer is invalid. It points the address of non-allocated and/or non-executable region.");	break;
					case MH_ERROR_UNSUPPORTED_FUNCTION:	wcscpy_s(wzError,_MAX_PATH,L"ERROR: The specified target function cannot be hooked.");															break;
					case MH_ERROR_MEMORY_ALLOC:			wcscpy_s(wzError,_MAX_PATH,L"ERROR: Failed to allocate memory.");																				break;
					case MH_ERROR_MEMORY_PROTECT:		wcscpy_s(wzError,_MAX_PATH,L"ERROR: Failed to change the memory protection.");																	break;
					case MH_ERROR_MODULE_NOT_FOUND:		wcscpy_s(wzError,_MAX_PATH,L"ERROR: The specified module is not loaded.");																		break;
					case MH_ERROR_FUNCTION_NOT_FOUND:	wcscpy_s(wzError,_MAX_PATH,L"ERROR: The specified function is not found.");																		break;
				}
				wcscat_s(wzError,_MAX_PATH,L"\n(failed to initialize disassembly, probably extension already loaded)");
				MessageBoxW(NULL,wzError,L"ExplorerBgToolRe",MB_ICONERROR|MB_SYSTEMMODAL|MB_OK);
			}
			return;
		}
	}
	else // chiamate successive, se l'inizializzazione ando' a buon fine ricarica la configurazione (NON le immagini)
	{
		//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnWindowLoad(): ALREADY hooked\n"));

		if(!global_bInitFailed)
			LoadSettings();
	}
}

/*
	OnDocComplete()

	Viene chiamata alla fine dell'apertura della finestra, permettendo di avere uno sfondo diverso per ogni singolo folder, inclusi
	(alcuni) folders virtuali di sistema.

	Quando Explorer finisce di caricare una schermata (da qui il nome OnDocComplete, che ricalca l'evento standard DocumentComplete
	delle interfacce della Shell come IWebBrowser2 o DWebBrowserEvents2), la DLL riceve il path del folder corrente.

	Quando si tratta di un folder normale, il path sara' qualcosa tipo C:\Users\admin\Pictures, ma se si naviga in "Questo PC", il
	path sara' la stringa del CLSID virtuale ::{20D04FE0-3AEA-1069-A2D8-08002B30309D}.

	Tale path (sia esso un percorso reale o un CLSID speciale), viene usato come nome della sezione (il testo tra parentesi quadre
	[...] nel file .ini) in cui cercare la chiave "img". Se tale chiave viene trovata, la funzione cerca il nome del file nella lista
	delle immagini, assegnando il suo indice alla struttura dati della finestra corrente (duiList) se la ricerca va a buon fine.

	Comunque sia, in alcuni casi, come per il Control Panel, il disegno dello sfondo NON funziona, a prescindere se si usa il CLSID
	"moderno" (::{5399E694-6CE5-4D6C-8FCE-1D8870FDCBA0}) o quello antico (::{21EC2020-3AEA-1069-A2DD-08002B30309D}), perche' Windows
	usa metodi come DirectUI che impediscono che il meccanismo usato dalla DLL per il disegno della sfondo della finestra funzioni
	correttamente.

	Con l'architettura "legacy" (quella usata dalla DLL) le cartelle classiche usano ShellView e controlli standard come SysListView32
	o SysTreeView32. La DLL, tramite l'hook di CreateWindowExW, intercetta la creazione di queste finestre, ottiene l'handle relativo e
	e ci disegna dentro.

	Con l'architettura "DirectUI" invece viene usato un "canvas" di rendering grafico, per cui Explorer non crea una finestra "cartella",
	ma crea un'area di rendering dove il sistema "dipinge" i vari elementi. In tal caso, quando la DLL prova a intercettare il momento in
	cui la "finestra" viene creata, o non trova nulla da agganciare, o il motore grafico di Windows sovrascrive istantaneamente qualsiasi
	cosa si provi a disegnare in quell'area, perche' il canvas e' gestito da un processo di rendering che ha la priorita' assoluta sugli
	hooks di basso livello.
*/
void OnDocComplete(std::wstring path,DWORD threadID)
{
	// il thread che riceve DocumentComplete e' lo stesso thread UI proprietario della pagina (la schermata di Explorer)
	if(global_stConfig.specialfolders)
	{
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnDocComplete(): enabled, received path: %s (%ld)\n",path.c_str(),::GetCurrentThreadId()));

		// controlla se il path corrisponde in realta' ad un CLSID, restituendolo in tal caso
		// (es. se viene scritto il percorso reale del folder My Documents invece di specificare il CLSID)
		// occhio perche' solo verifica contro alcuni (pochi) CLSID definiti internamente
		// prestare attenzione alle conversioni, perche' potrebbero vanificare l'eventuale path con wildcard (se
		// traduce un path a CLSID, il CLSID non fara' mai match con i valori con * nella lista dei nomi per gli
		// special folders)
		std::wstring normalizedPath = PathToCLSID(path);

		// qui deve leggere obbligatoriamente dal file .ini, perche' non conosce a priori il nome del path che riceve
		std::wstring cfgPath = GetCurDllDir() + L"\\config.ini";
		std::wstring fileName = GetIniString(cfgPath.c_str(),normalizedPath.c_str(),L"img");

		// il path ricevuto non esiste come nome di sezione special folder, verifica allora se tale path puo' fare
		// match con gli eventuali nomi di sezione con * caricati nella lista corrispondente
		if(fileName.empty())
		{
			// se quanto ricevuto ("C:\Windows\System") dovesse fare match con uno special folder con * ("C:\Windows*"),
			// allora ricava l'immagine associata
			int index = DoesPathMatchData(path,global_stConfig.dirSkeleton);
			if(index >= 0)
			{
				fileName = global_stConfig.dirSkeleton[index].value;
			}
			else
			{
				TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"OnDocComplete(): no image defined for %s\n",normalizedPath.c_str()));
				return;
			}
		}
		else
		{
			TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnDocComplete(): image for %s is %s\n",normalizedPath.c_str(),fileName.c_str()));
		}

		// cerca l'immagine (per nome file) nelle liste per poter ricavare il puntatore al bitmap relativo
		MyData* pLocalData = GetLocalData(false);
		if(pLocalData && !pLocalData->duiList.empty())
		{
			bool imageFound = false;
			BitmapGDI* pTargetBmp = nullptr;

			// cerca prima nella lista delle immagini per i folder speciali
			for(auto& img : global_stConfig.specialFoldersImageList)
			{
				if(img.fileName==fileName)
				{
					pTargetBmp = img.bmp;
					imageFound = true;
					TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnDocComplete(): %s found in special folder image list\n",fileName.c_str()));
					break;
				}
			}

			// non trovata, cerca nella lista del calderone
			if(!imageFound)
			{
				for(auto& img : global_stConfig.imageList)
				{
					if(img.fileName==fileName)
					{
						pTargetBmp = img.bmp;
						imageFound = true;
						TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnDocComplete(): %s found in standard image list\n",fileName.c_str()));
						break;
					}
				}
			}

			// applica il risultato alle finestre di questo thread e forza il ridisegno
			// (la InvalidateRect() provochera' l'invio di un WM_PAINT che attivera' la MyFillRect())
			if(imageFound && pTargetBmp)
			{
				for(auto& pair : pLocalData->duiList)
				{
					pair.second.pActiveBmp = pTargetBmp; // aggancio del puntatore al bitmap
					::InvalidateRect(pair.first, NULL, TRUE); 
				}
			}
			else // immagine specificata nel .ini ma fisicamente non esistente o _caricamento_fallito_per_formato_NON_supportato_
			{
				TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"OnDocComplete(): ERROR: image %s for special folder %s cannot be loaded (unsupported format or no such file)\n",fileName.c_str(),normalizedPath.c_str()));

				for(auto& pair : pLocalData->duiList)
				{
					pair.second.pActiveBmp = nullptr; // lo sfondo della finestra dell'Explorer rimarra' vuoto
					::InvalidateRect(pair.first,NULL,TRUE);
				}

				if(global_stConfig.showerrors)
				{
					wchar_t wzBuffer[1024] = {0};
					swprintf_s(wzBuffer,1024-1,L"ERROR: the image <%s> specified for the special folder <%s> cannot be loaded (unsupported format or no such file).",fileName.c_str(),normalizedPath.c_str());
					MessageBoxW(NULL,wzBuffer,L"ExplorerBgToolRe",MB_ICONERROR|MB_SYSTEMMODAL|MB_OK);
				}
			}
		}
	}
	else
	{
		TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"OnDocComplete(): disabled (%ld)\n",::GetCurrentThreadId()));
	}
}

/*
	ExplorerSubclassProc()

	Effettua il subclassing della finestra dell'Explorer.

	All'inizio la DLL, dopo aver filtrato i processi che la possono caricare (explorer.exe ed il loader), aggancia con un
	hook a livello globale le funzioni necessarie per il disegno dell'immagine nella finestra.
	Questo significa che nei processi in cui viene iniettata, le funzioni agganciate vengono dirottate al codice della DLL
	e dato che tali funzioni includono la CreateWindow, qui puo' effettuare il subclassing della finestra ed impostare il
	livello di trasparenza.
*/
LRESULT CALLBACK ExplorerSubclassProc(HWND hWnd,UINT uMsg,WPARAM wParam,LPARAM lParam)
{
    MyData* pLocalData = GetLocalData(false);
    WNDPROC oldProc = NULL;

    if (pLocalData)
    {
        auto __iter = pLocalData->parentList.find(hWnd);
        if (__iter != pLocalData->parentList.end())
        {
            oldProc = __iter->second;
        }
    }

    switch (uMsg)
    {
		// deve mettersi in ascolto del messaggio WM_NCACTIVATE, non importa quale controllo interno 
		// (lista file, menu, barra percorsi) l'utente stia cliccando, dato che questo e' il messaggio
		// che permette intercettare istantaneamente l'attivazione della macro-finestra
        case WM_NCACTIVATE:
        {
            // wParam e' TRUE se la finestra si attiva, FALSE se diventa inattiva
            BOOL bWindowActive = (BOOL)wParam;
            
            if(!bWindowActive)
            {
                // finestra in secondo piano (inattiva): diventa piu' trasparente (es. 100)
                ::SetLayeredWindowAttributes(hWnd,0,global_stConfig.backgroundalpha,LWA_ALPHA);
            }
            else
            {
                // finestra in primo piano (attiva): torna solida/leggibile (es. 255)
                ::SetLayeredWindowAttributes(hWnd,0,global_stConfig.foregroundalpha,LWA_ALPHA);
            }
        }
        break;

        case WM_DESTROY:
            if(pLocalData)
                pLocalData->parentList.erase(hWnd);
            break;
    }

	// chiama la WndProc originale del processo, salvata in precedenza
    if(oldProc)
        return(::CallWindowProcW(oldProc,hWnd,uMsg,wParam,lParam));
        
    return(::DefWindowProcW(hWnd,uMsg,wParam,lParam));
}

/*
	MyCreateWindowExW()

	Hook per la creazione della fienstra.
*/
HWND MyCreateWindowExW(	DWORD     dwExStyle,
						LPCWSTR   lpClassName,
						LPCWSTR   lpWindowName,
						DWORD     dwStyle,
						int       X,
						int       Y,
						int       nWidth,
						int       nHeight,
						HWND      hWndParent,
						HMENU     hMenu,
						HINSTANCE hInstance,
						LPVOID    lpParam)
{
	static int nPrevIndex = -1;

	// permette ad Explorer creare la finestra
	HWND hWnd = _CreateWindowExW_(dwExStyle,lpClassName,lpWindowName,dwStyle,X,Y,nWidth,nHeight,hWndParent,hMenu,hInstance,lpParam);
	if(hWnd)
	{
		// ricava il nome della classe della finestra
		std::wstring ClassName = ::GetWindowClassName(hWnd);

		// questa TRACE, commentata, e' comunque interessante per togliersi lo sfizio di vedere scorrere i nomi delle varie "finestre" dell'Explorer...
		//TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"MyCreateWindowExW(): classname: %s\n",ClassName.c_str()));

		// deve essere la CabinetWClass (la cornice esterna di Explorer che include la barra del titolo, i pulsanti del menu, etc.)
		if(ClassName==L"CabinetWClass")
		{
			// inizio subclassing della finestra pricipale per la trasparenza
			MyData* pLocalData = GetLocalData(true);
			if(pLocalData)
			{
				// abilita lo stile Layered per la trasparenza
				LONG_PTR exStyle = ::GetWindowLongPtrW(hWnd,GWL_EXSTYLE);
				::SetWindowLongPtrW(hWnd,GWL_EXSTYLE,exStyle | WS_EX_LAYERED);

				// forza la applicazione del cambio di stile
				::SetWindowPos(hWnd,NULL,0,0,0,0,SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

				// imposta la trasparenza iniziale
				::SetLayeredWindowAttributes(hWnd,0,global_stConfig.backgroundalpha,LWA_ALPHA);

				// applica il subclassing
				// la SetWindowLongPtrW() restituisce l'indirizzo della procedura originale di Explorer, lo salva
				// nella mappa affinche il codice in ExplorerSubclassProc() possa poi effettuare la chiamata
				WNDPROC oldProc = (WNDPROC)::SetWindowLongPtrW(hWnd,GWLP_WNDPROC,(LONG_PTR)ExplorerSubclassProc);
        
				pLocalData->parentList[hWnd] = oldProc;
			}
			// fine subclassing
		}

		// Explorer oggigiorno usa il sistema DirectUI, quindi si assicura di agganciare la classe relativa e la
		// parte della finestra dove vengono elencati i files (la "vista della Shell")
		// insomma, cerca la DirectUIHWND pero' solo se suo padre e' SHELLDLL_DefView, questo per distinguere perche'
		// il sistema DirectUI viene usato per qualsiasi controllo
		if(ClassName==L"DirectUIHWND" && ::GetWindowClassName(hWndParent)==L"SHELLDLL_DefView")
		{
			HWND parent = ::GetParent(hWndParent);
			auto clsname = ::GetWindowClassName(parent);
			if(clsname==L"ShellTabWindowClass" || clsname==L"#32770")
			{
				// alloca la mappa privata del thread corrente
				MyData* pLocalData = GetLocalData(true);
				if(pLocalData)
				{
					MyData::data data;
					data.hdc = 0;
					data.pActiveBmp = nullptr;
					size_t imgSize = global_stConfig.imageList.size();

					if(imgSize > 0)
					{
						// occhio, si puo' cambiare al volo il valore per random nel .ini, MA il nuovo valore (true/false) sara'
						// effettivo alla seconda nuova finestra visualizzata, non alla prima, perche' per ogni nuova finestra la
						// sequenza di chiamate e': prima MyCreateWindowExW() e poi OnWindowLoad(), che e' dove viene ricaricata
						// la configurazione, quindi per risolvere bisognerebbe rileggere il valore specifico qui dentro...
						// ...ma conviene?

						// devono esserci almeno 2 immagini per usare il modo random
						if(global_stConfig.random && imgSize > 1)
						{
							data.imgIndex = rand_w(0,(int)imgSize-1);	// fastrand
							if(data.imgIndex==nPrevIndex)				// se il nuovo indice e' uguale al precedente
							{
								do {
									data.imgIndex = rand_w(0,(int)imgSize-1);
								} while(data.imgIndex==nPrevIndex);
							}
							nPrevIndex = data.imgIndex;
						}
						else
							data.imgIndex = 0;
	
						data.pActiveBmp = global_stConfig.imageList[data.imgIndex].bmp; // aggancio bitmap

						TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"MyCreateWindowExW(): image index %d\n",data.imgIndex));
						TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"MyCreateWindowExW(): %s\n",data.pActiveBmp ? L"bitmap ptr OK" : L"ERROR: bitmap ptr is NULL!"));

						// inserisce nella mappa l'handle della finestra e l'indice dell'immagine da usare
						pLocalData->duiList.insert(std::make_pair(hWnd,data));
					}
					else
					{
						TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"MyCreateWindowExW(): ERROR: image list is empty\n"));
					}

					TRACEEXPR((_TRACE_FLAG_INFO,_L(__FILE__),__LINE__,L"MyCreateWindowExW(): loading (pointer set to) %s\n",global_stConfig.imageList[data.imgIndex].fileName.c_str()));
				}
			}
		}
	}

	return(hWnd);
}

/*
	MyDestroyWindow()
*/
BOOL MyDestroyWindow(HWND hWnd)
{
	// lascia che Windows esegua la distruzione della finestra (invio WM_DESTROY, etc.)
	// gli hook grafici seguiranno funzionando perche' i dati TLS continuano ad esistere
	BOOL ret = _DestroyWindow_(hWnd);

	// ora che la finestra e' "spenta", aggiorna i dati del thread corrente
	MyData* pLocalData = GetLocalData(false);
	if(pLocalData)
	{
		auto __iter = pLocalData->duiList.find(hWnd);
		if(__iter != pLocalData->duiList.end())
		{
			pLocalData->duiList.erase(__iter);
            
			// se questo thread ha chiuso l'ultima delle finestre che gestiva,
			// elimina la sua struttura MyData
			// stando fuori da DllMain, non rischia nessun Loader Lock Deadlock
			if(pLocalData->duiList.empty())
			{
				delete pLocalData;
				::TlsSetValue(global_dwTlsIndex,NULL);
			}
		}
	}

	return(ret);
}

/*
	MyBeginPaint()

	Cerca nel mappa l'handle della finestra e gli associa il DC.
*/
HDC MyBeginPaint(HWND hWnd,LPPAINTSTRUCT lpPaint)
{
	HDC hDC = _BeginPaint_(hWnd,lpPaint);

	MyData* pLocalData = GetLocalData(false);
	if(pLocalData)
	{
		auto __iter = pLocalData->duiList.find(hWnd);
		if(__iter != pLocalData->duiList.end())
			__iter->second.hdc = hDC;
	}

	return(hDC);
}

/*
	MyFillRect()

	Disegna il bitmap nella finestra di Explorer.

	La funzione viene intercettata durante il ciclo di disegno della finestra (spesso scatenata da un WM_PAINT).
	Chiamare InvalidateRect() dentro una routine di disegno e' una pratica rischiosa: dice a Windows che la finestra
	e' di nuovo "sporca" e che deve pianificare immediatamente un altro WM_PAINT. Anche se l'effetto "ciclo infinito"
	viene evitato, dato che poi sotto viene aggiornato pLocalData->size = wndSize, questo approccio causa un doppio 
	passaggio di disegno asincrono ad ogni ridimensionamento della finestra, motivo principale per cui i componenti 
	agganciati in Explorer spesso mostrano uno sfarfallio (flickering).
	La soluzione ideale sarebbe gestire il refresh della finestra a seguito del cambio di dimensioni, intercettando i
	messaggi WM_SIZE o WM_WINDOWPOSCHANGED della finestra di Explorer, lasciando che FillRect si limiti esclusivamente
	a disegnare la bitmap basandosi sulle dimensioni correnti.
	Pero', nella prova effettuata nel codice subclassando la finestra dell'Explorer, il redisegno NON funziona per cui
	si continua ad usare la soluzione originale...
*/
int MyFillRect(HDC hDC,const RECT* lprc,HBRUSH hbr)
{
	int ret = _FillRect_(hDC,lprc,hbr);

	// ricava l'handle della finestra a partire dal DC
	MyData::data _data;
	HWND hWnd = GetHWNDFromDUI(hDC,_data);
    
	// usa il puntatore diretto all'immagine attiva impostato in precedenza
	BitmapGDI* pBgBmp = _data.pActiveBmp;

	// se il puntatore e' nullo, prova ad usare il vecchio indice numerico
	if(!pBgBmp)
	{
		size_t imgSize = global_stConfig.imageList.size();
		if(imgSize > 0 && (_data.imgIndex >= 0 && _data.imgIndex < (int)imgSize))
			pBgBmp = global_stConfig.imageList[_data.imgIndex].bmp;
	}

	// se ha agganciato un'immagine valida, altrimenti esce lasciando lo sfondo originale di Explorer
	if(hWnd && pBgBmp) 
	{
		RECT pRc;
		::GetWindowRect(hWnd,&pRc);
		SIZE wndSize = {pRc.right - pRc.left,pRc.bottom - pRc.top};

		BOOL sizeChanged = FALSE;
		MyData* pLocalData = GetLocalData(false);
		if(pLocalData)
		{
			if(pLocalData->size.cx!=wndSize.cx || pLocalData->size.cy!=wndSize.cy)
				sizeChanged = TRUE;
		}
		if(sizeChanged && global_stConfig.displaymode!=0)
			::InvalidateRect(hWnd,0,TRUE);

		::SaveDC(hDC);
		::IntersectClipRect(hDC,lprc->left,lprc->top,lprc->right,lprc->bottom);

		// l'indice della lista delle immagini ha sfarfallato (ad es. "img=" file non trovato)
		// esce quindi senza disegnare nulla, lasciando lo sfondo originale di Explorer...
		// ...COMMENTATO, perche' ora usa il puntatore al bitmap, tale controllo lo farebbe
		// uscire per gli indici (non validi) delle immagini dei folder speciali
		//if(_data.imgIndex < 0 || _data.imgIndex >= global_stConfig.imageList.size())
		//	return(ret); 
		//BitmapGDI* pBgBmp = global_stConfig.imageList[_data.imgIndex].bmp;

		POINT pos;
		SIZE dstSize = {pBgBmp->Size.cx, pBgBmp->Size.cy};
		switch(global_stConfig.displaymode)
		{
			case 0:	pos = {0,0};
					break;
			case 1: pos.x = wndSize.cx - pBgBmp->Size.cx;
					pos.y = 0;
					break;
			case 2:	pos.x = 0;
					pos.y = wndSize.cy - pBgBmp->Size.cy;
					break;
			case 3: pos.x = wndSize.cx - pBgBmp->Size.cx;
					pos.y = wndSize.cy - pBgBmp->Size.cy;
					break;
			case 4:	pos.x = (wndSize.cx - pBgBmp->Size.cx) >> 1;
					pos.y = (wndSize.cy - pBgBmp->Size.cy) >> 1;
					break;
			case 5:	dstSize = {wndSize.cx,wndSize.cy};
					pos = {0,0};
					break;
			case 6:
			{
				// originale:
				/*static auto calcAspectRatio = [](int fromWidth,int fromHeight,int toWidthOrHeight,bool isWidth)
				{
					return(isWidth ? (int)round(((float)fromHeight * ((float)toWidthOrHeight / (float)fromWidth))) : (int)round(((float)fromWidth * ((float)toWidthOrHeight / (float)fromHeight))));
				};*/
				// nuovo: elimina la virgola mobile ed usa solo interi puri per prestazioni
				static auto calcAspectRatio = [](int fromWidth, int fromHeight, int toWidthOrHeight, bool isWidth)
				{
					if (isWidth)
						return (fromHeight * toWidthOrHeight + fromWidth / 2) / fromWidth;
					else
						return (fromWidth * toWidthOrHeight + fromHeight / 2) / fromHeight;
				};

				int newWidth = calcAspectRatio(pBgBmp->Size.cx,pBgBmp->Size.cy,wndSize.cy,FALSE);
				int newHeight = wndSize.cy;
				pos.x = (newWidth - wndSize.cx) / 2;
				if(pos.x!=0)
					pos.x = -pos.x;
				pos.y = 0;

				if(newWidth < wndSize.cx)
				{
					newWidth = wndSize.cx;
					newHeight = calcAspectRatio(pBgBmp->Size.cx,pBgBmp->Size.cy,wndSize.cx,TRUE);
					pos.x = 0;
					pos.y = (newHeight - wndSize.cy) / 2;
					if(pos.y!=0)
						pos.y = -pos.y;
				}
				dstSize = {newWidth,newHeight};
			}
			break;
			
			default:pos.x = wndSize.cx - pBgBmp->Size.cx;
					pos.y = wndSize.cy - pBgBmp->Size.cy;
					break;
		}

		/*
		se si vuole la massima velocita' e NON interessano le trasparenze dei PNG (es. usa solo JPG):
		- ultimo parametro di BLENDFUNCTION: 0 (equivale a: "ignora il canale alpha dei singoli pixel")
		- formato bitmap: e' completamente indifferente (ARGB o PARGB non cambia nulla, perche' Windows non guardera' mai quel canale)
		- risultato: piu' veloce, ma se carica un PNG trasparente, le zone trasparenti diventano nere

		se si vuole supporto totale per i PNG con trasparenze native, ombre e sfumature:
		- ultimo parametro di BLENDFUNCTION: AC_SRC_ALPHA (equivale a: "usa il canale alpha di ogni singolo pixel")
		- formato bitmap obbligatorio: PixelFormat32bppPARGB (Pre-moltiplicato), usando l'ARGB normale, la formula matematica di Windows
		  sballa e si ottengono gli aloni scuri sui bordi sfumati
		- risultato: trasparenze perfette, i PNG si vedono divinamente e lo sfondo si fonde perfettamente
		*/

		// originale: usava AC_SRC_ALPHA con "PixelFormat32bppARGB" nel costruttore di BitmapGDI in WinAPI.cpp
		//BLENDFUNCTION bf = {AC_SRC_OVER,0,global_stConfig.alphablend,AC_SRC_ALPHA};

		// modificato: usa AC_SRC_ALPHA con "PixelFormat32bppPARGB" nel costruttore di BitmapGDI in WinAPI.cpp
		// usando con 0 invece di AC_SRC_ALPHA, se l'immagine non ha trasparenze proprie sara' piu' veloce, ma
		// con alcuni PNG la trasparenza viene visualizzata oscura/nera
		//BLENDFUNCTION bf = { AC_SRC_OVER, 0, global_stConfig.alphablend, 0 };
		BLENDFUNCTION bf = {AC_SRC_OVER,0,global_stConfig.alphablend,AC_SRC_ALPHA};

		::AlphaBlend(hDC,pos.x,pos.y,dstSize.cx,dstSize.cy,pBgBmp->pMem,0,0,pBgBmp->Size.cx,pBgBmp->Size.cy,bf);

		::RestoreDC(hDC,-1);

		if(pLocalData)
			pLocalData->size = wndSize;
	}

	return(ret);
}

/*
	MyCreateCompatibleDC()
*/
HDC MyCreateCompatibleDC(HDC hDC)
{
	HDC retDC = _CreateCompatibleDC_(hDC);

	MyData* pLocalData = GetLocalData(false);
	if(pLocalData)
	{
		auto __iter = pLocalData->duiList.find(::WindowFromDC(hDC));
		if(__iter != pLocalData->duiList.end())
		{
			__iter->second.hdc = retDC;
			return(retDC);
		}
	}

	return(retDC);
}
