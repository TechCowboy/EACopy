// (c) Electronic Arts. All Rights Reserved.

#include "EACopyServer.h"
#include <tchar.h>
#include <windows.h>

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// If port is changed from default, name will change to include port. (Like EACopyService1339 if port is set to 1339)
#if defined(_DEBUG)
#define DEFAULTSERVICENAME        L"EACopyServiceDEBUG"
#else
#define DEFAULTSERVICENAME        L"EACopyService"
#endif
#define DEFAULTSERVICEDISPLAYNAME L"EACopy Accelerator Service"

wchar_t g_serviceName[1024];
wchar_t g_serviceDisplayName[1024];

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// internal variables
SERVICE_STATUS          g_ssStatus;       // current status of the service
SERVICE_STATUS_HANDLE   g_sshStatusHandle;
DWORD                   g_dwErr;
bool                    g_runningAsService;
bool					g_serviceCrashed;		// This is set when we're exiting 'uncleanly'. I.e exiting cleanly but due
												// to the service being compromised due to SEH
int						g_argc;
wchar_t**				g_argv;
Server					g_server;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void printHelp()
{
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef(L"  EACopyService v%hs - Copy Accelerator. (c) Electronic Arts.  All Rights Reserved.", ServerVersion);
	logInfoLinef(L"-------------------------------------------------------------------------------");
	logInfoLinef();
	logInfoLinef(L"             Usage :: EACopyService [options]");
	logInfoLinef();
	logInfoLinef(L"              /P:n :: Port that server will listen on (defaults to %i).", DefaultPort);
	logInfoLinef(L"         /UNSECURE :: Will not check if client has access to network path using smb.");
	logInfoLinef(L"        /HISTORY:n :: Max number of files tracked in history (defaults to %i).", DefaultHistorySize);
	logInfoLinef();
	logInfoLinef(L"                /J :: Enable unbuffered I/O for all files.");
	logInfoLinef(L"               /NJ :: Disable unbuffered I/O for all files.");
	logInfoLinef();
	logInfoLinef(L"         /LOG:file :: output status to LOG file (overwrite existing log).");
	logInfoLinef(L"          /VERBOSE :: output debug logging.");
	logInfoLinef();
	logInfoLinef(L"          /INSTALL :: Install and start as auto starting windows service.");
	logInfoLinef(L"                      Will start with parameters provided with /INSTALL call");
	logInfoLinef(L"           /REMOVE :: Stop and remove service.");
	logInfoLinef();
	logInfoLinef(L"           /USER:u :: User used when installing service. Defaults to LocalSystem account");
	logInfoLinef(L"           /PASS:p :: Password used when installing service");
	logInfoLinef();
}

bool readSettings(ServerSettings& outSettings, WString& outLogFileName, uint argc, wchar_t** argv)
{
	// Read options
	uint argIndex = 1;
	while (argIndex < argc)
	{
		wchar_t* arg = argv[argIndex++];

		if (startsWithIgnoreCase(arg, L"/P:"))
		{
			outSettings.listenPort = _wtoi(arg + 3);
		}
		else if (equalsIgnoreCase(arg, L"/UNSECURE"))
		{
			outSettings.useSecurityFile = false;
		}
		else if (startsWithIgnoreCase(arg, L"/HISTORY:"))
		{
			outSettings.maxHistory = _wtoi(arg + 9);
		}
		else if (equalsIgnoreCase(arg, L"/J"))
		{
			outSettings.useBufferedIO = UseBufferedIO_Enabled;
		}
		else if (equalsIgnoreCase(arg, L"/NJ"))
		{
			outSettings.useBufferedIO = UseBufferedIO_Disabled;
		}
		else if(startsWithIgnoreCase(arg, L"/LOG:"))
		{
			outLogFileName = arg + 5;
		}
		else if(startsWithIgnoreCase(arg, L"/USER:"))
		{
			outSettings.user = arg + 6;
		}
		else if(startsWithIgnoreCase(arg, L"/PASS:"))
		{
			outSettings.password = arg + 6;
		}
		else if (equalsIgnoreCase(arg, L"/VERBOSE"))
		{
			outSettings.logDebug = true;
		}
		else
		{
			logErrorf(L"Unknown option %ls. Use /? for help", arg);
			return false;
		}
	}
	return true;
}

