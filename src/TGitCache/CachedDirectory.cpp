// TortoiseGit - a Windows shell extension for easy version control

// External Cache Copyright (C) 2005-2008 - TortoiseSVN
// Copyright (C) 2008-2017 - TortoiseGit

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
#include "CachedDirectory.h"
#include "GitAdminDir.h"
#include "GitStatusCache.h"
#include "PathUtils.h"
#include "GitStatus.h"
#include <set>

CCachedDirectory::CCachedDirectory(void)
	: m_currentFullStatus(git_wc_status_none)
	, m_mostImportantFileStatus(git_wc_status_none)
	, m_bRecursive(true)
	, m_FetchingStatus(FALSE)
{
}

CCachedDirectory::~CCachedDirectory(void)
{
}

CCachedDirectory::CCachedDirectory(const CTGitPath& directoryPath)
	: CCachedDirectory()
{
	ATLASSERT(directoryPath.IsDirectory() || !PathFileExists(directoryPath.GetWinPath()));

	directoryPath.HasAdminDir(); // make sure HasAdminDir is always initialized
	m_directoryPath = directoryPath;
	ATLASSERT(directoryPath.GetWinPathString().Find(L"\\\\") < 0);
	m_directoryPath.GetGitPathString(); // make sure git path string is set
	ATLASSERT(m_directoryPath.GetGitPathString().Find(L'\\') < 0);
}

BOOL CCachedDirectory::SaveToDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define WRITEVALUETOFILE(x) if (fwrite(&x, sizeof(x), 1, pFile)!=1) return false;

	unsigned int value = GIT_CACHE_VERSION;
	WRITEVALUETOFILE(value);	// 'version' of this save-format
	value = (int)m_entryCache.size();
	WRITEVALUETOFILE(value);	// size of the cache map
	// now iterate through the maps and save every entry.
	for (const auto& entry : m_entryCache)
	{
		const CString& key = entry.first;
		value = key.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)key, sizeof(TCHAR), value, pFile)!=value)
				return false;
			if (!entry.second.SaveToDisk(pFile))
				return false;
		}
	}
	value = (int)m_childDirectories.size();
	WRITEVALUETOFILE(value);
	for (const auto& entry : m_childDirectories)
	{
		const CString& path = entry.first;
		value = path.GetLength();
		WRITEVALUETOFILE(value);
		if (value)
		{
			if (fwrite((LPCTSTR)path, sizeof(TCHAR), value, pFile)!=value)
				return false;
			git_wc_status_kind status = entry.second;
			WRITEVALUETOFILE(status);
		}
	}
	value = m_directoryPath.GetWinPathString().GetLength();
	WRITEVALUETOFILE(value);
	if (value)
	{
		if (fwrite(m_directoryPath.GetWinPath(), sizeof(TCHAR), value, pFile)!=value)
			return false;
	}
	if (!m_ownStatus.SaveToDisk(pFile))
		return false;
	WRITEVALUETOFILE(m_currentFullStatus);
	WRITEVALUETOFILE(m_mostImportantFileStatus);
	return true;
}

