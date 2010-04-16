// Copyright (c) 2005-2009 Nokia Corporation and/or its subsidiary(-ies).
// All rights reserved.
// This component and the accompanying materials are made available
// under the terms of the License "Eclipse Public License v1.0"
// which accompanies this distribution, and is available
// at the URL "http://www.eclipse.org/legal/epl-v10.html".
//
// Initial Contributors:
// Nokia Corporation - initial contribution.
//
// Contributors:
//
// Description:
//

#include "memmodel.h"
#include "mm.h"
#include "mmu.h"

#include "mpager.h"
#include "mrom.h"
#include "mobject.h"
#include "mmapping.h"
#include "maddressspace.h"
#include "mmanager.h"
#include "mptalloc.h"
#include "mpagearray.h"
#include "mswap.h"
#include "mthrash.h"
#include "mpagecleaner.h"

#include "cache_maintenance.inl"


const TUint16 KDefaultYoungOldRatio = 3;
const TUint16 KDefaultMinPages = 256;
const TUint16 KDefaultOldOldestRatio = 3;

const TUint KMinOldPages = 1;

/*	On a 32 bit system without PAE can't have more than 2^(32-KPageShift) pages.
 *	Subtract 1 so it doesn't overflow when converted to bytes.
*/
const TUint	KAbsoluteMaxPageCount = (1u<<(32-KPageShift))-1u;

/*
Limit the maximum number of oldest pages to bound the time taken by SelectPagesToClean(), which is
called with the MmuLock held.
*/
const TUint KMaxOldestPages = 32;

static DMutex* ThePageCleaningLock = NULL;

DPager ThePager;


DPager::DPager()
	: iMinimumPageCount(0), iMaximumPageCount(0), iYoungOldRatio(0),
	  iYoungCount(0), iOldCount(0), iOldestCleanCount(0),
	  iNumberOfFreePages(0), iReservePageCount(0), iMinimumPageLimit(0)
#ifdef __DEMAND_PAGING_BENCHMARKS__
	, iBenchmarkLock(TSpinLock::EOrderGenericIrqHigh3)
#endif	  
	{
	}


void DPager::InitCache()
	{
	//
	// This routine doesn't acquire any mutexes because it should be called before the system
	// is fully up and running. I.e. called before another thread can preempt this.
	//
	TRACEB(("DPager::InitCache()"));
	// If any pages have been reserved then they will have already been allocated and 
	// therefore should be counted as part of iMinimumPageCount.
	__NK_ASSERT_DEBUG(iReservePageCount == iMinimumPageCount);
	__NK_ASSERT_DEBUG(!CacheInitialised());

#if defined(__CPU_ARM)

/** Minimum number of young pages the demand paging live list may have.
	Need at least 4 mapped pages to guarantee to be able to execute all ARM instructions,
	plus enough pages for 4 page tables to map those pages, plus enough pages for the
	page table info structures of those page tables.
	(Worst case is a Thumb-2 STM instruction with both instruction and data straddling chunk
	boundaries.)
*/
	iMinYoungPages = 4											// pages
					+(4+KPtClusterSize-1)/KPtClusterSize		// page table pages
					+(4+KPageTableInfosPerPage-1)/KPageTableInfosPerPage;	// page table info pages

#elif defined(__CPU_X86)

/*	Need at least 6 mapped pages to guarantee to be able to execute all ARM instructions,
	plus enough pages for 6 page tables to map those pages, plus enough pages for the
	page table info structures of those page tables.
	(Worst case is (?) a MOV [X],[Y] instruction with instruction, 'X' and 'Y' all
	straddling chunk boundaries.)
*/
	iMinYoungPages = 6											// pages
					+(6+KPtClusterSize-1)/KPtClusterSize		// page table pages
					+(6+KPageTableInfosPerPage-1)/KPageTableInfosPerPage;	// page table info pages

#else
#error Unknown CPU
#endif

#ifdef __SMP__
	// Adjust min page count so that all CPUs are guaranteed to make progress.
	TInt numberOfCpus = NKern::NumberOfCpus();
	iMinYoungPages *= numberOfCpus;
#endif

	// A minimum young/old ratio of 1 means that we need at least twice iMinYoungPages pages...
	iAbsoluteMinPageCount = 2*iMinYoungPages;

	__NK_ASSERT_DEBUG(KMinOldPages<=iAbsoluteMinPageCount/2);

	// Read any paging config data.
	SDemandPagingConfig config = TheRomHeader().iDemandPagingConfig;

	// Set the list ratios...
	iYoungOldRatio = KDefaultYoungOldRatio;
	if(config.iYoungOldRatio)
		iYoungOldRatio = config.iYoungOldRatio;
	iOldOldestRatio = KDefaultOldOldestRatio;
	if(config.iSpare[2])
		iOldOldestRatio = config.iSpare[2];

	// Set the minimum page counts...
	iMinimumPageLimit = iMinYoungPages * (1 + iYoungOldRatio) / iYoungOldRatio
									   + DPageReadRequest::ReservedPagesRequired();
	
	if(iMinimumPageLimit < iAbsoluteMinPageCount)
		iMinimumPageLimit = iAbsoluteMinPageCount;

	if (K::MemModelAttributes & (EMemModelAttrRomPaging | EMemModelAttrCodePaging | EMemModelAttrDataPaging))
	    iMinimumPageCount = KDefaultMinPages; 
	else
		{// No paging is enabled so set the minimum cache size to the minimum
		// allowable with the current young old ratio.
	    iMinimumPageCount = iMinYoungPages * (iYoungOldRatio + 1);
		}

	if(config.iMinPages)
		iMinimumPageCount = config.iMinPages;
	if(iMinimumPageCount < iAbsoluteMinPageCount)
		iMinimumPageCount = iAbsoluteMinPageCount;
	if (iMinimumPageLimit + iReservePageCount > iMinimumPageCount)
		iMinimumPageCount = iMinimumPageLimit + iReservePageCount;

	iInitMinimumPageCount = iMinimumPageCount;

	// Set the maximum page counts...
	iMaximumPageCount = KMaxTInt;
	if(config.iMaxPages)
		iMaximumPageCount = config.iMaxPages;
	if (iMaximumPageCount > KAbsoluteMaxPageCount)
		iMaximumPageCount = KAbsoluteMaxPageCount;
	iInitMaximumPageCount = iMaximumPageCount;

	TRACEB(("DPager::InitCache() live list min=%d max=%d ratio=%d",iMinimumPageCount,iMaximumPageCount,iYoungOldRatio));

	// Verify the page counts are valid.
	__NK_ASSERT_ALWAYS(iMaximumPageCount >= iMinimumPageCount);
	TUint minOldAndOldest = iMinimumPageCount / (1 + iYoungOldRatio);
	__NK_ASSERT_ALWAYS(minOldAndOldest >= KMinOldPages);
	__NK_ASSERT_ALWAYS(iMinimumPageCount >= minOldAndOldest);

	// Need at least iMinYoungPages pages mapped to execute worst case CPU instruction
	TUint minYoung = iMinimumPageCount - minOldAndOldest;
	__NK_ASSERT_ALWAYS(minYoung >= iMinYoungPages);

	// Verify that the young old ratio can be met even when there is only the 
	// minimum number of old pages.
	TInt ratioLimit = (iMinimumPageCount-KMinOldPages)/KMinOldPages;
	__NK_ASSERT_ALWAYS(iYoungOldRatio <= ratioLimit);

	// There should always be enough old pages to allow the oldest lists ratio.
	TUint oldestCount = minOldAndOldest / (1 + iOldOldestRatio);
	__NK_ASSERT_ALWAYS(oldestCount);

	iNumberOfFreePages = 0;
	iNumberOfDirtyPages = 0;

	// Allocate RAM pages and put them all on the old list.
	// Reserved pages have already been allocated and already placed on the
	// old list so don't allocate them again.
	RamAllocLock::Lock();
	iYoungCount = 0;
	iOldCount = 0;
	iOldestDirtyCount = 0;
	__NK_ASSERT_DEBUG(iOldestCleanCount == iReservePageCount);
	Mmu& m = TheMmu;
	for(TUint i = iReservePageCount; i < iMinimumPageCount; i++)
		{
		// Allocate a single page
		TPhysAddr pagePhys;
		TInt r = m.AllocRam(&pagePhys, 1, 
							(Mmu::TRamAllocFlags)(EMemAttNormalCached|Mmu::EAllocNoWipe|Mmu::EAllocNoPagerReclaim), 
							EPageDiscard);
		if(r!=KErrNone)
			__NK_ASSERT_ALWAYS(0);
		MmuLock::Lock();
		AddAsFreePage(SPageInfo::FromPhysAddr(pagePhys));
		MmuLock::Unlock();
		}
	RamAllocLock::Unlock();

	__NK_ASSERT_DEBUG(CacheInitialised());
	TRACEB(("DPager::InitCache() end with young=%d old=%d oldClean=%d oldDirty=%d min=%d free=%d max=%d",iYoungCount,iOldCount,iOldestCleanCount,iOldestDirtyCount,iMinimumPageCount,iNumberOfFreePages,iMaximumPageCount));
	}


#ifdef _DEBUG
TBool DPager::CheckLists()
	{
#if 0
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	SDblQueLink* head = &iOldList.iA;
	TInt n = iOldCount;
	SDblQueLink* link = head;
	while(n--)
		{
		link = link->iNext;
		if(link==head)
			return false;
		}
	link = link->iNext;
	if(link!=head)
		return false;

	head = &iYoungList.iA;
	n = iYoungCount;
	link = head;
	while(n--)
		{
		link = link->iNext;
		if(link==head)
			return false;
		}
	link = link->iNext;
	if(link!=head)
		return false;

//	TRACEP(("DP: y=%d o=%d f=%d",iYoungCount,iOldCount,iNumberOfFreePages));
#endif
//	TraceCounts();
	return true;
	}

void DPager::TraceCounts()
	{
	TRACEP(("DP: y=%d o=%d f=%d min=%d max=%d ml=%d res=%d",
		iYoungCount,iOldCount,iNumberOfFreePages,iMinimumPageCount,
		iMaximumPageCount,iMinimumPageLimit,iReservePageCount));
	}

#endif


TBool DPager::HaveTooManyPages()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	return iMinimumPageCount+iNumberOfFreePages > iMaximumPageCount;
	}


TBool DPager::HaveMaximumPages()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	return iMinimumPageCount+iNumberOfFreePages >= iMaximumPageCount;
	}


void DPager::AddAsYoungestPage(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(CheckLists());
	__NK_ASSERT_DEBUG(aPageInfo->PagedState()==SPageInfo::EUnpaged);

	aPageInfo->SetPagedState(SPageInfo::EPagedYoung);
	iYoungList.AddHead(&aPageInfo->iLink);
	++iYoungCount;
	}


void DPager::AddAsFreePage(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(CheckLists());

	__NK_ASSERT_DEBUG(aPageInfo->PagedState()==SPageInfo::EUnpaged);
	TheMmu.PageFreed(aPageInfo);
	__NK_ASSERT_DEBUG(aPageInfo->PagedState()==SPageInfo::EUnpaged);

	// add as oldest page...
	aPageInfo->SetPagedState(SPageInfo::EPagedOldestClean);
	iOldestCleanList.Add(&aPageInfo->iLink);
	++iOldestCleanCount;

	Event(EEventPageInFree,aPageInfo);
	}


