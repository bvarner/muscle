/* This file is Copyright 2000-2013 Meyer Sound Laboratories Inc.  See the included LICENSE.txt file for details. */

#include <stdio.h>
#include <stdarg.h>
#include "dataio/FileDataIO.h"
#include "regex/StringMatcher.h"
#include "syslog/LogCallback.h"
#include "system/SetupSystem.h"
#include "system/SystemInfo.h"  // for GetFilePathSeparator()
#include "util/Directory.h"
#include "util/FilePathInfo.h"
#include "util/Hashtable.h"
#include "util/NestCount.h"
#include "util/String.h"
#include "util/StringTokenizer.h"

#ifdef MUSCLE_USE_MSVC_STACKWALKER
# include <dbghelp.h>
# include <tchar.h>
#endif

#if !defined(MUSCLE_INLINE_LOGGING) && defined(MUSCLE_ENABLE_ZLIB_ENCODING)
# include "zlib/zlib/zlib.h"
#endif

#if defined(__APPLE__)
# include "AvailabilityMacros.h"  // so we can find out if this version of MacOS/X is new enough to include backtrace() and friends
#endif

#if (defined(__linux__) && !defined(ANDROID)) || (defined(MAC_OS_X_VERSION_10_5) && defined(MAC_OS_X_VERSION_MAX_ALLOWED) && (MAC_OS_X_VERSION_MAX_ALLOWED >= MAC_OS_X_VERSION_10_5))
# include <execinfo.h>
# define MUSCLE_USE_BACKTRACE 1
#endif

// Work-around for Android not providing timegm()... we'll just include the implementation inline, right here!  --jaf, jfm
#if defined(ANDROID)
/*
 * Copyright (c) 1997 Kungliga Tekniska Hˆgskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */
