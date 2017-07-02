#include <windows.h>
#include <pdh.h>

#include "resource.h"
#include "SystemTraySDK.h"
#include "ServiceBase.h"


#pragma warning (disable : 4996)

#define	WM_ICON_NOTIFY WM_APP+10
#define MY_WND_CLS "shywndcls"

#define WAIT_QUITE_SAMPLES 5 // 30


struct SharedData
{
    // data filled by the user tray app and read by the service
    unsigned int runCounter;  // debug signal the ui is alive
    int hadUserInput;  // 0/1 was there user input?
    int samplingDisabled; // 0/1
    int userAction;  // 0 - none,  1 - start now,  2 - stop now
    
    // filled in the service, read by the app
    int processRunning; // 0/1
};


#define SHARED_BUF_SIZE 256
#define SHARED_MEM_NAME "Global\\IdleRunnerShared"

HANDLE g_hMapFile = NULL;
SharedData* g_sharedData = NULL;



class MySystemTray : public CSystemTray
{
protected:
    virtual void CustomizeMenu(HMENU hSubMenu) {
        if (g_sharedData->samplingDisabled)
        {
            ModifyMenu(hSubMenu, IDM_TOGGLEWATCHING, MF_BYCOMMAND | MF_STRING, IDM_TOGGLEWATCHING, "Start Watching");
        }
        else
        {
            ModifyMenu(hSubMenu, IDM_TOGGLEWATCHING, MF_BYCOMMAND | MF_STRING, IDM_TOGGLEWATCHING, "Stop Watching");
        }
    }
};


bool g_isService = false;
MySystemTray g_trayIcon;
static PDH_HQUERY cpuQuery = 0;
static PDH_HCOUNTER cpuTotal = 0;
double maxCpu = 0;
HWND hWnd = 0;

// config
int idleTimeMSec = 60 * 15;
char* runProcess = nullptr;
FILE* logFile = nullptr;

#define LOG(...) do { if (logFile != nullptr) { fprintf(logFile, __VA_ARGS__); fprintf(logFile, "\n"); fflush(logFile); } } while(0)

void getThisPath(char path[MAX_PATH])
{
    GetModuleFileName(NULL, path, MAX_PATH);
    int exeLen = strlen(path);
    int dirEnd = 0;
    for(dirEnd = exeLen - 1; dirEnd >= 0; --dirEnd) 
        if (path[dirEnd] == '\\') 
            break;
    path[dirEnd] = 0;
}

void startLog(const char* name)
{
    char path[MAX_PATH] = {0};
    getThisPath(path);

    strcat(path, "\\");
    strcat(path, name);
    if (logFile != nullptr)
        fclose(logFile);
    logFile = fopen(path, "w");
}

void readConfig()
{
    // get where we are
    char path[MAX_PATH] = {0};
    getThisPath(path);
    strcat(path, "\\config.txt");

    FILE* f = fopen(path, "r");
    if (f == NULL) {
        if (!g_isService)
            MessageBoxA(NULL, "did not find config file", "Error", MB_OK);
        return;
    }
    fseek(f, 0, SEEK_END);
    int sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char*)malloc(sz + 1); 
    sz = fread(buf, 1, sz, f);
    fclose(f);
    buf[sz] = 0;

    // parse
    char *lineStart = buf, *bufend = buf + sz;
    char *key = nullptr, *value = nullptr;
    for(char* p = buf; p < bufend; ++p)
    {
        if (*p == ':') {
            *p = 0;
            for(char *wp = p - 1; (*wp == ' ' || *wp == '\t') && wp > lineStart; --wp)
                *wp = 0; // trim spaces
            key = lineStart;
            for(value = p + 1; *value == ' ' && value < bufend; ++value)
                ; // trim
        }
        else if (*p == '\n' || p == bufend - 1) {
            if (*p == '\n')
                *p = 0;
            for(char *wp = p - 1; (*wp == ' ' || *wp == '\t') && wp > lineStart; --wp)
                *wp = 0; // trim spaces
            lineStart = p + 1;

            if (strcmp(key, "idleSec") == 0) {
                idleTimeMSec = atoi(value) * 1000;
                LOG("CFG idleSec=%d", idleTimeMSec);
            }
            else if (strcmp(key, "run") == 0) {
                if (runProcess != nullptr)
                    free(runProcess);
                int vsz = strlen(value);
                runProcess = (char*)malloc(vsz + 1);
                strcpy(runProcess, value);
                runProcess[vsz] = 0;
                LOG("CFG proc=%s", runProcess);
            }
        }
    }

    free(buf);
}




