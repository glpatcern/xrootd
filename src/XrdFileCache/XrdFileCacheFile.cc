//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------


#include "XrdFileCacheFile.hh"
#include "XrdFileCacheIO.hh"
#include "XrdFileCacheTrace.hh"

#include <stdio.h>
#include <sstream>
#include <fcntl.h>
#include <assert.h>
#include "XrdCl/XrdClLog.hh"
#include "XrdCl/XrdClConstants.hh"
#include "XrdCl/XrdClFile.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSys/XrdSysTimer.hh"
#include "XrdOss/XrdOss.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "Xrd/XrdScheduler.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdPosix/XrdPosixFile.hh"
#include "XrdPosix/XrdPosix.hh"
#include "XrdFileCache.hh"
#include "Xrd/XrdScheduler.hh"

using namespace XrdFileCache;


namespace XrdPosixGlobals
{
   extern XrdScheduler *schedP;
}

namespace
{
   const int PREFETCH_MAX_ATTEMPTS = 10;

   class DiskSyncer : public XrdJob
   {
   private:
      File *m_file;
   public:
      DiskSyncer(File *pref, const char *desc="") :
         XrdJob(desc),
         m_file(pref)
      {}
      void DoIt()
      {
         m_file->Sync();
      }
   };
}

namespace
{
   Cache* cache() { return &Cache::GetInstance(); }
}

File::File(IO *io, std::string& disk_file_path, long long iOffset, long long iFileSize) :
m_io(io),
m_output(NULL),
m_infoFile(NULL),
m_cfi(Cache::GetInstance().GetTrace(), Cache::GetInstance().RefConfiguration().m_prefetch_max_blocks > 0),
m_temp_filename(disk_file_path),
m_offset(iOffset),
m_fileSize(iFileSize),
m_stateCond(0), // We will explicitly lock the condition before use.
m_syncer(new DiskSyncer(this, "XrdFileCache::DiskSyncer")),
m_non_flushed_cnt(0),
m_in_sync(false),
m_downloadCond(0),
m_prefetchState(kOn),
m_prefetchReadCnt(0),
m_prefetchHitCnt(0),
m_prefetchScore(1),
m_traceID("File")
{
   Open();
}

void File::BlockRemovedFromWriteQ(Block* b)
{
 m_downloadCond.Lock();
 dec_ref_count(b);
 TRACEF(Dump, "File::BlockRemovedFromWriteQ() check write queues block = " << (void*)b << " idx= " << b->m_offset/m_cfi.GetBufferSize());
 m_downloadCond.UnLock();
}

File::~File()
{
   if (m_infoFile)
   {
      m_syncStatusMutex.Lock();

      bool needs_sync = ! m_writes_during_sync.empty();
      m_syncStatusMutex.UnLock();
      if (needs_sync || m_non_flushed_cnt > 0)
      {
         Sync();
         m_cfi.WriteHeader(m_infoFile);
      }

      // write statistics in *cinfo file
      AppendIOStatToFileInfo();
      m_infoFile->Fsync();

      m_syncStatusMutex.UnLock();


      m_infoFile->Close();
      delete m_infoFile;
      m_infoFile = NULL;
   }

   if (m_output)
   {
      m_output->Close();
      delete m_output;
      m_output = NULL;
   }

   delete m_syncer; 
   m_syncer = NULL;

   // print just for curiosity
   TRACEF(Debug, "File::~File() ended, prefetch score = " <<  m_prefetchScore);
}

bool File::ioActive()
{
   // Retruns true if delay is needed
   
   TRACEF(Debug, "File::Initiate close start");

   m_stateCond.Lock();
   if (m_prefetchState != kStopped) {
      m_prefetchState = kStopped;
      cache()->DeRegisterPrefetchFile(this);
   }

   m_stateCond.UnLock();


   // remove failed blocks and check if map is empty
   m_downloadCond.Lock();
   /*      
   // high debug print 
   for (BlockMap_i it = m_block_map.begin(); it != m_block_map.end(); ++it) {
   Block* b = it->second;
   TRACEF(Dump, "File::InitiateClose() block idx = " <<  b->m_offset/m_cfi.GetBufferSize() << " prefetch = " << b->preferch <<  " refcnt " << b->refcnt);

   }
   */
   BlockMap_i itr = m_block_map.begin();
   while (itr != m_block_map.end()) {
      if (itr->second->is_failed() && itr->second->m_refcnt == 1) {
         BlockMap_i toErase = itr;
         ++itr;
         TRACEF(Debug, "Remove failed block " <<  itr->second->m_offset/m_cfi.GetBufferSize());
         free_block(toErase->second);
      }
      else {
         ++itr;
      }
   }

   bool blockMapEmpty =  m_block_map.empty();
   m_downloadCond.UnLock();

   if ( blockMapEmpty)
   {
      // file is not active when block map is empty and sync is done
      XrdSysMutexHelper _lck(&m_syncStatusMutex);
      if (m_in_sync == false) {
         return false;
      }
   }

   return true;
}