# include <sys/types.h>
# include <time.h>
static bool is_leap(unsigned y) {y += 1900; return (y % 4) == 0 && ((y % 100) != 0 || (y % 400) == 0);}
static time_t timegm (struct tm *tm)
{
   static const unsigned ndays[2][12] ={{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}, {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
   time_t res = 0;
   for (int i = 70; i < tm->tm_year; ++i) res += is_leap(i) ? 366 : 365;
   for (int i = 0;  i < tm->tm_mon;  ++i) res += ndays[is_leap(tm->tm_year)][i];
   res += tm->tm_mday - 1; res *= 24;
   res += tm->tm_hour;     res *= 60;
   res += tm->tm_min;      res *= 60;
   res += tm->tm_sec;
   return res;
}
#endif

namespace muscle {

#ifdef THIS_FUNCTION_IS_NOT_ACTUALLY_USED_I_JUST_KEEP_IT_HERE_SO_I_CAN_QUICKLY_COPY_AND_PASTE_IT_INTO_THIRD_PARTY_CODE_WHEN_NECESSARY_SAYS_JAF
# include <execinfo.h>
void PrintStackTrace()
{
   FILE * optFile = stdout;
   void *array[256];
   size_t size = backtrace(array, 256);
   char ** strings = backtrace_symbols(array, 256);
   if (strings)
   {
      fprintf(optFile, "--Stack trace follows (%zd frames):\n", size);
      for (size_t i = 0; i < size; i++) fprintf(optFile, "  %s\n", strings[i]);
      fprintf(optFile, "--End Stack trace\n");
      free(strings);
   }
   else fprintf(optFile, "PrintStackTrace:  Error, could not generate stack trace!\n");
}
#endif

// Begin windows stack trace code.  This code is simplified, so it Only works for _MSC_VER >= 1300
#if defined(MUSCLE_USE_MSVC_STACKWALKER) && !defined(MUSCLE_INLINE_LOGGING)

/**********************************************************************
 * 
 * Liberated from:
 * 
 *   http://www.codeproject.com/KB/threads/StackWalker.aspx
 *
 **********************************************************************/

class StackWalkerInternal;  // forward
class StackWalker
{
public:
   typedef enum StackWalkOptions
   {
     // No addition info will be retrived 
     // (only the address is available)
     RetrieveNone = 0,
     
     // Try to get the symbol-name
     RetrieveSymbol = 1,
     
     // Try to get the line for this symbol
     RetrieveLine = 2,
     
     // Try to retrieve the module-infos
     RetrieveModuleInfo = 4,
     
     // Also retrieve the version for the DLL/EXE
     RetrieveFileVersion = 8,
     
     // Contains all the abouve
     RetrieveVerbose = 0xF,
     
     // Generate a "good" symbol-search-path
     SymBuildPath = 0x10,
     
     // Also use the public Microsoft-Symbol-Server
     SymUseSymSrv = 0x20,
     
     // Contains all the above "Sym"-options
     SymAll = 0x30,
     
     // Contains all options (default)
     OptionsAll = 0x3F,

     OptionsJAF = (RetrieveSymbol|RetrieveLine)

   } StackWalkOptions;

   StackWalker(
     FILE * optOutFile,
     String * optOutString,
     int options = OptionsAll, // 'int' is by design, to combine the enum-flags
     LPCSTR szSymPath = NULL, 
     DWORD dwProcessId = GetCurrentProcessId(), 
     HANDLE hProcess = GetCurrentProcess()
     );
   ~StackWalker();

   typedef BOOL (__stdcall *PReadProcessMemoryRoutine)(
     HANDLE      hProcess,
     DWORD64     qwBaseAddress,
     PVOID       lpBuffer,
     DWORD       nSize,
     LPDWORD     lpNumberOfBytesRead,
     LPVOID      pUserData  // optional data, which was passed in "ShowCallstack"
     );

   BOOL LoadModules();

   BOOL ShowCallstack(
     uint32 maxDepth,
     HANDLE hThread = GetCurrentThread(), 
     const CONTEXT *context = NULL, 
     PReadProcessMemoryRoutine readMemoryFunction = NULL,
     LPVOID pUserData = NULL  // optional to identify some data in the 'readMemoryFunction'-callback
     );

protected:
    enum {STACKWALK_MAX_NAMELEN = 1024}; // max name length for found symbols

protected:
   // Entry for each Callstack-Entry
   typedef struct CallstackEntry
   {
     DWORD64 offset;  // if 0, we have no valid entry
     CHAR name[STACKWALK_MAX_NAMELEN];
     CHAR undName[STACKWALK_MAX_NAMELEN];
     CHAR undFullName[STACKWALK_MAX_NAMELEN];
     DWORD64 offsetFromSmybol;
     DWORD offsetFromLine;
     DWORD lineNumber;
     CHAR lineFileName[STACKWALK_MAX_NAMELEN];
     DWORD symType;
     LPCSTR symTypeString;
     CHAR moduleName[STACKWALK_MAX_NAMELEN];
     DWORD64 baseOfImage;
     CHAR loadedImageName[STACKWALK_MAX_NAMELEN];
   } CallstackEntry;

   typedef enum CallstackEntryType {firstEntry, nextEntry, lastEntry};

   void OnSymInit(LPCSTR szSearchPath, DWORD symOptions, LPCSTR szUserName);
   void OnLoadModule(LPCSTR img, LPCSTR mod, DWORD64 baseAddr, DWORD size, DWORD result, LPCSTR symType, LPCSTR pdbName, ULONGLONG fileVersion);
   void OnCallstackEntry(CallstackEntryType eType, CallstackEntry &entry);
   void OnDbgHelpErr(LPCSTR szFuncName, DWORD gle, DWORD64 addr);
   void OnOutput(LPCSTR szText)
   {
      if (this->m_outFile) fputs("  ", m_outFile);
                      else puts("  ");
      if (this->m_outFile) fputs(szText, m_outFile);
                      else puts(szText);
      if (this->m_outString) (*this->m_outString) += szText;
   }


   StackWalkerInternal *m_sw;
   HANDLE m_hProcess;
   DWORD m_dwProcessId;
   BOOL m_modulesLoaded;
   LPSTR m_szSymPath;

   FILE * m_outFile;      // added by jaf (because subclassing is overkill for my needs here)
   String * m_outString;  // ditto

   int m_options;

   static BOOL __stdcall ReadProcMemCallback(HANDLE hProcess, DWORD64 qwBaseAddress, PVOID lpBuffer, DWORD nSize, LPDWORD lpNumberOfBytesRead);

   friend StackWalkerInternal;
};

void _Win32PrintStackTraceForContext(FILE * optFile, CONTEXT * context, uint32 maxDepth)
{
   fprintf(optFile, "--Stack trace follows:\n");
   StackWalker(optFile, NULL, StackWalker::OptionsJAF).ShowCallstack(maxDepth, GetCurrentThread(), context);
   fprintf(optFile, "--End Stack trace\n");
}


// The following is defined for x86 (XP and higher), x64 and IA64:
#define GET_CURRENT_CONTEXT(c, contextFlags) \
  do { \
    memset(&c, 0, sizeof(CONTEXT)); \
    c.ContextFlags = contextFlags; \
    RtlCaptureContext(&c); \
} while(0);

// Some missing defines (for VC5/6):
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif  

// secure-CRT_functions are only available starting with VC8
#if _MSC_VER < 1400
#define strcpy_s strcpy
#define strcat_s(dst, len, src) strcat(dst, src)
#define _snprintf_s _snprintf
#define _tcscat_s _tcscat
#endif

// Normally it should be enough to use 'CONTEXT_FULL' (better would be 'CONTEXT_ALL')
#define USED_CONTEXT_FLAGS CONTEXT_FULL

class StackWalkerInternal
{
public:
  StackWalkerInternal(StackWalker *parent, HANDLE hProcess)
  {
    m_parent = parent;
    m_hDbhHelp = NULL;
    pSC = NULL;
    m_hProcess = hProcess;
    m_szSymPath = NULL;
    pSFTA = NULL;
    pSGLFA = NULL;
    pSGMB = NULL;
    pSGMI = NULL;
    pSGO = NULL;
    pSGSFA = NULL;
    pSI = NULL;
    pSLM = NULL;
    pSSO = NULL;
    pSW = NULL;
    pUDSN = NULL;
    pSGSP = NULL;
  }
  ~StackWalkerInternal()
  {
    if (pSC != NULL)
      pSC(m_hProcess);  // SymCleanup
    if (m_hDbhHelp != NULL)
      FreeLibrary(m_hDbhHelp);
    m_hDbhHelp = NULL;
    m_parent = NULL;
    if(m_szSymPath != NULL)
      free(m_szSymPath);
    m_szSymPath = NULL;
  }
  BOOL Init(LPCSTR szSymPath)
  {
    if (m_parent == NULL)
      return FALSE;
    // Dynamically load the Entry-Points for dbghelp.dll:
    // First try to load the newsest one from
    CHAR szTemp[4096];
    // But before wqe do this, we first check if the ".local" file exists
    if (GetModuleFileNameA(NULL, szTemp, 4096) > 0)
    {
      strncat(szTemp, ".local", sizeof(szTemp));
      if (GetFileAttributesA(szTemp) == INVALID_FILE_ATTRIBUTES)
      {
        // ".local" file does not exist, so we can try to load the dbghelp.dll from the "Debugging Tools for Windows"
        if (GetEnvironmentVariableA("ProgramFiles", szTemp, 4096) > 0)
        {
          strncat(szTemp, "\\Debugging Tools for Windows\\dbghelp.dll", sizeof(szTemp));
          // now check if the file exists:
          if (GetFileAttributesA(szTemp) != INVALID_FILE_ATTRIBUTES)
          {
            m_hDbhHelp = LoadLibraryA(szTemp);
          }
        }
          // Still not found? Then try to load the 64-Bit version:
        if ( (m_hDbhHelp == NULL) && (GetEnvironmentVariableA("ProgramFiles", szTemp, 4096) > 0) )
        {
          strncat(szTemp, "\\Debugging Tools for Windows 64-Bit\\dbghelp.dll", sizeof(szTemp));
          if (GetFileAttributesA(szTemp) != INVALID_FILE_ATTRIBUTES)
          {
            m_hDbhHelp = LoadLibraryA(szTemp);
          }
        }
      }
    }
    if (m_hDbhHelp == NULL)  // if not already loaded, try to load a default-one
      m_hDbhHelp = LoadLibraryA("dbghelp.dll");
    if (m_hDbhHelp == NULL)
      return FALSE;
    pSI = (tSI) GetProcAddress(m_hDbhHelp, "SymInitialize" );
    pSC = (tSC) GetProcAddress(m_hDbhHelp, "SymCleanup" );

    pSW = (tSW) GetProcAddress(m_hDbhHelp, "StackWalk64" );
    pSGO = (tSGO) GetProcAddress(m_hDbhHelp, "SymGetOptions" );
    pSSO = (tSSO) GetProcAddress(m_hDbhHelp, "SymSetOptions" );

    pSFTA = (tSFTA) GetProcAddress(m_hDbhHelp, "SymFunctionTableAccess64" );
    pSGLFA = (tSGLFA) GetProcAddress(m_hDbhHelp, "SymGetLineFromAddr64" );
    pSGMB = (tSGMB) GetProcAddress(m_hDbhHelp, "SymGetModuleBase64" );
    pSGMI = (tSGMI) GetProcAddress(m_hDbhHelp, "SymGetModuleInfo64" );
    //pSGMI_V3 = (tSGMI_V3) GetProcAddress(m_hDbhHelp, "SymGetModuleInfo64" );
    pSGSFA = (tSGSFA) GetProcAddress(m_hDbhHelp, "SymGetSymFromAddr64" );
    pUDSN = (tUDSN) GetProcAddress(m_hDbhHelp, "UnDecorateSymbolName" );
    pSLM = (tSLM) GetProcAddress(m_hDbhHelp, "SymLoadModule64" );
    pSGSP =(tSGSP) GetProcAddress(m_hDbhHelp, "SymGetSearchPath" );

    if ( pSC == NULL || pSFTA == NULL || pSGMB == NULL || pSGMI == NULL ||
      pSGO == NULL || pSGSFA == NULL || pSI == NULL || pSSO == NULL ||
      pSW == NULL || pUDSN == NULL || pSLM == NULL )
    {
      FreeLibrary(m_hDbhHelp);
      m_hDbhHelp = NULL;
      pSC = NULL;
      return FALSE;
    }

    // SymInitialize
    if (szSymPath != NULL)
      m_szSymPath = _strdup(szSymPath);
    if (this->pSI(m_hProcess, m_szSymPath, FALSE) == FALSE)
      this->m_parent->OnDbgHelpErr("SymInitialize", GetLastError(), 0);
      
    DWORD symOptions = this->pSGO();  // SymGetOptions
    symOptions |= SYMOPT_LOAD_LINES;
    symOptions |= SYMOPT_FAIL_CRITICAL_ERRORS;
    //symOptions |= SYMOPT_NO_PROMPTS;
    // SymSetOptions
    symOptions = this->pSSO(symOptions);

    char buf[StackWalker::STACKWALK_MAX_NAMELEN] = {0};
    if (this->pSGSP != NULL)
    {
      if (this->pSGSP(m_hProcess, buf, StackWalker::STACKWALK_MAX_NAMELEN) == FALSE)
        this->m_parent->OnDbgHelpErr("SymGetSearchPath", GetLastError(), 0);
    }
    char szUserName[1024] = {0};
    DWORD dwSize = 1024;
    GetUserNameA(szUserName, &dwSize);
    this->m_parent->OnSymInit(buf, symOptions, szUserName);

    return TRUE;
  }

  StackWalker *m_parent;

  HMODULE m_hDbhHelp;
  HANDLE m_hProcess;
  LPSTR m_szSymPath;

typedef struct IMAGEHLP_MODULE64_V2 {
    DWORD    SizeOfStruct;           // set to sizeof(IMAGEHLP_MODULE64)
    DWORD64  BaseOfImage;            // base load address of module
    DWORD    ImageSize;              // virtual size of the loaded module
    DWORD    TimeDateStamp;          // date/time stamp from pe header
    DWORD    CheckSum;               // checksum from the pe header
    DWORD    NumSyms;                // number of symbols in the symbol table
    SYM_TYPE SymType;                // type of symbols loaded
    CHAR     ModuleName[32];         // module name
    CHAR     ImageName[256];         // image name
    CHAR     LoadedImageName[256];   // symbol file name
};

  // SymCleanup()
  typedef BOOL (__stdcall *tSC)( IN HANDLE hProcess );
  tSC pSC;

  // SymFunctionTableAccess64()
  typedef PVOID (__stdcall *tSFTA)( HANDLE hProcess, DWORD64 AddrBase );
  tSFTA pSFTA;

  // SymGetLineFromAddr64()
  typedef BOOL (__stdcall *tSGLFA)( IN HANDLE hProcess, IN DWORD64 dwAddr, OUT PDWORD pdwDisplacement, OUT PIMAGEHLP_LINE64 Line );
  tSGLFA pSGLFA;

  // SymGetModuleBase64()
  typedef DWORD64 (__stdcall *tSGMB)( IN HANDLE hProcess, IN DWORD64 dwAddr );
  tSGMB pSGMB;

  // SymGetModuleInfo64()
  typedef BOOL (__stdcall *tSGMI)( IN HANDLE hProcess, IN DWORD64 dwAddr, OUT IMAGEHLP_MODULE64_V2 *ModuleInfo );
  tSGMI pSGMI;

  // SymGetOptions()
  typedef DWORD (__stdcall *tSGO)( VOID );
  tSGO pSGO;

  // SymGetSymFromAddr64()
  typedef BOOL (__stdcall *tSGSFA)( IN HANDLE hProcess, IN DWORD64 dwAddr, OUT PDWORD64 pdwDisplacement, OUT PIMAGEHLP_SYMBOL64 Symbol );
  tSGSFA pSGSFA;

  // SymInitialize()
  typedef BOOL (__stdcall *tSI)( IN HANDLE hProcess, IN PSTR UserSearchPath, IN BOOL fInvadeProcess );
  tSI pSI;

  // SymLoadModule64()
  typedef DWORD64 (__stdcall *tSLM)( IN HANDLE hProcess, IN HANDLE hFile, IN PSTR ImageName, IN PSTR ModuleName, IN DWORD64 BaseOfDll, IN DWORD SizeOfDll );
  tSLM pSLM;

  // SymSetOptions()
  typedef DWORD (__stdcall *tSSO)( IN DWORD SymOptions );
  tSSO pSSO;

  // StackWalk64()
  typedef BOOL (__stdcall *tSW)( 
    DWORD MachineType, 
    HANDLE hProcess,
    HANDLE hThread, 
    LPSTACKFRAME64 StackFrame, 
    PVOID ContextRecord,
    PREAD_PROCESS_MEMORY_ROUTINE64 ReadMemoryRoutine,
    PFUNCTION_TABLE_ACCESS_ROUTINE64 FunctionTableAccessRoutine,
    PGET_MODULE_BASE_ROUTINE64 GetModuleBaseRoutine,
    PTRANSLATE_ADDRESS_ROUTINE64 TranslateAddress );
  tSW pSW;

  // UnDecorateSymbolName()
  typedef DWORD (__stdcall WINAPI *tUDSN)( PCSTR DecoratedName, PSTR UnDecoratedName, DWORD UndecoratedLength, DWORD Flags );
  tUDSN pUDSN;

  typedef BOOL (__stdcall WINAPI *tSGSP)(HANDLE hProcess, PSTR SearchPath, DWORD SearchPathLength);
  tSGSP pSGSP;

private:
  // **************************************** ToolHelp32 ************************
  #define MAX_MODULE_NAME32 255
  #define TH32CS_SNAPMODULE   0x00000008
  #pragma pack( push, 8 )
  typedef struct tagMODULEENTRY32
  {
      DWORD   dwSize;
      DWORD   th32ModuleID;       // This module
      DWORD   th32ProcessID;      // owning process
      DWORD   GlblcntUsage;       // Global usage count on the module
      DWORD   ProccntUsage;       // Module usage count in th32ProcessID's context
      BYTE  * modBaseAddr;        // Base address of module in th32ProcessID's context
      DWORD   modBaseSize;        // Size in bytes of module starting at modBaseAddr
      HMODULE hModule;            // The hModule of this module in th32ProcessID's context
      char    szModule[MAX_MODULE_NAME32 + 1];
      char    szExePath[MAX_PATH];
  } MODULEENTRY32;
  typedef MODULEENTRY32 *  PMODULEENTRY32;
  typedef MODULEENTRY32 *  LPMODULEENTRY32;
  #pragma pack( pop )

  BOOL GetModuleListTH32(HANDLE hProcess, DWORD pid)
  {
    // CreateToolhelp32Snapshot()
    typedef HANDLE (__stdcall *tCT32S)(DWORD dwFlags, DWORD th32ProcessID);
    // Module32First()
    typedef BOOL (__stdcall *tM32F)(HANDLE hSnapshot, LPMODULEENTRY32 lpme);
    // Module32Next()
    typedef BOOL (__stdcall *tM32N)(HANDLE hSnapshot, LPMODULEENTRY32 lpme);

    // try both dlls...
    const CHAR *dllname[] = { "kernel32.dll", "tlhelp32.dll" };
    HINSTANCE hToolhelp = NULL;
    tCT32S pCT32S = NULL;
    tM32F pM32F = NULL;
    tM32N pM32N = NULL;

    HANDLE hSnap;
    MODULEENTRY32 me;
    me.dwSize = sizeof(me);
    BOOL keepGoing;
    size_t i;

    for (i = 0; i<(sizeof(dllname) / sizeof(dllname[0])); i++ )
    {
      hToolhelp = LoadLibraryA( dllname[i] );
      if (hToolhelp == NULL)
        continue;
      pCT32S = (tCT32S) GetProcAddress(hToolhelp, "CreateToolhelp32Snapshot");
      pM32F = (tM32F) GetProcAddress(hToolhelp, "Module32First");
      pM32N = (tM32N) GetProcAddress(hToolhelp, "Module32Next");
      if ( (pCT32S != NULL) && (pM32F != NULL) && (pM32N != NULL) )
        break; // found the functions!
      FreeLibrary(hToolhelp);
      hToolhelp = NULL;
    }

    if (hToolhelp == NULL)
      return FALSE;

    hSnap = pCT32S( TH32CS_SNAPMODULE, pid );
    if (hSnap == (HANDLE) -1)
      return FALSE;

    keepGoing = !!pM32F( hSnap, &me );
    int cnt = 0;
    while (keepGoing)
    {
      this->LoadModule(hProcess, me.szExePath, me.szModule, (DWORD64) me.modBaseAddr, me.modBaseSize);
      cnt++;
      keepGoing = !!pM32N( hSnap, &me );
    }
    CloseHandle(hSnap);
    FreeLibrary(hToolhelp);
    return (cnt <= 0) ? FALSE : TRUE;
  }  // GetModuleListTH32

  // **************************************** PSAPI ************************
  typedef struct _MODULEINFO {
      LPVOID lpBaseOfDll;
      DWORD SizeOfImage;
      LPVOID EntryPoint;
  } MODULEINFO, *LPMODULEINFO;

  BOOL GetModuleListPSAPI(HANDLE hProcess)
  {
    // EnumProcessModules()
    typedef BOOL (__stdcall *tEPM)(HANDLE hProcess, HMODULE *lphModule, DWORD cb, LPDWORD lpcbNeeded );
    // GetModuleFileNameEx()
    typedef DWORD (__stdcall *tGMFNE)(HANDLE hProcess, HMODULE hModule, LPSTR lpFilename, DWORD nSize );
    // GetModuleBaseName()
    typedef DWORD (__stdcall *tGMBN)(HANDLE hProcess, HMODULE hModule, LPSTR lpFilename, DWORD nSize );
    // GetModuleInformation()
    typedef BOOL (__stdcall *tGMI)(HANDLE hProcess, HMODULE hModule, LPMODULEINFO pmi, DWORD nSize );

    HINSTANCE hPsapi;
    tEPM pEPM;
    tGMFNE pGMFNE;
    tGMBN pGMBN;
    tGMI pGMI;

    DWORD i;
    //ModuleEntry e;
    DWORD cbNeeded;
    MODULEINFO mi;
    HMODULE *hMods = 0;
    char *tt = NULL;
    char *tt2 = NULL;
    const SIZE_T TTBUFLEN = 8096;
    int cnt = 0;

    hPsapi = LoadLibraryA("psapi.dll");
    if (hPsapi == NULL)
      return FALSE;

    pEPM = (tEPM) GetProcAddress( hPsapi, "EnumProcessModules" );
    pGMFNE = (tGMFNE) GetProcAddress( hPsapi, "GetModuleFileNameExA" );
    pGMBN = (tGMFNE) GetProcAddress( hPsapi, "GetModuleBaseNameA" );
    pGMI = (tGMI) GetProcAddress( hPsapi, "GetModuleInformation" );
    if ( (pEPM == NULL) || (pGMFNE == NULL) || (pGMBN == NULL) || (pGMI == NULL) )
    {
      // we couldn´t find all functions
      FreeLibrary(hPsapi);
      return FALSE;
    }

    hMods = (HMODULE*) malloc(sizeof(HMODULE) * (TTBUFLEN / sizeof HMODULE));
    tt = (char*) malloc(sizeof(char) * TTBUFLEN);
    tt2 = (char*) malloc(sizeof(char) * TTBUFLEN);
    if ( (hMods == NULL) || (tt == NULL) || (tt2 == NULL) )
      goto cleanup;

    if ( ! pEPM( hProcess, hMods, TTBUFLEN, &cbNeeded ) )
    {
      //_fprintf(fLogFile, "%lu: EPM failed, GetLastError = %lu\n", g_dwShowCount, gle );
      goto cleanup;
    }

    if ( cbNeeded > TTBUFLEN )
    {
      //_fprintf(fLogFile, "%lu: More than %lu module handles. Huh?\n", g_dwShowCount, lenof( hMods ) );
      goto cleanup;
    }

    for ( i = 0; i < cbNeeded / sizeof hMods[0]; i++ )
    {
      // base address, size
      pGMI(hProcess, hMods[i], &mi, sizeof mi );
      // image file name
      tt[0] = 0;
      pGMFNE(hProcess, hMods[i], tt, TTBUFLEN );

      // module name
      tt2[0] = 0;
      pGMBN(hProcess, hMods[i], tt2, TTBUFLEN );

      DWORD dwRes = this->LoadModule(hProcess, tt, tt2, (DWORD64) mi.lpBaseOfDll, mi.SizeOfImage);
      if (dwRes != ERROR_SUCCESS) this->m_parent->OnDbgHelpErr("LoadModule", dwRes, 0);
      cnt++;
    }

  cleanup:
    if (hPsapi != NULL) FreeLibrary(hPsapi);
    if (tt2 != NULL) free(tt2);
    if (tt != NULL) free(tt);
    if (hMods != NULL) free(hMods);

    return cnt != 0;
  }  // GetModuleListPSAPI

  DWORD LoadModule(HANDLE hProcess, LPCSTR img, LPCSTR mod, DWORD64 baseAddr, DWORD size)
  {
    CHAR *szImg = _strdup(img);
    CHAR *szMod = _strdup(mod);
    DWORD result = ERROR_SUCCESS;
    if ( (szImg == NULL) || (szMod == NULL) )
      result = ERROR_NOT_ENOUGH_MEMORY;
    else
    {
      if (pSLM(hProcess, 0, szImg, szMod, baseAddr, size) == 0)
        result = GetLastError();
    }
    ULONGLONG fileVersion = 0;
    if ( (m_parent != NULL) && (szImg != NULL) )
    {
      // try to retrive the file-version:
      if ( (this->m_parent->m_options & StackWalker::RetrieveFileVersion) != 0)
      {
        VS_FIXEDFILEINFO *fInfo = NULL;
        DWORD dwHandle;
        DWORD dwSize = GetFileVersionInfoSizeA(szImg, &dwHandle);
        if (dwSize > 0)
        {
          LPVOID vData = malloc(dwSize);
          if (vData != NULL)
          {
            if (GetFileVersionInfoA(szImg, dwHandle, dwSize, vData) != 0)
            {
              UINT len;
              CHAR szSubBlock[] = "\\";
              if (VerQueryValueA(vData, szSubBlock, (LPVOID*) &fInfo, &len) == 0)
                fInfo = NULL;
              else
              {
                fileVersion = ((ULONGLONG)fInfo->dwFileVersionLS) + ((ULONGLONG)fInfo->dwFileVersionMS << 32);
              }
            }
            free(vData);
          }
        }
      }

      // Retrive some additional-infos about the module
      IMAGEHLP_MODULE64_V2 Module;
      const char *szSymType = "-unknown-";
      if (this->GetModuleInfo(hProcess, baseAddr, &Module) != FALSE)
      {
        switch(Module.SymType)
        {
          case SymNone:
            szSymType = "-nosymbols-";
            break;
          case SymCoff:
            szSymType = "COFF";
            break;
          case SymCv:
            szSymType = "CV";
            break;
          case SymPdb:
            szSymType = "PDB";
            break;
          case SymExport:
            szSymType = "-exported-";
            break;
          case SymDeferred:
            szSymType = "-deferred-";
            break;
          case SymSym:
            szSymType = "SYM";
            break;
          case 8: //SymVirtual:
            szSymType = "Virtual";
            break;
          case 9: // SymDia:
            szSymType = "DIA";
            break;
        }
      }
      this->m_parent->OnLoadModule(img, mod, baseAddr, size, result, szSymType, Module.LoadedImageName, fileVersion);
    }
    if (szImg != NULL) free(szImg);
    if (szMod != NULL) free(szMod);
    return result;
  }
public:
  BOOL LoadModules(HANDLE hProcess, DWORD dwProcessId)
  {
    // first try toolhelp32
    if (GetModuleListTH32(hProcess, dwProcessId))
      return true;
    // then try psapi
    return GetModuleListPSAPI(hProcess);
  }

  BOOL GetModuleInfo(HANDLE hProcess, DWORD64 baseAddr, IMAGEHLP_MODULE64_V2 *pModuleInfo)
  {
    if(this->pSGMI == NULL)
    {
      SetLastError(ERROR_DLL_INIT_FAILED);
      return FALSE;
    }

    // as defined in VC7.1)...
    pModuleInfo->SizeOfStruct = sizeof(IMAGEHLP_MODULE64_V2);
    void *pData = malloc(4096); // reserve enough memory, so the bug in v6.3.5.1 does not lead to memory-overwrites...
    if (pData == NULL)
    {
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return FALSE;
    }
    memcpy(pData, pModuleInfo, sizeof(IMAGEHLP_MODULE64_V2));
    if (this->pSGMI(hProcess, baseAddr, (IMAGEHLP_MODULE64_V2*) pData) != FALSE)
    {
      // only copy as much memory as is reserved...
      memcpy(pModuleInfo, pData, sizeof(IMAGEHLP_MODULE64_V2));
      pModuleInfo->SizeOfStruct = sizeof(IMAGEHLP_MODULE64_V2);
      free(pData);
      return TRUE;
    }
    free(pData);
    SetLastError(ERROR_DLL_INIT_FAILED);
    return FALSE;
  }
};

// #############################################################
StackWalker::StackWalker(FILE * optOutFile, String * optOutString, int options, LPCSTR szSymPath, DWORD dwProcessId, HANDLE hProcess)
{
  this->m_outFile = optOutFile;
  this->m_outString = optOutString;
  this->m_options = options;
  this->m_modulesLoaded = FALSE;
  this->m_hProcess = hProcess;
  this->m_sw = new StackWalkerInternal(this, this->m_hProcess);
  this->m_dwProcessId = dwProcessId;
  if (szSymPath != NULL)
  {
    this->m_szSymPath = _strdup(szSymPath);
    this->m_options |= SymBuildPath;
  }
  else
    this->m_szSymPath = NULL;
}

StackWalker::~StackWalker()
{
  if (m_szSymPath != NULL)
    free(m_szSymPath);
  m_szSymPath = NULL;
  if (this->m_sw != NULL)
    delete this->m_sw;
  this->m_sw = NULL;
}

BOOL StackWalker::LoadModules()
{
  if (this->m_sw == NULL)
  {
    SetLastError(ERROR_DLL_INIT_FAILED);
    return FALSE;
  }
  if (m_modulesLoaded != FALSE)
    return TRUE;

  // Build the sym-path:
  char *szSymPath = NULL;
  if ( (this->m_options & SymBuildPath) != 0)
  {
    const size_t nSymPathLen = 4096;
    szSymPath = (char*) malloc(nSymPathLen);
    if (szSymPath == NULL)
    {
      SetLastError(ERROR_NOT_ENOUGH_MEMORY);
      return FALSE;
    }
    szSymPath[0] = 0;
    // Now first add the (optional) provided sympath:
    if (this->m_szSymPath != NULL)
    {
      strcat_s(szSymPath, nSymPathLen, this->m_szSymPath);
      strcat_s(szSymPath, nSymPathLen, ";");
    }

    strcat_s(szSymPath, nSymPathLen, ".;");

    const size_t nTempLen = 1024;
    char szTemp[nTempLen];
    // Now add the current directory:
    if (GetCurrentDirectoryA(nTempLen, szTemp) > 0)
    {
      szTemp[nTempLen-1] = 0;
      strcat_s(szSymPath, nSymPathLen, szTemp);
      strcat_s(szSymPath, nSymPathLen, ";");
    }

    // Now add the path for the main-module:
    if (GetModuleFileNameA(NULL, szTemp, nTempLen) > 0)
    {
      szTemp[nTempLen-1] = 0;
      for (char *p = (szTemp+strlen(szTemp)-1); p >= szTemp; --p)
      {
        // locate the rightmost path separator
        if ( (*p == '\\') || (*p == '/') || (*p == ':') )
        {
          *p = 0;
          break;
        }
      }  // for (search for path separator...)
      if (strlen(szTemp) > 0)
      {
        strcat_s(szSymPath, nSymPathLen, szTemp);
        strcat_s(szSymPath, nSymPathLen, ";");
      }
    }
    if (GetEnvironmentVariableA("_NT_SYMBOL_PATH", szTemp, nTempLen) > 0)
    {
      szTemp[nTempLen-1] = 0;
      strcat_s(szSymPath, nSymPathLen, szTemp);
      strcat_s(szSymPath, nSymPathLen, ";");
    }
    if (GetEnvironmentVariableA("_NT_ALTERNATE_SYMBOL_PATH", szTemp, nTempLen) > 0)
    {
      szTemp[nTempLen-1] = 0;
      strcat_s(szSymPath, nSymPathLen, szTemp);
      strcat_s(szSymPath, nSymPathLen, ";");
    }
    if (GetEnvironmentVariableA("SYSTEMROOT", szTemp, nTempLen) > 0)
    {
      szTemp[nTempLen-1] = 0;
      strcat_s(szSymPath, nSymPathLen, szTemp);
      strcat_s(szSymPath, nSymPathLen, ";");
      // also add the "system32"-directory:
      strcat_s(szTemp, nTempLen, "\\system32");
      strcat_s(szSymPath, nSymPathLen, szTemp);
      strcat_s(szSymPath, nSymPathLen, ";");
    }

    if ( (this->m_options & SymBuildPath) != 0)
    {
      if (GetEnvironmentVariableA("SYSTEMDRIVE", szTemp, nTempLen) > 0)
      {
        szTemp[nTempLen-1] = 0;
        strcat_s(szSymPath, nSymPathLen, "SRV*");
        strcat_s(szSymPath, nSymPathLen, szTemp);
        strcat_s(szSymPath, nSymPathLen, "\\websymbols");
        strcat_s(szSymPath, nSymPathLen, "*http://msdl.microsoft.com/download/symbols;");
      }
      else
        strcat_s(szSymPath, nSymPathLen, "SRV*c:\\websymbols*http://msdl.microsoft.com/download/symbols;");
    }
  }

  // First Init the whole stuff...
  BOOL bRet = this->m_sw->Init(szSymPath);
  if (szSymPath != NULL) free(szSymPath); szSymPath = NULL;
  if (bRet == FALSE)
  {
    this->OnDbgHelpErr("Error while initializing dbghelp.dll", 0, 0);
    SetLastError(ERROR_DLL_INIT_FAILED);
    return FALSE;
  }

  bRet = this->m_sw->LoadModules(this->m_hProcess, this->m_dwProcessId);
  if (bRet != FALSE)
    m_modulesLoaded = TRUE;
  return bRet;
}


// The following is used to pass the "userData"-Pointer to the user-provided readMemoryFunction
// This has to be done due to a problem with the "hProcess"-parameter in x64...
// Because this class is in no case multi-threading-enabled (because of the limitations 
// of dbghelp.dll) it is "safe" to use a static-variable
static StackWalker::PReadProcessMemoryRoutine s_readMemoryFunction = NULL;
static LPVOID s_readMemoryFunction_UserData = NULL;

BOOL StackWalker::ShowCallstack(uint32 maxDepth, HANDLE hThread, const CONTEXT *context, PReadProcessMemoryRoutine readMemoryFunction, LPVOID pUserData)
{
  CONTEXT c;;
  CallstackEntry csEntry;
  IMAGEHLP_SYMBOL64 *pSym = NULL;
  StackWalkerInternal::IMAGEHLP_MODULE64_V2 Module;
  IMAGEHLP_LINE64 Line;

  if (m_modulesLoaded == FALSE)
    this->LoadModules();  // ignore the result...

  if (this->m_sw->m_hDbhHelp == NULL)
  {
    SetLastError(ERROR_DLL_INIT_FAILED);
    return FALSE;
  }

  s_readMemoryFunction = readMemoryFunction;
  s_readMemoryFunction_UserData = pUserData;

  if (context == NULL)
  {
    // If no context is provided, capture the context
    if (hThread == GetCurrentThread())
    {
      GET_CURRENT_CONTEXT(c, USED_CONTEXT_FLAGS);
    }
    else
    {
      SuspendThread(hThread);
      memset(&c, 0, sizeof(CONTEXT));
      c.ContextFlags = USED_CONTEXT_FLAGS;
      if (GetThreadContext(hThread, &c) == FALSE)
      {
        ResumeThread(hThread);
        return FALSE;
      }
    }
  }
  else
    c = *context;

  // init STACKFRAME for first call
  STACKFRAME64 s; // in/out stackframe
  memset(&s, 0, sizeof(s));
  DWORD imageType;
#ifdef _M_IX86
  // normally, call ImageNtHeader() and use machine info from PE header
  imageType = IMAGE_FILE_MACHINE_I386;
  s.AddrPC.Offset = c.Eip;
  s.AddrPC.Mode = AddrModeFlat;
  s.AddrFrame.Offset = c.Ebp;
  s.AddrFrame.Mode = AddrModeFlat;
  s.AddrStack.Offset = c.Esp;
  s.AddrStack.Mode = AddrModeFlat;
#elif _M_X64
  imageType = IMAGE_FILE_MACHINE_AMD64;
  s.AddrPC.Offset = c.Rip;
  s.AddrPC.Mode = AddrModeFlat;
  s.AddrFrame.Offset = c.Rsp;
  s.AddrFrame.Mode = AddrModeFlat;
  s.AddrStack.Offset = c.Rsp;
  s.AddrStack.Mode = AddrModeFlat;
#elif _M_IA64
  imageType = IMAGE_FILE_MACHINE_IA64;
  s.AddrPC.Offset = c.StIIP;
  s.AddrPC.Mode = AddrModeFlat;
  s.AddrFrame.Offset = c.IntSp;
  s.AddrFrame.Mode = AddrModeFlat;
  s.AddrBStore.Offset = c.RsBSP;
  s.AddrBStore.Mode = AddrModeFlat;
  s.AddrStack.Offset = c.IntSp;
  s.AddrStack.Mode = AddrModeFlat;
#else
# error "StackWalker:  Platform not supported!"
#endif

  pSym = (IMAGEHLP_SYMBOL64 *) malloc(sizeof(IMAGEHLP_SYMBOL64) + STACKWALK_MAX_NAMELEN);
  if (!pSym) goto cleanup;  // not enough memory...
  memset(pSym, 0, sizeof(IMAGEHLP_SYMBOL64) + STACKWALK_MAX_NAMELEN);
  pSym->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
  pSym->MaxNameLength = STACKWALK_MAX_NAMELEN;

  memset(&Line, 0, sizeof(Line));
  Line.SizeOfStruct = sizeof(Line);

  memset(&Module, 0, sizeof(Module));
  Module.SizeOfStruct = sizeof(Module);

  for (uint32 frameNum=0; frameNum<maxDepth; frameNum++)
  {
    // get next stack frame (StackWalk64(), SymFunctionTableAccess64(), SymGetModuleBase64())
    // if this returns ERROR_INVALID_ADDRESS (487) or ERROR_NOACCESS (998), you can
    // assume that either you are done, or that the stack is so hosed that the next
    // deeper frame could not be found.
    // CONTEXT need not to be suplied if imageTyp is IMAGE_FILE_MACHINE_I386!
    if ( ! this->m_sw->pSW(imageType, this->m_hProcess, hThread, &s, &c, ReadProcMemCallback, this->m_sw->pSFTA, this->m_sw->pSGMB, NULL) )
    {
      this->OnDbgHelpErr("StackWalk64", GetLastError(), s.AddrPC.Offset);
      break;
    }

    csEntry.offset = s.AddrPC.Offset;
    csEntry.name[0] = 0;
    csEntry.undName[0] = 0;
    csEntry.undFullName[0] = 0;
    csEntry.offsetFromSmybol = 0;
    csEntry.offsetFromLine = 0;
    csEntry.lineFileName[0] = 0;
    csEntry.lineNumber = 0;
    csEntry.loadedImageName[0] = 0;
    csEntry.moduleName[0] = 0;
    if (s.AddrPC.Offset == s.AddrReturn.Offset)
    {
      this->OnDbgHelpErr("StackWalk64-Endless-Callstack!", 0, s.AddrPC.Offset);
      break;
    }
    if (s.AddrPC.Offset != 0)
    {
      // we seem to have a valid PC
      // show procedure info (SymGetSymFromAddr64())
      if (this->m_sw->pSGSFA(this->m_hProcess, s.AddrPC.Offset, &(csEntry.offsetFromSmybol), pSym) != FALSE)
      {
        // TODO: Mache dies sicher...!
        strcpy_s(csEntry.name, pSym->Name);
        // UnDecorateSymbolName()
        this->m_sw->pUDSN( pSym->Name, csEntry.undName, STACKWALK_MAX_NAMELEN, UNDNAME_NAME_ONLY );
        this->m_sw->pUDSN( pSym->Name, csEntry.undFullName, STACKWALK_MAX_NAMELEN, UNDNAME_COMPLETE );
      }
      else
      {
        this->OnDbgHelpErr("SymGetSymFromAddr64", GetLastError(), s.AddrPC.Offset);
      }

      // show line number info, NT5.0-method (SymGetLineFromAddr64())
      if (this->m_sw->pSGLFA != NULL )
      { // yes, we have SymGetLineFromAddr64()
        if (this->m_sw->pSGLFA(this->m_hProcess, s.AddrPC.Offset, &(csEntry.offsetFromLine), &Line) != FALSE)
        {
          csEntry.lineNumber = Line.LineNumber;
          // TODO: Mache dies sicher...!
          strcpy_s(csEntry.lineFileName, Line.FileName);
        }
        else
        {
          this->OnDbgHelpErr("SymGetLineFromAddr64", GetLastError(), s.AddrPC.Offset);
        }
      } // yes, we have SymGetLineFromAddr64()

      // show module info (SymGetModuleInfo64())
      if (this->m_sw->GetModuleInfo(this->m_hProcess, s.AddrPC.Offset, &Module ) != FALSE)
      { // got module info OK
        switch ( Module.SymType )
        {
        case SymNone:
          csEntry.symTypeString = "-nosymbols-";
          break;
        case SymCoff:
          csEntry.symTypeString = "COFF";
          break;
        case SymCv:
          csEntry.symTypeString = "CV";
          break;
        case SymPdb:
          csEntry.symTypeString = "PDB";
          break;
        case SymExport:
          csEntry.symTypeString = "-exported-";
          break;
        case SymDeferred:
          csEntry.symTypeString = "-deferred-";
          break;
        case SymSym:
          csEntry.symTypeString = "SYM";
          break;
#if API_VERSION_NUMBER >= 9
        case SymDia:
          csEntry.symTypeString = "DIA";
          break;
#endif
        case 8: //SymVirtual:
          csEntry.symTypeString = "Virtual";
          break;
        default:
          //_snprintf( ty, sizeof ty, "symtype=%ld", (long) Module.SymType );
          csEntry.symTypeString = NULL;
          break;
        }

        // TODO: Mache dies sicher...!
        strcpy_s(csEntry.moduleName, Module.ModuleName);
        csEntry.baseOfImage = Module.BaseOfImage;
        strcpy_s(csEntry.loadedImageName, Module.LoadedImageName);
      } // got module info OK
      else
      {
        this->OnDbgHelpErr("SymGetModuleInfo64", GetLastError(), s.AddrPC.Offset);
      }
    } // we seem to have a valid PC

    CallstackEntryType et = nextEntry;
    if (frameNum == 0) et = firstEntry;
    this->OnCallstackEntry(et, csEntry);
    
    if (s.AddrReturn.Offset == 0)
    {
      this->OnCallstackEntry(lastEntry, csEntry);
      SetLastError(ERROR_SUCCESS);
      break;
    }
  } // for ( frameNum )

  cleanup:
    if (pSym) free( pSym );

  if (context == NULL)
    ResumeThread(hThread);

  return TRUE;
}

BOOL __stdcall StackWalker::ReadProcMemCallback(
    HANDLE      hProcess,
    DWORD64     qwBaseAddress,
    PVOID       lpBuffer,
    DWORD       nSize,
    LPDWORD     lpNumberOfBytesRead
    )
{
  if (s_readMemoryFunction == NULL)
  {
    SIZE_T st;
    BOOL bRet = ReadProcessMemory(hProcess, (LPVOID) qwBaseAddress, lpBuffer, nSize, &st);
    *lpNumberOfBytesRead = (DWORD) st;
    //printf("ReadMemory: hProcess: %p, baseAddr: %p, buffer: %p, size: %d, read: %d, result: %d\n", hProcess, (LPVOID) qwBaseAddress, lpBuffer, nSize, (DWORD) st, (DWORD) bRet);
    return bRet;
  }
  else
  {
    return s_readMemoryFunction(hProcess, qwBaseAddress, lpBuffer, nSize, lpNumberOfBytesRead, s_readMemoryFunction_UserData);
  }
}

void StackWalker::OnLoadModule(LPCSTR img, LPCSTR mod, DWORD64 baseAddr, DWORD size, DWORD result, LPCSTR symType, LPCSTR pdbName, ULONGLONG fileVersion)
{
   CHAR buffer[STACKWALK_MAX_NAMELEN];
   if (fileVersion == 0) _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "%s:%s (%p), size: %d (result: %d), SymType: '%s', PDB: '%s'\n", img, mod, (LPVOID) baseAddr, size, result, symType, pdbName);
   else
   {
      DWORD v4 = (DWORD) fileVersion & 0xFFFF;
      DWORD v3 = (DWORD) (fileVersion>>16) & 0xFFFF;
      DWORD v2 = (DWORD) (fileVersion>>32) & 0xFFFF;
      DWORD v1 = (DWORD) (fileVersion>>48) & 0xFFFF;
      _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "%s:%s (%p), size: %d (result: %d), SymType: '%s', PDB: '%s', fileVersion: %d.%d.%d.%d\n", img, mod, (LPVOID) baseAddr, size, result, symType, pdbName, v1, v2, v3, v4);
   }
#ifdef REMOVED_BY_JAF_TOO_MUCH_INFORMATION
   OnOutput(buffer);
#endif
}

void StackWalker::OnCallstackEntry(CallstackEntryType eType, CallstackEntry &entry)
{
  CHAR buffer[STACKWALK_MAX_NAMELEN];
  if ( (eType != lastEntry) && (entry.offset != 0) )
  {
    if (entry.name[0] == 0)
      strcpy_s(entry.name, "(function-name not available)");
    if (entry.undName[0] != 0)
      strcpy_s(entry.name, entry.undName);
    if (entry.undFullName[0] != 0)
      strcpy_s(entry.name, entry.undFullName);
    if (entry.lineFileName[0] == 0)
    {
      strcpy_s(entry.lineFileName, "(filename not available)");
      if (entry.moduleName[0] == 0)
        strcpy_s(entry.moduleName, "(module-name not available)");
      _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "%p (%s): %s: %s\n", (LPVOID) entry.offset, entry.moduleName, entry.lineFileName, entry.name);
    }
    else
      _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "%s (%d): %s\n", entry.lineFileName, entry.lineNumber, entry.name);
    OnOutput(buffer);
  }
}