BOOL CCachedDirectory::LoadFromDisk(FILE * pFile)
{
	AutoLocker lock(m_critSec);
#define LOADVALUEFROMFILE(x) if (fread(&x, sizeof(x), 1, pFile)!=1) return false;
	try
	{
		unsigned int value = 0;
		LOADVALUEFROMFILE(value);
		if (value != GIT_CACHE_VERSION)
			return false;		// not the correct version
		int mapsize = 0;
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sKey;
				if (fread(sKey.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
				{
					sKey.ReleaseBuffer(0);
					return false;
				}
				sKey.ReleaseBuffer(value);
				CStatusCacheEntry entry;
				if (!entry.LoadFromDisk(pFile))
					return false;
				// only read non empty keys (just needed for transition from old TGit clients)
				if (!sKey.IsEmpty())
					m_entryCache[sKey] = entry;
			}
		}
		LOADVALUEFROMFILE(mapsize);
		for (int i=0; i<mapsize; ++i)
		{
			LOADVALUEFROMFILE(value);
			if (value > MAX_PATH)
				return false;
			if (value)
			{
				CString sPath;
				if (fread(sPath.GetBuffer(value), sizeof(TCHAR), value, pFile)!=value)
				{
					sPath.ReleaseBuffer(0);
					return false;
				}
				sPath.ReleaseBuffer(value);
				git_wc_status_kind status;
				LOADVALUEFROMFILE(status);
				m_childDirectories[sPath] = status;
			}
		}
		LOADVALUEFROMFILE(value);
		if (value > MAX_PATH)
			return false;
		if (value)
		{
			CString sPath;
			if (fread(sPath.GetBuffer(value+1), sizeof(TCHAR), value, pFile)!=value)
			{
				sPath.ReleaseBuffer(0);
				return false;
			}
			sPath.ReleaseBuffer(value);
			// make sure paths do not end with backslash (just needed for transition from old TGit clients)
			if (sPath.GetLength() > 3 && sPath[sPath.GetLength() - 1] == L'\\')
				sPath.TrimRight(L'\\');
			m_directoryPath.SetFromWin(sPath);
			m_directoryPath.GetGitPathString(); // make sure git path string is set
		}
		if (!m_ownStatus.LoadFromDisk(pFile))
			return false;

		LOADVALUEFROMFILE(m_currentFullStatus);
		LOADVALUEFROMFILE(m_mostImportantFileStatus);
	}
	catch ( CAtlException )
	{
		return false;
	}
	return true;
}

void CCachedDirectory::GetStatusFromGit(const CTGitPath& path, const CString& sProjectRoot)
{
	CString subpaths;
	CString s = path.GetGitPathString();
	if (s.GetLength() > sProjectRoot.GetLength())
	{
		if (s[sProjectRoot.GetLength()] == L'\\')
			subpaths = s.Right(s.GetLength() - sProjectRoot.GetLength() - 2);
		else
			subpaths = s.Right(s.GetLength() - sProjectRoot.GetLength() - 1);
	}

	EnumFiles(path, sProjectRoot, subpaths);
}

/// bFetch is true, fetch all status, call by crawl.
/// bFetch is false, get cache status, return quickly.