//______________________________________________________________________________

void File::WakeUp()
{
   // called if this object is recycled by other IO
   m_stateCond.Lock();
   if (m_prefetchState != kComplete) m_prefetchState = kOn;
   m_stateCond.UnLock();
}

//==============================================================================

bool File::Open()
{
   TRACEF(Dump, "File::Open() open file for disk cache ");

   XrdOss  &m_output_fs =  *Cache::GetInstance().GetOss();
   // Create the data file itself.
   XrdOucEnv myEnv;
   if ( m_output_fs.Create(Cache::GetInstance().RefConfiguration().m_username.c_str(), m_temp_filename.c_str(), 0600, myEnv, XRDOSS_mkpath) !=XrdOssOK)
   { 
      TRACEF(Error, "File::Open() can't create data file, " << strerror(errno));
      return false;
   }

   m_output = m_output_fs.newFile(Cache::GetInstance().RefConfiguration().m_username.c_str());
   if (m_output->Open(m_temp_filename.c_str(), O_RDWR, 0600, myEnv) != XrdOssOK)
   {
      TRACEF(Error, "File::Open() can't get FD for data file, " << strerror(errno));
      delete m_output;
      m_output = 0;
      return false;
   }
   

   // Create the info file
   std::string ifn = m_temp_filename + Info::m_infoExtension;

   struct stat infoStat;
   bool fileExisted = (Cache::GetInstance().GetOss()->Stat(ifn.c_str(), &infoStat) == XrdOssOK);

   // AMT: the folowing below is a sanity check, it is not expected to happen. Could be an assert
   if (fileExisted && (infoStat.st_size == 0)) {
      TRACEF(Error, "File::Open() info file stored zero data file size");
      return false;
   }

   if (m_output_fs.Create(Cache::GetInstance().RefConfiguration().m_username.c_str(), ifn.c_str(), 0600, myEnv, XRDOSS_mkpath) !=  XrdOssOK)
   {
       TRACEF(Error, "File::Open() can't create info file, " << strerror(errno));
       return false;
   }
   m_infoFile = m_output_fs.newFile(Cache::GetInstance().RefConfiguration().m_username.c_str());
   if (m_infoFile->Open(ifn.c_str(), O_RDWR, 0600, myEnv) != XrdOssOK)
   {
      TRACEF(Error, "File::Open() can't get FD info file, " << strerror(errno));
      delete m_infoFile;
      m_infoFile = 0;
      return false;
   }

   if (fileExisted)
   {
      int res = m_cfi.Read(m_infoFile);
      TRACEF(Debug, "Reading existing info file bytes = " << res);
      m_downloadCond.Lock();
      // this method  is called from constructor, no need to lock downloadStaus
      bool complete = m_cfi.IsComplete();
      if (complete) m_prefetchState = kComplete;
      m_downloadCond.UnLock();
   }
   else {
      m_fileSize = m_fileSize;
      m_cfi.SetBufferSize(Cache::GetInstance().RefConfiguration().m_bufferSize);
      m_cfi.SetFileSize(m_fileSize);
      m_cfi.WriteHeader(m_infoFile);
      m_infoFile->Fsync();
      int ss = (m_fileSize - 1)/m_cfi.GetBufferSize() + 1;
      TRACEF(Debug, "Creating new file info, data size = " <<  m_fileSize << " num blocks = "  << ss);
   }

   if (m_prefetchState != kComplete) cache()->RegisterPrefetchFile(this);
   return true;
}


//==============================================================================
// Read and helpers
//==============================================================================



