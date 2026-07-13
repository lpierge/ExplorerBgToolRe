/*
	WinAPI.cpp

	ExplorerBgToolRe(dux)
	Il progetto e' una refattorizzazione/riscrittura del progetto originale ExploreBgTool:

		copyright originale:
		WinAPI声明
		Author: Maple
		date: 2021-7-13 Create
		Copyright winmoes.com
		(reperibile su: https://github.com/Maplespe/explorerTool)

	Viene cambiato il disegno originale e vengono corretti vari bugs, sopratutto relativi a problemi di 
	concorrenza e di memoria (leaks e incongruenze varie), cercando di ottimizzare per quanto sia possibile.

	Vedi le note in dllmain.cpp

	Luca Piergentili, 18/06/2026
*/
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <shlobj.h>
#pragma comment(lib,"shell32.lib")
#include <shlwapi.h>
#pragma comment(lib,"shlwapi.lib")
#include <comdef.h>
#include <gdiplus.h>
#include "WinAPI.h"
#include <string>
#include <vector>
#include <cwchar>

#include "traceexpr.h"
#define _TRACE_FLAG			_TRFLAG_TRACECONSOLE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
//#define _TRACE_FLAG			_TRFLAG_NOTRACE // opzioni: _TRFLAG_NOTRACE, _TRFLAG_TRACEFILE, _TRFLAG_TRACECONSOLE, _TRFLAG_TRACEOUTPUT, _TRFLAG_TRACEBREAKPOINT
#define _TRACE_FLAG_INFO	_TRACE_FLAG
#define _TRACE_FLAG_WARN	_TRACE_FLAG
#define _TRACE_FLAG_ERR		_TRACE_FLAG

/*
	FileExists()

	Controlla l'esistenza del file.
	Presente come FileIsExist() nel progetto originale, e' stata riscritta completamente.
*/
bool FileExists(std::wstring filePath)
{
	bool bFileExists = false;
	HANDLE hHandle = INVALID_HANDLE_VALUE;
	
	if((hHandle = ::CreateFileW(filePath.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL))!=INVALID_HANDLE_VALUE)
	{
		::CloseHandle(hHandle);
		bFileExists = true;
	}
	
	return(bFileExists);
}

/*
	DoesDirectoryExist()

	Controlla l'esistenza della directory.
	Presente come FileIsExist() nel progetto originale, e' stata riscritta completamente.
*/
bool DoesDirectoryExist(std::wstring filePath,LPDWORD pdwLastError)
{
    if(pdwLastError)
		*pdwLastError = 0L;

	/* ricava gli attributi del pathname */
	DWORD dwAttributes = ::GetFileAttributesW(filePath.c_str());

	/* errore */
	if(dwAttributes==INVALID_FILE_ATTRIBUTES)
	{
		if(pdwLastError)
		{
			DWORD dwError = ::GetLastError();

			/* semplicemente non esiste il pathname (ad es. esiste C:\TMP ma non esiste C:\TMP\DATA) */
			/* FALSE + 0 */
			if(dwError==ERROR_FILE_NOT_FOUND || dwError==ERROR_PATH_NOT_FOUND)
				*pdwLastError = 0L;
			/* il pathname non e' valido nel sistema (non esiste fisicamente come risorsa, ad es. Z:) */
			/* FALSE + ERROR_INVALID_DRIVE|ERROR_INVALID_NAME|ERROR_BAD_PATHNAME */
			else if(dwError==ERROR_INVALID_DRIVE || dwError==ERROR_INVALID_NAME || dwError==ERROR_BAD_PATHNAME)
				*pdwLastError = dwError;
			/* FALSE + ERROR_INVALID_DATA */
			else
				*pdwLastError = ERROR_INVALID_DATA;
        }
		return(false);
    }

	/* non e' INVALID_FILE_ATTRIBUTES, ma e' realmente una directory esistente ? */
	return((dwAttributes & FILE_ATTRIBUTE_DIRECTORY)!=0L);
}

/*
	GetFileName()

	Estrae il nome file dal percorso completo.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente.
*/
std::wstring GetFileName(const wchar_t* lpPath)
{
    const wchar_t* lpSlash = wcsrchr(lpPath,L'\\');
    
    // se lo trova, il nome del file inizia un carattere dopo (lpSlash + 1)
    // se non lo trova, meglio
    return(std::wstring(lpSlash ? lpSlash + 1 : lpPath));
}

/*
	GetFileSize()

	Ricava la dimensione del file.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente.
	(le immagini caricate NON possono mai essere files > 4GB!)
*/
DWORD GetFileSize(const wchar_t* lpPath)
{
	WIN32_FILE_ATTRIBUTE_DATA fileInfo = {0};

	// chiamata nativa Win32 passando il puntatore raw
	if(!::GetFileAttributesExW(lpPath,GetFileExInfoStandard,&fileInfo))
		return((DWORD)-1L); 

	// sgancia la zavorra: le immagini stanno tutte dentro nFileSizeLow
	return(fileInfo.nFileSizeLow);
}