void initializeServiceName()
{
	wcscpy(g_serviceName, DEFAULTSERVICENAME);
	wcscpy(g_serviceDisplayName, DEFAULTSERVICEDISPLAYNAME);
	for (uint i=1; i!=g_argc; ++i)
	{
		if (!startsWithIgnoreCase(g_argv[i], L"/P:"))
			continue;
		wcscat(g_serviceName, g_argv[i] + 3);
		wcscat(g_serviceDisplayName, L" (port ");
		wcscat(g_serviceDisplayName, g_argv[i] + 3);
		wcscat(g_serviceDisplayName, L")");
		break;
	}
}

void addLastErrorToMessageLog(const wchar_t* lpszMsg)
{
	if (!g_runningAsService)
		return;

	g_dwErr = GetLastError();
	HANDLE hEventSource = RegisterEventSourceW(NULL, g_serviceName);
	if (!hEventSource)
		return;

	wchar_t szMsg[eacopy_sizeof_array(g_serviceName) + 100 ];
	swprintf_s(szMsg,eacopy_sizeof_array(g_serviceName) + 100, L"%ls error: %d", g_serviceName, g_dwErr);
	const wchar_t* lpszStrings[2] = { szMsg, lpszMsg };
	ReportEventW(hEventSource, EVENTLOG_ERROR_TYPE, 0, 0, NULL, 2, 0, &lpszStrings[0], NULL);
	DeregisterEventSource(hEventSource);
}

VOID addInfoToMessageLog(const wchar_t* lpszMsg)
{
	if (!g_runningAsService)
		return;
	HANDLE hEventSource = RegisterEventSourceW(NULL, g_serviceName);
	if (!hEventSource)
		return;
	const wchar_t* lpszStrings[1] = { lpszMsg };
	ReportEventW(hEventSource, EVENTLOG_INFORMATION_TYPE, 0, 0, NULL, 1, 0, &lpszStrings[0], NULL);
	DeregisterEventSource(hEventSource);
}

BOOL reportServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode, DWORD dwWaitHint)
{
	if (!g_runningAsService) // when debugging we don't report to the SCM
		return TRUE;

	if (dwCurrentState == SERVICE_START_PENDING)
		g_ssStatus.dwControlsAccepted = 0;
	else
		g_ssStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PAUSE_CONTINUE;

	g_ssStatus.dwCurrentState = dwCurrentState;
	g_ssStatus.dwWin32ExitCode = dwWin32ExitCode;
	g_ssStatus.dwWaitHint = dwWaitHint;
	
	static DWORD dwCheckPoint = 1;

	if (dwCurrentState == SERVICE_RUNNING || dwCurrentState == SERVICE_STOPPED)
		g_ssStatus.dwCheckPoint = 0;
	else
		g_ssStatus.dwCheckPoint = dwCheckPoint++;


	// Report the status of the service to the service control manager.
	//
	
	BOOL res = SetServiceStatus( g_sshStatusHandle, &g_ssStatus);
	if (res)
		return TRUE;

	wchar_t errMsg[512];
	swprintf_s(errMsg, L"SetServiceStatus failed: '%ls'", getLastErrorText().c_str());

	addLastErrorToMessageLog(errMsg);
	return res;
}

DWORD WINAPI serviceControl(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext)
{
	switch (dwControl)
	{
	case SERVICE_CONTROL_STOP:
		addInfoToMessageLog(L"Stop");
		reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 60000);
		g_server.stop();
		return NO_ERROR;

	case SERVICE_CONTROL_SHUTDOWN:
		addInfoToMessageLog(L"Shutdown");
		reportServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 60000);
		g_server.stop();
		return NO_ERROR;

	case SERVICE_CONTROL_PAUSE:
		addInfoToMessageLog(L"Pause");
		reportServiceStatus(SERVICE_PAUSE_PENDING, NO_ERROR, 0);
		//g_server.pause();
		reportServiceStatus(SERVICE_PAUSED, NO_ERROR, 0);
		return NO_ERROR;

	case SERVICE_CONTROL_CONTINUE:
		addInfoToMessageLog(L"Continue");
		reportServiceStatus(SERVICE_CONTINUE_PENDING, NO_ERROR, 0);
		//g_server.resume();
		reportServiceStatus(SERVICE_RUNNING, NO_ERROR, 0);
		return NO_ERROR;

	case SERVICE_CONTROL_INTERROGATE:
		// Fall through to send current status
		break;

	default:
		// invalid control code
		//
		return ERROR_CALL_NOT_IMPLEMENTED;
	}

	reportServiceStatus(g_ssStatus.dwCurrentState, NO_ERROR, 0);
	return NO_ERROR;
}