//namespace
//{
bool File::overlap(int       blk,      // block to query
                long long blk_size, //
                long long req_off,  // offset of user request
                int       req_size, // size of user request
                // output:
                long long &off,     // offset in user buffer
                long long &blk_off, // offset in block
                long long &size)    // size to copy
   {
      const long long beg     = blk * blk_size;
      const long long end     = beg + blk_size;
      const long long req_end = req_off + req_size;

      if (req_off < end && req_end > beg)
      {
         const long long ovlp_beg = std::max(beg, req_off);
         const long long ovlp_end = std::min(end, req_end);

         off     = ovlp_beg - req_off;
         blk_off = ovlp_beg - beg;
         size    = ovlp_end - ovlp_beg;

         assert(size <= blk_size);
         return true;
      }
      else
      {
         return false;
      }
   }
//}

//------------------------------------------------------------------------------

Block* File::RequestBlock(int i, bool prefetch)
{
   // Must be called w/ block_map locked.
   // Checks on size etc should be done before.
   //
   // Reference count is 0 so increase it in calling function if you want to
   // catch the block while still in memory.


   const long long   BS = m_cfi.GetBufferSize();
   const int last_block = m_cfi.GetSizeInBits() - 1;

   long long off     = i * BS;
   long long this_bs = (i == last_block) ? m_fileSize - off : BS;

   Block *b = new Block(this, off, this_bs, prefetch); // should block be reused to avoid recreation

   TRACEF(Dump, "File::RequestBlock() " <<  i << "prefetch" <<  prefetch << "address " << (void*)b);
   BlockResponseHandler* oucCB = new BlockResponseHandler(b);
   m_io->GetInput()->Read(*oucCB, (char*)b->get_buff(), off, (int)this_bs);

   m_block_map[i] = b;

   if (m_prefetchState == kOn && m_block_map.size() > Cache::GetInstance().RefConfiguration().m_prefetch_max_blocks)
   {
      m_prefetchState = kHold;
      cache()->DeRegisterPrefetchFile(this); 
   }
   return b;
}

//------------------------------------------------------------------------------

int File::RequestBlocksDirect(DirectResponseHandler *handler, IntList_t& blocks,
                              char* req_buf, long long req_off, long long req_size)
{
   const long long BS = m_cfi.GetBufferSize();

   // XXX Use readv to load more at the same time. 

   long long total = 0;

   for (IntList_i ii = blocks.begin() ; ii != blocks.end(); ++ii)
   {
      // overlap and request
      long long off;     // offset in user buffer
      long long blk_off; // offset in block
      long long size;    // size to copy

      overlap(*ii, BS, req_off, req_size, off, blk_off, size);

      m_io->GetInput()->Read( *handler, req_buf + off, *ii * BS + blk_off, size);
      TRACEF(Dump, "RequestBlockDirect success, idx = " <<  *ii << " size = " <<  size);
      
      total += size;
   }

   return total;
}

//------------------------------------------------------------------------------

int File::ReadBlocksFromDisk(std::list<int>& blocks,
                             char* req_buf, long long req_off, long long req_size)
{
   TRACEF(Dump, "File::ReadBlocksFromDisk " <<  blocks.size());
   const long long BS = m_cfi.GetBufferSize();

   long long total = 0;

   // XXX Coalesce adjacent reads.

   for (IntList_i ii = blocks.begin() ; ii != blocks.end(); ++ii)
   {
      // overlap and read
      long long off;     // offset in user buffer
      long long blk_off; // offset in block
      long long size;    // size to copy

      overlap(*ii, BS, req_off, req_size, off, blk_off, size);

      long long rs = m_output->Read(req_buf + off, *ii * BS + blk_off -m_offset, size);
      TRACEF(Dump, "File::ReadBlocksFromDisk block idx = " <<  *ii << " size= " << size);

      if (rs < 0) {
         TRACEF(Error, "File::ReadBlocksFromDisk neg retval = " <<  rs << " idx = " << *ii );
         return rs;
      }


      // AMT I think we should exit in this case too
      if (rs !=size) {
         TRACEF(Error, "File::ReadBlocksFromDisk incomplete size = " <<  rs << " idx = " << *ii);
         return -1;
      }

      total += rs;

      CheckPrefetchStatDisk(*ii);
   } 

   m_stats.m_BytesDisk += total;
   return total;
}