CStatusCacheEntry CCachedDirectory::GetStatusForMember(const CTGitPath& path, bool bRecursive,  bool bFetch /* = true */)
{
	CString sProjectRoot;
	bool bIsVersionedPath;

	bool bRequestForSelf = false;
	if(path.IsEquivalentToWithoutCase(m_directoryPath))
		bRequestForSelf = true;

	{
		AutoLocker lock(m_critSec);
		// HasAdminDir might modify m_directoryPath, so we need to do it synchronized
		bIsVersionedPath = m_directoryPath.HasAdminDir(&sProjectRoot);
	}

	if (m_directoryPath.IsAdminDir())
	{
		// We're being asked for the status of an .git directory
		// It's not worth asking for this
		return CStatusCacheEntry();
	}

	if (!bRequestForSelf && path.GetUIFileOrDirectoryName() == L".git")
	{
		// We're being asked for the status of an .git directory
		// It's not worth asking for this
		return CStatusCacheEntry();
	}
	
	if (!bIsVersionedPath)
	{
		// We are a folder which is not in a working copy
		m_ownStatus.SetStatus(nullptr);

		// If a user removes the .svn directory, we get here with m_entryCache
		// not being empty, but still us being unversioned
		if (!m_entryCache.empty())
			m_entryCache.clear();
		ATLASSERT(m_entryCache.empty());

		// However, a member *DIRECTORY* might be the top of WC
		// so we need to ask them to get their own status
		if (!path.IsDirectory())
		{
			if (bRequestForSelf)
				ATLASSERT(FALSE);
			if (path.Exists())
				return CStatusCacheEntry();
			// the entry doesn't exist anymore!
			// but we can't remove it from the cache here:
			// the GetStatusForMember() method is called only with a read
			// lock and not a write lock!
			// So mark it for crawling, and let the crawler remove it
			// later
			CGitStatusCache::Instance().AddFolderForCrawling(path.GetContainingDirectory());
			return CStatusCacheEntry();
		}

		// If we're in the special case of a directory being asked for its own status
		// and this directory is unversioned, then we should just return that here
		if (bRequestForSelf)
			return CStatusCacheEntry();

		// We know that we are requesting a sub directory which might be the top of a WC
		// Check whether we already know anything about it
		CCachedDirectory* dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(path);
		if (dirEntry)
		{
			// We don't have directory status in our cache
			// Ask the directory if it knows its own status
			if (dirEntry->IsOwnStatusValid())
			{
				// To keep recursive status up to date, we'll request that children are all crawled again
				// We have to do this because the directory watcher isn't very reliable (especially under heavy load)
				// and also has problems with SUBSTed drives.
				// TODO: If nothing has changed in those directories, this crawling is fast and only
				// accesses two files for each directory.
				if (bRecursive)
				{
					AutoLocker lock(dirEntry->m_critSec);
					for (auto it = dirEntry->m_childDirectories.cbegin(); it != dirEntry->m_childDirectories.cend(); ++it)
						CGitStatusCache::Instance().AddFolderForCrawling(it->first);
				}
				return dirEntry->GetOwnStatus(bRecursive);
			}
		}
		CGitStatusCache::Instance().AddFolderForCrawling(path.GetDirectory()); // update
		return CStatusCacheEntry();
	}

	// folder is versioned

	// if we currently are fetching the status of the directory
	// we want the status for, we just return an empty entry here
	// and don't wait for that fetching to finish.
	// That's because fetching the status can take a *really* long
	// time (e.g. if a commit is also in progress on that same
	// directory), and we don't want to make the explorer appear
	// to hang.
	if (!bFetch)
	{
		if (!IsOwnStatusValid() && !m_FetchingStatus) // and not already fetching
			CGitStatusCache::Instance().AddFolderForCrawling(path.GetDirectory());
		else
			int bla = 65;
		if (bRequestForSelf)
		{
			if (!m_ownStatus.IsVersioned())
			{
				git_wc_status2_t status2 = { git_wc_status_unversioned, git_wc_status_unversioned, false, false };
				status2.text_status = status2.prop_status = git_wc_status_normal;
				m_ownStatus.SetStatus(&status2);
			}
			return GetOwnStatus(bRecursive);
		}

		if (path.IsDirectory())
		{
			// We know that we are requesting a sub directory which might be the top of a WC
			// Check whether we already know anything about it
			CCachedDirectory* dirEntry = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(path);
			if (dirEntry)
			{
				// We don't have directory status in our cache
				// Ask the directory if it knows its own status
				if (dirEntry->IsOwnStatusValid())
				{
					// To keep recursive status up to date, we'll request that children are all crawled again
					// We have to do this because the directory watcher isn't very reliable (especially under heavy load)
					// and also has problems with SUBSTed drives.
					// TODO: If nothing has changed in those directories, this crawling is fast and only
					// accesses two files for each directory.
					if (bRecursive)
					{
						AutoLocker lock(dirEntry->m_critSec);
						for (auto it = dirEntry->m_childDirectories.cbegin(); it != dirEntry->m_childDirectories.cend(); ++it)
						{
							CGitStatusCache::Instance().AddFolderForCrawling(it->first);
						}
					}
					return dirEntry->GetOwnStatus(bRecursive);
				}
			}
			if (!m_FetchingStatus)
				CGitStatusCache::Instance().AddFolderForCrawling(path.GetDirectory());
			else
				int bla2 = 55;
		}

		//All file ignored if under ignore directory
		/*if (m_ownStatus.GetEffectiveStatus() == git_wc_status_ignored)
			return CStatusCacheEntry(git_wc_status_ignored);*/
		/*if (m_ownStatus.GetEffectiveStatus() == git_wc_status_unversioned)
			return CStatusCacheEntry(git_wc_status_unversioned);*/

		// Look up a file in our own cache
		CString strCacheKey = GetCacheKey(path);
		AutoLocker lock(m_critSec);
		CacheEntryMap::iterator itMap = m_entryCache.find(strCacheKey);
		if (itMap != m_entryCache.end())
		{
			// We've hit the cache - check for timeout
			if (!itMap->second.HasExpired((LONGLONG)GetTickCount64()))
			{
				if (itMap->second.DoesFileTimeMatch(path.GetLastWriteTime()))
				{
					if ((itMap->second.GetEffectiveStatus() != git_wc_status_missing) || (!PathFileExists(path.GetWinPath())))
					{
						// Note: the filetime matches after a modified has been committed too.
						// So in that case, we would return a wrong status (e.g. 'modified' instead
						// of 'normal') here.
						return itMap->second;
					}
				}
			}
		}
		return CStatusCacheEntry(git_wc_status_unknown);
	}

	{
		AutoLocker lock(m_critSec);
		m_mostImportantFileStatus = git_wc_status_none;
		m_childDirectories.clear();
		m_entryCache.clear();
		m_ownStatus.SetStatus(nullptr);
	}

	GetStatusFromGit(path, sProjectRoot);

	// Now that we've refreshed our Git status, we can see if it's
	// changed the 'most important' status value for this directory.
	// If it has, then we should tell our parent
	UpdateCurrentStatus();

	return m_currentFullStatus;
}