TInt DPager::PageFreed(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(CheckLists());

	switch(aPageInfo->PagedState())
		{
	case SPageInfo::EUnpaged:
		return KErrNotFound;

	case SPageInfo::EPagedYoung:
		__NK_ASSERT_DEBUG(iYoungCount);
		aPageInfo->iLink.Deque();
		--iYoungCount;
		break;

	case SPageInfo::EPagedOld:
		__NK_ASSERT_DEBUG(iOldCount);
		aPageInfo->iLink.Deque();
		--iOldCount;
		break;

	case SPageInfo::EPagedOldestClean:
		__NK_ASSERT_DEBUG(iOldestCleanCount);
		aPageInfo->iLink.Deque();
		--iOldestCleanCount;
		break;

	case SPageInfo::EPagedOldestDirty:
		__NK_ASSERT_DEBUG(iOldestDirtyCount);
		aPageInfo->iLink.Deque();
		--iOldestDirtyCount;
		break;

	case SPageInfo::EPagedPinned:
		// this can occur if a pinned mapping is being unmapped when memory is decommitted.
		// the decommit will have succeeded because the the mapping no longer vetoes this,
		// however the unpinning hasn't yet got around to changing the page state.
		// When the state change happens the page will be put back on the live list so
		// we don't have to do anything now...
		return KErrNone;

	case SPageInfo::EPagedPinnedMoved:
		// This page was pinned when it was moved but it has not been returned 
		// to the free pool yet so make sure it is...
		aPageInfo->SetPagedState(SPageInfo::EUnpaged);	// Must be unpaged before returned to free pool.
		return KErrNotFound;

	default:
		__NK_ASSERT_DEBUG(0);
		return KErrNotFound;
		}

	// Update the dirty page count as required...
	if (aPageInfo->IsDirty())
		{
		aPageInfo->SetReadOnly();
		SetClean(*aPageInfo);
		}

	// add as oldest page...
	aPageInfo->SetPagedState(SPageInfo::EPagedOldestClean);
	iOldestCleanList.Add(&aPageInfo->iLink);
	++iOldestCleanCount;

	return KErrNone;
	}


extern TBool IsPageTableUnpagedRemoveAllowed(SPageInfo* aPageInfo);

void DPager::RemovePage(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(CheckLists());

	switch(aPageInfo->PagedState())
		{
	case SPageInfo::EPagedYoung:
		__NK_ASSERT_DEBUG(iYoungCount);
		aPageInfo->iLink.Deque();
		--iYoungCount;
		break;

	case SPageInfo::EPagedOld:
		__NK_ASSERT_DEBUG(iOldCount);
		aPageInfo->iLink.Deque();
		--iOldCount;
		break;

	case SPageInfo::EPagedOldestClean:
		__NK_ASSERT_DEBUG(iOldestCleanCount);
		aPageInfo->iLink.Deque();
		--iOldestCleanCount;
		break;

	case SPageInfo::EPagedOldestDirty:
		__NK_ASSERT_DEBUG(iOldestDirtyCount);
		aPageInfo->iLink.Deque();
		--iOldestDirtyCount;
		break;

	case SPageInfo::EPagedPinned:
		__NK_ASSERT_DEBUG(0);
	case SPageInfo::EUnpaged:
#ifdef _DEBUG
		if (!IsPageTableUnpagedRemoveAllowed(aPageInfo))
			__NK_ASSERT_DEBUG(0);
		break;
#endif
	default:
		__NK_ASSERT_DEBUG(0);
		return;
		}

	aPageInfo->SetPagedState(SPageInfo::EUnpaged);
	}


void DPager::ReplacePage(SPageInfo& aOldPageInfo, SPageInfo& aNewPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(CheckLists());

	__NK_ASSERT_DEBUG(aOldPageInfo.PagedState() == aNewPageInfo.PagedState());
	switch(aOldPageInfo.PagedState())
		{
		case SPageInfo::EPagedYoung:
		case SPageInfo::EPagedOld:
		case SPageInfo::EPagedOldestClean:
		case SPageInfo::EPagedOldestDirty:
			{// Update the list links point to the new page.
			__NK_ASSERT_DEBUG(iYoungCount);
			SDblQueLink* prevLink = aOldPageInfo.iLink.iPrev;
#ifdef _DEBUG
			SDblQueLink* nextLink = aOldPageInfo.iLink.iNext;
			__NK_ASSERT_DEBUG(prevLink == aOldPageInfo.iLink.iPrev);
			__NK_ASSERT_DEBUG(prevLink->iNext == &aOldPageInfo.iLink);
			__NK_ASSERT_DEBUG(nextLink == aOldPageInfo.iLink.iNext);
			__NK_ASSERT_DEBUG(nextLink->iPrev == &aOldPageInfo.iLink);
#endif
			aOldPageInfo.iLink.Deque();
			aNewPageInfo.iLink.InsertAfter(prevLink);
			aOldPageInfo.SetPagedState(SPageInfo::EUnpaged);
#ifdef _DEBUG
			__NK_ASSERT_DEBUG(prevLink == aNewPageInfo.iLink.iPrev);
			__NK_ASSERT_DEBUG(prevLink->iNext == &aNewPageInfo.iLink);
			__NK_ASSERT_DEBUG(nextLink == aNewPageInfo.iLink.iNext);
			__NK_ASSERT_DEBUG(nextLink->iPrev == &aNewPageInfo.iLink);
#endif
			}
			break;
		case SPageInfo::EPagedPinned:
			// Mark the page as 'pinned moved' so that when the page moving invokes 
			// Mmu::FreeRam() it returns this page to the free pool.
			aOldPageInfo.ClearPinCount();
			aOldPageInfo.SetPagedState(SPageInfo::EPagedPinnedMoved);
			break;
		case SPageInfo::EPagedPinnedMoved:
			// Shouldn't happen as the ram alloc mutex will be held for the 
			// entire time the page's is paged state == EPagedPinnedMoved.
		case SPageInfo::EUnpaged:
			// Shouldn't happen as we only move pinned memory and unpinning will 
			// atomically add the page to the live list and it can't be removed 
			// from the live list without the ram alloc mutex.
			__NK_ASSERT_DEBUG(0);
			break;
		}	
	}


TInt DPager::TryStealOldestPage(SPageInfo*& aPageInfoOut)
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	// find oldest page in list...
	SDblQueLink* link;
	if (iOldestCleanCount)
		{
		__NK_ASSERT_DEBUG(!iOldestCleanList.IsEmpty());
		link = iOldestCleanList.Last();
		}
	else if (iOldestDirtyCount)
		{
		__NK_ASSERT_DEBUG(!iOldestDirtyList.IsEmpty());
		link = iOldestDirtyList.Last();
		}
	else if (iOldCount)
		{
		__NK_ASSERT_DEBUG(!iOldList.IsEmpty());
		link = iOldList.Last();
		}
	else
		{
		__NK_ASSERT_DEBUG(iYoungCount);
		__NK_ASSERT_ALWAYS(!iYoungList.IsEmpty());
		link = iYoungList.Last();
		}
	SPageInfo* pageInfo = SPageInfo::FromLink(link);

	if (pageInfo->IsDirty() && !PageCleaningLock::IsHeld())
		return 1;

	// try to steal it from owning object...
	TInt r = StealPage(pageInfo);	
	if (r == KErrNone)
		{
		BalanceAges();
		aPageInfoOut = pageInfo;
		}
	
	return r;
	}


SPageInfo* DPager::StealOldestPage()
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	TBool pageCleaningLockHeld = EFalse;
	for(;;)
		{
		SPageInfo* pageInfo = NULL;
		TInt r = TryStealOldestPage(pageInfo);
		
		if (r == KErrNone)
			{
			if (pageCleaningLockHeld)
				{
				MmuLock::Unlock();
				PageCleaningLock::Unlock();
				MmuLock::Lock();
				}
			return pageInfo;
			}
		else if (r == 1)
			{
			__NK_ASSERT_ALWAYS(!pageCleaningLockHeld);
			MmuLock::Unlock();
			PageCleaningLock::Lock();
			MmuLock::Lock();
			pageCleaningLockHeld = ETrue;
			}
		// else retry...
		}
	}

#ifdef __CPU_CACHE_HAS_COLOUR

template <class T, TInt maxObjects> class TSequentialColourSelector
	{
public:
	static const TInt KMaxLength = maxObjects;
	static const TInt KArrayLength = _ALIGN_UP(KMaxLength, KPageColourCount);
	
	FORCE_INLINE TSequentialColourSelector()
		{
		memclr(this, sizeof(*this));
		}

	FORCE_INLINE TBool FoundLongestSequence()
		{
		return iLongestLength >= KMaxLength;
		}

	FORCE_INLINE void AddCandidate(T* aObject, TInt aColour)
		{
		// allocate objects to slots based on colour
		for (TInt i = aColour ; i < KArrayLength ; i += KPageColourCount)
			{
			if (!iSlot[i])
				{
				iSlot[i] = aObject;
				iSeqLength[i] = i == 0 ? 1 : iSeqLength[i - 1] + 1;
				TInt j = i + 1;
				while(j < KArrayLength && iSeqLength[j])
					iSeqLength[j++] += iSeqLength[i];
				TInt currentLength = iSeqLength[j - 1];
				if (currentLength > iLongestLength)
					{
					iLongestLength = currentLength;
					iLongestStart = j - currentLength;
					}
				break;
				}
			}
		}

	FORCE_INLINE TInt FindLongestRun(T** aObjectsOut)
		{
		if (iLongestLength == 0)
			return 0;

		if (iLongestLength < KMaxLength && iSlot[0] && iSlot[KArrayLength - 1])
			{
			// check possibility of wrapping

			TInt i = 1;
			while (iSlot[i]) ++i;  // find first hole
			TInt wrappedLength = iSeqLength[KArrayLength - 1] + iSeqLength[i - 1];
			if (wrappedLength > iLongestLength)
				{
				iLongestLength = wrappedLength;
				iLongestStart = KArrayLength - iSeqLength[KArrayLength - 1];
				}
			}		

		iLongestLength = Min(iLongestLength, KMaxLength);

		__NK_ASSERT_DEBUG(iLongestStart >= 0 && iLongestStart < KArrayLength);
		__NK_ASSERT_DEBUG(iLongestStart + iLongestLength < 2 * KArrayLength);

		TInt len = Min(iLongestLength, KArrayLength - iLongestStart);
		wordmove(aObjectsOut, &iSlot[iLongestStart], len * sizeof(T*));
		wordmove(aObjectsOut + len, &iSlot[0], (iLongestLength - len) * sizeof(T*));
		
		return iLongestLength;
		}

private:
	T* iSlot[KArrayLength];
	TInt8 iSeqLength[KArrayLength];
	TInt iLongestStart;
	TInt iLongestLength;
	};

TInt DPager::SelectPagesToClean(SPageInfo** aPageInfosOut)
	{
	// select up to KMaxPagesToClean oldest dirty pages with sequential page colours
	
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	TSequentialColourSelector<SPageInfo, KMaxPagesToClean> selector;

	SDblQueLink* link = iOldestDirtyList.Last();
	while (link != &iOldestDirtyList.iA)
		{
		SPageInfo* pi = SPageInfo::FromLink(link);
		if (!pi->IsWritable())  
			{
			// the page may be in the process of being restricted, stolen or decommitted, but don't
			// check for this as it will occur infrequently and will be detected by CheckModified
			// anyway
			TInt colour = pi->Index() & KPageColourMask;
			selector.AddCandidate(pi, colour);
			if (selector.FoundLongestSequence())
				break;
			}
		link = link->iPrev;
		}
	
	return selector.FindLongestRun(aPageInfosOut);
	}

#else

TInt DPager::SelectPagesToClean(SPageInfo** aPageInfosOut)
	{
	// no page colouring restrictions, so just take up to KMaxPagesToClean oldest dirty pages
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	TInt pageCount = 0;
	SDblQueLink* link = iOldestDirtyList.Last();
	while (link != &iOldestDirtyList.iA && pageCount < KMaxPagesToClean)
		{
		SPageInfo* pi = SPageInfo::FromLink(link);
		if (!pi->IsWritable())
			{
			// the page may be in the process of being restricted, stolen or decommitted, but don't
			// check for this as it will occur infrequently and will be detected by CheckModified
			// anyway
			aPageInfosOut[pageCount++] = pi;
			}
		link = link->iPrev;
		}
	return pageCount;
	}

#endif


TInt DPager::CleanSomePages(TBool aBackground)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(PageCleaningLock::IsHeld());
	// ram alloc lock may or may not be held

	SPageInfo* pageInfos[KMaxPagesToClean];
	TInt pageCount = SelectPagesToClean(&pageInfos[0]);
	
	if (pageCount == 0)
		return 0;
	
	TheDataPagedMemoryManager->CleanPages(pageCount, pageInfos, aBackground);

	for (TInt i = 0 ; i < pageCount ; ++i)
		{
		SPageInfo* pi = pageInfos[i];
		if (pi)
			{
			__NK_ASSERT_DEBUG(pi->PagedState() == SPageInfo::EPagedOldestDirty && iOldestDirtyCount);
			__NK_ASSERT_DEBUG(!pi->IsDirty() && !pi->IsWritable());
		
			pi->iLink.Deque();
			iOldestCleanList.AddHead(&pi->iLink);
			--iOldestDirtyCount;
			++iOldestCleanCount;
			pi->SetPagedState(SPageInfo::EPagedOldestClean);
			}
		}

	return pageCount;
	}