//------------------------------------------------------------------------------

int File::Read(char* iUserBuff, long long iUserOff, int iUserSize)
{
   const long long BS = m_cfi.GetBufferSize();

   // lock
   // loop over reqired blocks:
   //   - if on disk, ok;
   //   - if in ram or incoming, inc ref-count
   //   - if not available, request and inc ref count
   // before requesting the hell and more (esp. for sparse readv) assess if
   //   passing the req to client is actually better.
   // unlock

   bool preProcOK = true; 
   m_downloadCond.Lock();

   // XXX Check for blocks to free? Later ...

   const int idx_first = iUserOff / BS;
   const int idx_last  = (iUserOff + iUserSize - 1) / BS;

   BlockList_t  blks_to_process, blks_processed;
   IntList_t    blks_on_disk,    blks_direct;

   for (int block_idx = idx_first; block_idx <= idx_last; ++block_idx)
   {
      TRACEF(Dump, "File::Read() idx " << block_idx);
      BlockMap_i bi = m_block_map.find(block_idx);  

      // In RAM or incoming?
      if (bi != m_block_map.end())
      {
         // XXXX if failed before -- retry if timestamp sufficient or fail?
         // XXXX Or just push it and handle errors in one place later?

         inc_ref_count(bi->second);
         TRACEF(Dump, "File::Read() " << iUserBuff << "inc_ref_count for existing block << " << bi->second << " idx = " <<  block_idx);
         blks_to_process.push_front(bi->second);
      }
      // On disk?
      else if (m_cfi.TestBit(offsetIdx(block_idx)))
      {
         TRACEF(Dump, "File::Read()  read from disk " <<  (void*)iUserBuff << " idx = " << block_idx);
         blks_on_disk.push_back(block_idx);
      }
      // Then we have to get it ...
      else
      {
         // Is there room for one more RAM Block?
         if ( cache()->HaveFreeWritingSlots() && cache()->RequestRAMBlock())
         {
            TRACEF(Dump, "File::Read() inc_ref_count new " <<  (void*)iUserBuff << " idx = " << block_idx);
            Block *b = RequestBlock(block_idx, false);
            // assert(b);
            if (!b) {
               preProcOK = false;
               break;
            }
            inc_ref_count(b);
            blks_to_process.push_back(b);
         }
         // Nope ... read this directly without caching.
         else
         {
            TRACEF(Dump, "File::Read() direct block " << block_idx);
            blks_direct.push_back(block_idx);
         }
      } 

   }

   m_downloadCond.UnLock();

   if (!preProcOK) {
      for (BlockList_i i = blks_to_process.begin(); i!= blks_to_process.end(); ++i )
         dec_ref_count(*i);
      return -1;   // AMT ???
   }

   long long bytes_read = 0;

   // First, send out any direct requests.
   // XXX Could send them all out in a single vector read.
   DirectResponseHandler *direct_handler = 0;
   int  direct_size = 0;

   if (!blks_direct.empty())
   {
      direct_handler = new DirectResponseHandler(blks_direct.size());

      direct_size = RequestBlocksDirect(direct_handler, blks_direct, iUserBuff, iUserOff, iUserSize);
      // failed to send direct client request
      if (direct_size < 0) {
         for (BlockList_i i = blks_to_process.begin(); i!= blks_to_process.end(); ++i )
            dec_ref_count(*i);
         delete direct_handler;
         return -1;   // AMT ???
      }
      TRACEF(Dump, "File::Read() direct read finished, size = " << direct_size);
   }

   // Second, read blocks from disk.
   if ((!blks_on_disk.empty()) && (bytes_read >= 0)) {
      int rc = ReadBlocksFromDisk(blks_on_disk, iUserBuff, iUserOff, iUserSize);
      TRACEF(Dump, "File::Read() " << (void*)iUserBuff <<" from disk finished size = " << rc);
      if (rc >= 0)
      {
         bytes_read += rc;
      }
      else
      {
         bytes_read = rc;
         TRACEF(Error, "File::Read() failed read from disk");
         // AMT commented line below should not be an immediate return, can have block refcount increased and map increased
         // return rc;
      }
   }

   // Third, loop over blocks that are available or incoming
   while ( (! blks_to_process.empty()) && (bytes_read >= 0))
   {
      BlockList_t finished;

      {
         XrdSysCondVarHelper _lck(m_downloadCond);

         BlockList_i bi = blks_to_process.begin();
         while (bi != blks_to_process.end())
         {
            if ((*bi)->is_finished())
            {
               TRACEF(Dump, "File::Read() requested block downloaded " << (void*)(*bi));
               finished.push_back(*bi);
               BlockList_i bj = bi++;
               blks_to_process.erase(bj);
            }
            else
            {
               ++bi;
            }
         }

         if (finished.empty())
         {
            TRACEF(Dump, "File::Read() wait block begin" );

            m_downloadCond.Wait();

            TRACEF(Dump, "File::Read() wait block end");

            continue;
         }
      }


      BlockList_i bi = finished.begin();
      while (bi != finished.end())
      {
         if ((*bi)->is_ok())
         {
            long long user_off;     // offset in user buffer
            long long off_in_block; // offset in block
            long long size_to_copy;    // size to copy

            // clLog()->Dump(XrdCl::AppMsg, "File::Read() Block finished ok.");
            overlap((*bi)->m_offset/BS, BS, iUserOff, iUserSize, user_off, off_in_block, size_to_copy);

            TRACEF(Dump, "File::Read() ub=" << (void*)iUserBuff  << " from finished block  " << (*bi)->m_offset/BS << " size " << size_to_copy);
            memcpy(&iUserBuff[user_off], &((*bi)->m_buff[off_in_block]), size_to_copy);
            bytes_read += size_to_copy;
            m_stats.m_BytesRam += size_to_copy;
            CheckPrefetchStatRAM(*bi);
         }
         else // it has failed ... krap up.
         {
            TRACEF(Error, "File::Read(), block  "<< (*bi)->m_offset/BS << "finished with error ");
            bytes_read = -1;
            errno = (*bi)->m_errno;
            break;
         }
         ++bi;
      }

      std::copy(finished.begin(), finished.end(), std::back_inserter(blks_processed));
      finished.clear();
   }

   // Fourth, make sure all direct requests have arrived
   if ((direct_handler != 0) && (bytes_read >= 0 ))
   {
      TRACEF(Dump, "File::Read() waiting for direct requests ");
      XrdSysCondVarHelper _lck(direct_handler->m_cond);

      if (direct_handler->m_to_wait > 0)
      {
         direct_handler->m_cond.Wait();
      }

      if (direct_handler->m_errno == 0)
      {
         bytes_read += direct_size;
         m_stats.m_BytesMissed += direct_size;
      }
      else
      {
         errno = direct_handler->m_errno;
         bytes_read = -1;
      }

      delete direct_handler;
   }
   assert(iUserSize >= bytes_read);

   // Last, stamp and release blocks, release file.
   {
      XrdSysCondVarHelper _lck(m_downloadCond);

      // AMT what is stamp block ??? 

      // blks_to_process can be non-empty, if we're exiting with an error.
      std::copy(blks_to_process.begin(), blks_to_process.end(), std::back_inserter(blks_processed));

      for (BlockList_i bi = blks_processed.begin(); bi != blks_processed.end(); ++bi)
      {
         TRACEF(Dump, "File::Read() dec_ref_count " << (void*)(*bi) << " idx = " << (int)((*bi)->m_offset/BufferSize()));
         dec_ref_count(*bi);
         // XXXX stamp block
      }
   }

   return bytes_read;
}