// from service
bool createSharedMem()
{
   g_hMapFile = CreateFileMapping(
                 INVALID_HANDLE_VALUE,    // use paging file
                 NULL,                    // default security
                 PAGE_READWRITE,          // read/write access
                 0,                       // maximum object size (high-order DWORD)
                 SHARED_BUF_SIZE,                // maximum object size (low-order DWORD)
                 SHARED_MEM_NAME);        // name of mapping object

   if (g_hMapFile == NULL) {
      LOG("Could not create file mapping object (%d).", GetLastError());
      return false;
   }
   LPVOID ptr = (LPTSTR) MapViewOfFile(g_hMapFile,   // handle to map object
                        FILE_MAP_ALL_ACCESS, // read/write permission
                        0,
                        0,
                        SHARED_BUF_SIZE);

   if (ptr == NULL)
   {
      LOG("Could not map view of file (%d).", GetLastError());
      CloseHandle(g_hMapFile);
      g_hMapFile = NULL;
      return false;
   }
   LOG("Created shared memory");

   g_sharedData = (SharedData*)ptr;
   memset(ptr, 0, SHARED_BUF_SIZE);
   return true;
}

// from app

bool openSharedMem()
{
   g_hMapFile = OpenFileMapping(
                   FILE_MAP_ALL_ACCESS,   // read/write access
                   FALSE,                 // do not inherit the name
                   SHARED_MEM_NAME);               // name of mapping object

   if (g_hMapFile == NULL)
   {
      LOG("Could not open file mapping object (%d).", GetLastError());
      return false;
   }

   LPVOID ptr = MapViewOfFile(g_hMapFile, // handle to map object
               FILE_MAP_ALL_ACCESS,  // read/write permission
               0,
               0,
               SHARED_BUF_SIZE);

   if (ptr == NULL)
   {
        LOG("Could not map view of file (%d).\n", GetLastError());
        CloseHandle(g_hMapFile);
        g_hMapFile = NULL;
        return false;
   }
   LOG("Opened shared memory");

   g_sharedData = (SharedData*)ptr;
   return true;
}


void closeSharedMem()
{
    UnmapViewOfFile(g_sharedData);
    CloseHandle(g_hMapFile);
}




void stopService();


void uiStopWatching()
{
    LOG("stopping watching");
    g_trayIcon.SetIcon(IDI_XEYES);
    KillTimer(hWnd, 1);
    g_sharedData->samplingDisabled = true;
}
void uiStartWatching()
{
    LOG("starting watching");
    g_trayIcon.SetIcon(IDI_GLASSES);
    SetTimer(hWnd, 1, 1000, NULL);
    g_sharedData->samplingDisabled = false;
}



void uiStopProcess()
{
    
}


void uiSampleFunc()
{
    if (g_sharedData->processRunning)
        return;

    ++g_sharedData->runCounter;

    LASTINPUTINFO lii;
    lii.cbSize = sizeof(lii);
    if (GetLastInputInfo(&lii) == 0) {
        LOG("Failed GetLastInputInfo");
        return;
    }

    auto now = GetTickCount();
    if ((int)(now - lii.dwTime) < idleTimeMSec) {
        g_sharedData->hadUserInput = 1;
        LOG("No: Input %d", (int)(now - lii.dwTime));
        return;
    }
    else {
        g_sharedData->hadUserInput = 1;
    }
}



void svcStartProcess()
{
    g_sharedData->processRunning = 1;

    STARTUPINFO si = { sizeof(STARTUPINFO) };
    PROCESS_INFORMATION pi { 0 };
    if (CreateProcessA(NULL, runProcess, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi) == 0) {
        LOG("Failed creating process %d", GetLastError());
    }
    else
    {
        LOG("Created process %d waiting...", pi.dwProcessId);
        CloseHandle(pi.hThread);
        if (WaitForSingleObject(pi.hProcess, INFINITE) == WAIT_FAILED) {
            LOG("Failed waiting for process");
        }
    }

    g_sharedData->processRunning = 0;
}


int g_quiteSamples = 0; // count how many quite samples we've seen before starting the process

void svcSampleFunc()
{
    if (g_sharedData->samplingDisabled)
        return;

    if (g_sharedData->hadUserInput) {
        LOG("No  (%d): Input", g_sharedData->runCounter);
        g_quiteSamples = 0;
        return;
    }

    double cpuval = -1;
    if (cpuQuery != 0 && cpuTotal != 0)
    {
        if (PdhCollectQueryData(cpuQuery) == ERROR_SUCCESS)
        {
            Sleep(100); // sample interval - don't want to sample between long intervals
            if (PdhCollectQueryData(cpuQuery) == ERROR_SUCCESS)
            {
                PDH_FMT_COUNTERVALUE counterVal;
                if (PdhGetFormattedCounterValue(cpuTotal, PDH_FMT_DOUBLE, NULL, &counterVal) == ERROR_SUCCESS)
                {
                    cpuval = counterVal.doubleValue;
                    if (counterVal.doubleValue >= maxCpu) {
                        LOG("No  (%d): CPU %lf", g_sharedData->runCounter, counterVal.doubleValue);
                        g_quiteSamples = 0;
                        return;
                    }

                }
            }
        }
    }

    ++g_quiteSamples;
    LOG("Yes (%d, %d): Input %d CPU %lf", g_sharedData->runCounter, g_quiteSamples, g_sharedData->hadUserInput, cpuval);

    if (g_quiteSamples > WAIT_QUITE_SAMPLES) {
        g_quiteSamples = 0;
        svcStartProcess();
    }

}


