// Ex.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <windows.h>

enum Buffer
{
    bufFormatMain = 0,
    bufFormatStdOut,
    bufFormatStdErr,
    bufLastErrorMain,
    bufLastErrorStdOut,
    bufLastErrorStdErr,
    bufCount
};

const DWORD c_PipeBufferSize = 0x1000;

DWORD  g_cchBufSize[bufCount] = {0};
LPTSTR g_pszBuffer[bufCount] = {NULL};
HANDLE g_hStdOut = INVALID_HANDLE_VALUE;
HANDLE g_hStdErr = INVALID_HANDLE_VALUE;
HANDLE g_hLogFile = INVALID_HANDLE_VALUE;
HANDLE g_hLogFileMutex = INVALID_HANDLE_VALUE;

void ConPrint( LPCTSTR szOutput )
{
    DWORD dwBytesWritten = 0;
    if( !WriteFile( g_hStdOut, szOutput, _tcslen(szOutput), &dwBytesWritten, NULL ) )
        return;
}

void ExpandBuffer( Buffer buf, DWORD cchReqSize = 0 )
{
    if( cchReqSize == 0 )
        cchReqSize = (g_cchBufSize[buf] + 1000) * 2;
    
    if( cchReqSize < g_cchBufSize[buf] )
        return;

    if( g_pszBuffer[buf] != NULL )
    {
        if( !HeapFree( GetProcessHeap(), NULL, g_pszBuffer[buf] ) )
        {
            g_pszBuffer[buf] = NULL;
            g_cchBufSize[buf] = 0;
            return;
        }
    }

    g_pszBuffer[buf] = (LPTSTR) HeapAlloc( GetProcessHeap(), NULL, cchReqSize * sizeof(TCHAR) );
    if( g_pszBuffer[buf] == NULL )
    {
        g_cchBufSize[buf] = 0;
        return;
    }

    g_cchBufSize[buf] = cchReqSize;
}

LPCTSTR Format( Buffer buf, LPCTSTR szFormat, ... )
{
    va_list arglist;
    va_start(arglist, szFormat);

    while( _vsntprintf_s( g_pszBuffer[buf], g_cchBufSize[buf], g_cchBufSize[buf] - 1, szFormat, arglist ) == -1 )
        ExpandBuffer(buf);

    va_end(arglist);

    return g_pszBuffer[buf];
}

LPCTSTR GetLastErrorMessage(Buffer buf)
{
    LPVOID pvMsgBuf = NULL;

    FormatMessage( 
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM | 
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
        (LPTSTR) &pvMsgBuf,
        0,
        NULL 
    );

    DWORD d = _tcslen( (LPCTSTR) pvMsgBuf );
    ExpandBuffer( buf, d + 1 );
    _tcscpy_s( g_pszBuffer[buf], g_cchBufSize[buf], (LPCTSTR) pvMsgBuf );

    LocalFree( pvMsgBuf );

    return g_pszBuffer[buf];
}

void LogPrint( LPCTSTR szOutput )
{
    if( g_hLogFile == INVALID_HANDLE_VALUE )
        return;

    if( SetFilePointer( g_hLogFile, 0, NULL, FILE_END ) == -1 )
        return;

    DWORD dwBytesWritten = 0;
    if( !WriteFile( g_hLogFile, szOutput, _tcslen(szOutput), &dwBytesWritten, NULL ) )
        return;
}

struct ListenerParameters
{
    HANDLE hReadPipe;
    LPBYTE pBuffer;
    HANDLE hWritePipe;
    BOOL   fConsoleOn;
    BOOL   fLogFileOn;
    Buffer bufFormat;
    Buffer bufLastError;
};