//------------------------------------------------------------------------------

void File::WriteBlockToDisk(Block* b)
{
   int retval = 0;
   // write block buffer into disk file
   long long offset = b->m_offset - m_offset;
   long long size = (offset +  m_cfi.GetBufferSize()) > m_fileSize ? (m_fileSize - offset) : m_cfi.GetBufferSize();
   int buffer_remaining = size;
   int buffer_offset = 0;
   int cnt = 0;
   const char* buff = &b->m_buff[0];
   while ((buffer_remaining > 0) && // There is more to be written
          (((retval = m_output->Write(buff, offset + buffer_offset, buffer_remaining)) != -1)
           || (errno == EINTR))) // Write occurs without an error
   {
      buffer_remaining -= retval;
      buff += retval;
      cnt++;

      if (buffer_remaining)
      {
         TRACEF(Warning, "File::WriteToDisk() reattempt " << cnt << " writing missing " << buffer_remaining << " for block  offset " << b->m_offset);
      }
      if (cnt > PREFETCH_MAX_ATTEMPTS)
      {
         TRACEF(Error, "File::WriteToDisk() write block with off = " <<  b->m_offset <<" failed too manny attempts ");
         return;
      }
   }

   // set bit fetched
   TRACEF(Dump, "File::WriteToDisk() success set bit for block " <<  b->m_offset << " size " <<  size);
   int pfIdx =  (b->m_offset - m_offset)/m_cfi.GetBufferSize();

   m_downloadCond.Lock();
   assert((m_cfi.TestBit(pfIdx) == false) && "Block not yet fetched.");
   m_cfi.SetBitFetched(pfIdx);
   m_downloadCond.UnLock();

   {
      XrdSysCondVarHelper _lck(m_downloadCond);
      // clLog()->Dump(XrdCl::AppMsg, "File::WriteToDisk() dec_ref_count %d %s", pfIdx, lPath());
      dec_ref_count(b);
   }

   // set bit synced
   bool schedule_sync = false;
   {
      XrdSysMutexHelper _lck(&m_syncStatusMutex);

      if (m_in_sync)
      {
         m_writes_during_sync.push_back(pfIdx);
      }
      else
      {
         m_cfi.SetBitWriteCalled(pfIdx);
         ++m_non_flushed_cnt;
         if (m_non_flushed_cnt >= 100 )
         {
            schedule_sync     = true;
            m_in_sync         = true;
            m_non_flushed_cnt = 0;
         }

      }
   }

   if (schedule_sync)
   {
      XrdPosixGlobals::schedP->Schedule(m_syncer);
   }
}