DWORD WINAPI serviceSampleThread(LPVOID lpParameter)
{
    LOG("Sampling Thread");
    while(true)
    {
        Sleep(1000);
        svcSampleFunc();
    }
}

void svcStartWatching()
{
    LOG("Starting sampling");
    CreateThread(NULL, 0, serviceSampleThread, 0, 0, NULL);
}




void initSampling()
{
    auto r = PdhOpenQuery(NULL, NULL, &cpuQuery);
    if (r != ERROR_SUCCESS)
    {
        LOG("failed PdhOpenQuery %d", r);
        return;
    }
    r = PdhAddEnglishCounterW(cpuQuery, L"\\Processor Information(_Total)\\% Processor Time", NULL, &cpuTotal);
    if (r != ERROR_SUCCESS)  
    {
        LOG("Failed PdhAddEnglishCounterW %d", r);
        return;
    }

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    maxCpu = 100.0 / (double)(sysInfo.dwNumberOfProcessors + 1); // slightly less than 100% of a single processor
    LOG("Cpu Check max=%lf", maxCpu);
        

}




// Mesage handler for about box.
LRESULT CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_INITDIALOG:
				return TRUE;

		case WM_COMMAND:
			if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) 
			{
				EndDialog(hDlg, LOWORD(wParam));
				return TRUE;
			}
			break;
	}
    return FALSE;
}


LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	int wmId, wmEvent;

	switch (message) 
	{
        case WM_ICON_NOTIFY:
            return g_trayIcon.OnTrayNotification(wParam, lParam);

		case WM_COMMAND:
			wmId    = LOWORD(wParam); 
			wmEvent = HIWORD(wParam); 
			// Parse the menu selections:
			switch (wmId)
			{
            case IDM_TOGGLEWATCHING: 
                if (g_sharedData->samplingDisabled) 
                    uiStartWatching();
                else
                    uiStopWatching();
                break;
			case IDM_EXIT:
                stopService();
				DestroyWindow(hWnd);
				break;
			default:
				return DefWindowProc(hWnd, message, wParam, lParam);
			}
			break;
        case WM_TIMER:
            uiSampleFunc();
            break;
		case WM_DESTROY:
			PostQuitMessage(0);
			break;
		default:
			return DefWindowProc(hWnd, message, wParam, lParam);
   }
   return 0;
}

ATOM MyRegisterClass(HINSTANCE hInstance)
{
	WNDCLASSEXA wcex;

	wcex.cbSize = sizeof(WNDCLASSEX); 

	wcex.style			= CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc	= (WNDPROC)WndProc;
	wcex.cbClsExtra		= 0;
	wcex.cbWndExtra		= 0;
	wcex.hInstance		= hInstance;
	wcex.hIcon			= LoadIcon(hInstance, (LPCTSTR)IDI_GLASSES);
	wcex.hCursor		= LoadCursor(NULL, IDC_ARROW);
	wcex.hbrBackground	= (HBRUSH)(COLOR_WINDOW+1);
	wcex.lpszMenuName	= (LPCSTR)IDC_TASKBARDEMO;
	wcex.lpszClassName	= MY_WND_CLS;
	wcex.hIconSm		= LoadIcon(wcex.hInstance, (LPCTSTR)IDI_GLASSES);

	return RegisterClassExA(&wcex);
}



class RunnerService : public CServiceBase
{
public:

    RunnerService(PWSTR pszServiceName, BOOL fCanStop = TRUE, BOOL fCanShutdown = TRUE, BOOL fCanPauseContinue = FALSE);
    virtual ~RunnerService(void);

protected:

    virtual void OnStart(DWORD dwArgc, PWSTR *pszArgv);
    virtual void OnStop();

    void ServiceWorkerThread(void);

private:

    BOOL m_fStopping;
    HANDLE m_hStoppedEvent;
};

void serviceCheck(const char* cmdLine);