void WINAPI serviceMain(DWORD dwArgc, LPWSTR *lpszArgv)
{
	initializeServiceName();

	// register our service control handler:
	//
	g_sshStatusHandle = RegisterServiceCtrlHandlerExW(g_serviceName, serviceControl, NULL);

	if (!g_sshStatusHandle)
	{
		wchar_t msg[256];
		swprintf_s(msg, L"Failed to register control handler! err = %ls", getLastErrorText().c_str());
		addLastErrorToMessageLog(msg);
		return;
	}

	// SERVICE_STATUS members that don't change in example
	//
	g_ssStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ssStatus.dwServiceSpecificExitCode = 0;

	WString logFileName;
	ServerSettings settings;
	Log log;

	// report the status to the service control manager.
	//
	if (!reportServiceStatus(SERVICE_START_PENDING, NO_ERROR, 3000))
		goto cleanup;

	if (!readSettings(settings, logFileName, g_argc, g_argv))
		goto cleanup;

	log.init(logFileName.c_str(), settings.logDebug, true);
	g_server.start(settings, log, false, reportServiceStatus);
	log.deinit();

	if (g_serviceCrashed)
		return;				// Unclean shutdown -- do not report SERVICE_STOPPED since this
							// would make Windows not restart the service.
cleanup:
	reportServiceStatus(SERVICE_STOPPED, g_dwErr, 0);
}

BOOL WINAPI ctrlEventHandler(DWORD dwCtrlType)
{
   if (dwCtrlType != CTRL_BREAK_EVENT && dwCtrlType != CTRL_C_EVENT)
	   return FALSE;
	g_server.stop();
	return TRUE;
}

int installService(int argc, wchar_t** argv)
{
	WString logFileName;
	ServerSettings settings;
	if (!readSettings(settings, logFileName, argc, argv))
		return 0;

	wchar_t szPath[512];
	if (GetModuleFileNameW(NULL, szPath, eacopy_sizeof_array(szPath)) == 0)
	{
		logErrorf(L"Unable to install %ls - %ls", g_serviceDisplayName, getLastErrorText().c_str());
		return -1;
	}

	if (argc > 1)
	{
		for (int i = 1; i < argc; ++i)
		{
			if(startsWithIgnoreCase(argv[i], L"/USER") || startsWithIgnoreCase(argv[i], L"/PASS"))
				continue;
			wcscat_s(szPath, eacopy_sizeof_array(szPath), L" ");
			wcscat_s(szPath, eacopy_sizeof_array(szPath), argv[i]);
		}
	}

	SC_HANDLE manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!manager)
	{
		logErrorf(L"OpenSCManager failed - %ls", getLastErrorText().c_str());
		return -1;
	}
	ScopeGuard managerGuard([&]() { CloseServiceHandle(manager); });

	const wchar_t* username = NULL;
	const wchar_t* password = NULL;
	if (!settings.user.empty())
		username = settings.user.c_str();
	if (!settings.password.empty())
		password = settings.password.c_str();

	SC_HANDLE service = CreateServiceW(manager, g_serviceName, g_serviceDisplayName, 
							SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
							SERVICE_ERROR_NORMAL, szPath, NULL, NULL, L"", username, password);

	if (!service)
		service = OpenServiceW(manager, g_serviceName, SERVICE_ALL_ACCESS);

	if (!service)
	{
		logErrorf(L"CreateService failed - %ls", getLastErrorText().c_str());
		return -1;
	}
	ScopeGuard serviceGuard([&]() { CloseServiceHandle(service); });

	SERVICE_DESCRIPTION desc;
	desc.lpDescription =  (LPSTR)"EACopy Accelerator Service.";

	if (!ChangeServiceConfig2(service, SERVICE_CONFIG_DESCRIPTION, &desc))
	{
		logErrorf(L"failed to set service description, errcode = %d", GetLastError());
		return -1;
	}

	// Enable automatic restart

	SERVICE_FAILURE_ACTIONS_FLAG flag = { TRUE };
	if (!ChangeServiceConfig2(service, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag))
	{
		logErrorf(L"Failed to set failure action flag, errcode = %d", GetLastError());
		return -1;
	}

	SC_ACTION acts[] = {
		{ SC_ACTION_RESTART, 10000 /* milliseconds */ },
		{ SC_ACTION_RESTART, 30000 /* milliseconds */ },
		{ SC_ACTION_RESTART, 60000 /* milliseconds */ }
	};
	SERVICE_FAILURE_ACTIONS actions = { 60 * 60 * 24, NULL, NULL, sizeof acts / sizeof(SC_ACTION), &acts[0] };

	if (!ChangeServiceConfig2(service, SERVICE_CONFIG_FAILURE_ACTIONS, &actions))
	{
		logErrorf(L"Failed to set failure actions, errcode = %d", GetLastError());
		return -1;
	}

	logInfoLinef(L"%ls installed. commandline = %ls", g_serviceDisplayName, szPath );

	// Autostart server
	if (!StartServiceA(service, argc, (LPCSTR*)argv))
	{
		logInfoLinef();
		DWORD error = GetLastError();
		if (error == ERROR_SERVICE_LOGON_FAILED)
			logErrorf(L"Failed to start service due to a logon failure. MAKE SURE USER IS ADDED TO \"Log on as a service\" policy.");
		else
			logErrorf(L"Failed to start service - %ls", getErrorText(error).c_str());
		return -1;
	}

	return 0;
}

