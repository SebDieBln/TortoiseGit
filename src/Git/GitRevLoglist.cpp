// TortoiseGit - a Windows shell extension for easy version control

// Copyright (C) 2008-2015 - TortoiseGit

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
//

#include "stdafx.h"
#include <ATLComTime.h>
#include "GitRevLoglist.h"
#include "Git.h"
#include "gitdll.h"
#include "UnicodeUtils.h"

typedef CComCritSecLock<CComCriticalSection> CAutoLocker;

GitRevLoglist::GitRevLoglist(void)
{
	GitRev();
	m_Action = 0;
	m_RebaseAction = 0;
	m_IsFull = FALSE;
	m_IsUpdateing = FALSE;
	m_IsCommitParsed = FALSE;
	m_IsDiffFiles = FALSE;
	m_CallDiffAsync = nullptr;
	m_IsSimpleListReady = FALSE;
	m_Mark = 0;

	SecureZeroMemory(&m_GitCommit, sizeof(GIT_COMMIT));
}

GitRevLoglist::~GitRevLoglist(void)
{
	if (!m_IsCommitParsed && m_GitCommit.m_pGitCommit)
		git_free_commit(&m_GitCommit);
}

void GitRevLoglist::Clear()
{
	GitRev::Clear();
	m_Action = 0;
	m_Files.Clear();
	m_Ref.Empty();
	m_RefAction.Empty();
	m_Mark = 0;
}


int GitRevLoglist::SafeGetSimpleList(CGit* git)
{
	if (InterlockedExchange(&m_IsUpdateing, TRUE) == TRUE)
		return 0;

	m_SimpleFileList.clear();
	git->CheckAndInitDll();
	GIT_COMMIT commit = { 0 };
	GIT_COMMIT_LIST list;
	GIT_HASH parent;

	CAutoLocker lock(g_Git.m_critGitDllSec);

	try
	{
		if (git_get_commit_from_hash(&commit, m_CommitHash.m_hash))
			return -1;
	}
	catch (char *)
	{
		return -1;
	}

	int i = 0;
	bool isRoot = m_ParentHash.empty();
	git_get_commit_first_parent(&commit, &list);
	while (git_get_commit_next_parent(&list, parent) == 0 || isRoot)
	{
		GIT_FILE file = 0;
		int count = 0;
		try
		{
			if (isRoot)
				git_root_diff(git->GetGitSimpleListDiff(), commit.m_hash, &file, &count, 0);
			else
				git_do_diff(git->GetGitSimpleListDiff(), parent, commit.m_hash, &file, &count, 0);
		}
		catch (char *)
		{
			return -1;
		}

		isRoot = false;

		for (int j = 0; j < count; ++j)
		{
			char* newname;
			char* oldname;
			int mode, IsBin, inc, dec;

			try
			{
				git_get_diff_file(git->GetGitSimpleListDiff(), file, j, &newname, &oldname, &mode, &IsBin, &inc, &dec);
			}
			catch (char *)
			{
				return -1;
			}

			m_SimpleFileList.push_back(CUnicodeUtils::GetUnicode(newname));
		}

		git_diff_flush(git->GetGitSimpleListDiff());
		++i;
	}

	InterlockedExchange(&m_IsUpdateing, FALSE);
	InterlockedExchange(&m_IsSimpleListReady, TRUE);
	git_free_commit(&commit);

	return 0;
}

int GitRevLoglist::SafeFetchFullInfo(CGit* git)
{
	if (InterlockedExchange(&m_IsUpdateing, TRUE) == TRUE)
		return 0;

	m_Files.Clear();
	git->CheckAndInitDll();
	GIT_COMMIT commit = { 0 };
	GIT_COMMIT_LIST list;
	GIT_HASH parent;

	CAutoLocker lock(g_Git.m_critGitDllSec);

	try
	{
		if (git_get_commit_from_hash(&commit, m_CommitHash.m_hash))
			return -1;
	}
	catch (char *)
	{
		return -1;
	}

	int i = 0;
	git_get_commit_first_parent(&commit, &list);
	bool isRoot = (list == nullptr);

	while (git_get_commit_next_parent(&list, parent) == 0 || isRoot)
	{
		GIT_FILE file = 0;
		int count = 0;

		try
		{
			if (isRoot)
				git_root_diff(git->GetGitDiff(), m_CommitHash.m_hash, &file, &count, 1);
			else
				git_do_diff(git->GetGitDiff(), parent, commit.m_hash, &file, &count, 1);
		}
		catch (char*)
		{
			git_free_commit(&commit);
			return -1;
		}
		isRoot = false;

		CTGitPath path;
		CString strnewname;
		CString stroldname;

		for (int j = 0; j < count; ++j)
		{
			path.Reset();
			char* newname;
			char* oldname;

			strnewname.Empty();
			stroldname.Empty();

			int mode, IsBin, inc, dec;
			git_get_diff_file(git->GetGitDiff(), file, j, &newname, &oldname, &mode, &IsBin, &inc, &dec);

			git->StringAppend(&strnewname, (BYTE*)newname, CP_UTF8);
			git->StringAppend(&stroldname, (BYTE*)oldname, CP_UTF8);

			path.SetFromGit(strnewname, &stroldname);
			path.ParserAction((BYTE)mode);
			path.m_ParentNo = i;

			m_Action |= path.m_Action;

			if (IsBin)
			{
				path.m_StatAdd = _T("-");
				path.m_StatDel = _T("-");
			}
			else
			{
				path.m_StatAdd.Format(_T("%d"), inc);
				path.m_StatDel.Format(_T("%d"), dec);
			}
			m_Files.AddPath(path);
		}
		git_diff_flush(git->GetGitDiff());
		++i;
	}

	InterlockedExchange(&m_IsUpdateing, FALSE);
	InterlockedExchange(&m_IsFull, TRUE);
	git_free_commit(&commit);

	return 0;
}