TBool DPager::HasPagesToClean()
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	return iOldestDirtyCount > 0;
	}


TInt DPager::RestrictPage(SPageInfo* aPageInfo, TRestrictPagesType aRestriction)
	{
	TRACE(("DPager::RestrictPage(0x%08x,%d)",aPageInfo,aRestriction));
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	TInt r;
	if(aPageInfo->Type()==SPageInfo::EUnused)
		{
		// page was unused, so nothing to do...
		r = KErrNone;
		}
	else
		{
		// get memory object which owns the page...
		__NK_ASSERT_DEBUG(aPageInfo->Type()==SPageInfo::EManaged);
		DMemoryObject* memory = aPageInfo->Owner();
		memory->Open();

		// try restricting access to page...
		r = memory->iManager->RestrictPage(memory,aPageInfo,aRestriction);
		__NK_ASSERT_DEBUG(r!=KErrNotSupported);

		// close memory object...
		MmuLock::Unlock();
		memory->AsyncClose();
		MmuLock::Lock();
		}

	TRACE(("DPager::RestrictPage returns %d",r));
	return r;
	}


TInt DPager::StealPage(SPageInfo* aPageInfo)
	{
	TRACE(("DPager::StealPage(0x%08x)",aPageInfo));
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
 
	__UNLOCK_GUARD_START(MmuLock);
	RemovePage(aPageInfo);

	TInt r;
	if(aPageInfo->Type()==SPageInfo::EUnused)
		{
		// page was unused, so nothing to do...
		r = KErrNone;
		__UNLOCK_GUARD_END(MmuLock);
		MmuLock::Unlock();
		}
	else
		{
		// get memory object which owns the page...
		__NK_ASSERT_DEBUG(aPageInfo->Type()==SPageInfo::EManaged);
		DMemoryObject* memory = aPageInfo->Owner();
		memory->Open();

		// try and steal page from memory object...
		__UNLOCK_GUARD_END(MmuLock); // StealPage must be called without releasing the MmuLock
		r = memory->iManager->StealPage(memory,aPageInfo);
		__NK_ASSERT_DEBUG(r!=KErrNotSupported);

		// close memory object...
		MmuLock::Unlock();
		memory->AsyncClose();
		}

	MmuLock::Lock();

	if(r==KErrNone)
		Event(EEventPageOut,aPageInfo);

	TRACE(("DPager::StealPage returns %d",r));
	return r;
	}


static TBool DiscardCanStealPage(SPageInfo* aOldPageInfo, TBool aBlockRest)
	{
 	// If the page is pinned or if the page is dirty and a general defrag is being performed then
	// don't attempt to steal it
	return aOldPageInfo->Type() == SPageInfo::EUnused ||
		(aOldPageInfo->PagedState() != SPageInfo::EPagedPinned && (!aBlockRest || !aOldPageInfo->IsDirty()));	
	}


TInt DPager::DiscardPage(SPageInfo* aOldPageInfo, TUint aBlockZoneId, TBool aBlockRest)
	{
	// todo: assert MmuLock not released
	
	TRACE(("> DPager::DiscardPage %08x", aOldPageInfo));
	
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	if (!DiscardCanStealPage(aOldPageInfo, aBlockRest))
		{
		// The page is pinned or is dirty and this is a general defrag so move the page.
		DMemoryObject* memory = aOldPageInfo->Owner();
		// Page must be managed if it is pinned or dirty.
		__NK_ASSERT_DEBUG(aOldPageInfo->Type()==SPageInfo::EManaged);
		__NK_ASSERT_DEBUG(memory);
		MmuLock::Unlock();
		TPhysAddr newAddr;
		TRACE2(("DPager::DiscardPage delegating pinned/dirty page to manager"));
		TInt r = memory->iManager->MovePage(memory, aOldPageInfo, newAddr, aBlockZoneId, aBlockRest);
		TRACE(("< DPager::DiscardPage %d", r));
		return r;
		}

	TInt r = KErrNone;
	SPageInfo* newPageInfo = NULL;
	TBool havePageCleaningLock = EFalse;

	TBool needNewPage;
	TBool needPageCleaningLock;
	while(needNewPage = (iNumberOfFreePages == 0 && newPageInfo == NULL),
		  needPageCleaningLock = (aOldPageInfo->IsDirty() && !havePageCleaningLock),
		  needNewPage || needPageCleaningLock)
		{
		MmuLock::Unlock();

		if (needNewPage)
			{
			// Allocate a new page for the live list as it has reached its minimum size.
			TUint flags = EMemAttNormalCached | Mmu::EAllocNoWipe;
			newPageInfo = GetPageFromSystem((Mmu::TRamAllocFlags)flags, aBlockZoneId, aBlockRest);
			if (!newPageInfo)
				{
				TRACE(("< DPager::DiscardPage KErrNoMemory"));
				r = KErrNoMemory;
				MmuLock::Lock();
				break;
				}
			}

		if (needPageCleaningLock)
			{
			// Acquire the page cleaning mutex so StealPage can clean it
			PageCleaningLock::Lock();
			havePageCleaningLock = ETrue;
			}

		// Re-acquire the mmulock and re-check that the page is not pinned or dirty.
		MmuLock::Lock();
		if (!DiscardCanStealPage(aOldPageInfo, aBlockRest))
			{
			// Page is now pinned or dirty so give up as it is in use.
			r = KErrInUse;
			break;
			}
		}

	if (r == KErrNone)
		{
		// Attempt to steal the page
		r = StealPage(aOldPageInfo);  // temporarily releases MmuLock if page is dirty
		}
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	if (r == KErrCompletion)
		{// This was a page table that has been freed but added to the 
		// live list as a free page.  Remove from live list and continue.
		__NK_ASSERT_DEBUG(!aOldPageInfo->IsDirty());
		RemovePage(aOldPageInfo);
		r = KErrNone;
		}

	if (r == KErrNone && iNumberOfFreePages == 0)
		{
		if (newPageInfo)
			{
			// Add a new page to the live list if we have one as discarding the old page will reduce
			// the live list below the minimum.
			AddAsFreePage(newPageInfo);
			newPageInfo = NULL;
			}
		else
			{
			// Otherwise the live list shrank when page was being cleaned so have to give up
			AddAsFreePage(aOldPageInfo);
			BalanceAges();                  // temporarily releases MmuLock
			r = KErrInUse;
			}
		}

	if (r == KErrNone)
		{
		// We've successfully discarded the page and ensured the live list is large enough, so
		// return it to the free pool.
		ReturnPageToSystem(*aOldPageInfo);  // temporarily releases MmuLock
		BalanceAges();                      // temporarily releases MmuLock
		}

	if (newPageInfo)
		{
		// New page not required so just return it to the system.  This is safe as
		// iNumberOfFreePages will have this page counted but as it is not on the live list noone
		// else can touch it.
		if (iNumberOfFreePages == 0)
			AddAsFreePage(newPageInfo);
		else
			ReturnPageToSystem(*newPageInfo);   // temporarily releases MmuLock
		}

	if (havePageCleaningLock)
		{
		// Release the page cleaning mutex
		MmuLock::Unlock();
		PageCleaningLock::Unlock();
		MmuLock::Lock();
		}	
	
	MmuLock::Unlock();
	TRACE(("< DPager::DiscardPage returns %d", r));
	return r;	
	}


TBool DPager::TryGrowLiveList()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	MmuLock::Unlock();
	SPageInfo* sparePage = GetPageFromSystem((Mmu::TRamAllocFlags)(EMemAttNormalCached|Mmu::EAllocNoWipe));
	MmuLock::Lock();

	if(!sparePage)
		return false;

	// add page to live list...
	AddAsFreePage(sparePage);
	return true;
	}


SPageInfo* DPager::GetPageFromSystem(Mmu::TRamAllocFlags aAllocFlags, TUint aBlockZoneId, TBool aBlockRest)
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());

	TPhysAddr pagePhys;
	TInt r = TheMmu.AllocRam(&pagePhys, 1, 
							(Mmu::TRamAllocFlags)(aAllocFlags|Mmu::EAllocNoPagerReclaim), 
							EPageDiscard, aBlockZoneId, aBlockRest);
	if(r!=KErrNone)
		return NULL;

	MmuLock::Lock();
	++iNumberOfFreePages;
	MmuLock::Unlock();

	return SPageInfo::FromPhysAddr(pagePhys);
	}


void DPager::ReturnPageToSystem()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	ReturnPageToSystem(*StealOldestPage());
	}


void DPager::ReturnPageToSystem(SPageInfo& aPageInfo)
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());

	// should be unpaged at this point, otherwise Mmu::FreeRam will just give it back to us
	__NK_ASSERT_DEBUG(aPageInfo.PagedState() == SPageInfo::EUnpaged);

	__NK_ASSERT_DEBUG(iNumberOfFreePages>0);
	--iNumberOfFreePages;

	MmuLock::Unlock();

	TPhysAddr pagePhys = aPageInfo.PhysAddr();
	TheMmu.FreeRam(&pagePhys, 1, EPageDiscard);

	MmuLock::Lock();
	}


SPageInfo* DPager::PageInAllocPage(Mmu::TRamAllocFlags aAllocFlags)
	{
	TBool pageCleaningLockHeld = EFalse;
	SPageInfo* pageInfo;
	TPhysAddr pagePhys;
	TInt r = KErrGeneral;
	
	RamAllocLock::Lock();
	MmuLock::Lock();

find_a_page:
	// try getting a free page from our live list...
	if (iOldestCleanCount)
		{
		pageInfo = SPageInfo::FromLink(iOldestCleanList.Last());
		if(pageInfo->Type()==SPageInfo::EUnused)
			goto try_steal_oldest_page;
		}

	// try getting a free page from the system pool...
	if(!HaveMaximumPages())
		{
		MmuLock::Unlock();
		pageInfo = GetPageFromSystem(aAllocFlags);
		if(pageInfo)
			goto done;
		MmuLock::Lock();
		}

	// try stealing a clean page...
	if (iOldestCleanCount)
		goto try_steal_oldest_page;

	// see if we can clean multiple dirty pages in one go...
	if (KMaxPagesToClean > 1 && iOldestDirtyCount > 1)
		{
		// if we don't hold the page cleaning mutex then temporarily release ram alloc mutex and
		// acquire page cleaning mutex; if we hold it already just proceed
		if (!pageCleaningLockHeld)
			{
			MmuLock::Unlock();
			RamAllocLock::Unlock();
			PageCleaningLock::Lock();			
			MmuLock::Lock();
			}
		
		// there may be clean pages now if we've waited on the page cleaning mutex, if so don't
		// bother cleaning but just restart
		if (iOldestCleanCount == 0)
			CleanSomePages(EFalse);
		
		if (!pageCleaningLockHeld)
			{
			MmuLock::Unlock();
			PageCleaningLock::Unlock();			
			RamAllocLock::Lock();
			MmuLock::Lock();
			}
		
		if (iOldestCleanCount > 0)
			goto find_a_page;
		}

	// as a last resort, steal a page from the live list...
	
try_steal_oldest_page:
	__NK_ASSERT_ALWAYS(iOldestCleanCount|iOldestDirtyCount|iOldCount|iYoungCount);
	r = TryStealOldestPage(pageInfo);
	// if this fails we restart whole process
	if (r < KErrNone)
		goto find_a_page;

	// if we need to clean, acquire page cleaning mutex for life of this function
	if (r == 1)
		{
		__NK_ASSERT_ALWAYS(!pageCleaningLockHeld);
		MmuLock::Unlock();
		PageCleaningLock::Lock();
		MmuLock::Lock();
		pageCleaningLockHeld = ETrue;
		goto find_a_page;		
		}

	// otherwise we're done!
	__NK_ASSERT_DEBUG(r == KErrNone);
	MmuLock::Unlock();

	// make page state same as a freshly allocated page...
	pagePhys = pageInfo->PhysAddr();
	TheMmu.PagesAllocated(&pagePhys,1,aAllocFlags);

done:
	if (pageCleaningLockHeld)
		PageCleaningLock::Unlock();
	RamAllocLock::Unlock();

	return pageInfo;
	}