/*
	EnumFiles()

	Ricerca lo skeleton di files specificato.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente per usare 
	direttamente le API Win32 ed ottimizzare il passaggio dei parametri e la gestione delle stringhe.
*/
void EnumFiles(const wchar_t* lpPath,const wchar_t* lpAppend,std::vector<std::wstring>& fileList)
{
	// prepara lo skeleton di ricerca
	wchar_t wzSearchMask[MAX_PATH+1] = {0};
	if(swprintf_s(wzSearchMask,MAX_PATH,L"%s\\%s",lpPath,lpAppend) < 0)
		return; // errore: il percorso supera MAX_PATH

	// usa l'API nativa di Windows invece della CRT dell'originale
	WIN32_FIND_DATAW fd = {0};
	HANDLE hFind = ::FindFirstFileW(wzSearchMask,&fd);
	if(hFind!=INVALID_HANDLE_VALUE)
	{
		wchar_t wzFullPath[MAX_PATH+1] = {0};

		do
		{
			// controlla che sia un file e non una directory (evita anche "." e "..")
			if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				// compone il percorso completo
				if(swprintf_s(wzFullPath,MAX_PATH,L"%s\\%s",lpPath,fd.cFileName) > 0)
					fileList.push_back(wzFullPath);
			}
		} while(::FindNextFileW(hFind,&fd));

		::FindClose(hFind);
	}
}

/*
	ExtractToken()

	Estrae il seguente token dalla stringa, modificandola.
	I token vanno separati da virgola.
	Implementata ex-novo.
*/
std::wstring ExtractToken(std::wstring& str)
{
	if(str.empty())
		return(L"");
    
	// elimina gli spazi iniziali
	size_t start = str.find_first_not_of(L" \t");
	if(start==std::wstring::npos)
	{
		str.clear();
		return(L"");
	}
    
	size_t end = str.find(L',',start);
	std::wstring token;
    
	if(end==std::wstring::npos)
	{
		// ultimo token
		token = str.substr(start);
		str.clear();
	}
	else
	{
		token = str.substr(start,end - start);
		str = str.substr(end + 1);
	}
    
	// rimuove gli spazi finali
	while(!token.empty() && iswspace(token.back()))
		token.pop_back();
    
	return(token);
}

/*
	GetIniString()

	Ricava dal file di configurazione .ini il valore per il campo specificato.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente per correggerne il disegno.
	L'originale si chiedeva ogni volta se il file .ini esisteva (cosa gia' gestita da GetPrivateProfileStringW) e, cosa piu' grave
	in termini di logica e prestazioni, allocava ogni volta un buffer delle dimensioni del file .ini per contenere il valore del
	campo (calcolando oltretutto la dimensione come se fosse per un file > 4BG!), quando il valore massimo che puo' essere presente
	nel .ini e' un percorso di file, che per definizione non puo' oltrepassare i MAX_PATH.
*/
std::wstring GetIniString(const wchar_t* lpFilePath,const wchar_t* lpAppName,const wchar_t* lpKeyName)
{
	wchar_t wzBuffer[512] = {0};

	DWORD dwCopied = ::GetPrivateProfileStringW(lpAppName, 
												lpKeyName, 
												L"",
												wzBuffer, 
												MAX_PATH, 
												lpFilePath);

	// costruisce e restituisce la std::wstring usando la lunghezza gia' nota,
	// il che evita al compilatore (C++) di dover ricalcolare la lunghezza ed
	// evitando copie inutili al ritorno
	return(std::wstring(wzBuffer,dwCopied));
}

/*
	GetWindowClassName()

	Ricava il nome della classe per la finestra.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente per ovviare al grave memory leak
	che produceva:

		wchar_t* pText = new wchar_t[MAX_PATH];
		::GetClassNameW(hWnd, pText, MAX_PATH);
		return std::wstring(pText);

	Basicamente nell'originale la DLL chiedeva al Memory Manager di Windows di allocare 520 byte (260 caratteri × 2 byte)
	nell'heap del processo dell'Explorer, potendo arrivare a far accumulare centinaia di megabyte di RAM e rallentando
	l'intero sistema operativo fino a causare un crash o il congelamento del desktop, dato che la memoria allocata (pText)
	non veniva mai rilasciata. Lo stesso problema esisteva nella GetWindowTitle() originale, che e' stata comunque elminata.
*/
std::wstring GetWindowClassName(HWND hWnd)
{
    wchar_t szClassName[MAX_PATH+1] = {0};
    
    // GetClassNameW() restituisce il numero di caratteri effettivamente copiati
    int nLen = ::GetClassNameW(hWnd,szClassName,MAX_PATH);
    
    // costruisce la std::wstring passando la lunghezza gia' nota
    // questo evita al C++ di dover calcolare la lunghezza con wcslen() internamente
    return std::wstring(szClassName, nLen);
}