void StackWalker::OnDbgHelpErr(LPCSTR szFuncName, DWORD gle, DWORD64 addr)
{
  CHAR buffer[STACKWALK_MAX_NAMELEN];
  _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "ERROR: %s, GetLastError: %d (Address: %p)\n", szFuncName, gle, (LPVOID) addr);
  OnOutput(buffer);
}

void StackWalker::OnSymInit(LPCSTR szSearchPath, DWORD symOptions, LPCSTR szUserName)
{
  CHAR buffer[STACKWALK_MAX_NAMELEN];
  _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "SymInit: Symbol-SearchPath: '%s', symOptions: %d, UserName: '%s'\n", szSearchPath, symOptions, szUserName);
#ifdef REMOVED_BY_JAF_TOO_MUCH_INFORMATION
  OnOutput(buffer);
#endif

  // Also display the OS-version
  OSVERSIONINFOEXA ver; ZeroMemory(&ver, sizeof(OSVERSIONINFOEXA));
  ver.dwOSVersionInfoSize = sizeof(ver);
  if (GetVersionExA( (OSVERSIONINFOA*) &ver) != FALSE)
  {
    _snprintf_s(buffer, STACKWALK_MAX_NAMELEN, "OS-Version: %d.%d.%d (%s) 0x%x-0x%x\n", ver.dwMajorVersion, ver.dwMinorVersion, ver.dwBuildNumber, ver.szCSDVersion, ver.wSuiteMask, ver.wProductType);
#ifdef REMOVED_BY_JAF_TOO_MUCH_INFORMATION
    OnOutput(buffer);
#endif
  }
}