TBool DPager::GetFreePages(TInt aNumPages)
	{
	TRACE(("DPager::GetFreePages(%d)",aNumPages));

	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());

	MmuLock::Lock();
	while(aNumPages>0 && (TInt)NumberOfFreePages()>=aNumPages)
		{
		ReturnPageToSystem();
		--aNumPages;
		}
	MmuLock::Unlock();

	TRACE(("DPager::GetFreePages returns %d",!aNumPages));
	return !aNumPages;
	}


void DPager::DonatePages(TUint aCount, TPhysAddr* aPages)
	{
	TRACE(("DPager::DonatePages(%d,?)",aCount));
	__ASSERT_CRITICAL;
	RamAllocLock::Lock();
	MmuLock::Lock();

	TPhysAddr* end = aPages+aCount;
	while(aPages<end)
		{
		TPhysAddr pagePhys = *aPages++;
		if(RPageArray::State(pagePhys)!=RPageArray::ECommitted)
			continue; // page is not present

#ifdef _DEBUG
		SPageInfo* pi = SPageInfo::SafeFromPhysAddr(pagePhys&~KPageMask);
		__NK_ASSERT_DEBUG(pi);
#else
		SPageInfo* pi = SPageInfo::FromPhysAddr(pagePhys);
#endif
		switch(pi->PagedState())
			{
		case SPageInfo::EUnpaged:
			// Change the type of this page to discardable and 
			// then add it to live list.
			// Only the DDiscardableMemoryManager should be invoking this and
			// its pages will be movable before they are donated.
			__NK_ASSERT_DEBUG(pi->Owner()->iManager->PageType() == EPageMovable);
			TheMmu.ChangePageType(pi, EPageMovable, EPageDiscard);
			break;

		case SPageInfo::EPagedYoung:
		case SPageInfo::EPagedOld:
		case SPageInfo::EPagedOldestDirty:
		case SPageInfo::EPagedOldestClean:
			continue; // discard already been allowed

		case SPageInfo::EPagedPinned:
			__NK_ASSERT_DEBUG(0);
		default:
			__NK_ASSERT_DEBUG(0);
			continue;
			}

		// put page on live list...
		AddAsYoungestPage(pi);
		++iNumberOfFreePages;

		Event(EEventPageDonate,pi);

		// re-balance live list...
		RemoveExcessPages();
		BalanceAges();
		}

	MmuLock::Unlock();
	RamAllocLock::Unlock();
	}


TInt DPager::ReclaimPages(TUint aCount, TPhysAddr* aPages)
	{
	TRACE(("DPager::ReclaimPages(%d,?)",aCount));
	__ASSERT_CRITICAL;
	RamAllocLock::Lock();
	MmuLock::Lock();

	TInt r = KErrNone;
	TPhysAddr* end = aPages+aCount;
	while(aPages<end)
		{
		TPhysAddr pagePhys = *aPages++;
		TBool changeType = EFalse;

		if(RPageArray::State(pagePhys)!=RPageArray::ECommitted)
			{
			r = KErrNotFound; // too late, page has gone
			continue;
			}

#ifdef _DEBUG
		SPageInfo* pi = SPageInfo::SafeFromPhysAddr(pagePhys&~KPageMask);
		__NK_ASSERT_DEBUG(pi);
#else
		SPageInfo* pi = SPageInfo::FromPhysAddr(pagePhys);
#endif
		switch(pi->PagedState())
			{
		case SPageInfo::EUnpaged:
			continue; // discard already been disallowed

		case SPageInfo::EPagedYoung:
		case SPageInfo::EPagedOld:
		case SPageInfo::EPagedOldestClean:
		case SPageInfo::EPagedOldestDirty:
			changeType = ETrue;
			break; // remove from live list

		case SPageInfo::EPagedPinned:
			__NK_ASSERT_DEBUG(0);
		default:
			__NK_ASSERT_DEBUG(0);
			break;
			}

		// check paging list has enough pages before we remove one...
		if(iNumberOfFreePages<1)
			{
			// need more pages so get a page from the system...
			if(!TryGrowLiveList())
				{
				// out of memory...
				r = KErrNoMemory;
				break;
				}
			// retry the page reclaim...
			--aPages;
			continue;
			}

		if (changeType)
			{// Change the type of this page to movable, wait until any retries
			// have been attempted as we can't change a page's type twice.
			// Only the DDiscardableMemoryManager should be invoking this and
			// its pages should be movable once they are reclaimed.
			__NK_ASSERT_DEBUG(pi->Owner()->iManager->PageType() == EPageMovable);
			TheMmu.ChangePageType(pi, EPageDiscard, EPageMovable);
			}

		// remove page from paging list...
		__NK_ASSERT_DEBUG(iNumberOfFreePages>0);
		--iNumberOfFreePages;
		RemovePage(pi);

		Event(EEventPageReclaim,pi);

		// re-balance live list...
		BalanceAges();
		}

	// we may have added a spare free page to the live list without removing one,
	// this could cause us to have too many pages, so deal with this...
	RemoveExcessPages();

	MmuLock::Unlock();
	RamAllocLock::Unlock();
	return r;
	}


TInt VMHalFunction(TAny*, TInt aFunction, TAny* a1, TAny* a2);

void DPager::Init3()
	{
	TRACEB(("DPager::Init3()"));
	TheRomMemoryManager->Init3();
	TheDataPagedMemoryManager->Init3();
	TheCodePagedMemoryManager->Init3();
	TInt r = Kern::AddHalEntry(EHalGroupVM, VMHalFunction, 0);
	__NK_ASSERT_ALWAYS(r==KErrNone);
	PageCleaningLock::Init();
	}


void DPager::Fault(TFault aFault)
	{
	Kern::Fault("DPager",aFault);
	}


void DPager::BalanceAges()
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	TBool restrictPage = EFalse;
	SPageInfo* pageInfo = NULL;
	TUint oldestCount = iOldestCleanCount + iOldestDirtyCount;
	if((iOldCount + oldestCount) * iYoungOldRatio < iYoungCount)
		{
		// Need more old pages so make one young page into an old page...
		__NK_ASSERT_DEBUG(!iYoungList.IsEmpty());
		__NK_ASSERT_DEBUG(iYoungCount);
		SDblQueLink* link = iYoungList.Last()->Deque();
		--iYoungCount;

		pageInfo = SPageInfo::FromLink(link);
		pageInfo->SetPagedState(SPageInfo::EPagedOld);

		iOldList.AddHead(link);
		++iOldCount;

		Event(EEventPageAged,pageInfo);
		// Delay restricting the page until it is safe to release the MmuLock.
		restrictPage = ETrue;
		}

	// Check we have enough oldest pages.
	if (oldestCount < KMaxOldestPages &&
		oldestCount * iOldOldestRatio < iOldCount)
		{
		__NK_ASSERT_DEBUG(!iOldList.IsEmpty());
		__NK_ASSERT_DEBUG(iOldCount);
		SDblQueLink* link = iOldList.Last()->Deque();
		--iOldCount;

		SPageInfo* oldestPageInfo = SPageInfo::FromLink(link);
		if (oldestPageInfo->IsDirty())
			{
			oldestPageInfo->SetPagedState(SPageInfo::EPagedOldestDirty);
			iOldestDirtyList.AddHead(link);
			++iOldestDirtyCount;
			PageCleaner::NotifyPagesToClean();
			Event(EEventPageAgedDirty,oldestPageInfo);
			}
		else
			{
			oldestPageInfo->SetPagedState(SPageInfo::EPagedOldestClean);
			iOldestCleanList.AddHead(link);
			++iOldestCleanCount;
			Event(EEventPageAgedClean,oldestPageInfo);
			}
		}

	if (restrictPage)
		{
		// Make the recently aged old page inaccessible.  This is done last as it 
		// will release the MmuLock and therefore the page counts may otherwise change.
		RestrictPage(pageInfo,ERestrictPagesNoAccessForOldPage);
		}
	}


void DPager::RemoveExcessPages()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	while(HaveTooManyPages())
		ReturnPageToSystem();
	}


void DPager::RejuvenatePageTable(TPte* aPt)
	{
	SPageInfo* pi = SPageInfo::FromPhysAddr(Mmu::PageTablePhysAddr(aPt));

	SPageTableInfo* pti = SPageTableInfo::FromPtPtr(aPt);
	if(!pti->IsDemandPaged())
		{
		__NK_ASSERT_DEBUG(pi->PagedState()==SPageInfo::EUnpaged);
		return;
		}

	TRACE2(("DP: %O Rejuvenate PT 0x%08x 0x%08x",TheCurrentThread,pi->PhysAddr(),aPt));
	switch(pi->PagedState())
		{
	case SPageInfo::EPagedYoung:
	case SPageInfo::EPagedOld:
	case SPageInfo::EPagedOldestClean:
	case SPageInfo::EPagedOldestDirty:
		RemovePage(pi);
		AddAsYoungestPage(pi);
		BalanceAges();
		break;

	case SPageInfo::EUnpaged:
		AddAsYoungestPage(pi);
		BalanceAges();
		break;

	case SPageInfo::EPagedPinned:
		break;

	default:
		__NK_ASSERT_DEBUG(0);
		break;
		}
	}


TInt DPager::PteAndInfoFromLinAddr(	TInt aOsAsid, TLinAddr aAddress, DMemoryMappingBase* aMapping, 
									TUint aMapInstanceCount, TPte*& aPte, SPageInfo*& aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());	

	// Verify the mapping is still mapped and has not been reused.
	if (aMapInstanceCount != aMapping->MapInstanceCount() || aMapping->BeingDetached())
		return KErrAbort;

	aPte = Mmu::SafePtePtrFromLinAddr(aAddress,aOsAsid);
	if(!aPte)
		return KErrNotFound;

	TPte pte = *aPte;
	if(pte==KPteUnallocatedEntry)
		return KErrNotFound;

	SPageInfo* pi = SPageInfo::SafeFromPhysAddr(pte & ~KPageMask);
	if(!pi)
		return KErrNotFound;
	aPageInfo = pi;

	return KErrNone;
	}