//------------------------------------------------------------------------------

void File::Sync()
{
   TRACEF( Dump, "File::Sync()");
   m_output->Fsync();
   m_cfi.WriteHeader(m_infoFile);
   int written_while_in_sync;
   {
      XrdSysMutexHelper _lck(&m_syncStatusMutex);
      for (std::vector<int>::iterator i = m_writes_during_sync.begin(); i != m_writes_during_sync.end(); ++i)
      {
         m_cfi.SetBitWriteCalled(*i);
      }
      written_while_in_sync = m_non_flushed_cnt = (int) m_writes_during_sync.size();
      m_writes_during_sync.clear();
      m_in_sync = false;
   }
   TRACEF(Dump, "File::Sync() "<< written_while_in_sync  << " blocks written during sync.");
   m_infoFile->Fsync();
}

//______________________________________________________________________________

void File::inc_ref_count(Block* b)
{
   // Method always called under lock
   b->m_refcnt++;
   TRACEF(Dump, "File::inc_ref_count " << b << " refcnt  " << b->m_refcnt);
}

//______________________________________________________________________________

void File::dec_ref_count(Block* b)
{
   // Method always called under lock
    b-> m_refcnt--;
    assert(b->m_refcnt >= 0);

    //AMT ... this is ugly, ... File::Read() can decrease ref count before waiting to be , prefetch starts with refcnt 0
    if ( b->m_refcnt == 0 && b->is_finished()) {
       free_block(b);
    }
}

void File::free_block(Block* b)
{
   int i = b->m_offset/BufferSize();
   TRACEF(Dump, "File::free_block block " << b << "  idx =  " <<  i);
   delete m_block_map[i];
   size_t ret = m_block_map.erase(i);
   if (ret != 1)
   {
      // assert might be a better option than a warning
      TRACEF(Warning, "File::OnBlockZeroRefCount did not erase " <<  i  << " from map");
   }
   else
   {
      cache()->RAMBlockReleased();
   }

   if (m_prefetchState == kHold && m_block_map.size() < Cache::GetInstance().RefConfiguration().m_prefetch_max_blocks)
   {
      m_prefetchState = kOn;
      cache()->RegisterPrefetchFile(this); 
   }
}

//------------------------------------------------------------------------------