#endif  // Windows stack trace code

// Win32 doesn't have localtime_r, so we have to roll our own
#if defined(WIN32)
static inline struct tm * muscle_localtime_r(time_t * clock, struct tm * result)
{
   // Note that in Win32, (ret) points to thread-local storage, so this really
   // is thread-safe despite the fact that it looks like it isn't!
   struct tm * ret = localtime(clock);
   if (ret) *result = *ret;
   return ret;
}
static inline struct tm * muscle_gmtime_r(time_t * clock, struct tm * result)
{
   // Note that in Win32, (ret) points to thread-local storage, so this really
   // is thread-safe despite the fact that it looks like it isn't!
   struct tm * ret = gmtime(clock);
   if (ret) *result = *ret;
   return ret;
}
#else
static inline struct tm * muscle_localtime_r(time_t * clock, struct tm * result) {return localtime_r(clock, result);}
static inline struct tm * muscle_gmtime_r(   time_t * clock, struct tm * result) {return gmtime_r(clock, result);}
#endif

#ifndef MUSCLE_INLINE_LOGGING

#define MAX_STACK_TRACE_DEPTH ((uint32)(256))

status_t PrintStackTrace(FILE * optFile, uint32 maxDepth)
{
   TCHECKPOINT;

   if (optFile == NULL) optFile = stdout;

#if defined(MUSCLE_USE_BACKTRACE)
   void *array[MAX_STACK_TRACE_DEPTH];
   size_t size = backtrace(array, muscleMin(maxDepth, MAX_STACK_TRACE_DEPTH));
   char ** strings = backtrace_symbols(array, size);
   if (strings)
   {
      fprintf(optFile, "--Stack trace follows (%zd frames):\n", size);
      for (size_t i = 0; i < size; i++) fprintf(optFile, "  %s\n", strings[i]);
      fprintf(optFile, "--End Stack trace\n");
      free(strings);
      return B_NO_ERROR;
   }
   else fprintf(optFile, "PrintStackTrace:  Error, could not generate stack trace!\n");
#elif defined(MUSCLE_USE_MSVC_STACKWALKER)
   _Win32PrintStackTraceForContext(optFile, NULL, maxDepth);
#else
   (void) maxDepth;  // shut the compiler up
   fprintf(optFile, "PrintStackTrace:  Error, stack trace printing not available on this platform!\n");
#endif

   return B_ERROR;  // I don't know how to do this for other systems!
}