TInt DPager::TryRejuvenate(	TInt aOsAsid, TLinAddr aAddress, TUint aAccessPermissions, TLinAddr aPc,
							DMemoryMappingBase* aMapping, TUint aMapInstanceCount, DThread* aThread, 
							TAny* aExceptionInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	START_PAGING_BENCHMARK;

	SPageInfo* pi;
	TPte* pPte;
	TPte pte;
	TInt r = PteAndInfoFromLinAddr(aOsAsid, aAddress, aMapping, aMapInstanceCount, pPte, pi);
	if (r != KErrNone)
		{
		if (aThread->IsRealtime())
			{// This thread is real time so it shouldn't be accessing paged out paged memory
			// unless there is a paging trap.
			MmuLock::Unlock();
			// Ensure that we abort when the thread is not allowed to access paged out pages.
			if (CheckRealtimeThreadFault(aThread, aExceptionInfo) != KErrNone)
				r = KErrAbort;
			MmuLock::Lock();
			}
		return r;
		}
	pte = *pPte;
	SPageInfo::TType type = pi->Type();
	SPageInfo::TPagedState state = pi->PagedState();

	if (aThread->IsRealtime() && 
		state != SPageInfo::EPagedPinned && 
		state != SPageInfo::EPagedPinnedMoved)
		{// This thread is real time so it shouldn't be accessing unpinned paged memory
		// unless there is a paging trap.
		MmuLock::Unlock();
		r = CheckRealtimeThreadFault(aThread, aExceptionInfo);
		MmuLock::Lock();
		if (r != KErrNone)
			return r;
		// We had to release the MmuLock have to reverify the status of the page and mappings.
		r = PteAndInfoFromLinAddr(aOsAsid, aAddress, aMapping, aMapInstanceCount, pPte, pi);
		if (r != KErrNone)
			return r;
		pte = *pPte;
		type = pi->Type();
		state = pi->PagedState();
		}

	if (type != SPageInfo::EManaged)
		return KErrNotFound;

	if(state==SPageInfo::EUnpaged)
		return KErrNotFound;

	DMemoryObject* memory = pi->Owner();
	TUint index = pi->Index();

	TPhysAddr page = memory->iPages.Page(index);
	if(!RPageArray::IsPresent(page))
		return KErrNotFound;

	TPhysAddr physAddr = pi->PhysAddr();
	if ((page^physAddr) >= (TPhysAddr)KPageSize)
		{// Page array entry should contain same physical address as PTE unless the 
		// page has or is being moved and this mapping accessed the page.
		// Get the page info for the page that we should be using.
		physAddr = page & ~KPageMask;
		pi = SPageInfo::SafeFromPhysAddr(physAddr);
		if(!pi)
			return KErrNotFound;

		type = pi->Type();
		if (type!=SPageInfo::EManaged)
			return KErrNotFound;

		state = pi->PagedState();
		if(state==SPageInfo::EUnpaged)
			return KErrNotFound;

		memory = pi->Owner();
		index = pi->Index();

		// Update pte to point to the correct physical address for this memory object's page.
		pte = (pte & KPageMask) | physAddr;
		}

	if(aAccessPermissions&EReadWrite)
		{// The mapping that took the fault permits writes and is still attached 
		// to the memory object therefore the object can't be read only.
		__NK_ASSERT_DEBUG(!memory->IsReadOnly());
		SetWritable(*pi);
		}

	pte = Mmu::MakePteAccessible(pte,aAccessPermissions&EReadWrite);
	TRACE2(("!PTE %x=%x",pPte,pte));
	*pPte = pte;
	CacheMaintenance::SinglePteUpdated((TLinAddr)pPte);
	InvalidateTLBForPage((aAddress&~KPageMask)|aOsAsid);

	Event(EEventPageRejuvenate,pi,aPc,aAddress,aAccessPermissions);

	TBool balance = false;
	if(	state==SPageInfo::EPagedYoung || state==SPageInfo::EPagedOld || 
		state==SPageInfo::EPagedOldestClean || state==SPageInfo::EPagedOldestDirty)
		{
		RemovePage(pi);
		AddAsYoungestPage(pi);
		// delay BalanceAges because we don't want to release MmuLock until after
		// RejuvenatePageTable has chance to look at the page table page...
		balance = true;
		}
	else
		{// Clear the modifier so that if this page is being moved then this 
		// access is detected. For non-pinned pages the modifier is cleared 
		// by RemovePage().
		__NK_ASSERT_DEBUG(state==SPageInfo::EPagedPinned);
		pi->SetModifier(0);
		}

	RejuvenatePageTable(pPte);

	if(balance)
		BalanceAges();

	END_PAGING_BENCHMARK(EPagingBmRejuvenate);
	return KErrNone;
	}


TInt DPager::PageInAllocPages(TPhysAddr* aPages, TUint aCount, Mmu::TRamAllocFlags aAllocFlags)
	{
	TUint n = 0;
	while(n<aCount)
		{
		SPageInfo* pi = PageInAllocPage(aAllocFlags);
		if(!pi)
			goto fail;
		aPages[n++] = pi->PhysAddr();
		}
	return KErrNone;
fail:
	PageInFreePages(aPages,n);
	return KErrNoMemory;
	}


void DPager::PageInFreePages(TPhysAddr* aPages, TUint aCount)
	{
	while(aCount--)
		{
		MmuLock::Lock();
		SPageInfo* pi = SPageInfo::FromPhysAddr(aPages[aCount]);
		switch(pi->PagedState())
			{
		case SPageInfo::EPagedYoung:
		case SPageInfo::EPagedOld:
		case SPageInfo::EPagedOldestClean:
		case SPageInfo::EPagedOldestDirty:
			RemovePage(pi);
			// fall through...
		case SPageInfo::EUnpaged:
			AddAsFreePage(pi);
			break;

		case SPageInfo::EPagedPinned:
			__NK_ASSERT_DEBUG(0);
			break;
		default:
			__NK_ASSERT_DEBUG(0);
			break;
			}
		MmuLock::Unlock();
		}
	}


void DPager::PagedInUnneeded(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	Event(EEventPageInUnneeded,aPageInfo);
	AddAsFreePage(aPageInfo);
	}


void DPager::PagedIn(SPageInfo* aPageInfo)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	switch(aPageInfo->PagedState())
		{
	case SPageInfo::EPagedYoung:
	case SPageInfo::EPagedOld:
	case SPageInfo::EPagedOldestClean:
	case SPageInfo::EPagedOldestDirty:
		RemovePage(aPageInfo);
		AddAsYoungestPage(aPageInfo);
		BalanceAges();
		break;

	case SPageInfo::EUnpaged:
		AddAsYoungestPage(aPageInfo);
		BalanceAges();
		break;

	case SPageInfo::EPagedPinned:
		// Clear the modifier so that if this page is being moved then this 
		// access is detected. For non-pinned pages the modifier is cleared by RemovePage().
		aPageInfo->SetModifier(0);
		break;

	default:
		__NK_ASSERT_DEBUG(0);
		break;
		}
	}


void DPager::PagedInPinned(SPageInfo* aPageInfo, TPinArgs& aPinArgs)
	{
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	Pin(aPageInfo,aPinArgs);
	}


void DPager::Pin(SPageInfo* aPageInfo, TPinArgs& aPinArgs)
	{
	__ASSERT_CRITICAL;
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(aPinArgs.HaveSufficientPages(1));

	aPageInfo->IncPinCount();
	Event(EEventPagePin,aPageInfo);

	// remove page from live list...
	switch(aPageInfo->PagedState())
		{
	case SPageInfo::EPagedYoung:
		__NK_ASSERT_DEBUG(iYoungCount);
		aPageInfo->iLink.Deque();
		--iYoungCount;
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()==1);
		break;

	case SPageInfo::EPagedOld:
		__NK_ASSERT_DEBUG(iOldCount);
		aPageInfo->iLink.Deque();
		--iOldCount;
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()==1);
		break;

	case SPageInfo::EPagedOldestClean:
		__NK_ASSERT_DEBUG(iOldestCleanCount);
		aPageInfo->iLink.Deque();
		--iOldestCleanCount;
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()==1);
		break;

	case SPageInfo::EPagedOldestDirty:
		__NK_ASSERT_DEBUG(iOldestDirtyCount);
		aPageInfo->iLink.Deque();
		--iOldestDirtyCount;
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()==1);
		break;

	case SPageInfo::EPagedPinned:
		// nothing more to do...
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()>1);
		return;

	case SPageInfo::EUnpaged:
		__NK_ASSERT_DEBUG(aPageInfo->PinCount()==1);
		TRACE2(("DPager::PinPage page was unpaged"));
		// This could be a page in the process of being stolen.
		// Could also be page for storing page table infos, which aren't necessarily
		// on the live list.
		break;

	default:
		__NK_ASSERT_DEBUG(0);
		return;
		}

	// page has now been removed from the live list and is pinned...
	aPageInfo->SetPagedState(SPageInfo::EPagedPinned);

	if(aPinArgs.iReplacementPages==TPinArgs::EUseReserveForPinReplacementPages)
		{
		// pinned paged counts as coming from reserve pool...
		aPageInfo->SetPinnedReserve();
		}
	else
		{
		// we used up a replacement page...
		--aPinArgs.iReplacementPages;
		}

	BalanceAges();
	}


void DPager::Unpin(SPageInfo* aPageInfo, TPinArgs& aPinArgs)
	{
	__ASSERT_CRITICAL;
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__NK_ASSERT_DEBUG(aPageInfo->PagedState()==SPageInfo::EPagedPinned);
	__NK_ASSERT_DEBUG(aPageInfo->PinCount()>0);

	TUint pinCount = aPageInfo->DecPinCount();
	Event(EEventPageUnpin,aPageInfo);

	if(pinCount)
		return;

	aPageInfo->SetPagedState(SPageInfo::EUnpaged);

	if(!aPageInfo->ClearPinnedReserve())
		{
		// was not a pinned reserve page, so we how have a spare replacement page,
		// which can be used again or freed later ...
		__NK_ASSERT_DEBUG(aPinArgs.iReplacementPages!=TPinArgs::EUseReserveForPinReplacementPages);
		++aPinArgs.iReplacementPages;
		}

	AddAsYoungestPage(aPageInfo);
	BalanceAges();
	}


TInt TPinArgs::AllocReplacementPages(TUint aNumPages)
	{
	if(iUseReserve)
		{
		__NK_ASSERT_DEBUG(iReplacementPages==0 || iReplacementPages==EUseReserveForPinReplacementPages);
		iReplacementPages = EUseReserveForPinReplacementPages;
		}
	else
		{
		if(aNumPages>iReplacementPages)
			{
			if(!ThePager.AllocPinReplacementPages(aNumPages-iReplacementPages))
				return KErrNoMemory;
			iReplacementPages = aNumPages;
			}
		}
	return KErrNone;
	}


void TPinArgs::FreeReplacementPages()
	{
	if(iReplacementPages!=0 && iReplacementPages!=EUseReserveForPinReplacementPages)
		ThePager.FreePinReplacementPages(iReplacementPages);
	iReplacementPages = 0;
	}


TBool DPager::AllocPinReplacementPages(TUint aNumPages)
	{
	TRACE2(("DPager::AllocPinReplacementPages(0x%x)",aNumPages));
	__ASSERT_CRITICAL;
	RamAllocLock::Lock();
	MmuLock::Lock();

	TBool ok = false;
	do
		{
		if(iNumberOfFreePages>=aNumPages)
			{
			iNumberOfFreePages -= aNumPages;
			ok = true;
			break;
			}
		}
	while(TryGrowLiveList());

	MmuLock::Unlock();
	RamAllocLock::Unlock();
	return ok;
	}


void DPager::FreePinReplacementPages(TUint aNumPages)
	{
	TRACE2(("DPager::FreePinReplacementPage(0x%x)",aNumPages));
	__ASSERT_CRITICAL;

	RamAllocLock::Lock();
	MmuLock::Lock();

	iNumberOfFreePages += aNumPages;
	RemoveExcessPages();

	MmuLock::Unlock();
	RamAllocLock::Unlock();
	}


TBool DPager::ReservePage()
	{
	__NK_ASSERT_DEBUG(RamAllocLock::IsHeld());
	__NK_ASSERT_DEBUG(MmuLock::IsHeld());
	__ASSERT_CRITICAL;
	__NK_ASSERT_DEBUG(iMinimumPageCount >= iMinimumPageLimit+iReservePageCount);
	while(iMinimumPageCount==iMinimumPageLimit+iReservePageCount && iNumberOfFreePages==0)
		{
		if(!TryGrowLiveList())
			return false;
		}
	if(iMinimumPageCount==iMinimumPageLimit+iReservePageCount)
		{
		++iMinimumPageCount;
		--iNumberOfFreePages;
		if(iMinimumPageCount>iMaximumPageCount)
			iMaximumPageCount = iMinimumPageCount;
		}
	++iReservePageCount;
	__NK_ASSERT_DEBUG(iMinimumPageCount >= iMinimumPageLimit+iReservePageCount);
	__NK_ASSERT_DEBUG(iMinimumPageCount+iNumberOfFreePages <= iMaximumPageCount);
	return ETrue;
	}


TBool DPager::ReservePages(TUint aRequiredCount, TUint& aCount)
	{
	__ASSERT_CRITICAL;

	RamAllocLock::Lock();
	MmuLock::Lock();
	while(aCount<aRequiredCount)
		{
		if(!ReservePage())
			break;
		++aCount;
		MmuLock::Flash();
		}
	TBool enoughPages = aCount==aRequiredCount;
	MmuLock::Unlock();
	RamAllocLock::Unlock();

	if(!enoughPages)
		UnreservePages(aCount);

	return enoughPages;
	}


void DPager::UnreservePages(TUint& aCount)
	{
	MmuLock::Lock();
	iReservePageCount -= aCount;
	aCount = 0;
	MmuLock::Unlock();
	}