/*
	PathToCLSID()

	Verifica se il path e' in realta' (corrisponde a) un CLSID.
	Per una lista di CLSID:
	registro: HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\FolderDescriptions
	web: https://winaero.com/clsid-guid-shell-list-windows-10/

	Restituisce il CLSID se trovato o lo stesso path ricevuto in input altrimenti.
	Implementata ex-novo.
*/
std::wstring PathToCLSID(std::wstring path)
{
	struct KnownFolderEntry {
		KNOWNFOLDERID kfid;
		const wchar_t* clsid;
	};

	// array per la associazioni
	static const KnownFolderEntry MyFolders[] = {
		{FOLDERID_Documents, L"::{450D8FBA-AD25-11D0-98A8-0800361B1103}"},
		{FOLDERID_Desktop,   L"::{75048700-EF1F-11D0-9888-006097DEACF9}"},
		{FOLDERID_Downloads, L"::{374DE290-123F-4565-9164-39C4925E467B}"},
		{FOLDERID_Pictures,  L"::{33E28130-4E1E-4676-835A-98395C3BC3BB}"}
	};

	// verifica che NON sia gia' un CLSID
	if(path.size() >= 3 && path[0]==L':' && path[1]==L':' && path[2]==L'{')
		return(path);

	// normalizza il path
	wchar_t szCanonical[MAX_PATH+1] = {0};
	if(!::PathCanonicalizeW(szCanonical,path.c_str()))
		return(path);

	// cerca l'eventuale corrispondenza
	for(const auto& entry : MyFolders)
	{
		wchar_t* pszPath = NULL;
		if(SUCCEEDED(::SHGetKnownFolderPath(entry.kfid,0,NULL,&pszPath)))
		{
			if(_wcsicmp(szCanonical,pszPath)==0)
			{
				::CoTaskMemFree(pszPath);
				return(entry.clsid);
			}

			::CoTaskMemFree(pszPath);
		}
	}

	return(path);
}

/*
	BitmapGDI()

	Classe per la gestione (caricamento) dell'immagine via GDI+.
	Presente con lo stesso nome nel progetto originale, e' stata riscritta completamente per ovviare ai
	problemi di caricamento, blocco del file, allocazione di memoria, etc., come:
	- la SHCreateMemStream che duplica il blocco di memoria allocata anteriormente
	- il controllo ineffettivo di "if(src)"
	- il leak GDI generato perche' non viene salvato ne' riselezionato l'handle del bitmap precedente al
	  selezionare un bitmap in un DC hOldBmp
	- la duplicazione inutile di memoria dato che la classe mantiene in memoria sia Gdiplus::Bitmap* src 
	  (GDI+) sia HBITMAP pBmp dentro un HDC pMem (GDI nativo)
	- il mismatch delle chiamate nel distruttore
*/
// originale buggata:
/*
BitmapGDI::BitmapGDI(std::wstring path)
{
	FILE* file = nullptr;
	_wfopen_s(&file, path.c_str(), L"rb");
	if (file) {
		fseek(file, 0L, SEEK_END);
		long len = ftell(file);
		rewind(file);
		BYTE* pdata = new BYTE[len];
		fread(pdata, 1, len, file);
		fclose(file);

		IStream* stream = SHCreateMemStream(pdata, len);
		delete[] pdata;

		src = Gdiplus::Bitmap::FromStream(stream);
		if (src) {
			pMem = CreateCompatibleDC(0);
			Size = { (LONG)src->GetWidth(), (LONG)src->GetHeight() };
			src->GetHBITMAP(0, &pBmp);
			SelectObject(pMem, pBmp);

			stream->Release();
		}
		else if (stream)
			stream->Release();
	}
}*/
/*
BitmapGDI::~BitmapGDI()
{
delete src;
	if (pMem)
		DeleteDC(pMem);
	if (pBmp)
		DeleteObject(pBmp);
}
*/
// nuova:
BitmapGDI::BitmapGDI(const wchar_t* lpPath)
{
	pMem = NULL;
	pBmp = NULL;
	Size = { 0, 0 };
	src = NULL;
	hOldBmp = NULL;

	size_t len = wcslen(lpPath);

	// se mai si volessero gestire le icone...
	if(len > 4 && _wcsicmp(&lpPath[len-4], L".ico")==0)
	{
		HICON hIcon = (HICON)::LoadImageW(NULL,lpPath,IMAGE_ICON,256,256,LR_LOADFROMFILE);
		if(hIcon)
		{
			src = Gdiplus::Bitmap::FromHICON(hIcon);
			::DestroyIcon(hIcon);
		}
	}
	else
	{
		// gestione files, evita il lock del file con il trucco del clone
		Gdiplus::Bitmap* pTmpBmp = Gdiplus::Bitmap::FromFile(lpPath);
		Gdiplus::Status status = pTmpBmp->GetLastStatus();
		if(pTmpBmp && status==Gdiplus::Ok)
		{
			// clona l'immagine in un record puramente residente in RAM, eliminando cosi' il legame con il file fisico sul disco
			// vedi le note in MyFillRect() in dllmain.cpp riguardo al flag PixelFormat32bppPARGB/PixelFormat32bppARGB
			src = pTmpBmp->Clone(0,0,pTmpBmp->GetWidth(),pTmpBmp->GetHeight(),PixelFormat32bppPARGB);
		}
		else
		{
			// per i "nuovi" formati come WEBP, dato che GDI non li supporta direttamente, andrebbe investigata l'interfaccia WIC...
			src = nullptr;
			TRACEEXPR((_TRACE_FLAG_ERR,_L(__FILE__),__LINE__,L"BitmapGDI(): ERROR: FromFile() failed for %s (%d:%s)\n",lpPath,(int)status,GetGDIPlusStatusName(status)));
		}

		// distruggendo pTmpBmp, il file sul disco viene sbloccato immediatamente
		// nessun bisogno di FILE*, fseek o IStream
		delete pTmpBmp; 
	}

	// crea il contesto grafico solo se l'immagine e' valida
	if(src && src->GetLastStatus()==Gdiplus::Ok)
	{
		pMem = ::CreateCompatibleDC(NULL);
		Size.cx = (LONG)src->GetWidth();
		Size.cy = (LONG)src->GetHeight();
        
		// estrae l'HBITMAP nativo da usare con BitBlt
		src->GetHBITMAP(0,&pBmp);
        
		if(pMem && pBmp)
		{
			// SelectObject() restituisce la vecchia bitmap predefinita 1x1, la salva
			hOldBmp = (HBITMAP)::SelectObject(pMem,pBmp);
		}
	}
}