DWORD WINAPI ListenerThread(LPVOID lpParameter)
{
    DWORD dwBytesRead;
    DWORD dwBytesWritten;
    DWORD dwRetCode = ERROR_SUCCESS;
    ListenerParameters* pParameters = (ListenerParameters*) lpParameter;

    //
    // Look for output from the child and pass it on to the console and
    // the log file.
    //
    while( ReadFile(pParameters->hReadPipe, pParameters->pBuffer, c_PipeBufferSize, &dwBytesRead, NULL) )
    {
        if( pParameters->fConsoleOn )
        {
            if( !WriteFile(pParameters->hWritePipe, pParameters->pBuffer, dwBytesRead, &dwBytesWritten, NULL) )
            {
                ConPrint(Format(pParameters->bufFormat, _T("Error writing to console:\n%s\n"),
                    GetLastErrorMessage(pParameters->bufLastError)));
                dwRetCode = GetLastError();
            }
        }

        if( g_hLogFile != INVALID_HANDLE_VALUE &&
            pParameters->fLogFileOn )
        {
            if( WaitForSingleObject(g_hLogFileMutex, INFINITE) != WAIT_OBJECT_0 )
                break;

            if( SetFilePointer( g_hLogFile, 0, NULL, FILE_END ) == -1 )
            {
                ReleaseMutex(g_hLogFileMutex);

                ConPrint(Format(pParameters->bufFormat, _T("Error seeking to end of log file:\n%s\n"),
                    GetLastErrorMessage(pParameters->bufLastError)));
                dwRetCode = GetLastError();
            }

            if( !WriteFile(g_hLogFile, pParameters->pBuffer, dwBytesRead, &dwBytesWritten, NULL) )
            {
                ConPrint(Format(pParameters->bufFormat, _T("Error writing to log file:\n%s\n"),
                    GetLastErrorMessage(pParameters->bufLastError)));
                dwRetCode = GetLastError();
            }

            ReleaseMutex(g_hLogFileMutex);
        }
    }

    return dwRetCode;
}