status_t GetStackTrace(String & retStr, uint32 maxDepth)
{
   TCHECKPOINT;

#if defined(MUSCLE_USE_BACKTRACE)
   void *array[MAX_STACK_TRACE_DEPTH];
   size_t size = backtrace(array, muscleMin(maxDepth, MAX_STACK_TRACE_DEPTH));
   char ** strings = backtrace_symbols(array, size);
   if (strings)
   {
      char buf[128];
      sprintf(buf, "--Stack trace follows (%zd frames):", size); retStr += buf;
      for (size_t i = 0; i < size; i++) 
      {
         retStr += "\n  ";
         retStr += strings[i];
      }
      retStr += "\n--End Stack trace\n";
      free(strings);
      return B_NO_ERROR;
   }
#elif defined(MUSCLE_USE_MSVC_STACKWALKER)
   StackWalker(NULL, &retStr, StackWalker::OptionsJAF).ShowCallstack(maxDepth);
#else
   (void) retStr;   // shut the compiler up
   (void) maxDepth;
#endif

   return B_ERROR;
}

static NestCount _inLogPreamble;

static const char * const _logLevelNames[] = {
   "None",
   "Critical Errors Only",
   "Errors Only",
   "Warnings and Errors Only",
   "Informational",
   "Debug",
   "Trace"
};

static const char * const _logLevelKeywords[] = {
   "none",
   "critical",
   "errors",
   "warnings",
   "info",
   "debug",
   "trace"
};

DefaultConsoleLogger :: DefaultConsoleLogger() : _consoleLogLevel(MUSCLE_LOG_INFO)
{
    // empty
}

void DefaultConsoleLogger :: Log(const LogCallbackArgs & a)
{
   if (a.GetLogLevel() <= _consoleLogLevel) 
   {
      vprintf(a.GetText(), *a.GetArgList());                  
      fflush(stdout);
   }
}

void DefaultConsoleLogger :: Flush()
{
   fflush(stdout);
}

DefaultFileLogger :: DefaultFileLogger() : _fileLogLevel(MUSCLE_LOG_NONE), _maxLogFileSize(MUSCLE_NO_LIMIT), _maxNumLogFiles(MUSCLE_NO_LIMIT), _compressionEnabled(false), _logFileOpenAttemptFailed(false)
{
   // empty
}

DefaultFileLogger :: ~DefaultFileLogger()
{
   CloseLogFile();
}

void DefaultFileLogger :: Log(const LogCallbackArgs & a)
{
   if ((a.GetLogLevel() <= GetFileLogLevel())&&(EnsureLogFileCreated(a) == B_NO_ERROR))
   {
      vfprintf(_logFile.GetFile(), a.GetText(), *a.GetArgList());
      _logFile.FlushOutput();
      if ((_maxLogFileSize != MUSCLE_NO_LIMIT)&&(_inLogPreamble.IsInBatch() == false))  // wait until we're outside the preamble to avoid breaking up lines too much
      {
         int64 curFileSize = _logFile.GetPosition();
         if ((curFileSize < 0)||(curFileSize >= (int64)_maxLogFileSize))
         {
            uint32 tempStoreSize = _maxLogFileSize;
            _maxLogFileSize = MUSCLE_NO_LIMIT;  // otherwise we'd recurse indefinitely here!
            CloseLogFile();
            _maxLogFileSize = tempStoreSize;
            (void) EnsureLogFileCreated(a);  // force the opening of the new log file right now, so that the open message show up in the right order
         }
      }
   }
}

void DefaultFileLogger :: Flush()
{
   _logFile.FlushOutput();
}

uint32 DefaultFileLogger :: AddPreExistingLogFiles(const String & filePattern)
{
   String dirPart, filePart;
   int32 lastSlash = filePattern.LastIndexOf(GetFilePathSeparator());   
   if (lastSlash >= 0)
   {
      dirPart = filePattern.Substring(0, lastSlash);
      filePart = filePattern.Substring(lastSlash+1);
   }
   else 
   {
      dirPart  = ".";
      filePart = filePattern;
   }

   Hashtable<String, uint64> pathToTime;
   if (filePart.HasChars())
   {
      StringMatcher sm(filePart);

      Directory d(dirPart());
      if (d.IsValid())
      {
         const char * nextName; 
         while((nextName = d.GetCurrentFileName()) != NULL)
         {
            String fn = nextName;
            if (sm.Match(fn))
            {
               String fullPath = dirPart+GetFilePathSeparator()+fn;
               FilePathInfo fpi(fullPath());
               if ((fpi.Exists())&&(fpi.IsRegularFile())) pathToTime.Put(fullPath, fpi.GetCreationTime());
            }
            d++;
         }
      }

      // Now we sort by creation time...
      pathToTime.SortByValue();

      // And add the results to our _oldFileNames queue.  That way when the log file is opened, the oldest files will be deleted (if appropriate)
      for (HashtableIterator<String, uint64> iter(pathToTime); iter.HasData(); iter++) (void) _oldLogFileNames.AddTail(iter.GetKey());
   }
   return pathToTime.GetNumItems();
}

status_t DefaultFileLogger :: EnsureLogFileCreated(const LogCallbackArgs & a)
{
   if ((_logFile.GetFile() == NULL)&&(_logFileOpenAttemptFailed == false))
   {
      String logFileName = _prototypeLogFileName;
      if (logFileName.IsEmpty()) logFileName = "%f.log";

      HumanReadableTimeValues hrtv; (void) GetHumanReadableTimeValues(SecondsToMicros(a.GetWhen()), hrtv);
      logFileName = hrtv.ExpandTokens(logFileName);

      _logFile.SetFile(fopen(logFileName(), "w"));
      if (_logFile.GetFile() != NULL) 
      {
         _activeLogFileName = logFileName;
         LogTime(MUSCLE_LOG_DEBUG, "Created Log file [%s]\n", _activeLogFileName());

         while(_oldLogFileNames.GetNumItems() >= _maxNumLogFiles)
         {
            const char * c = _oldLogFileNames.Head()();
                 if (remove(c) == 0)  LogTime(MUSCLE_LOG_DEBUG, "Deleted old Log file [%s]\n", c);
            else if (errno != ENOENT) LogTime(MUSCLE_LOG_ERROR, "Error deleting old Log file [%s]\n", c);
            _oldLogFileNames.RemoveHead();
         }

         String headerString = GetLogFileHeaderString(a);
         if (headerString.HasChars()) fprintf(_logFile.GetFile(), "%s\n", headerString()); 
      }
      else 
      {
         _activeLogFileName.Clear();
         _logFileOpenAttemptFailed = true;  // avoid an indefinite number of log-failed messages
         LogTime(MUSCLE_LOG_ERROR, "Failed to open Log file [%s], logging to file is now disabled.\n", logFileName());
      }
   }
   return (_logFile.GetFile() != NULL) ? B_NO_ERROR : B_ERROR;
}