CStatusCacheEntry CCachedDirectory::GetCacheStatusForMember(const CTGitPath& path)
{
	// no disk access!
	AutoLocker lock(m_critSec);
	CacheEntryMap::iterator itMap = m_entryCache.find(GetCacheKey(path));
	if(itMap != m_entryCache.end())
		return itMap->second;

	return CStatusCacheEntry();
}

int CCachedDirectory::EnumFiles(const CTGitPath& path, CString sProjectRoot, const CString& sSubPath)
{
	if (InterlockedExchange(&m_FetchingStatus, TRUE))
		return -1;

	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": EnumFiles %s\n", path.GetWinPath());

	// strip "\" at the end, otherwise cache lookups for drives do not work correctly
	sProjectRoot.TrimRight(L'\\');

	GitStatus *pStatus = &CGitStatusCache::Instance().m_GitStatus;
	UNREFERENCED_PARAMETER(pStatus);

	if (!path.IsDirectory())
	{
		CString parentDir = sSubPath;
		int begin = parentDir.ReverseFind(L'/');
		parentDir.Truncate(begin < 0 ? 0: begin);
		pStatus->EnumDirStatus(sProjectRoot, parentDir, GetStatusCallback, this);
	}
	else
		pStatus->EnumDirStatus(sProjectRoot, sSubPath, GetStatusCallback, this);

	RefreshMostImportant(false);

	InterlockedExchange(&m_FetchingStatus, FALSE);

	return 0;
}
void
CCachedDirectory::AddEntry(const CTGitPath& path, const git_wc_status2_t* pGitStatus, DWORD validuntil /* = 0*/)
{
	if (path.IsDirectory())
	{
		// no lock here:
		// AutoLocker lock(m_critSec);
		// because GetDirectoryCacheEntry() can try to obtain a write lock
		CCachedDirectory * childDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(path);
		if (childDir)
		{
			if ((childDir->GetCurrentFullStatus() != git_wc_status_ignored) || !pGitStatus || (pGitStatus->text_status != git_wc_status_unversioned))
				childDir->m_ownStatus.SetStatus(pGitStatus);
		}
	}
	else
	{
		AutoLocker lock(m_critSec);
		auto cachekey = GetCacheKey(path);
		CacheEntryMap::iterator entry_it = m_entryCache.lower_bound(cachekey);
		if (entry_it != m_entryCache.end() && entry_it->first == cachekey)
		{
			if (pGitStatus)
			{
				if (entry_it->second.GetEffectiveStatus() > git_wc_status_none && entry_it->second.GetEffectiveStatus() != pGitStatus->text_status)
				{
					CGitStatusCache::Instance().UpdateShell(path);
					CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": shell update for %s\n", path.GetWinPath());
				}
			}
		}
		else
			entry_it = m_entryCache.insert(entry_it, std::make_pair(cachekey, CStatusCacheEntry()));
		entry_it->second = CStatusCacheEntry(pGitStatus, path.GetLastWriteTime(), validuntil); // TODO here
	}
}

CString
CCachedDirectory::GetCacheKey(const CTGitPath& path)
{
	// All we put into the cache as a key is just the end portion of the pathname
	// There's no point storing the path of the containing directory for every item
	return path.GetWinPathString().Mid(m_directoryPath.GetWinPathString().GetLength()).TrimLeft(L'\\');
}