BitmapGDI::~BitmapGDI()
{
	// rilascia la memoria di Gdiplus se e' ancora attiva
	if(src)
	{
		delete src;
		src = nullptr;
	}

	// ripristina lo stato del DC prima di eliminarlo
	if(pMem)
	{
		if(hOldBmp)
		{
			// rimettiamo la bitmap predefinita 1x1 nel DC
			// in tal modo 'pBmp' viene deselezionata formalmente
			::SelectObject(pMem,hOldBmp);
		}

		::DeleteDC(pMem);
		pMem = NULL;
	}

	// ora 'pBmp' e' totalmente libera da ogni vincolo e puo' essere distrutta in sicurezza
	if(pBmp)
	{
		::DeleteObject(pBmp);
		pBmp = NULL;
	}
}

/*
	GetGDIPlusStatusName()
*/
const wchar_t* BitmapGDI::GetGDIPlusStatusName(Gdiplus::Status status)
{
	switch(status)
	{
		case Gdiplus::Ok:                        return L"Ok";
		case Gdiplus::GenericError:              return L"GenericError";
		case Gdiplus::InvalidParameter:          return L"InvalidParameter";
		case Gdiplus::OutOfMemory:               return L"OutOfMemory";
		case Gdiplus::ObjectBusy:                return L"ObjectBusy";
		case Gdiplus::InsufficientBuffer:        return L"InsufficientBuffer";
		case Gdiplus::NotImplemented:            return L"NotImplemented";
		case Gdiplus::Win32Error:                return L"Win32Error";
		case Gdiplus::WrongState:                return L"WrongState";
		case Gdiplus::Aborted:                   return L"Aborted";
		case Gdiplus::FileNotFound:              return L"FileNotFound";
		case Gdiplus::ValueOverflow:             return L"ValueOverflow";
		case Gdiplus::AccessDenied:              return L"AccessDenied";
		case Gdiplus::UnknownImageFormat:        return L"UnknownImageFormat";
		case Gdiplus::FontFamilyNotFound:        return L"FontFamilyNotFound";
		case Gdiplus::FontStyleNotFound:         return L"FontStyleNotFound";
		case Gdiplus::NotTrueTypeFont:           return L"NotTrueTypeFont";
		case Gdiplus::UnsupportedGdiplusVersion: return L"UnsupportedGdiplusVersion";
		case Gdiplus::GdiplusNotInitialized:     return L"GdiplusNotInitialized";
		case Gdiplus::PropertyNotFound:          return L"PropertyNotFound";
		case Gdiplus::PropertyNotSupported:      return L"PropertyNotSupported";
		//case Gdiplus::ProfileNotFound:           return L"ProfileNotFound";
		default:                                 return L"Unknown";
	}
}