void DefaultFileLogger :: CloseLogFile()
{
   if (_logFile.GetFile())
   {
      LogTime(MUSCLE_LOG_DEBUG, "Closing Log file [%s]\n", _activeLogFileName());
      String oldFileName = _activeLogFileName;  // default file to delete later, will be changed if/when we've made the .gz file
      _activeLogFileName.Clear();   // do this first to avoid reentrancy issues
      _logFile.Shutdown();

#ifdef MUSCLE_ENABLE_ZLIB_ENCODING
      if (_compressionEnabled)
      {
         FileDataIO inIO(fopen(oldFileName(), "rb"));
         if (inIO.GetFile() != NULL)
         {
            String gzName = oldFileName + ".gz";
            gzFile gzOut = gzopen(gzName(), "wb9"); // 9 for maximum compression
            if (gzOut != Z_NULL)
            {
               bool ok = true;
               while(1)
               {
                  char buf[128*1024];
                  int32 bytesRead = inIO.Read(buf, sizeof(buf));
                  if (bytesRead < 0) break;  // EOF

                  int bytesWritten = gzwrite(gzOut, buf, bytesRead);
                  if (bytesWritten <= 0)
                  {
                     ok = false;  // write error, oh dear
                     break;
                  }
               } 
               gzclose(gzOut);

               if (ok)
               {
                  inIO.Shutdown();
                  if (remove(oldFileName()) != 0) LogTime(MUSCLE_LOG_ERROR, "Error deleting log file [%s] after compressing it to [%s]!\n", oldFileName(), gzName());
                  oldFileName = gzName;
               }
               else 
               {
                  if (remove(gzName()) != 0) LogTime(MUSCLE_LOG_ERROR, "Error deleting gzip'd log file [%s] after compression failed!\n", gzName());
               }
            }
            else LogTime(MUSCLE_LOG_ERROR, "Could not open compressed Log file [%s]!\n", gzName());
         }
         else LogTime(MUSCLE_LOG_ERROR, "Could not reopen Log file [%s] to compress it!\n", oldFileName());
      }
#endif
      if (_maxNumLogFiles != MUSCLE_NO_LIMIT) (void) _oldLogFileNames.AddTail(oldFileName);  // so we can delete it later
   }
}

LogLineCallback :: LogLineCallback() : _writeTo(_buf)
{
   _buf[0] = '\0';
   _buf[sizeof(_buf)-1] = '\0';  // just in case vsnsprintf() has to truncate
}

LogLineCallback :: ~LogLineCallback() 
{
   // empty
}

void LogLineCallback :: Log(const LogCallbackArgs & a)
{
   TCHECKPOINT;

   // Generate the new text
#ifdef __MWERKS__
   int bytesAttempted = vsprintf(_writeTo, a.GetText(), *a.GetArgList());  // BeOS/PPC doesn't know vsnprintf :^P
#elif WIN32
   int bytesAttempted = _vsnprintf(_writeTo, (sizeof(_buf)-1)-(_writeTo-_buf), a.GetText(), *a.GetArgList());  // the -1 is for the guaranteed NUL terminator
#else
   int bytesAttempted = vsnprintf(_writeTo, (sizeof(_buf)-1)-(_writeTo-_buf), a.GetText(), *a.GetArgList());  // the -1 is for the guaranteed NUL terminator
#endif
   bool wasTruncated = (bytesAttempted != (int)strlen(_writeTo));  // do not combine with above line!

   // Log any newly completed lines
   char * logFrom  = _buf;
   char * searchAt = _writeTo;
   LogCallbackArgs tmp(a);
   while(true)
   {
      char * nextReturn = strchr(searchAt, '\n');
      if (nextReturn)
      {
         *nextReturn = '\0';  // terminate the string
         tmp.SetText(logFrom);
         LogLine(tmp);
         searchAt = logFrom = nextReturn+1;
      }
      else 
      {
         // If we ran out of buffer space and no carriage returns were detected,
         // then we need to just dump what we have and move on, there's nothing else we can do
         if (wasTruncated)
         {
            tmp.SetText(logFrom);
            LogLine(tmp);
            _buf[0] = '\0';
            _writeTo = searchAt = logFrom = _buf;
         }
         break;
      }
   }

   // And finally, move any remaining incomplete lines back to the beginning of the array, for next time
   if (logFrom > _buf) 
   {
      int slen = (int) strlen(logFrom);
      memmove(_buf, logFrom, slen+1);  // include NUL byte
      _writeTo = &_buf[slen];          // point to our just-moved NUL byte
   }
   else _writeTo = strchr(searchAt, '\0');

   _lastLog = a;
}
 
void LogLineCallback :: Flush()
{
   TCHECKPOINT;

   if (_writeTo > _buf)
   {
      _lastLog.SetText(_buf);
      LogLine(_lastLog);
      _writeTo = _buf;
      _buf[0] = '\0';
   }
}

static Mutex _logMutex;
static Hashtable<LogCallbackRef, Void> _logCallbacks;
static DefaultConsoleLogger _dcl;
static DefaultFileLogger _dfl;

status_t LockLog()
{
#ifdef MUSCLE_SINGLE_THREAD_ONLY
   return B_NO_ERROR;
#else
   return _logMutex.Lock();
#endif
}

status_t UnlockLog()
{
#ifdef MUSCLE_SINGLE_THREAD_ONLY
   return B_NO_ERROR;
#else
   return _logMutex.Unlock();
#endif
}

const char * GetLogLevelName(int ll)
{
   return ((ll>=0)&&(ll<(int) ARRAYITEMS(_logLevelNames))) ? _logLevelNames[ll] : "???";
}

const char * GetLogLevelKeyword(int ll)
{
   return ((ll>=0)&&(ll<(int) ARRAYITEMS(_logLevelKeywords))) ? _logLevelKeywords[ll] : "???";
}

int ParseLogLevelKeyword(const char * keyword)
{
   for (uint32 i=0; i<ARRAYITEMS(_logLevelKeywords); i++) if (strcmp(keyword, _logLevelKeywords[i]) == 0) return i;
   return -1;
}

int GetFileLogLevel()
{
   return _dfl.GetFileLogLevel();
}

String GetFileLogName()
{
   return _dfl.GetFileLogName();
}

uint32 GetFileLogMaximumSize()
{
   return _dfl.GetMaxLogFileSize();
}

uint32 GetMaxNumLogFiles()
{
   return _dfl.GetMaxNumLogFiles();
}

bool GetFileLogCompressionEnabled()
{
   return _dfl.GetFileCompressionEnabled();
}

int GetConsoleLogLevel()
{
   return _dcl.GetConsoleLogLevel();
}

int GetMaxLogLevel()
{
   return muscleMax(_dcl.GetConsoleLogLevel(), _dfl.GetFileLogLevel());
}

