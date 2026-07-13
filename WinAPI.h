/*
	WinAPI.h

	ExplorerBgToolRe(dux)
	Il progetto e' una refattorizzazione/riscrittura del progetto originale ExploreBgTool:

		copyright originale:
		WinAPI声明
		Author: Maple
		date: 2021-7-13 Create
		Copyright winmoes.com
		(reperibile su: https://github.com/Maplespe/explorerTool)

	Viene cambiato il disegno originale e vengono corretti vari bugs, sopratutto relativi a problemi di concorrenza e di
	memoria (leaks e incongruenze varie), cercando di ottimizzare per quanto sia possibile.

	Vedi le note in dllmain.cpp

	Luca Piergentili, 18/06/2026
*/
#ifndef _WINAPI_H
#define _WINAPI_H 1

#include <windows.h>
#include <comdef.h>
#include <gdiplus.h>
#include <string>
#include <vector>

bool			FileExists			(std::wstring FilePath);
bool			DoesDirectoryExist	(std::wstring FilePath,LPDWORD pdwLastError = NULL);
std::wstring	GetFileName			(const wchar_t* lpPath);
DWORD			GetFileSize			(const wchar_t* lpPath);
void			EnumFiles			(const wchar_t* lpPath,const wchar_t* lpAppend,std::vector<std::wstring>& fileList);
std::wstring	ExtractToken		(std::wstring& str);
std::wstring	GetIniString		(const wchar_t* lpFilePath,const wchar_t* lpAppName,const wchar_t* lpKeyName);
std::wstring	GetWindowClassName	(HWND hWnd);
std::wstring	PathToCLSID			(std::wstring path);

// cambiato il parametro del costruttore a wchar_t* ed aggiunto un HBITMAP per tracciare lo stato originale
class BitmapGDI
{
public:
    BitmapGDI(const wchar_t* lpPath);
    ~BitmapGDI();
	const wchar_t* GetGDIPlusStatusName(Gdiplus::Status status);

    HDC pMem = 0;
    HBITMAP pBmp = 0;
    HBITMAP hOldBmp = 0;
    SIZE Size{};
    Gdiplus::Bitmap* src = 0;
};

#endif // _WINAPI_H