CString
CCachedDirectory::GetFullPathString(const CString& cacheKey)
{
	CString fullpath(m_directoryPath.GetWinPathString());
	fullpath += L'\\';
	fullpath += cacheKey;
	return fullpath;
}

BOOL CCachedDirectory::GetStatusCallback(const CString& path, git_wc_status_kind status, bool isDir, void* baton, bool assumeValid, bool skipWorktree)
{
	git_wc_status2_t _status;
	git_wc_status2_t *status2 = &_status;

	status2->prop_status = status2->text_status = status;
	status2->assumeValid = assumeValid;
	status2->skipWorktree = skipWorktree;

	CCachedDirectory* pThis = (CCachedDirectory*)baton;

	if (path == L"C:\\Users\\strickroth\\Desktop\\tortoisegit\\src/kernel32.pdb/")
		int bla = 5;

	CString tpath(path);
	tpath.Replace(L'/', L'\\');
	tpath.TrimRight(L'\\');

	CTGitPath gitPath;
	gitPath.SetFromWin(tpath, isDir);

	//	if(status->entry)
	{
		if (isDir)
		{	/*gitpath is directory*/
			if (!gitPath.IsEquivalentToWithoutCase(pThis->m_directoryPath))
			{
				// Make sure we know about this child directory
				// This initial status value is likely to be overwritten from below at some point
				git_wc_status_kind s = GitStatus::GetMoreImportant(status2->text_status, status2->prop_status);

				CCachedDirectory * cdir = CGitStatusCache::Instance().GetDirectoryCacheEntryNoCreate(gitPath);
				if (cdir)
				{
					// This child directory is already in our cache!
					// So ask this dir about its recursive status
					git_wc_status_kind st = GitStatus::GetMoreImportant(s, cdir->GetCurrentFullStatus());
					pThis->SetChildStatus(gitPath.GetWinPath(), st);
				}
				else
				{
					// the child directory is not in the cache. Create a new entry for it in the cache which is
					// initially 'unversioned'. But we added that directory to the crawling list above, which
					// means the cache will be updated soon.
					CGitStatusCache::Instance().GetDirectoryCacheEntry(gitPath);
					pThis->SetChildStatus(gitPath.GetWinPath(), s);
				}

			}
			{
				/*if ( status <  git_wc_status_normal)
				{
				if (::PathFileExists(path + L"\\.git"))
				{ // this is submodule
				CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": skip submodule %s\n", (LPCTSTR)path);
				return FALSE;
				}
				}*/
				/*if (pThis->m_bRecursive)
				{
				// Add any versioned directory, which is not our 'self' entry, to the list for having its status updated
				//OutputDebugStringA("AddFolderCrawl: ");OutputDebugStringW(svnPath.GetWinPathString());OutputDebugStringA("\r\n");
				if (status >= git_wc_status_normal || (CGitStatusCache::Instance().IsUnversionedAsModified() && status == git_wc_status_unversioned))
				CGitStatusCache::Instance().AddFolderForCrawling(gitPath);
				}*/
			}
		}
	}
		//else /* gitpath is file*/
		/*{
			// Keep track of the most important status of all the files in this directory
			// Don't include subdirectories in this figure, because they need to provide their
			// own 'most important' value
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->text_status);
			pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, status2->prop_status);
			if ((status2->text_status == git_wc_status_unversioned) && (CGitStatusCache::Instance().IsUnversionedAsModified()))
			{
				// treat unversioned files as modified
				if (pThis->m_mostImportantFileStatus != git_wc_status_added)
					pThis->m_mostImportantFileStatus = GitStatus::GetMoreImportant(pThis->m_mostImportantFileStatus, git_wc_status_modified);
			}
		}*/

	pThis->AddEntry(gitPath, status2);

	return FALSE;
}

bool
CCachedDirectory::IsOwnStatusValid() const
{
	return m_ownStatus.HasBeenSet() && !m_ownStatus.HasExpired(GetTickCount64());
}

void CCachedDirectory::Invalidate()
{
	m_ownStatus.Invalidate();
}