int removeService()
{
	SC_HANDLE manager = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
	if (!manager)
	{
		logErrorf(L"OpenSCManager failed - %ls", getLastErrorText().c_str());
		return -1;
	}
	ScopeGuard managerGuard([&]() { CloseServiceHandle(manager); });

	SC_HANDLE service = OpenServiceW(manager, g_serviceName, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);

	if (!service)
	{
		logErrorf(L"OpenService failed - %ls", getLastErrorText().c_str());
		return -1;
	}
	ScopeGuard serviceGuard([&]() { CloseServiceHandle(service); });

	if (ControlService(service, SERVICE_CONTROL_STOP, &g_ssStatus))
	{
		logInfof(L"Stopping %ls.", g_serviceDisplayName);
		Sleep(1000);

		while (QueryServiceStatus(service, &g_ssStatus))
		{
			if (g_ssStatus.dwCurrentState != SERVICE_STOP_PENDING)
				break;
			logInfof(L".");
			Sleep(1000);
		}

		logInfoLinef();
		if (g_ssStatus.dwCurrentState != SERVICE_STOPPED)
		{
			logErrorf(L"%ls failed to stop.", g_serviceDisplayName);
			return -1;
		}
			
		logInfoLinef(L"%ls stopped.", g_serviceDisplayName);
	}

	if (!DeleteService(service))
	{
		logErrorf(L"DeleteService failed - %ls", getLastErrorText().c_str());
		return -1;
	}

	logInfoLinef(L"%ls removed.", g_serviceDisplayName);
	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy

int __cdecl wmain(int argc, wchar_t** argv)
{
	using namespace eacopy;

	g_argc = argc;
	g_argv = argv;

	initializeServiceName();


	// Try to figure out some context based on which session we're running in
	// and decide which mode to default to.
	bool isInteractiveSession = false;
	DWORD dwSessionId = 0;
	if (ProcessIdToSessionId(GetCurrentProcessId(), &dwSessionId))
		if (dwSessionId != 0) 
			isInteractiveSession = true;

	// Started by service controller
	if (!isInteractiveSession)
	{
		g_runningAsService = true;
		SERVICE_TABLE_ENTRYW dispatchTable[] = { { (LPWSTR)g_serviceName, (LPSERVICE_MAIN_FUNCTIONW)&serviceMain}, { NULL, NULL} };
		if (!StartServiceCtrlDispatcherW(dispatchTable))
			addLastErrorToMessageLog(L"StartServiceCtrlDispatcherW failed.");
		return 0;
	}

	// Started by user
	if (argc > 1 && equalsIgnoreCase(argv[1], L"/?"))
	{
		printHelp();
		return 0;
	}

	// Check if it is an install or removal of service
	int argIndex = 1;
	while (argIndex < argc)
	{
		wchar_t* arg = argv[argIndex];
		if (equalsIgnoreCase(arg, L"/INSTALL"))
		{
			--argc;
			argv[argIndex] = argv[argc]; // Remove own argument
			argv[argc] = nullptr;
			return installService(argc, argv);
		}
		else if (equalsIgnoreCase(arg, L"/REMOVE"))
		{
			return removeService();
		}
		++argIndex;
	}

	// Normal run, just execute the server
	SetConsoleCtrlHandler(ctrlEventHandler, TRUE);

	WString logFileName;
	ServerSettings settings;
	if (!readSettings(settings, logFileName, argc, argv))
		return -1;

	logInfoLinef(L"Server v%hs - Starting... (Add /? for help)", ServerVersion);

	Log log;
	log.init(logFileName.c_str(), settings.logDebug, true);
	ScopeGuard logGuard([&]() { log.deinit(); });

	g_server.start(settings, log, true, reportServiceStatus);
	return 0;
}