extern "C" int __cdecl _tmain(int argc, _TCHAR* argv[])
{
    DWORD   dwRetCode = ERROR_INVALID_FUNCTION;
    LPTSTR  pszChildCmdLine = NULL;
    LPTSTR  pszArgs = NULL;
    LPCTSTR pszCmdPrefix = _T("cmd.exe /x/c ");
    DWORD   cchChildCmdLine = 0;
    DWORD   dwChildCmdLineStartOffset = 0;

    STARTUPINFO ChildStartupInfo;

    TCHAR szMaxPathBuffer[MAX_PATH];
    DWORD cchReqBufSize;

    SECURITY_ATTRIBUTES saAttr; 

    HANDLE hStdOutReadPipe = INVALID_HANDLE_VALUE;
    HANDLE hStdOutReadPipeDup = INVALID_HANDLE_VALUE;
    HANDLE hStdOutWritePipe = INVALID_HANDLE_VALUE;

    HANDLE hStdErrReadPipe = INVALID_HANDLE_VALUE;
    HANDLE hStdErrReadPipeDup = INVALID_HANDLE_VALUE;
    HANDLE hStdErrWritePipe = INVALID_HANDLE_VALUE;

    HANDLE hStdOutListenerThread = INVALID_HANDLE_VALUE;
    HANDLE hStdErrListenerThread = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION ChildProcessInfo;
    ZeroMemory(&ChildProcessInfo, sizeof(ChildProcessInfo));
    ChildProcessInfo.hProcess = INVALID_HANDLE_VALUE;
    ChildProcessInfo.hThread = INVALID_HANDLE_VALUE;

    BYTE  StdOutPipeBuffer[c_PipeBufferSize];
    BYTE  StdErrPipeBuffer[c_PipeBufferSize];

    bool  fConsoleOutputOn = true;
    bool  fLogFileOutputOn = true;
    bool  fConsoleErrorOn = true;
    bool  fLogFileErrorOn = true;

    TCHAR   szPidFile[MAX_PATH] = {0};
    HANDLE  hPidFile = INVALID_HANDLE_VALUE;

    //
    // Save off our inherited std handles
    //
    g_hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hStdErr = GetStdHandle(STD_ERROR_HANDLE);

    //
    // Give our string buffers an initial allocation
    //
    ExpandBuffer(bufFormatMain);
    ExpandBuffer(bufFormatStdOut);
    ExpandBuffer(bufFormatStdErr);
    ExpandBuffer(bufLastErrorMain);
    ExpandBuffer(bufLastErrorStdOut);
    ExpandBuffer(bufLastErrorStdErr);

    //
    // Look for switches directed at us
    //
    dwChildCmdLineStartOffset = argc;
    for(int i = 1; i < argc; i++ )
    {
        LPCTSTR pszSwitch = argv[i];

        if( 0 == _tcsicmp(pszSwitch, _T("-nc")) )
        {
            fConsoleOutputOn = false;
            fConsoleErrorOn = false;
        }
        else if( 0 == _tcsicmp(pszSwitch, _T("-nl")) )
        {
            fLogFileOutputOn = false;
            fLogFileErrorOn = false;
        }
        else if( 0 == _tcsicmp(pszSwitch, _T("-nco")) )
            fConsoleOutputOn = false;
        else if( 0 == _tcsicmp(pszSwitch, _T("-nlo")) )
            fLogFileOutputOn = false;                  
        else if( 0 == _tcsicmp(pszSwitch, _T("-nce")) )
            fConsoleErrorOn = false;
        else if( 0 == _tcsicmp(pszSwitch, _T("-nle")) )
            fLogFileErrorOn = false;
        else if( 0 == _tcsicmp(pszSwitch, _T("-pid")) )
        {
            if (i + 1 == argc)
            {
                ConPrint(_T("Error: -pid switch specified without associated file name\n"));
                dwRetCode = GetLastError();
                goto Error;
            }

            _tcscpy_s(szPidFile, _countof(szPidFile), argv[++i]);
        }
        else
        {
            dwChildCmdLineStartOffset = i;
            break;
        }
    }

    //
    // Output the current process' PID to the specified file
    //
    if (_tcslen(szPidFile) > 0)
    {
        DWORD dwPid = GetCurrentProcessId();
        if( (hPidFile = CreateFile(szPidFile, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL ) ) == INVALID_HANDLE_VALUE )
        {
            ConPrint(Format(bufFormatMain, _T("Error creating PID file '%s':\n%s\n"),
                szMaxPathBuffer, GetLastErrorMessage(bufLastErrorMain)));
            dwRetCode = GetLastError();
            goto Error;
        }

        DWORD dwBytesWritten;
        TCHAR szPidString[64] = {0};
        if (-1 == _stprintf_s(szPidString, _countof(szPidString), _T("%d"), dwPid))
        {
            ConPrint(Format(bufFormatMain, _T("Error formatting PID string:\n%d\n"), dwPid));
            goto Error;
        }

        if( !WriteFile(hPidFile, szPidString, _tcslen(szPidString) * sizeof(TCHAR), &dwBytesWritten, NULL) )
        {
            ConPrint(Format(bufFormatMain, _T("Error writing to PID file:\n%s\n"),
                GetLastErrorMessage(bufLastErrorMain)));
            dwRetCode = GetLastError();
            goto Error;
        }

        CloseHandle(hPidFile);
        hPidFile = INVALID_HANDLE_VALUE;
    }

    //
    // Figure out where log file output is going
    //
    cchReqBufSize = GetEnvironmentVariable(_T("LOGFILE"), szMaxPathBuffer, MAX_PATH);
    if( cchReqBufSize > 0 && cchReqBufSize <= MAX_PATH )
    {
        if( (g_hLogFile = CreateFile(szMaxPathBuffer, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, NULL ) ) == INVALID_HANDLE_VALUE )
        {
            ConPrint(Format(bufFormatMain, _T("Error opening log file '%s':\n%s\n"),
                szMaxPathBuffer, GetLastErrorMessage(bufLastErrorMain)));
            dwRetCode = GetLastError();
            goto Error;
        }
    }

    //
    // Set the bInheritHandle flag so pipe handles are inherited. 
    //
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
    saAttr.bInheritHandle = TRUE; 
    saAttr.lpSecurityDescriptor = NULL; 

    //
    // Create the output pipe for the child process
    //
    if( !CreatePipe(&hStdOutReadPipe, &hStdOutWritePipe, &saAttr, 0) )
    {
        ConPrint(Format(bufFormatMain, _T("Error creating output pipe for child process:\n%s\n"),
            GetLastErrorMessage(bufLastErrorMain)));
        dwRetCode = GetLastError();
        goto Error;
    }

    if( !DuplicateHandle(GetCurrentProcess(), hStdOutReadPipe,
        GetCurrentProcess(), &hStdOutReadPipeDup, 0,
        FALSE,
        DUPLICATE_SAME_ACCESS) )
    {
        ConPrint(Format(bufFormatMain, _T("Error creating output pipe for child process:\n%s\n"),
            GetLastErrorMessage(bufLastErrorMain)));
        dwRetCode = GetLastError();
        goto Error;
    }

    CloseHandle(hStdOutReadPipe);
    hStdOutReadPipe = INVALID_HANDLE_VALUE;

    //
    // Create the error pipe for the child process
    //
    if( !CreatePipe(&hStdErrReadPipe, &hStdErrWritePipe, &saAttr, 0) )
    {
        ConPrint(Format(bufFormatMain, _T("Error creating output pipe for child process:\n%s\n"),
            GetLastErrorMessage(bufLastErrorMain)));
        dwRetCode = GetLastError();
        goto Error;
    }

    if( !DuplicateHandle(GetCurrentProcess(), hStdErrReadPipe,
        GetCurrentProcess(), &hStdErrReadPipeDup, 0,
        FALSE,
        DUPLICATE_SAME_ACCESS) )
    {
        ConPrint(Format(bufFormatMain, _T("Error creating output pipe for child process:\n%s\n"),
            GetLastErrorMessage(bufLastErrorMain)));
        dwRetCode = GetLastError();
        goto Error;
    }

    CloseHandle(hStdErrReadPipe);
    hStdErrReadPipe = INVALID_HANDLE_VALUE;

    //
    // Set up the startup info
    //
    ZeroMemory( &ChildStartupInfo, sizeof(ChildStartupInfo) );
    ChildStartupInfo.cb = sizeof(ChildStartupInfo);

    //
    // Redirect the output handle
    //
    SetStdHandle(STD_OUTPUT_HANDLE, hStdOutWritePipe);
    SetStdHandle(STD_ERROR_HANDLE, hStdErrWritePipe);

    //
    // Allocate space for the non-constant command line
    //
    cchChildCmdLine = _tcslen(pszCmdPrefix) + 1;
    for( int i = dwChildCmdLineStartOffset; i < argc; i++ )
    {
        // Allocate enough for the argument, two quotes and a space
        cchChildCmdLine += _tcslen(argv[i]) + 3;
    }

    pszChildCmdLine = (LPTSTR) HeapAlloc(GetProcessHeap(), NULL, cchChildCmdLine * sizeof(TCHAR));
    if( pszChildCmdLine == NULL )
    {
        ConPrint(_T("Memory allocation error.\n"));
        dwRetCode = GetLastError();
        goto Error;
    }

    pszArgs = (LPTSTR) HeapAlloc(GetProcessHeap(), NULL, cchChildCmdLine * sizeof(TCHAR));
    pszArgs[0] = _T('\0');
    if( pszArgs == NULL )
    {
        ConPrint(_T("Memory allocation error.\n"));
        dwRetCode = GetLastError();
        goto Error;
    }

    // Copy arguments into a single string
    for( int i = dwChildCmdLineStartOffset; i < argc; i++ )
    {
        if (_tcsstr(argv[i], _T(" ")) != NULL)
            _tcscat_s(pszArgs, cchChildCmdLine, _T("\""));

        _tcscat_s(pszArgs, cchChildCmdLine, argv[i]);

        if (_tcsstr(argv[i], _T(" ")) != NULL)
            _tcscat_s(pszArgs, cchChildCmdLine, _T("\""));

        _tcscat_s(pszArgs, cchChildCmdLine, _T(" "));
    }

    //
    // Output the script name & command to the log file
    //
    cchReqBufSize = GetEnvironmentVariable(_T("SCRIPT_NAME"), szMaxPathBuffer, MAX_PATH);
    if( cchReqBufSize == 0 || cchReqBufSize >= MAX_PATH )
        _tcscpy_s( szMaxPathBuffer, _countof(szMaxPathBuffer), _T("SCRIPT_NAME not set") );

    LogPrint(Format(bufFormatMain, _T("[%s] %s\r\n"), szMaxPathBuffer, pszArgs ) );

    //
    // Try to create the process plainly (without using CMD.EXE)
    //
    _tcscpy_s(pszChildCmdLine, cchChildCmdLine, pszArgs);
    if( !CreateProcess( NULL, pszChildCmdLine,
        NULL, NULL, TRUE, 0, NULL, NULL,
        &ChildStartupInfo, &ChildProcessInfo ) )
    {
        //
        // Didn't work. Could be a cmd.exe command... Try that.
        //
        _tcscpy_s(pszChildCmdLine, cchChildCmdLine, pszCmdPrefix);
        _tcscat_s(pszChildCmdLine, cchChildCmdLine, pszArgs);
        if( !CreateProcess( NULL, pszChildCmdLine,
            NULL, NULL, TRUE, 0, NULL, NULL,
            &ChildStartupInfo, &ChildProcessInfo ) )
        {
            ConPrint(Format(bufFormatMain, _T("Error executing command line '%s':\n%s\n"),
                pszArgs, GetLastErrorMessage(bufLastErrorMain)));
            dwRetCode = GetLastError();
            goto Error;
        }
    }

    //
    // Close our copy of the pipe so that ReadFile returns when no one
    // owns the handle anymore.
    //
    CloseHandle(hStdOutWritePipe);
    hStdOutWritePipe = INVALID_HANDLE_VALUE;

    CloseHandle(hStdErrWritePipe);
    hStdErrWritePipe = INVALID_HANDLE_VALUE;

    g_hLogFileMutex = CreateMutex(NULL, FALSE, NULL);

    ListenerParameters StdOutParameters;
    StdOutParameters.bufFormat = bufFormatStdOut;
    StdOutParameters.bufLastError = bufLastErrorStdOut;
    StdOutParameters.fConsoleOn = fConsoleOutputOn;
    StdOutParameters.fLogFileOn = fLogFileOutputOn;
    StdOutParameters.hReadPipe = hStdOutReadPipeDup;
    StdOutParameters.hWritePipe = g_hStdOut;
    StdOutParameters.pBuffer = StdOutPipeBuffer;
    hStdOutListenerThread = CreateThread(NULL, 0, ListenerThread, &StdOutParameters, NULL, NULL);

    ListenerParameters StdErrParameters;
    StdErrParameters.bufFormat = bufFormatStdErr;
    StdErrParameters.bufLastError = bufLastErrorStdErr;
    StdErrParameters.fConsoleOn = fConsoleErrorOn;
    StdErrParameters.fLogFileOn = fLogFileErrorOn;
    StdErrParameters.hReadPipe = hStdErrReadPipeDup;
    StdErrParameters.hWritePipe = g_hStdErr;
    StdErrParameters.pBuffer = StdErrPipeBuffer;
    hStdErrListenerThread = CreateThread(NULL, 0, ListenerThread, &StdErrParameters, NULL, NULL);

    HANDLE rgThreadHandles[2] = { hStdOutListenerThread, hStdErrListenerThread };

    //
    // Wait for the listener threads to end.
    //
    WaitForMultipleObjects(2, rgThreadHandles, TRUE, INFINITE);

    for( int i = 0; i < 2; i++ )
    {
        if( !GetExitCodeThread( rgThreadHandles[i], &dwRetCode ) )
        {
            ConPrint(Format(bufFormatMain, _T("Error retrieving listener thread exit code.\n")));
            goto Error;
        }

        if( dwRetCode != ERROR_SUCCESS )
        {
            LPVOID pvMsgBuf = NULL;
            FormatMessage( 
                FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                FORMAT_MESSAGE_FROM_SYSTEM | 
                FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL,
                GetLastError(),
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), // Default language
                (LPTSTR) &pvMsgBuf,
                0,
                NULL 
            );

            ConPrint(Format(bufFormatMain, _T("Listener thread returned an error:\n%s\n"), pvMsgBuf));
            LocalFree( pvMsgBuf );

            goto Error;
        }
    }

    //
    // Make sure child process is dead
    //
    WaitForSingleObject(ChildProcessInfo.hProcess, INFINITE);

    //
    // Grab the exit code from the child process and return that as
    // our exit code as well.
    //
    if( !GetExitCodeProcess(ChildProcessInfo.hProcess, &dwRetCode) )
    {
        ConPrint(Format(bufFormatMain, _T("Error retrieving child process' exit code:\n%s\n"),
            GetLastErrorMessage(bufLastErrorMain)));
        dwRetCode = GetLastError();
        goto Error;
    }