TInt DPager::CheckRealtimeThreadFault(DThread* aThread, TAny* aExceptionInfo)
	{
	// realtime threads shouldn't take paging faults...
	DThread* client = aThread->iIpcClient;

	// If iIpcClient is set then we are accessing the address space of a remote thread.  If we are
	// in an IPC trap, this will contain information the local and remote addresses being accessed.
	// If this is not set then we assume than any fault must be the fault of a bad remote address.
	TIpcExcTrap* ipcTrap = (TIpcExcTrap*)aThread->iExcTrap;
	if (ipcTrap && !ipcTrap->IsTIpcExcTrap())
		ipcTrap = 0;
	if (client && (!ipcTrap || ipcTrap->ExcLocation(aThread, aExceptionInfo) == TIpcExcTrap::EExcRemote))
		{
		// kill client thread...
		if(K::IllegalFunctionForRealtimeThread(client,"Access to Paged Memory (by other thread)"))
			{
			// treat memory access as bad...
			return KErrAbort;
			}
		// else thread is in 'warning only' state so allow paging...
		}
	else
		{
		// kill current thread...
		if(K::IllegalFunctionForRealtimeThread(NULL,"Access to Paged Memory"))
			{
			// if current thread is in critical section, then the above kill will be deferred
			// and we will continue executing. We will handle this by returning an error
			// which means that the thread will take an exception (which hopefully is XTRAPed!)
			return KErrAbort;
			}
		// else thread is in 'warning only' state so allow paging...
		}
	return KErrNone;
	}


TInt DPager::HandlePageFault(	TLinAddr aPc, TLinAddr aFaultAddress, TUint aFaultAsid, TUint aFaultIndex,
								TUint aAccessPermissions, DMemoryObject* aMemory, DMemoryMapping* aMapping,
								TUint aMapInstanceCount, DThread* aThread, TAny* aExceptionInfo)
	{
	MmuLock::Lock();
	TInt r = TryRejuvenate(	aFaultAsid, aFaultAddress, aAccessPermissions, aPc, aMapping, aMapInstanceCount,
							aThread, aExceptionInfo);
	if(r == KErrNone || r == KErrAbort)
		{
		MmuLock::Unlock();
		}
	else
		{
		// rejuvenate failed, call memory manager to page in memory...
		Event(EEventPageInStart, 0, aPc, aFaultAddress, aAccessPermissions);
		MmuLock::Unlock();
		TheThrashMonitor.NotifyStartPaging();

		DMemoryManager* manager = aMemory->iManager;
		r = manager->HandleFault(aMemory, aFaultIndex, aMapping, aMapInstanceCount, aAccessPermissions);

		TheThrashMonitor.NotifyEndPaging();
		}
	return r;
	}


TInt DPager::ResizeLiveList()
	{
	MmuLock::Lock();
	TUint min = iMinimumPageCount;
	TUint max = iMaximumPageCount;
	MmuLock::Unlock();
	return ResizeLiveList(min,max);
	}


TInt DPager::ResizeLiveList(TUint aMinimumPageCount, TUint aMaximumPageCount)
	{
	TRACE(("DPager::ResizeLiveList(%d,%d) current young=%d old=%d min=%d free=%d max=%d",aMinimumPageCount,aMaximumPageCount,iYoungCount,iOldCount,iMinimumPageCount,iNumberOfFreePages,iMaximumPageCount));
	__NK_ASSERT_DEBUG(CacheInitialised());

	if(!aMaximumPageCount)
		{
		aMinimumPageCount = iInitMinimumPageCount;
		aMaximumPageCount = iInitMaximumPageCount;
		}
	if (aMaximumPageCount > KAbsoluteMaxPageCount)
		aMaximumPageCount = KAbsoluteMaxPageCount;

	// Min must not be greater than max...
	if(aMinimumPageCount>aMaximumPageCount)
		return KErrArgument;

	NKern::ThreadEnterCS();
	RamAllocLock::Lock();

	MmuLock::Lock();

	__NK_ASSERT_ALWAYS(iYoungOldRatio!=0);

	// Make sure aMinimumPageCount is not less than absolute minimum we can cope with...
	iMinimumPageLimit = iMinYoungPages * (1 + iYoungOldRatio) / iYoungOldRatio
						+ DPageReadRequest::ReservedPagesRequired();
	if(iMinimumPageLimit<iAbsoluteMinPageCount)
		iMinimumPageLimit = iAbsoluteMinPageCount;
	if(aMinimumPageCount<iMinimumPageLimit+iReservePageCount)
		aMinimumPageCount = iMinimumPageLimit+iReservePageCount;
	if(aMaximumPageCount<aMinimumPageCount)
		aMaximumPageCount=aMinimumPageCount;

	// Increase iMaximumPageCount?
	TInt extra = aMaximumPageCount-iMaximumPageCount;
	if(extra>0)
		iMaximumPageCount += extra;

	// Reduce iMinimumPageCount?
	TInt spare = iMinimumPageCount-aMinimumPageCount;
	if(spare>0)
		{
		iMinimumPageCount -= spare;
		iNumberOfFreePages += spare;
		}

	// Increase iMinimumPageCount?
	TInt r=KErrNone;
	while(iMinimumPageCount<aMinimumPageCount)
		{
		TUint newMin = aMinimumPageCount;
		TUint maxMin = iMinimumPageCount+iNumberOfFreePages;
		if(newMin>maxMin)
			newMin = maxMin;

		TUint delta = newMin-iMinimumPageCount;
		if(delta)
			{
			iMinimumPageCount = newMin;
			iNumberOfFreePages -= delta;
			continue;
			}

		if(!TryGrowLiveList())
			{
			r=KErrNoMemory;
			break;
			}
		}

	// Reduce iMaximumPageCount?
	while(iMaximumPageCount>aMaximumPageCount)
		{
		TUint newMax = aMaximumPageCount;
		TUint minMax = iMinimumPageCount+iNumberOfFreePages;
		if(newMax<minMax)
			newMax = minMax;

		TUint delta = iMaximumPageCount-newMax;
		if(delta)
			{
			iMaximumPageCount = newMax;
			continue;
			}

		ReturnPageToSystem();
		}

	TRACE(("DPager::ResizeLiveList end with young=%d old=%d min=%d free=%d max=%d",iYoungCount,iOldCount,iMinimumPageCount,iNumberOfFreePages,iMaximumPageCount));

#ifdef BTRACE_KERNEL_MEMORY
	BTrace4(BTrace::EKernelMemory,BTrace::EKernelMemoryDemandPagingCache,iMinimumPageCount << KPageShift);
#endif

	MmuLock::Unlock();

	RamAllocLock::Unlock();
	NKern::ThreadLeaveCS();

	return r;
	}


void DPager::FlushAll()
	{
	NKern::ThreadEnterCS();
	RamAllocLock::Lock();
	PageCleaningLock::Lock();

	TRACE(("DPager::FlushAll() live list young=%d old=%d min=%d free=%d max=%d",iYoungCount,iOldCount,iMinimumPageCount,iNumberOfFreePages,iMaximumPageCount));

	// look at all RAM pages in the system, and unmap all those used for paging
	const TUint32* piMap = (TUint32*)KPageInfoMap;
	const TUint32* piMapEnd = piMap+(KNumPageInfoPages>>5);
	SPageInfo* pi = (SPageInfo*)KPageInfoLinearBase;
	MmuLock::Lock();
	do
		{
		SPageInfo* piNext = pi+(KPageInfosPerPage<<5);
		for(TUint32 piFlags=*piMap++; piFlags; piFlags>>=1)
			{
			if(!(piFlags&1))
				{
				pi += KPageInfosPerPage;
				continue;
				}
			SPageInfo* piEnd = pi+KPageInfosPerPage;
			do
				{
				SPageInfo::TPagedState state = pi->PagedState();
				if (state==SPageInfo::EPagedYoung || state==SPageInfo::EPagedOld ||
					state==SPageInfo::EPagedOldestClean || state==SPageInfo::EPagedOldestDirty)
					{
					if (pi->Type() != SPageInfo::EUnused)
						{
						TInt r = StealPage(pi);
						if(r==KErrNone)
							AddAsFreePage(pi);
						MmuLock::Flash();
						}
					}
				++pi;
				if(((TUint)pi&(0xf<<KPageInfoShift))==0)
					MmuLock::Flash(); // every 16 page infos
				}
			while(pi<piEnd);
			}
		pi = piNext;
		}
	while(piMap<piMapEnd);
	MmuLock::Unlock();

	// reduce live page list to a minimum
	while(GetFreePages(1)) {}; 

	TRACE(("DPager::FlushAll() end with young=%d old=%d min=%d free=%d max=%d",iYoungCount,iOldCount,iMinimumPageCount,iNumberOfFreePages,iMaximumPageCount));

	PageCleaningLock::Unlock();
	RamAllocLock::Unlock();
	NKern::ThreadLeaveCS();
	}


void DPager::GetLiveListInfo(SVMCacheInfo& aInfo)
	{
	MmuLock::Lock(); // ensure consistent set of values are read...
	aInfo.iMinSize = iMinimumPageCount<<KPageShift;
	aInfo.iMaxSize = iMaximumPageCount<<KPageShift;
	aInfo.iCurrentSize = (iMinimumPageCount+iNumberOfFreePages)<<KPageShift;
	aInfo.iMaxFreeSize = iNumberOfFreePages<<KPageShift;
	MmuLock::Unlock();
	}


void DPager::GetEventInfo(SVMEventInfo& aInfoOut)
	{
	MmuLock::Lock(); // ensure consistent set of values are read...
	aInfoOut = iEventInfo;
	MmuLock::Unlock();
	}


void DPager::ResetEventInfo()
	{
	MmuLock::Lock();
	memclr(&iEventInfo, sizeof(iEventInfo));
	MmuLock::Unlock();
	}


TInt TestPageState(TLinAddr aAddr)
	{
	DMemModelProcess* process = (DMemModelProcess*)TheCurrentThread->iOwningProcess;
	// Get the os asid of current thread's process so no need to open a reference on it.
	TInt osAsid = process->OsAsid();
	TPte* ptePtr = 0;
	TPte pte = 0;
	TInt r = 0;
	SPageInfo* pageInfo = NULL;

	NKern::ThreadEnterCS();

	TUint offsetInMapping;
	TUint mapInstanceCount;
	DMemoryMapping* mapping = MM::FindMappingInAddressSpace(osAsid, aAddr, 1, offsetInMapping, mapInstanceCount);

	MmuLock::Lock();

	if(mapping)
		{
		DMemoryObject* memory = mapping->Memory();
		if(mapInstanceCount == mapping->MapInstanceCount() && memory)
			{
			DMemoryManager* manager = memory->iManager;
			if(manager==TheCodePagedMemoryManager)
				r |= EPageStateInRamCode|EPageStatePaged;
			}
		}

	ptePtr = Mmu::SafePtePtrFromLinAddr(aAddr,osAsid);
	if (!ptePtr)
		goto done;
	pte = *ptePtr;
	if (pte == KPteUnallocatedEntry)
		goto done;		
	r |= EPageStatePtePresent;
	if (pte!=Mmu::MakePteInaccessible(pte,0))
		r |= EPageStatePteValid;
	
	pageInfo = SPageInfo::SafeFromPhysAddr(pte&~KPageMask);
	if(pageInfo)
		{
		r |= pageInfo->Type();
		r |= pageInfo->PagedState()<<8;
		}
done:
	MmuLock::Unlock();
	if(mapping)
		mapping->Close();
	NKern::ThreadLeaveCS();
	return r;
	}