git_wc_status_kind CCachedDirectory::CalculateRecursiveStatus()
{
	// Combine our OWN folder status with the most important of our *FILES'* status.
	git_wc_status_kind retVal = GitStatus::GetMoreImportant(m_mostImportantFileStatus, m_ownStatus.GetEffectiveStatus());

	// folders can only be none, unversioned, normal, modified, and conflicted
	if (retVal == git_wc_status_deleted || retVal == git_wc_status_added)
		retVal = git_wc_status_modified;

	// Now combine all our child-directorie's status
	AutoLocker lock(m_critSec);
	ChildDirStatus::const_iterator it;
	for(it = m_childDirectories.begin(); it != m_childDirectories.end(); ++it)
	{
		retVal = GitStatus::GetMoreImportant(retVal, it->second);
	}

	return retVal;
}

// Update our composite status and deal with things if it's changed
void CCachedDirectory::UpdateCurrentStatus()
{
	git_wc_status_kind newStatus = CalculateRecursiveStatus();

	if ((newStatus != m_currentFullStatus) && (m_ownStatus.IsVersioned()))
	{
		if ((m_currentFullStatus != git_wc_status_none) && (m_ownStatus.GetEffectiveStatus() != git_wc_status_ignored))
		{
			// Our status has changed - tell the shell
			CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": Dir %s, status change from %d to %d\n", m_directoryPath.GetWinPath(), m_currentFullStatus, newStatus);
			CGitStatusCache::Instance().UpdateShell(m_directoryPath);
		}
		if (m_ownStatus.GetEffectiveStatus() != git_wc_status_ignored)
			m_currentFullStatus = newStatus;
		else
			m_currentFullStatus = git_wc_status_ignored;
	}
	// And tell our parent, if we've got one...
	// we tell our parent *always* about our status, even if it hasn't
	// changed. This is to make sure that the parent has really our current
	// status - the parent can decide itself if our status has changed
	// or not.
	CTGitPath parentPath = m_directoryPath.GetContainingDirectory();
	if(!parentPath.IsEmpty())
	{
		// We have a parent
		// just version controled directory need to cache.
		CString root1, root2;
		if (parentPath.HasAdminDir(&root1) && (CGitStatusCache::Instance().IsRecurseSubmodules() || m_directoryPath.HasAdminDir(&root2) && CPathUtils::ArePathStringsEqualWithCase(root1, root2)))
		{
			CCachedDirectory * cachedDir = CGitStatusCache::Instance().GetDirectoryCacheEntry(parentPath);
			if (cachedDir)
				cachedDir->UpdateChildDirectoryStatus(m_directoryPath, m_currentFullStatus);
		}
	}
}

// Receive a notification from a child that its status has changed
void CCachedDirectory::UpdateChildDirectoryStatus(const CTGitPath& childDir, git_wc_status_kind childStatus)
{
	git_wc_status_kind currentStatus = git_wc_status_none;//TODO
	{
		AutoLocker lock(m_critSec);
		currentStatus = m_childDirectories[childDir.GetWinPathString()];
	}
	if ((currentStatus != childStatus)||(!IsOwnStatusValid()))
	{
		{
			AutoLocker lock(m_critSec);
			m_childDirectories[childDir.GetWinPathString()] = childStatus;
		}
		UpdateCurrentStatus();
	}
}

void CCachedDirectory::SetChildStatus(const CString& childDir, git_wc_status_kind childStatus)
{
	AutoLocker lock(m_critSec);
	m_childDirectories[childDir] = childStatus;
}

CStatusCacheEntry CCachedDirectory::GetOwnStatus(bool bRecursive)
{
	// Don't return recursive status if we're unversioned ourselves.
	if(bRecursive && m_ownStatus.GetEffectiveStatus() > git_wc_status_unversioned)
	{
		CStatusCacheEntry recursiveStatus(m_ownStatus);
		UpdateCurrentStatus();
		recursiveStatus.ForceStatus(m_currentFullStatus);
		return recursiveStatus;
	}
	else
	{
		return m_ownStatus;
	}
}