status_t SetFileLogName(const String & logName)
{
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.SetLogFileName(logName);
      LogTime(MUSCLE_LOG_DEBUG, "File log name set to: %s\n", logName());
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

status_t SetOldLogFilesPattern(const String & pattern)
{
   if (LockLog() == B_NO_ERROR)
   {
      uint32 numAdded = _dfl.AddPreExistingLogFiles(pattern);
      LogTime(MUSCLE_LOG_DEBUG, "Old Log Files pattern set to: [%s] (" UINT32_FORMAT_SPEC " files matched)\n", pattern(), numAdded);
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

status_t SetFileLogMaximumSize(uint32 maxSizeBytes)
{
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.SetMaxLogFileSize(maxSizeBytes);
      if (maxSizeBytes == MUSCLE_NO_LIMIT) LogTime(MUSCLE_LOG_DEBUG, "File log maximum size set to: (unlimited).\n");
                                      else LogTime(MUSCLE_LOG_DEBUG, "File log maximum size set to: " UINT32_FORMAT_SPEC " bytes.\n", maxSizeBytes);
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

status_t SetMaxNumLogFiles(uint32 maxNumLogFiles)
{
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.SetMaxNumLogFiles(maxNumLogFiles);
      if (maxNumLogFiles == MUSCLE_NO_LIMIT) LogTime(MUSCLE_LOG_DEBUG, "Maximum number of log files set to: (unlimited).\n");
                                        else LogTime(MUSCLE_LOG_DEBUG, "Maximum number of log files to: " UINT32_FORMAT_SPEC "\n", maxNumLogFiles);
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

status_t SetFileLogCompressionEnabled(bool enable)
{
#ifdef MUSCLE_ENABLE_ZLIB_ENCODING
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.SetFileCompressionEnabled(enable);
      LogTime(MUSCLE_LOG_DEBUG, "File log compression %s.\n", enable?"enabled":"disabled");
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
#else
   if (enable) 
   {
      LogTime(MUSCLE_LOG_CRITICALERROR, "Can not enable log file compression, MUSCLE was compiled without MUSCLE_ENABLE_ZLIB_ENCODING specified!\n");
      return B_ERROR;
   }
   else return B_NO_ERROR;
#endif
}

void CloseCurrentLogFile()
{
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.CloseLogFile();
      (void) UnlockLog();
   }
}

status_t SetFileLogLevel(int loglevel)
{
   if (LockLog() == B_NO_ERROR)
   {
      _dfl.SetFileLogLevel(loglevel);
      LogTime(MUSCLE_LOG_DEBUG, "File logging level set to: %s\n", GetLogLevelName(_dfl.GetFileLogLevel()));
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

status_t SetConsoleLogLevel(int loglevel)
{
   if (LockLog() == B_NO_ERROR)
   {
      _dcl.SetConsoleLogLevel(loglevel);
      LogTime(MUSCLE_LOG_DEBUG, "Console logging level set to: %s\n", GetLogLevelName(_dcl.GetConsoleLogLevel()));
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR;
}

// Our 26-character alphabet of usable symbols
#define NUM_CHARS_IN_KEY_ALPHABET (sizeof(_keyAlphabet)-1)  // -1 because the NUL terminator doesn't count
static const char _keyAlphabet[] = "2346789BCDFGHJKMNPRSTVWXYZ";  // FogBugz #5808: vowels and some numerals omitted to avoid ambiguity and inadvertent swearing
static const uint32 _keySpaceSize = NUM_CHARS_IN_KEY_ALPHABET * NUM_CHARS_IN_KEY_ALPHABET * NUM_CHARS_IN_KEY_ALPHABET * NUM_CHARS_IN_KEY_ALPHABET;

uint32 GenerateSourceCodeLocationKey(const char * fileName, uint32 lineNumber)
{
#ifdef WIN32
   const char * lastSlash = strrchr(fileName, '\\');
#else
   const char * lastSlash = strrchr(fileName, '/');
#endif
   if (lastSlash) fileName = lastSlash+1;

   return ((CalculateHashCode(fileName,(uint32)strlen(fileName))+lineNumber)%(_keySpaceSize-1))+1;  // note that 0 is not considered a valid key value!
}

String SourceCodeLocationKeyToString(uint32 key)
{
   if (key == 0) return "";                  // 0 is not a valid key value
   if (key >= _keySpaceSize) return "????";  // values greater than or equal to our key space size are errors

   char buf[5]; buf[4] = '\0';
   for (int32 i=3; i>=0; i--)
   {
      buf[i] = _keyAlphabet[key % NUM_CHARS_IN_KEY_ALPHABET];
      key /= NUM_CHARS_IN_KEY_ALPHABET; 
   }
   return buf;
}

uint32 SourceCodeLocationKeyFromString(const String & ss)
{
   String s = ss.ToUpperCase().Trim();
   if (s.Length() != 4) return 0;  // codes must always be exactly 4 characters long!

   s.Replace('0', 'O');
   s.Replace('1', 'I');
   s.Replace('5', 'S');

   uint32 ret  = 0;
   uint32 base = 1;
   for (int32 i=3; i>=0; i--)
   {
      const char * p = strchr(_keyAlphabet, s[i]);
      if (p == NULL) return 0;  // invalid character!

      int whichChar = (int) (p-_keyAlphabet);
      ret += (whichChar*base);
      base *= NUM_CHARS_IN_KEY_ALPHABET;
   }
   return ret;
}

void GetStandardLogLinePreamble(char * buf, const LogCallbackArgs & a)
{
   struct tm ltm;
   time_t when = a.GetWhen();
   struct tm * temp = muscle_localtime_r(&when, &ltm);
#ifdef MUSCLE_INCLUDE_SOURCE_LOCATION_IN_LOGTIME
   sprintf(buf, "[%c %02i/%02i %02i:%02i:%02i] [%s] ", GetLogLevelName(a.GetLogLevel())[0], temp->tm_mon+1, temp->tm_mday, temp->tm_hour, temp->tm_min, temp->tm_sec, SourceCodeLocationKeyToString(GenerateSourceCodeLocationKey(a.GetSourceFile(), a.GetSourceLineNumber()))());
#else
   sprintf(buf, "[%c %02i/%02i %02i:%02i:%02i] ", GetLogLevelName(a.GetLogLevel())[0], temp->tm_mon+1, temp->tm_mday, temp->tm_hour, temp->tm_min, temp->tm_sec);
#endif
}

#define DO_LOGGING_CALLBACK(cb) \
{                               \
   va_list argList;             \
   va_start(argList, fmt);      \
   cb.Log(LogCallbackArgs(when, ll, sourceFile, sourceFunction, sourceLine, fmt, &argList)); \
   va_end(argList);             \
}

#define DO_LOGGING_CALLBACKS for (HashtableIterator<LogCallbackRef, Void> iter(_logCallbacks); iter.HasData(); iter++) if (iter.GetKey()()) DO_LOGGING_CALLBACK((*iter.GetKey()()));

#ifdef MUSCLE_INCLUDE_SOURCE_LOCATION_IN_LOGTIME
status_t _LogTime(const char * sourceFile, const char * sourceFunction, int sourceLine, int ll, const char * fmt, ...)
#else
status_t LogTime(int ll, const char * fmt, ...)
#endif
{
#ifndef MUSCLE_INCLUDE_SOURCE_LOCATION_IN_LOGTIME
   static const char * sourceFile = "";
   static const char * sourceFunction = "";
   static const int sourceLine = -1;
#endif

   status_t lockRet = LockLog();
   {
      // First, log the preamble
      time_t when = time(NULL);
      char buf[128];

      // First, send to the log file
      {
         NestCountGuard g(_inLogPreamble);  // must be inside the braces!
         va_list dummyList;
         va_start(dummyList, fmt);  // not used
         LogCallbackArgs lca(when, ll, sourceFile, sourceFunction, sourceLine, buf, &dummyList);
         GetStandardLogLinePreamble(buf, lca);
         lca.SetText(buf);
         _dfl.Log(lca);
         va_end(dummyList);
      }
      DO_LOGGING_CALLBACK(_dfl);
     
      // Then, send to the display
      {
         NestCountGuard g(_inLogPreamble);  // must be inside the braces!
         va_list dummyList;
         va_start(dummyList, fmt);  // not used
         _dcl.Log(LogCallbackArgs(when, ll, sourceFile, sourceFunction, sourceLine, buf, &dummyList));
         va_end(dummyList);
      }
      DO_LOGGING_CALLBACK(_dcl);  // must be outside of the braces!
   
      // Then log the actual message as supplied by the user
      if (lockRet == B_NO_ERROR) DO_LOGGING_CALLBACKS;
   }
   if (lockRet == B_NO_ERROR) UnlockLog();

   return lockRet;
}

status_t LogFlush()
{
   TCHECKPOINT;

   if (LockLog() == B_NO_ERROR)
   {
      for (HashtableIterator<LogCallbackRef, Void> iter(_logCallbacks); iter.HasData(); iter++) if (iter.GetKey()()) iter.GetKey()()->Flush();
      (void) UnlockLog();
      return B_NO_ERROR;
   }
   else return B_ERROR; 
}

status_t LogStackTrace(int ll, uint32 maxDepth)
{
   TCHECKPOINT;

#if defined(MUSCLE_USE_BACKTRACE)
   void *array[MAX_STACK_TRACE_DEPTH];
   size_t size = backtrace(array, muscleMin(maxDepth, MAX_STACK_TRACE_DEPTH));
   char ** strings = backtrace_symbols(array, size);
   if (strings)
   {
      LogTime(ll, "--Stack trace follows (%zd frames):\n", size);
      for (size_t i = 0; i < size; i++) LogTime(ll, "  %s\n", strings[i]);
      LogTime(ll, "--End Stack trace\n");
      free(strings);
      return B_NO_ERROR;
   }
#else
   (void) ll;        // shut the compiler up
   (void) maxDepth;  // shut the compiler up
#endif

   return B_ERROR;  // I don't know how to do this for other systems!
}

status_t Log(int ll, const char * fmt, ...)
{
   // No way to get these, since #define Log() as a macro causes
   // nasty namespace collisions with other methods/functions named Log()
   static const char * sourceFile     = "";
   static const char * sourceFunction = "";
   static const int sourceLine        = -1;

   status_t lockRet = LockLog();
   {
      time_t when = time(NULL);  // don't inline this, ya dummy
      DO_LOGGING_CALLBACK(_dfl);
      DO_LOGGING_CALLBACK(_dcl);
      if (lockRet == B_NO_ERROR) DO_LOGGING_CALLBACKS;
   }
   if (lockRet == B_NO_ERROR) (void) UnlockLog();
   return lockRet;
}

status_t PutLogCallback(const LogCallbackRef & cb)
{
   status_t ret = B_ERROR;
   if (LockLog() == B_NO_ERROR)
   {
      ret = _logCallbacks.PutWithDefault(cb);
      (void) UnlockLog();
   }
   return ret;
}

status_t ClearLogCallbacks()
{
   status_t ret = B_ERROR;
   if (LockLog() == B_NO_ERROR)
   {
      _logCallbacks.Clear();
      (void) UnlockLog();
   }
   return ret;
}

status_t RemoveLogCallback(const LogCallbackRef & cb)
{
   status_t ret = B_ERROR;
   if (LockLog() == B_NO_ERROR)
   {
      ret = _logCallbacks.Remove(cb);
      (void) UnlockLog();
   }
   return ret;
}

#endif

#ifdef WIN32
static const uint64 _windowsDiffTime = ((uint64)116444736)*NANOS_PER_SECOND; // add (1970-1601) to convert to Windows time base
#endif

status_t GetHumanReadableTimeValues(uint64 timeUS, HumanReadableTimeValues & v, uint32 timeType)
{
   TCHECKPOINT;

   if (timeUS == MUSCLE_TIME_NEVER) return B_ERROR;

   int microsLeft = (int)(timeUS % MICROS_PER_SECOND);

#ifdef WIN32
   // Borland's localtime() function is buggy, so we'll use the Win32 API instead.
   uint64 winTime = (timeUS*10) + _windowsDiffTime;  // Convert to (100ns units)

   FILETIME fileTime;
   fileTime.dwHighDateTime = (DWORD) ((winTime>>32) & 0xFFFFFFFF);
   fileTime.dwLowDateTime  = (DWORD) ((winTime>> 0) & 0xFFFFFFFF);

   SYSTEMTIME st;
   if (FileTimeToSystemTime(&fileTime, &st)) 
   {
      if (timeType == MUSCLE_TIMEZONE_UTC)
      {
         TIME_ZONE_INFORMATION tzi;
         if ((GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID)||(SystemTimeToTzSpecificLocalTime(&tzi, &st, &st) == false)) return B_ERROR;
      }
      v = HumanReadableTimeValues(st.wYear, st.wMonth-1, st.wDay-1, st.wDayOfWeek, st.wHour, st.wMinute, st.wSecond, microsLeft);
      return B_NO_ERROR;
   }
#else
   struct tm ltm, gtm;
   time_t timeS = (time_t) MicrosToSeconds(timeUS);  // timeS is seconds since 1970
   struct tm * ts = (timeType == MUSCLE_TIMEZONE_UTC) ? muscle_localtime_r(&timeS, &ltm) : muscle_gmtime_r(&timeS, &gtm);  // only convert if it isn't already local
   if (ts) 
   {
      v = HumanReadableTimeValues(ts->tm_year+1900, ts->tm_mon, ts->tm_mday-1, ts->tm_wday, ts->tm_hour, ts->tm_min, ts->tm_sec, microsLeft);
      return B_NO_ERROR;
   }
#endif

   return B_ERROR;
}

#ifdef WIN32
static bool MUSCLE_TzSpecificLocalTimeToSystemTime(LPTIME_ZONE_INFORMATION tzi, LPSYSTEMTIME st)
{
# if defined(__BORLANDC__) || defined(MUSCLE_USING_OLD_MICROSOFT_COMPILER) || defined(__MINGW32__)
#  if defined(_MSC_VER)
   typedef BOOL (*TzSpecificLocalTimeToSystemTimeProc) (IN LPTIME_ZONE_INFORMATION lpTimeZoneInformation, IN LPSYSTEMTIME lpLocalTime, OUT LPSYSTEMTIME lpUniversalTime);
#  else
   typedef WINBASEAPI BOOL WINAPI (*TzSpecificLocalTimeToSystemTimeProc) (IN LPTIME_ZONE_INFORMATION lpTimeZoneInformation, IN LPSYSTEMTIME lpLocalTime, OUT LPSYSTEMTIME lpUniversalTime);
#  endif

   // Some compilers' headers don't have this call, so we have to do it the hard way
   HMODULE lib = LoadLibrary(TEXT("kernel32.dll"));
   if (lib == NULL) return false;

   TzSpecificLocalTimeToSystemTimeProc tzProc = (TzSpecificLocalTimeToSystemTimeProc) GetProcAddress(lib, "TzSpecificLocalTimeToSystemTime");
   bool ret = ((tzProc)&&(tzProc(tzi, st, st)));
   FreeLibrary(lib);
   return ret;
# else
   return (TzSpecificLocalTimeToSystemTime(tzi, st, st) != 0);
# endif
}
#endif

status_t GetTimeStampFromHumanReadableTimeValues(const HumanReadableTimeValues & v, uint64 & retTimeStamp, uint32 timeType)
{
   TCHECKPOINT;

#ifdef WIN32
   SYSTEMTIME st; memset(&st, 0, sizeof(st));
   st.wYear         = v.GetYear();
   st.wMonth        = v.GetMonth()+1;
   st.wDayOfWeek    = v.GetDayOfWeek();
   st.wDay          = v.GetDayOfMonth()+1;
   st.wHour         = v.GetHour();
   st.wMinute       = v.GetMinute();
   st.wSecond       = v.GetSecond();
   st.wMilliseconds = v.GetMicrosecond()/1000;

   if (timeType == MUSCLE_TIMEZONE_UTC)
   {
      TIME_ZONE_INFORMATION tzi;
      if ((GetTimeZoneInformation(&tzi) == TIME_ZONE_ID_INVALID)||(MUSCLE_TzSpecificLocalTimeToSystemTime(&tzi, &st) == false)) return B_ERROR;
   }

   FILETIME fileTime;
   if (SystemTimeToFileTime(&st, &fileTime))
   {
      retTimeStamp = (((((uint64)fileTime.dwHighDateTime)<<32)|((uint64)fileTime.dwLowDateTime))-_windowsDiffTime)/10;
      return B_NO_ERROR;
   }
   else return B_ERROR;
#else
   struct tm ltm; memset(&ltm, 0, sizeof(ltm));
   ltm.tm_sec  = v.GetSecond();       /* seconds after the minute [0-60] */
   ltm.tm_min  = v.GetMinute();       /* minutes after the hour [0-59] */
   ltm.tm_hour = v.GetHour();         /* hours since midnight [0-23] */
   ltm.tm_mday = v.GetDayOfMonth()+1; /* day of the month [1-31] */
   ltm.tm_mon  = v.GetMonth();        /* months since January [0-11] */
   ltm.tm_year = v.GetYear()-1900;    /* years since 1900 */
   ltm.tm_wday = v.GetDayOfWeek();    /* days since Sunday [0-6] */
   ltm.tm_isdst = -1;  /* Let mktime() decide whether summer time is in effect */

   time_t tm = ((uint64)((timeType == MUSCLE_TIMEZONE_UTC) ? mktime(&ltm) : timegm(&ltm)));
   if (tm == -1) return B_ERROR;

   retTimeStamp = SecondsToMicros(tm);
   return B_NO_ERROR;
#endif
}


String HumanReadableTimeValues :: ToString() const
{
   return ExpandTokens("%T");  // Yes, this must be here in the .cpp file!
}

String HumanReadableTimeValues :: ExpandTokens(const String & origString) const
{
   if (origString.IndexOf('%') < 0) return origString;

   String newString = origString;
   (void) newString.Replace("%%", "%");  // do this first!
   (void) newString.Replace("%T", "%Q %D %Y %h:%m:%s");
   (void) newString.Replace("%t", "%Y/%M/%D %h:%m:%s");
   (void) newString.Replace("%f", "%Y-%M-%D_%hh%mm%s");

   static const char * _daysOfWeek[]   = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
   static const char * _monthsOfYear[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

   (void) newString.Replace("%Y", String("%1").Arg(GetYear()));
   (void) newString.Replace("%M", String("%1").Arg(GetMonth()+1,      "%02i"));
   (void) newString.Replace("%Q", String("%1").Arg(_monthsOfYear[muscleClamp(GetMonth(), 0, (int)(ARRAYITEMS(_monthsOfYear)-1))]));
   (void) newString.Replace("%D", String("%1").Arg(GetDayOfMonth()+1, "%02i"));
   (void) newString.Replace("%d", String("%1").Arg(GetDayOfMonth()+1, "%02i"));
   (void) newString.Replace("%W", String("%1").Arg(GetDayOfWeek()+1,  "%02i"));
   (void) newString.Replace("%w", String("%1").Arg(GetDayOfWeek()+1,  "%02i"));
   (void) newString.Replace("%q", String("%1").Arg(_daysOfWeek[muscleClamp(GetDayOfWeek(), 0, (int)(ARRAYITEMS(_daysOfWeek)-1))]));
   (void) newString.Replace("%h", String("%1").Arg(GetHour(),           "%02i"));
   (void) newString.Replace("%m", String("%1").Arg(GetMinute(),         "%02i"));
   (void) newString.Replace("%s", String("%1").Arg(GetSecond(),         "%02i"));
   (void) newString.Replace("%x", String("%1").Arg(GetMicrosecond(),    "%06i"));

   uint32 r1 = rand();
   uint32 r2 = rand();
   char buf[64]; sprintf(buf, UINT64_FORMAT_SPEC, (((uint64)r1)<<32)|((uint64)r2));
   (void) newString.Replace("%r", buf);

   return newString;
}

String GetHumanReadableTimeString(uint64 timeUS, uint32 timeType)
{
   TCHECKPOINT;

   if (timeUS == MUSCLE_TIME_NEVER) return ("(never)");
   else
   {
      HumanReadableTimeValues v;
      if (GetHumanReadableTimeValues(timeUS, v, timeType) == B_NO_ERROR)
      {
         char buf[256];
         sprintf(buf, "%02i/%02i/%02i %02i:%02i:%02i", v.GetYear(), v.GetMonth()+1, v.GetDayOfMonth()+1, v.GetHour(), v.GetMinute(), v.GetSecond());
         return String(buf);
      }
      return "";
   }
}
 
#ifdef WIN32
extern uint64 __Win32FileTimeToMuscleTime(const FILETIME & ft);  // from SetupSystem.cpp
#endif

uint64 ParseHumanReadableTimeString(const String & s, uint32 timeType)
{
   TCHECKPOINT;

   if (s.IndexOfIgnoreCase("never") >= 0) return MUSCLE_TIME_NEVER;

   StringTokenizer tok(s(), "/: ");
   const char * year   = tok();
   const char * month  = tok();
   const char * day    = tok();
   const char * hour   = tok();
   const char * minute = tok();
   const char * second = tok();

#if defined(WIN32) && defined(WINXP)
   SYSTEMTIME st; memset(&st, 0, sizeof(st));
   st.wYear      = (WORD) (year   ? atoi(year)   : 0);
   st.wMonth     = (WORD) (month  ? atoi(month)  : 0);
   st.wDay       = (WORD) (day    ? atoi(day)    : 0);
   st.wHour      = (WORD) (hour   ? atoi(hour)   : 0);
   st.wMinute    = (WORD) (minute ? atoi(minute) : 0);
   st.wSecond    = (WORD) (second ? atoi(second) : 0);

   if (timeType == MUSCLE_TIMEZONE_UTC)
   {
      TIME_ZONE_INFORMATION tzi;
      if (GetTimeZoneInformation(&tzi) != TIME_ZONE_ID_INVALID) (void) MUSCLE_TzSpecificLocalTimeToSystemTime(&tzi, &st);
   }

   FILETIME fileTime;
   return (SystemTimeToFileTime(&st, &fileTime)) ? __Win32FileTimeToMuscleTime(fileTime) : 0;
#else
   struct tm st; memset(&st, 0, sizeof(st));
   st.tm_sec  = second ? atoi(second)    : 0;
   st.tm_min  = minute ? atoi(minute)    : 0;
   st.tm_hour = hour   ? atoi(hour)      : 0;
   st.tm_mday = day    ? atoi(day)       : 0;
   st.tm_mon  = month  ? atoi(month)-1   : 0;
   st.tm_year = year   ? atoi(year)-1900 : 0;
   time_t timeS = mktime(&st);
   if (timeType == MUSCLE_TIMEZONE_LOCAL)
   {
      struct tm ltm;
      struct tm * t = muscle_gmtime_r(&timeS, &ltm);
      if (t) timeS += (timeS-mktime(t));  
   }
   return SecondsToMicros(timeS);
#endif
}

enum {
   TIME_UNIT_MICROSECOND,
   TIME_UNIT_MILLISECOND,
   TIME_UNIT_SECOND,
   TIME_UNIT_MINUTE,
   TIME_UNIT_HOUR,
   TIME_UNIT_DAY,
   TIME_UNIT_WEEK,
   TIME_UNIT_MONTH,
   TIME_UNIT_YEAR,
   NUM_TIME_UNITS
};

static const uint64 MICROS_PER_DAY = DaysToMicros(1);

static const uint64 _timeUnits[NUM_TIME_UNITS] = {
   1,                       // micros -> micros
   1000,                    // millis -> micros
   MICROS_PER_SECOND,       // secs   -> micros
   60*MICROS_PER_SECOND,    // mins   -> micros
   60*60*MICROS_PER_SECOND, // hours  -> micros
   MICROS_PER_DAY,          // days   -> micros
   7*MICROS_PER_DAY,        // weeks  -> micros
   30*MICROS_PER_DAY,       // months -> micros (well, sort of -- we assume a month is always 30  days, which isn't really true)
   365*MICROS_PER_DAY       // years  -> micros (well, sort of -- we assume a years is always 365 days, which isn't really true)
};
static const char * _timeUnitNames[NUM_TIME_UNITS] = {
   "microsecond",
   "millisecond",
   "second",
   "minute",
   "hour",
   "day",
   "week",
   "month",
   "year",
};

static bool IsFloatingPointNumber(const char * d)
{
   while(1)
   {
           if (*d == '.')            return true;
      else if (isdigit(*d) == false) return false;
      else                           d++;
   }
}

static uint64 GetTimeUnitMultiplier(const String & l, uint64 defaultValue)
{
   uint64 multiplier = defaultValue;
   String tmp(l); tmp = tmp.ToLowerCase();
        if ((tmp.StartsWith("us"))||(tmp.StartsWith("micro"))) multiplier = _timeUnits[TIME_UNIT_MICROSECOND];
   else if ((tmp.StartsWith("ms"))||(tmp.StartsWith("milli"))) multiplier = _timeUnits[TIME_UNIT_MILLISECOND];
   else if (tmp.StartsWith("mo"))                              multiplier = _timeUnits[TIME_UNIT_MONTH];
   else if (tmp.StartsWith("s"))                               multiplier = _timeUnits[TIME_UNIT_SECOND];
   else if (tmp.StartsWith("m"))                               multiplier = _timeUnits[TIME_UNIT_MINUTE];
   else if (tmp.StartsWith("h"))                               multiplier = _timeUnits[TIME_UNIT_HOUR];
   else if (tmp.StartsWith("d"))                               multiplier = _timeUnits[TIME_UNIT_DAY];
   else if (tmp.StartsWith("w"))                               multiplier = _timeUnits[TIME_UNIT_WEEK];
   else if (tmp.StartsWith("y"))                               multiplier = _timeUnits[TIME_UNIT_YEAR];
   return multiplier;
}

uint64 ParseHumanReadableTimeIntervalString(const String & s)
{
   if ((s.EqualsIgnoreCase("forever"))||(s.EqualsIgnoreCase("never"))||(s.StartsWithIgnoreCase("inf"))) return MUSCLE_TIME_NEVER;

   /** Find first digit */
   const char * d = s();
   while((*d)&&(isdigit(*d) == false)) d++;
   if (*d == '\0') return GetTimeUnitMultiplier(s, 0);  // in case the string is just "second" or "hour" or etc.

   /** Find first letter */
   const char * l = s();
   while((*l)&&(isalpha(*l) == false)) l++;
   if (*l == '\0') l = "s";  // default to seconds

   uint64 multiplier = GetTimeUnitMultiplier(l, _timeUnits[TIME_UNIT_SECOND]);   // default units is seconds
   const char * afterLetters = l;
   while((*afterLetters)&&((*afterLetters==',')||(isalpha(*afterLetters)||(isspace(*afterLetters))))) afterLetters++;

   uint64 ret = IsFloatingPointNumber(d) ? (uint64)(atof(d)*multiplier) : (Atoull(d)*multiplier);
   if (*afterLetters) ret += ParseHumanReadableTimeIntervalString(afterLetters);
   return ret;
}

String GetHumanReadableTimeIntervalString(uint64 intervalUS, uint32 maxClauses, uint64 minPrecision, bool * optRetIsAccurate)
{
   if (intervalUS == MUSCLE_TIME_NEVER) return "forever";

   // Find the largest unit that is still smaller than (micros)
   uint32 whichUnit = TIME_UNIT_MICROSECOND;
   for (uint32 i=0; i<NUM_TIME_UNITS; i++) 
   {
      if (_timeUnits[whichUnit] < intervalUS) whichUnit++;
                                         else break;
   }
   if ((whichUnit >= NUM_TIME_UNITS)||((whichUnit > 0)&&(_timeUnits[whichUnit] > intervalUS))) whichUnit--;

   uint64 numUnits = intervalUS/_timeUnits[whichUnit];
   char buf[256]; sprintf(buf, UINT64_FORMAT_SPEC " %s%s", numUnits, _timeUnitNames[whichUnit], (numUnits==1)?"":"s");
   String ret = buf;
  
   uint64 leftover = intervalUS%_timeUnits[whichUnit];
   if (leftover > 0)
   {
      if ((leftover > minPrecision)&&(maxClauses > 1)) ret += GetHumanReadableTimeIntervalString(leftover, maxClauses-1, minPrecision, optRetIsAccurate).Prepend(", ");
                                                  else if (optRetIsAccurate) *optRetIsAccurate = false;
   }
   else if (optRetIsAccurate) *optRetIsAccurate = true;

   return ret;
}

#ifndef MUSCLE_INLINE_LOGGING

extern uint32 GetAndClearFailedMemoryRequestSize();

void WarnOutOfMemory(const char * file, int line)
{
   // Yes, this technique is open to race conditions and other lossage.
   // But it will work in the one-error-only case, which is good enough
   // for now.
   LogTime(MUSCLE_LOG_CRITICALERROR, "ERROR--OUT OF MEMORY!  (" INT32_FORMAT_SPEC " bytes at %s:%i)\n", GetAndClearFailedMemoryRequestSize(), file, line);
}

#endif

}; // end namespace muscle