TInt VMHalFunction(TAny*, TInt aFunction, TAny* a1, TAny* a2)
	{
	switch(aFunction)
		{
	case EVMHalFlushCache:
		if(!TheCurrentThread->HasCapability(ECapabilityWriteDeviceData,__PLATSEC_DIAGNOSTIC_STRING("Checked by VMHalFunction(EVMHalFlushCache)")))
			K::UnlockedPlatformSecurityPanic();
		ThePager.FlushAll();
		return KErrNone;

	case EVMHalSetCacheSize:
		{
		if(!TheCurrentThread->HasCapability(ECapabilityWriteDeviceData,__PLATSEC_DIAGNOSTIC_STRING("Checked by VMHalFunction(EVMHalSetCacheSize)")))
			K::UnlockedPlatformSecurityPanic();
		TUint min = TUint(a1)>>KPageShift;
		if(TUint(a1)&KPageMask)
			++min;
		TUint max = TUint(a2)>>KPageShift;
		if(TUint(a2)&KPageMask)
			++max;
		return ThePager.ResizeLiveList(min,max);
		}

	case EVMHalGetCacheSize:
		{
		SVMCacheInfo info;
		ThePager.GetLiveListInfo(info);
		kumemput32(a1,&info,sizeof(info));
		}
		return KErrNone;

	case EVMHalGetEventInfo:
		{
		SVMEventInfo info;
		ThePager.GetEventInfo(info);
		Kern::InfoCopy(*(TDes8*)a1,(TUint8*)&info,sizeof(info));
		}
		return KErrNone;

	case EVMHalResetEventInfo:
		ThePager.ResetEventInfo();
		return KErrNone;

#ifdef __SUPPORT_DEMAND_PAGING_EMULATION__
	case EVMHalGetOriginalRomPages:
		RomOriginalPages(*((TPhysAddr**)a1), *((TUint*)a2));
		return KErrNone;
#endif

	case EVMPageState:
		return TestPageState((TLinAddr)a1);

	case EVMHalGetSwapInfo:
		{
		if ((K::MemModelAttributes & EMemModelAttrDataPaging) == 0)
			return KErrNotSupported;
		SVMSwapInfo info;
		GetSwapInfo(info);
		kumemput32(a1,&info,sizeof(info));
		}
		return KErrNone;

	case EVMHalGetThrashLevel:
		return TheThrashMonitor.ThrashLevel();

	case EVMHalSetSwapThresholds:
		{
		if(!TheCurrentThread->HasCapability(ECapabilityWriteDeviceData,__PLATSEC_DIAGNOSTIC_STRING("Checked by VMHalFunction(EVMHalSetSwapThresholds)")))
			K::UnlockedPlatformSecurityPanic();
		if ((K::MemModelAttributes & EMemModelAttrDataPaging) == 0)
			return KErrNotSupported;
		SVMSwapThresholds thresholds;
		kumemget32(&thresholds,a1,sizeof(thresholds));
		return SetSwapThresholds(thresholds);
		}

	case EVMHalSetThrashThresholds:
		if(!TheCurrentThread->HasCapability(ECapabilityWriteDeviceData,__PLATSEC_DIAGNOSTIC_STRING("Checked by VMHalFunction(EVMHalSetThrashThresholds)")))
			K::UnlockedPlatformSecurityPanic();
		return TheThrashMonitor.SetThresholds((TUint)a1, (TUint)a2);

#ifdef __DEMAND_PAGING_BENCHMARKS__
	case EVMHalGetPagingBenchmark:
		{
		TUint index = (TInt) a1;
		if (index >= EMaxPagingBm)
			return KErrNotFound;
		SPagingBenchmarkInfo info;
		ThePager.ReadBenchmarkData((TPagingBenchmark)index, info);
		kumemput32(a2,&info,sizeof(info));
		}		
		return KErrNone;
		
	case EVMHalResetPagingBenchmark:
		{
		TUint index = (TInt) a1;
		if (index >= EMaxPagingBm)
			return KErrNotFound;
		ThePager.ResetBenchmarkData((TPagingBenchmark)index);
		}
		return KErrNone;
#endif

	default:
		return KErrNotSupported;
		}
	}


#ifdef __DEMAND_PAGING_BENCHMARKS__

void DPager::ResetBenchmarkData(TPagingBenchmark aBm)
    {
    SPagingBenchmarkInfo& info = iBenchmarkInfo[aBm];
	__SPIN_LOCK_IRQ(iBenchmarkLock);
    info.iCount = 0;
    info.iTotalTime = 0;
    info.iMaxTime = 0;
    info.iMinTime = KMaxTInt;
	__SPIN_UNLOCK_IRQ(iBenchmarkLock);
    }
 
void DPager::RecordBenchmarkData(TPagingBenchmark aBm, TUint32 aStartTime, TUint32 aEndTime, TUint aCount)
    {
    SPagingBenchmarkInfo& info = iBenchmarkInfo[aBm];
#if !defined(HIGH_RES_TIMER) || defined(HIGH_RES_TIMER_COUNTS_UP)
    TInt64 elapsed = aEndTime - aStartTime;
#else
    TInt64 elapsed = aStartTime - aEndTime;
#endif
	__SPIN_LOCK_IRQ(iBenchmarkLock);
    info.iCount +=  aCount;
    info.iTotalTime += elapsed;
    if (elapsed > info.iMaxTime)
        info.iMaxTime = elapsed;
    if (elapsed < info.iMinTime)
        info.iMinTime = elapsed;
	__SPIN_UNLOCK_IRQ(iBenchmarkLock);
    }

void DPager::ReadBenchmarkData(TPagingBenchmark aBm, SPagingBenchmarkInfo& aDataOut)
	{
	__SPIN_LOCK_IRQ(iBenchmarkLock);
	aDataOut = iBenchmarkInfo[aBm];
	__SPIN_UNLOCK_IRQ(iBenchmarkLock);
	}

#endif //__DEMAND_PAGING_BENCHMARKS__


//
// Paging request management...
//

//
// DPagingRequest
//

DPagingRequest::DPagingRequest()
	: iMutex(NULL), iUseRegionCount(0)
	{
	}


void DPagingRequest::SetUseContiguous(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	__ASSERT_SYSTEM_LOCK;
	__NK_ASSERT_DEBUG(iUseRegionCount == 0);
	__NK_ASSERT_DEBUG(aCount > 0 && aCount <= EMaxPages);
	for (TUint i = 0 ; i < aCount ; ++i)
		{
		iUseRegionMemory[i] = aMemory;
		iUseRegionIndex[i] = aIndex + i;		
		}
	iUseRegionCount = aCount;
	}


void DPagingRequest::SetUseDiscontiguous(DMemoryObject** aMemory, TUint* aIndex, TUint aCount)
	{
	__ASSERT_SYSTEM_LOCK;
	__NK_ASSERT_DEBUG(iUseRegionCount == 0);
	__NK_ASSERT_DEBUG(aCount > 0 && aCount <= EMaxPages);
	for (TUint i = 0 ; i < aCount ; ++i)
		{
		iUseRegionMemory[i] = aMemory[i];
		iUseRegionIndex[i] = aIndex[i];
		}
	iUseRegionCount = aCount;
	}


void DPagingRequest::ResetUse()
	{
	__ASSERT_SYSTEM_LOCK;
	__NK_ASSERT_DEBUG(iUseRegionCount > 0);
	iUseRegionCount = 0;
	}


TBool DPagingRequest::CheckUseContiguous(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	if (iUseRegionCount != aCount)
		return EFalse;
	for (TUint i = 0 ; i < iUseRegionCount ; ++i)
		{
		if (iUseRegionMemory[i] != aMemory || iUseRegionIndex[i] != aIndex + i)
			return EFalse;
		}
	return ETrue;
	}


TBool DPagingRequest::CheckUseDiscontiguous(DMemoryObject** aMemory, TUint* aIndex, TUint aCount)
	{
	if (iUseRegionCount != aCount)
		return EFalse;
	for (TUint i = 0 ; i < iUseRegionCount ; ++i)
		{
		if (iUseRegionMemory[i] != aMemory[i] || iUseRegionIndex[i] != aIndex[i])
			return EFalse;
		}
	return ETrue;
	}


 TBool DPagingRequest::IsCollisionContiguous(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	// note this could be optimised as most of the time we will be checking read/read collusions,
	// both of which will be contiguous
	__ASSERT_SYSTEM_LOCK;
	for (TUint i = 0 ; i < iUseRegionCount ; ++i)
		{
		if (iUseRegionMemory[i] == aMemory &&
			TUint(iUseRegionIndex[i] - aIndex) < aCount)
			return ETrue;
		}
	return EFalse;
	}


TLinAddr DPagingRequest::MapPages(TUint aColour, TUint aCount, TPhysAddr* aPages)
	{
	__NK_ASSERT_DEBUG(iMutex->iCleanup.iThread == &Kern::CurrentThread());
	return iTempMapping.Map(aPages,aCount,aColour);
	}


void DPagingRequest::UnmapPages(TBool aIMBRequired)
	{
	__NK_ASSERT_DEBUG(iMutex->iCleanup.iThread == &Kern::CurrentThread());
	iTempMapping.Unmap(aIMBRequired);
	}

//
// DPoolPagingRequest
//

DPoolPagingRequest::DPoolPagingRequest(DPagingRequestPool::TGroup& aPoolGroup) :
	iPoolGroup(aPoolGroup)
	{
	}


void DPoolPagingRequest::Release()
	{
	NKern::LockSystem();
	ResetUse();
	Signal();
	}


void DPoolPagingRequest::Wait()
	{
	__ASSERT_SYSTEM_LOCK;
	++iUsageCount;
	TInt r = iMutex->Wait();
	__NK_ASSERT_ALWAYS(r == KErrNone);
	}


void DPoolPagingRequest::Signal()
	{
	__ASSERT_SYSTEM_LOCK;
	iPoolGroup.Signal(this);
	}

//
// DPageReadRequest
//

TInt DPageReadRequest::iAllocNext = 0;

DPageReadRequest::DPageReadRequest(DPagingRequestPool::TGroup& aPoolGroup) :
	DPoolPagingRequest(aPoolGroup)
	{
	// allocate space for mapping pages whilst they're being loaded...
	iTempMapping.Alloc(EMaxPages);
	}

TInt DPageReadRequest::Construct()
	{
	// allocate id and mutex...
	TUint id = (TUint)__e32_atomic_add_ord32(&iAllocNext, 1);
	_LIT(KLitPagingRequest,"PageReadRequest-");
	TBuf<sizeof("PageReadRequest-")+10> mutexName(KLitPagingRequest);
	mutexName.AppendNum(id);
	TInt r = K::MutexCreate(iMutex, mutexName, NULL, EFalse, KMutexOrdPageIn);
	if(r!=KErrNone)
		return r;

	// create memory buffer...
	TUint bufferSize = EMaxPages+1;
	DMemoryObject* bufferMemory;
	r = MM::MemoryNew(bufferMemory,EMemoryObjectUnpaged,bufferSize,EMemoryCreateNoWipe);
	if(r!=KErrNone)
		return r;
	MM::MemorySetLock(bufferMemory,iMutex);
	TPhysAddr physAddr;
	r = MM::MemoryAllocContiguous(bufferMemory,0,bufferSize,0,physAddr);
	(void)physAddr;
	if(r!=KErrNone)
		return r;
	DMemoryMapping* bufferMapping;
	r = MM::MappingNew(bufferMapping,bufferMemory,ESupervisorReadWrite,KKernelOsAsid);
	if(r!=KErrNone)
		return r;
	iBuffer = MM::MappingBase(bufferMapping);

	return r;
	}


//
// DPageWriteRequest
//


DPageWriteRequest::DPageWriteRequest()
	{
	iMutex = ThePageCleaningLock;
	// allocate space for mapping pages whilst they're being loaded...
	iTempMapping.Alloc(KMaxPagesToClean);
	}


void DPageWriteRequest::Release()
	{
	NKern::LockSystem();
	ResetUse();
	NKern::UnlockSystem();
	}


//
// DPagingRequestPool
//

DPagingRequestPool::DPagingRequestPool(TUint aNumPageReadRequest, TBool aWriteRequest)
	: iPageReadRequests(aNumPageReadRequest)
	{
	TUint i;
	for(i=0; i<aNumPageReadRequest; ++i)
		{
		DPageReadRequest* req = new DPageReadRequest(iPageReadRequests);
		__NK_ASSERT_ALWAYS(req);
		TInt r = req->Construct();
		__NK_ASSERT_ALWAYS(r==KErrNone);
		iPageReadRequests.iRequests[i] = req;
		iPageReadRequests.iFreeList.Add(req);
		}

	if (aWriteRequest)
		{
		iPageWriteRequest = new DPageWriteRequest();
		__NK_ASSERT_ALWAYS(iPageWriteRequest);
		}
	}


DPagingRequestPool::~DPagingRequestPool()
	{
	__NK_ASSERT_ALWAYS(0); // deletion not implemented
	}


