/*
 * WtsSessionManager.cpp - implementation of WtsSessionManager class
 *
 * Copyright (c) 2018-2021 Tobias Junghans <tobydox@veyon.io>
 *
 * This file is part of Veyon - https://veyon.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 */

#include <windows.h>
#include <wtsapi32.h>

#include "WindowsCoreFunctions.h"
#include "WtsSessionManager.h"


WtsSessionManager::SessionId WtsSessionManager::currentSession()
{
	auto sessionId = InvalidSession;
	if( ProcessIdToSessionId( GetCurrentProcessId(), &sessionId ) == 0 )
	{
		vWarning() << GetLastError();
		return InvalidSession;
	}

	return sessionId;
}



WtsSessionManager::SessionId WtsSessionManager::activeConsoleSession()
{
	return WTSGetActiveConsoleSessionId();
}



WtsSessionManager::SessionList WtsSessionManager::activeSessions()
{
	PWTS_SESSION_INFO sessions;
	DWORD sessionCount = 0;

	const auto result = WTSEnumerateSessions( WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount );
	if( result == false )
	{
		return {};
	}

	SessionList sessionList;
	sessionList.reserve( sessionCount );

	for( DWORD sessionIndex = 0; sessionIndex < sessionCount; ++sessionIndex )
	{
		const auto session = &sessions[sessionIndex];
		if( session->State == WTSActive ||
			QString::fromWCharArray(session->pWinStationName)
				.compare( QLatin1String("multiseat"), Qt::CaseInsensitive ) == 0 )
		{
			sessionList.append( session->SessionId );
		}
	}

	WTSFreeMemory( sessions );

	return sessionList;
}



QString WtsSessionManager::querySessionInformation( SessionId sessionId, SessionInfo sessionInfo )
{
	if( sessionId == InvalidSession )
	{
		vCritical() << "called with invalid session ID";
		return {};
	}

	WTS_INFO_CLASS infoClass = WTSInitialProgram;

	switch( sessionInfo )
	{
	case SessionInfo::UserName: infoClass = WTSUserName; break;
	case SessionInfo::DomainName: infoClass = WTSDomainName; break;
	default:
		vCritical() << "invalid session info" << sessionInfo << "requested";
		return {};
	}

	QString result;
	LPWSTR pBuffer = nullptr;
	DWORD dwBufferLen;

	if( WTSQuerySessionInformation( WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass,
									&pBuffer, &dwBufferLen ) )
	{
		result = QString::fromWCharArray( pBuffer );
	}
	else
	{
		const auto lastError = GetLastError();
		vCritical() << lastError;
	}

	WTSFreeMemory( pBuffer );

	vDebug() << sessionId << sessionInfo << result;

	return result;
}



bool WtsSessionManager::isRemote( SessionId sessionId )
{
	const auto WTSIsRemoteSession = static_cast<WTS_INFO_CLASS>(29);

	BOOL *isRDP = nullptr;
	DWORD dataLen = 0;

	if( WTSQuerySessionInformation( WTS_CURRENT_SERVER_HANDLE, sessionId, WTSIsRemoteSession,
									reinterpret_cast<LPTSTR *>(&isRDP), &dataLen ) &&
		isRDP )
	{
		const auto result = *isRDP;
		WTSFreeMemory( isRDP );
		return result;
	}

	return false;
}



WtsSessionManager::ProcessId WtsSessionManager::findWinlogonProcessId( SessionId sessionId )
{
	if( sessionId == InvalidSession )
	{
		vCritical() << "called with invalid session ID";
		return InvalidProcess;
	}

	PWTS_PROCESS_INFO processInfo = nullptr;
	DWORD processCount = 0;

	if( WTSEnumerateProcesses( WTS_CURRENT_SERVER_HANDLE, 0, 1, &processInfo, &processCount ) == false )
	{
		return InvalidProcess;
	}

	auto pid = InvalidProcess;
	const auto processName = QStringLiteral("winlogon.exe");

	for( DWORD proc = 0; proc < processCount; ++proc )
	{
		if( processInfo[proc].ProcessId == 0 )
		{
			continue;
		}

		if( processName.compare( QString::fromWCharArray( processInfo[proc].pProcessName ), Qt::CaseInsensitive ) == 0 &&
			sessionId == processInfo[proc].SessionId )
		{
			pid = processInfo[proc].ProcessId;
			break;
		}
	}

	WTSFreeMemory( processInfo );

	return pid;
}



WtsSessionManager::ProcessId WtsSessionManager::findUserProcessId( const QString& userName )
{
	DWORD sidLen = SECURITY_MAX_SID_SIZE; // Flawfinder: ignore
	char userSID[SECURITY_MAX_SID_SIZE]; // Flawfinder: ignore
	wchar_t domainName[MAX_PATH]; // Flawfinder: ignore
	domainName[0] = 0;
	DWORD domainLen = MAX_PATH;
	SID_NAME_USE sidNameUse;

	if( LookupAccountName( nullptr,		// system name
						   WindowsCoreFunctions::toConstWCharArray( userName ),
						   userSID,
						   &sidLen,
						   domainName,
						   &domainLen,
						   &sidNameUse ) == false )
	{
		vCritical() << "could not look up SID structure";
		return InvalidProcess;
	}

	PWTS_PROCESS_INFO processInfo;
	DWORD processCount = 0;

	if( WTSEnumerateProcesses( WTS_CURRENT_SERVER_HANDLE, 0, 1, &processInfo, &processCount ) == false )
	{
		vWarning() << "WTSEnumerateProcesses() failed:" << GetLastError();
		return InvalidProcess;
	}

	auto pid = InvalidProcess;

	for( DWORD proc = 0; proc < processCount; ++proc )
	{
		if( processInfo[proc].ProcessId > 0 &&
			processInfo[proc].pUserSid != nullptr &&
			EqualSid( processInfo[proc].pUserSid, userSID ) )
		{
			pid = processInfo[proc].ProcessId;
			break;
		}
	}

	WTSFreeMemory( processInfo );

	return pid;
}



WtsSessionManager::ProcessId WtsSessionManager::findProcessId( const QString& processName )
{
	PWTS_PROCESS_INFO processInfo = nullptr;
	DWORD processCount = 0;

	if( WTSEnumerateProcesses( WTS_CURRENT_SERVER_HANDLE, 0, 1, &processInfo, &processCount ) == false )
	{
		return InvalidProcess;
	}

	auto pid = InvalidProcess;

	for( DWORD proc = 0; proc < processCount; ++proc )
	{
		if( processInfo[proc].ProcessId == 0 )
		{
			continue;
		}

		if( processName.compare( QString::fromWCharArray( processInfo[proc].pProcessName ), Qt::CaseInsensitive ) == 0 )
		{
			pid = processInfo[proc].ProcessId;
			break;
		}
	}

	WTSFreeMemory( processInfo );

	return pid;
}