void File::ProcessBlockResponse(Block* b, int res)
{

   m_downloadCond.Lock();

   TRACEF(Dump, "File::ProcessBlockResponse " << (void*)b << "  " << b->m_offset/BufferSize());
   if (res >= 0) 
   {
      b->m_downloaded = true;
      TRACEF(Dump, "File::ProcessBlockResponse inc_ref_count " <<  (int)(b->m_offset/BufferSize()));
      inc_ref_count(b);
      cache()->AddWriteTask(b, true);
   }
   else
   {
      // AMT how long to keep?
      // when to retry?
      TRACEF(Error, "File::ProcessBlockResponse block " << b << "  " << (int)(b->m_offset/BufferSize()) << " error=" << res);
      // XrdPosixMap::Result(*status);
      // AMT could notfiy global cache we dont need RAM for that block
      b->set_error_and_free(errno);
      errno = 0;

      // ??? AMT how long to keep
      inc_ref_count(b);
   }

   m_downloadCond.Broadcast();

   m_downloadCond.UnLock();
}



 long long File::BufferSize() {
     return m_cfi.GetBufferSize();
 }

//______________________________________________________________________________
const char* File::lPath() const
{
return m_temp_filename.c_str();
}

//______________________________________________________________________________
int File::offsetIdx(int iIdx)
{
   return iIdx - m_offset/m_cfi.GetBufferSize();
}

//______________________________________________________________________________
void File::AppendIOStatToFileInfo()
{
   // lock in case several IOs want to write in *cinfo file
   if (m_infoFile)
   {
      Info::AStat as;
      as.DetachTime = time(0);
      as.BytesDisk = m_stats.m_BytesDisk;
      as.BytesRam = m_stats.m_BytesRam;
      as.BytesMissed = m_stats.m_BytesMissed;
      m_cfi.AppendIOStat(as, (XrdOssDF*)m_infoFile);
   }
   else
   {
      TRACEF(Warning, "File::AppendIOStatToFileInfo() info file not opened");
   }
}

//______________________________________________________________________________
void File::Prefetch()
{
   {
      XrdSysCondVarHelper _lck(m_stateCond);
      if (m_prefetchState != kOn)
         return;
   }
       
   // check index not on disk and not in RAM
   TRACEF(Dump, "File::Prefetch enter to check download status");
   bool found = false;
   for (int f=0; f < m_cfi.GetSizeInBits(); ++f)
   {
      XrdSysCondVarHelper _lck(m_downloadCond);
      if (!m_cfi.TestBit(f))
      {    
         f += m_offset/m_cfi.GetBufferSize();
         BlockMap_i bi = m_block_map.find(f);
         if (bi == m_block_map.end()) {
            TRACEF(Dump, "File::Prefetch take block " << f);
            cache()->RequestRAMBlock();
            RequestBlock(f, true);
            m_prefetchReadCnt++;
            m_prefetchScore = float(m_prefetchHitCnt)/m_prefetchReadCnt;
            found = true;
            break;
         }
      }
   }


   if (!found)  { 
      TRACEF(Dump, "File::Prefetch no free block found ");
      m_stateCond.Lock();
      m_prefetchState = kComplete;
      m_stateCond.UnLock();
      cache()->DeRegisterPrefetchFile(this); 
   }
}


//______________________________________________________________________________
void File::CheckPrefetchStatRAM(Block* b)
{
   if (Cache::GetInstance().RefConfiguration().m_prefetch_max_blocks) {
      if (b->m_prefetch) {
         m_prefetchHitCnt++;
         m_prefetchScore = float(m_prefetchHitCnt)/m_prefetchReadCnt;
      }
   }
}

//______________________________________________________________________________
void File::CheckPrefetchStatDisk(int idx)
{
   if (Cache::GetInstance().RefConfiguration().m_prefetch_max_blocks) {
      if (m_cfi.TestPrefetchBit(offsetIdx(idx)))
         m_prefetchHitCnt++;
   }
}

//______________________________________________________________________________
float File::GetPrefetchScore() const
{
   return m_prefetchScore;
}

XrdOucTrace* File::GetTrace()
{
   return Cache::GetInstance().GetTrace();
}

//==============================================================================
//==================    RESPONSE HANDLER      ==================================
//==============================================================================

void BlockResponseHandler::Done(int res)
{
    m_block->m_file->ProcessBlockResponse(m_block, res);

   delete this;
}

//------------------------------------------------------------------------------

void DirectResponseHandler::Done(int res)
{
   XrdSysCondVarHelper _lck(m_cond);

   --m_to_wait;

   if (res < 0)
   {
      m_errno = errno;
   }

   if (m_to_wait == 0)
   {
      m_cond.Signal();
   }
}