DPageReadRequest* DPagingRequestPool::AcquirePageReadRequest(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	NKern::LockSystem();

	DPoolPagingRequest* req;
	
	// check for collision with existing write
	if(iPageWriteRequest && iPageWriteRequest->IsCollisionContiguous(aMemory,aIndex,aCount))
		{
		NKern::UnlockSystem();
		PageCleaningLock::Lock();
		PageCleaningLock::Unlock();
		return 0; // caller expected to retry if needed
		}

	// get a request object to use...
	req = iPageReadRequests.GetRequest(aMemory,aIndex,aCount);

	// check no new read or write requests collide with us...
	if ((iPageWriteRequest && iPageWriteRequest->IsCollisionContiguous(aMemory,aIndex,aCount)) ||
		iPageReadRequests.FindCollisionContiguous(aMemory,aIndex,aCount))
		{
		// another operation is colliding with this region, give up and retry...
		req->Signal();
		return 0; // caller expected to retry if needed
		}

	// we have a request object which we can use...
	req->SetUseContiguous(aMemory,aIndex,aCount);

	NKern::UnlockSystem();
	return (DPageReadRequest*)req;
	}


DPageWriteRequest* DPagingRequestPool::AcquirePageWriteRequest(DMemoryObject** aMemory, TUint* aIndex, TUint aCount)
	{
	__NK_ASSERT_DEBUG(iPageWriteRequest);
	__NK_ASSERT_DEBUG(PageCleaningLock::IsHeld());

	NKern::LockSystem();

	// Collision with existing read requests is not possible here.  For a page to be read it must
	// not be present, and for it to be written it must be present and dirty.  There is no way for a
	// page to go between these states without an intervening read on an uninitialised (freshly
	// committed) page, which will wait on the first read request.  In other words something like
	// this:
	//
	//   read (blocks), decommit, re-commit, read (waits on mutex), write (now no pending reads!)
	//
	// Note that a read request can be outstanding and appear to collide with this write, but only
	// in the case when the thread making the read has blocked just after acquiring the request but
	// before it checks whether the read is still necessasry.  This makes it difficult to assert
	// that no collisions take place.
	
	iPageWriteRequest->SetUseDiscontiguous(aMemory,aIndex,aCount);
	NKern::UnlockSystem();
	
	return iPageWriteRequest;
	}


DPagingRequestPool::TGroup::TGroup(TUint aNumRequests)
	{
	iNumRequests = aNumRequests;
	iRequests = new DPoolPagingRequest*[aNumRequests];
	__NK_ASSERT_ALWAYS(iRequests);
	}


DPoolPagingRequest* DPagingRequestPool::TGroup::FindCollisionContiguous(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	__ASSERT_SYSTEM_LOCK;
	DPoolPagingRequest** ptr = iRequests;
	DPoolPagingRequest** ptrEnd = ptr+iNumRequests;
	while(ptr<ptrEnd)
		{
		DPoolPagingRequest* req = *ptr++;
		if(req->IsCollisionContiguous(aMemory,aIndex,aCount))
			return req;
		}
	return 0;
	}


static TUint32 RandomSeed = 33333;

DPoolPagingRequest* DPagingRequestPool::TGroup::GetRequest(DMemoryObject* aMemory, TUint aIndex, TUint aCount)
	{
	__NK_ASSERT_DEBUG(iNumRequests > 0);

	// try using an existing request which collides with this region...
	DPoolPagingRequest* req  = FindCollisionContiguous(aMemory,aIndex,aCount);
	if(!req)
		{
		// use a free request...
		req = (DPoolPagingRequest*)iFreeList.GetFirst();
		if(req)
			{
			// free requests aren't being used...
			__NK_ASSERT_DEBUG(req->iUsageCount == 0);
			}
		else
			{
			// pick a random request...
			RandomSeed = RandomSeed*69069+1; // next 'random' number
			TUint index = (TUint64(RandomSeed) * TUint64(iNumRequests)) >> 32;
			req = iRequests[index];
			__NK_ASSERT_DEBUG(req->iUsageCount > 0); // we only pick random when none are free
			}
		}

	// wait for chosen request object...
	req->Wait();

	return req;
	}


void DPagingRequestPool::TGroup::Signal(DPoolPagingRequest* aRequest)
	{
	// if there are no threads waiting on the mutex then return it to the free pool...
	__NK_ASSERT_DEBUG(aRequest->iUsageCount > 0);
	if (--aRequest->iUsageCount==0)
		iFreeList.AddHead(aRequest);

	aRequest->iMutex->Signal();
	}


/**
Register the specified paging device with the kernel.

@param aDevice	A pointer to the paging device to install

@return KErrNone on success
*/
EXPORT_C TInt Kern::InstallPagingDevice(DPagingDevice* aDevice)
	{
	TRACEB(("Kern::InstallPagingDevice(0x%08x) name='%s' type=%d",aDevice,aDevice->iName,aDevice->iType));

	__NK_ASSERT_DEBUG(!ThePager.CacheInitialised());
	__NK_ASSERT_ALWAYS(aDevice->iReadUnitShift <= KPageShift);

	TInt r = KErrNotSupported;	// Will return this if unsupported device type is installed

	// create the pools of page out and page in requests...
	const TBool writeReq = (aDevice->iType & DPagingDevice::EData) != 0;
	aDevice->iRequestPool = new DPagingRequestPool(KPagingRequestsPerDevice, writeReq);
	if(!aDevice->iRequestPool)
		{
		r = KErrNoMemory;
		goto exit;
		}

	if(aDevice->iType & DPagingDevice::ERom)
		{
		r = TheRomMemoryManager->InstallPagingDevice(aDevice);
		if(r!=KErrNone)
			goto exit;
		}

	if(aDevice->iType & DPagingDevice::ECode)
		{
		r = TheCodePagedMemoryManager->InstallPagingDevice(aDevice);
		if(r!=KErrNone)
			goto exit;
		}

	if(aDevice->iType & DPagingDevice::EData)
		{
		r = TheDataPagedMemoryManager->InstallPagingDevice(aDevice);
		if(r!=KErrNone)
			goto exit;
		}

 	if (K::MemModelAttributes & (EMemModelAttrRomPaging | EMemModelAttrCodePaging | EMemModelAttrDataPaging))
		TheThrashMonitor.Start();
	
 	if (K::MemModelAttributes & EMemModelAttrDataPaging)
		PageCleaner::Start();

exit:
	TRACEB(("Kern::InstallPagingDevice returns %d",r));
	return r;
	}



//
// DDemandPagingLock
//

EXPORT_C DDemandPagingLock::DDemandPagingLock()
	: iReservedPageCount(0), iLockedPageCount(0), iPinMapping(0)
	{
	}


EXPORT_C TInt DDemandPagingLock::Alloc(TInt aSize)
	{
	TRACEP(("DDemandPagingLock[0x%08x]::Alloc(0x%x)",this,aSize));
	iMaxPageCount = ((aSize-1+KPageMask)>>KPageShift)+1;

	TInt r = KErrNoMemory;

	NKern::ThreadEnterCS();

	TUint maxPt = DVirtualPinMapping::MaxPageTables(iMaxPageCount);
	// Note, we need to reserve whole pages even for page tables which are smaller
	// because pinning can remove the page from live list...
	TUint reserve = iMaxPageCount+maxPt*KNumPagesToPinOnePageTable;
	if(ThePager.ReservePages(reserve,(TUint&)iReservedPageCount))
		{
		iPinMapping = DVirtualPinMapping::New(iMaxPageCount);
		if(iPinMapping)
			r = KErrNone;
		else
			ThePager.UnreservePages((TUint&)iReservedPageCount);
		}

	NKern::ThreadLeaveCS();
	TRACEP(("DDemandPagingLock[0x%08x]::Alloc returns %d, iMaxPageCount=%d, iReservedPageCount=%d",this,r,iMaxPageCount,iReservedPageCount));
	return r;
	}


EXPORT_C void DDemandPagingLock::Free()
	{
	TRACEP(("DDemandPagingLock[0x%08x]::Free()"));
	Unlock();
	NKern::ThreadEnterCS();
	DVirtualPinMapping* pinMapping = (DVirtualPinMapping*)__e32_atomic_swp_ord_ptr(&iPinMapping, 0);
	if (pinMapping)
		pinMapping->Close();
	NKern::ThreadLeaveCS();
	ThePager.UnreservePages((TUint&)iReservedPageCount);
	}


EXPORT_C TInt DDemandPagingLock::Lock(DThread* aThread, TLinAddr aStart, TInt aSize)
	{
//	TRACEP(("DDemandPagingLock[0x%08x]::Lock(0x%08x,0x%08x,0x%08x)",this,aThread,aStart,aSize));
	if(iLockedPageCount)
		__NK_ASSERT_ALWAYS(0); // lock already used

	// calculate the number of pages that need to be locked...
	TUint mask=KPageMask;
	TUint offset=aStart&mask;
	TInt numPages = (aSize+offset+mask)>>KPageShift;
	if(numPages>iMaxPageCount)
		__NK_ASSERT_ALWAYS(0);

	NKern::ThreadEnterCS();

	// find mapping which covers the specified region...
	TUint offsetInMapping;
	TUint mapInstanceCount;
	DMemoryMapping* mapping = MM::FindMappingInThread((DMemModelThread*)aThread, aStart, aSize, offsetInMapping, mapInstanceCount);
	if(!mapping)
		{
		NKern::ThreadLeaveCS();
		return KErrBadDescriptor;
		}

	MmuLock::Lock(); 
	DMemoryObject* memory = mapping->Memory();
	if(mapInstanceCount != mapping->MapInstanceCount() || !memory)
		{// Mapping has been reused or no memory.
		MmuLock::Unlock();
		mapping->Close();
		NKern::ThreadLeaveCS();
		return KErrBadDescriptor;
		}

	if(!memory->IsDemandPaged())
		{
		// memory not demand paged, so we have nothing to do...
		MmuLock::Unlock();
		mapping->Close();
		NKern::ThreadLeaveCS();
		return KErrNone;
		}

	// Open a reference on the memory so it doesn't get deleted.
	memory->Open();
	MmuLock::Unlock();

	// pin memory...
	TUint index = (offsetInMapping>>KPageShift)+mapping->iStartIndex;
	TUint count = ((offsetInMapping&KPageMask)+aSize+KPageMask)>>KPageShift;
	TInt r = ((DVirtualPinMapping*)iPinMapping)->Pin(	memory,index,count,mapping->Permissions(),
														mapping, mapInstanceCount);

	if(r==KErrNotFound)
		{
		// some memory wasn't present, so treat this as an error...
		memory->Close();
		mapping->Close();
		NKern::ThreadLeaveCS();
		return KErrBadDescriptor;
		}

	// we can't fail to pin otherwise...
	__NK_ASSERT_DEBUG(r!=KErrNoMemory); // separate OOM assert to aid debugging
	__NK_ASSERT_ALWAYS(r==KErrNone);

	// indicate that we have actually pinned...
	__NK_ASSERT_DEBUG(iLockedPageCount==0);
	iLockedPageCount = count;

	// cleanup...
	memory->Close();
	mapping->Close();
	NKern::ThreadLeaveCS();

	return 1;
	}


EXPORT_C void DDemandPagingLock::DoUnlock()
	{
	NKern::ThreadEnterCS();
	((DVirtualPinMapping*)iPinMapping)->Unpin();
	__NK_ASSERT_DEBUG(iLockedPageCount);
	iLockedPageCount = 0;
	NKern::ThreadLeaveCS();
	}



//
// PageCleaningLock
//

_LIT(KLitPageCleaningLock,"PageCleaningLock");

void PageCleaningLock::Init()
	{
	__NK_ASSERT_DEBUG(!ThePageCleaningLock);
	TInt r = Kern::MutexCreate(ThePageCleaningLock, KLitPageCleaningLock, KMutexOrdPageOut);
	__NK_ASSERT_ALWAYS(r == KErrNone);
	}

void PageCleaningLock::Lock()
	{
	Kern::MutexWait(*ThePageCleaningLock);
	}


void PageCleaningLock::Unlock()
	{
	Kern::MutexSignal(*ThePageCleaningLock);
	}

TBool PageCleaningLock::IsHeld()
	{
	return ThePageCleaningLock->iCleanup.iThread == &Kern::CurrentThread();
	}