void CCachedDirectory::RefreshStatus(bool bRecursive)
{
	// Make sure that our own status is up-to-date
	GetStatusForMember(m_directoryPath,bRecursive);

	CTGitPathList updatePathList;
	CTGitPathList crawlPathList;
	CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": RefreshStatus for %s\n", m_directoryPath.GetWinPath());

	ULONGLONG now = GetTickCount64();
	{
		// get the file write times with FindFirstFile/FindNextFile since those
		// APIs only access the folder, not each file individually.
		// This reduces the disk access a *lot*.
		std::map<CString, ULONGLONG> filetimes;
		WIN32_FIND_DATA FindFileData;
		CAutoFindFile hFind = ::FindFirstFileEx(m_directoryPath.GetWinPathString() + L"\\*.*", FindExInfoBasic, &FindFileData, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
		if (hFind)
		{
			do
			{
				if (wcscmp(FindFileData.cFileName, L"..") == 0 || wcscmp(FindFileData.cFileName, L".") == 0)
					continue;

				ULARGE_INTEGER ft;
				ft.LowPart = FindFileData.ftLastWriteTime.dwLowDateTime;
				ft.HighPart = FindFileData.ftLastWriteTime.dwHighDateTime;

				filetimes[FindFileData.cFileName] = ft.QuadPart;
			} while (::FindNextFile(hFind, &FindFileData));

			hFind.CloseHandle(); // explicit close handle to shorten its life time
		}

		AutoLocker lock(m_critSec);
		// We also need to check if all our file members have the right date on them
		for (auto itMembers = m_entryCache.cbegin(); itMembers != m_entryCache.cend(); ++itMembers)
		{
			if (!itMembers->first || itMembers->first.IsEmpty())
				continue;

			CTGitPath filePath(GetFullPathString(itMembers->first));
			ATLASSERT(!filePath.IsEquivalentToWithoutCase(m_directoryPath));

			// we only have file members in our entry cache
			//ATLASSERT(!itMembers->second.IsDirectory());

			auto ftIt = filetimes.find(itMembers->first);
			if (ftIt == filetimes.end())
				continue;

			ULONGLONG ft = ftIt->second;
			if (itMembers->second.HasExpired(now) || !itMembers->second.DoesFileTimeMatch(ft))
			{
				// We need to request this item as well
				updatePathList.AddPath(filePath);
			}
		}

		if (bRecursive)
		{
			// crawl all sub folders too! Otherwise a change deep inside the
			// tree which has changed won't get propagated up the tree.
			for (auto it = m_childDirectories.cbegin(); it != m_childDirectories.cend(); ++it)
			{
				CTGitPath path;
				path.SetFromWin(it->first, true);
				crawlPathList.AddPath(path);
			}
		}
	}

	for (int i = 0; i < updatePathList.GetCount(); ++i)
		GetStatusForMember(updatePathList[i], bRecursive);

	for (int i = 0; i < crawlPathList.GetCount(); ++i)
		CGitStatusCache::Instance().AddFolderForCrawling(crawlPathList[i]);
}

void CCachedDirectory::RefreshMostImportant(bool bUpdateShell /* = true */)
{
	AutoLocker lock(m_critSec);
	CacheEntryMap::iterator itMembers;
	git_wc_status_kind newStatus = m_ownStatus.GetEffectiveStatus();
	for (itMembers = m_entryCache.begin(); itMembers != m_entryCache.end(); ++itMembers)
	{
		newStatus = GitStatus::GetMoreImportant(newStatus, itMembers->second.GetEffectiveStatus());
		if (((itMembers->second.GetEffectiveStatus() == git_wc_status_unversioned) || (itMembers->second.GetEffectiveStatus() == git_wc_status_none))
			&& (CGitStatusCache::Instance().IsUnversionedAsModified()))
		{
			// treat unversioned files as modified
			if (newStatus != git_wc_status_added)
				newStatus = GitStatus::GetMoreImportant(newStatus, git_wc_status_modified);
		}
	}
	if (bUpdateShell && newStatus != m_mostImportantFileStatus)
	{
		CTraceToOutputDebugString::Instance()(_T(__FUNCTION__) L": status change of path %s\n", m_directoryPath.GetWinPath());
		CGitStatusCache::Instance().UpdateShell(m_directoryPath);
	}
	// folders can only be none, unversioned, normal, modified, and conflicted
	if (newStatus == git_wc_status_deleted || newStatus == git_wc_status_added)
		newStatus = git_wc_status_modified;
	m_mostImportantFileStatus = newStatus;
}