Error:

    if( dwRetCode != ERROR_SUCCESS )
    {
        //
        // Print a nice, ugly, grepable error signature
        //
        LogPrint(Format(bufFormatMain, _T("******ERROR executing %s\r\n"), pszArgs));
    }

    for( int i = 0; i < bufCount; i++ )
    {
        if( g_pszBuffer[i] != NULL )
            HeapFree( GetProcessHeap(), NULL, g_pszBuffer[i] );
    }

    if( pszChildCmdLine != NULL )
        HeapFree( GetProcessHeap(), NULL, pszChildCmdLine );

    if( pszArgs != NULL )
        HeapFree( GetProcessHeap(), NULL, pszArgs );

    if( hStdOutReadPipe != INVALID_HANDLE_VALUE )
        CloseHandle( hStdOutReadPipe );

    if( hStdOutReadPipeDup != INVALID_HANDLE_VALUE )
        CloseHandle( hStdOutReadPipeDup );

    if( hStdOutWritePipe != INVALID_HANDLE_VALUE )
        CloseHandle( hStdErrWritePipe );

    if( hStdErrReadPipe != INVALID_HANDLE_VALUE )
        CloseHandle( hStdErrReadPipe );

    if( hStdErrReadPipeDup != INVALID_HANDLE_VALUE )
        CloseHandle( hStdErrReadPipeDup );

    if( hStdErrWritePipe != INVALID_HANDLE_VALUE )
        CloseHandle( hStdErrWritePipe );

    if( hStdOutListenerThread != INVALID_HANDLE_VALUE )
        CloseHandle(hStdOutListenerThread);

    if( hStdErrListenerThread != INVALID_HANDLE_VALUE )
        CloseHandle(hStdErrListenerThread);

    if( g_hLogFileMutex != INVALID_HANDLE_VALUE )
        CloseHandle(g_hLogFileMutex);

    if( g_hLogFile != INVALID_HANDLE_VALUE )
        CloseHandle( g_hLogFile );

    if( ChildProcessInfo.hThread != INVALID_HANDLE_VALUE )
        CloseHandle(ChildProcessInfo.hThread);

    if( ChildProcessInfo.hProcess != INVALID_HANDLE_VALUE )
        CloseHandle(ChildProcessInfo.hProcess);

    if( hPidFile != INVALID_HANDLE_VALUE )
        CloseHandle(hPidFile);

	return dwRetCode;
}