#define SVC_NAME "IdleRunnerSvc"
#define WIDEN2(x) L ## x
#define WIDEN(x) WIDEN2(x)

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    RunnerService service(WIDEN(SVC_NAME));
    if (CServiceBase::Run(service))
    {
        LOG("Service stopped");
        return 0;
    }
    startLog("log_ui.txt");
    LOG("Starting UI");

    serviceCheck(lpCmdLine);

    if (!createSharedMem())
        return 1;

    readConfig();

	MyRegisterClass(hInstance);
    hWnd = CreateWindowA(MY_WND_CLS, "title", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, NULL, NULL, hInstance, NULL);
    
    if (!hWnd)
    {
        LOG("Failed creating window");
        return 1;
    }
    
    if (!g_trayIcon.Create(hInstance,
                          hWnd,                            // Parent window
                          WM_ICON_NOTIFY,                  // Icon notify message to use
                          "This is a Tray Icon - Right click on me!",  // tooltip
                          ::LoadIcon(hInstance, (LPCTSTR)IDI_GLASSES),
                          IDR_POPUP_MENU)) 
    {
        LOG("Failed TrayIcon create");
        return 1;
    }

    uiStartWatching();


	// Main message loop:
	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) 
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

    //serviceCheck(lpCmdLine);

	return msg.wParam;
}



// ---------------------------------- service ------------------------



RunnerService::RunnerService(PWSTR pszServiceName, BOOL fCanStop, BOOL fCanShutdown, BOOL fCanPauseContinue)
: CServiceBase(pszServiceName, fCanStop, fCanShutdown, fCanPauseContinue)
{
    m_fStopping = FALSE;

    // Create a manual-reset event that is not signaled at first to indicate 
    // the stopped signal of the service.
    m_hStoppedEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (m_hStoppedEvent == NULL)
    {
        throw GetLastError();
    }
}


RunnerService::~RunnerService(void)
{

}



void RunnerService::OnStart(DWORD dwArgc, LPWSTR *lpszArgv)
{
    startLog("log_svc.txt");
    LOG("Started service");
    g_isService = true;

    readConfig();
    initSampling();
    createSharedMem();

    svcStartWatching();
}


void RunnerService::OnStop()
{
    LOG("Stopping service");
}

void stopService()
{
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS );
    if (scm == NULL) {
        LOG("OpenSCManager (stopService) failed %x", GetLastError());
        goto Cleanup;
    }

    SC_HANDLE svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc == NULL) {
        LOG("OpenService faled (stopService) %x", GetLastError());
        goto Cleanup;
    }

    SERVICE_STATUS ssp;
    if ( !ControlService(svc, SERVICE_CONTROL_STOP, &ssp))
    {
        LOG("ControlService (stopService) failed %x, %d", GetLastError(), ssp.dwCurrentState );
    }
    else
        LOG("Service stopped!");

Cleanup:
    if (scm)
        CloseServiceHandle(scm);
    if (svc)
        CloseServiceHandle(svc);
}


void serviceCheck(const char* cmdLine)
{
        // Open the local default service control manager database
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS );
    if (scm == NULL)
    {
        LOG("OpenSCManager (serviceCheck) failed %x", GetLastError());
        goto Cleanup;
    }

    char szPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, szPath, ARRAYSIZE(szPath)) == 0)
    {
        LOG("GetModuleFileName failed");
        goto Cleanup;
    }

    SC_HANDLE svc = OpenServiceA(scm, SVC_NAME, SERVICE_ALL_ACCESS);
    if (svc == NULL)
    {
        LOG("No service exists, installing...");

        svc = CreateServiceA(
            scm,                   // SCManager database
            SVC_NAME,                 // Name of service
            "Idle Runner Svc",                 // Name to display
            SERVICE_ALL_ACCESS,           // Desired access
            SERVICE_WIN32_OWN_PROCESS,      // Service type
            SERVICE_AUTO_START,                    // Service start type
            SERVICE_ERROR_NORMAL,           // Error control type
            szPath,                         // Service's binary
            NULL,                           // No load ordering group
            NULL,                           // No tag identifier
            NULL,                // Dependencies
            NULL,                     // Service running account
            NULL                     // Password of the account
            );

        if (svc == NULL) {
            LOG("Failed CreateService %x", GetLastError());
            goto Cleanup;
        }

        LOG("Craeted the service " SVC_NAME);
    }
    else
    {
        LOG("Service exists " SVC_NAME);
        // make sure it points to this exe

        if (ChangeServiceConfig(svc, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE , szPath, NULL, NULL, NULL, NULL, NULL, NULL) == 0) {
            LOG("Failed ChangeServiceConfig %d", GetLastError());
        }

    }

    if (StartServiceW(svc, 0, NULL) == 0) {
        LOG("Failed StartService %x", GetLastError());
        goto Cleanup;
    }

    LOG("Started service");

Cleanup:
    // Centralized cleanup for all allocated resources.
    if (scm)
        CloseServiceHandle(scm);
    if (svc)
        CloseServiceHandle(svc);
}

