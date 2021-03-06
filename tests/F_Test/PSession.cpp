/*********************************************************************
** Copyright (C) 2003 Terabit Pty Ltd.  All rights reserved.
**
** This file is part of the POSIX-Proactor module.
**
**  
**   
**
**
**
**
**
**********************************************************************/

// ============================================================================
/**
 *  @file PSession.cpp
 *
 *  PSession.cpp
 *
 *
 *  @author Alexander Libman <libman@terabit.com.au>
 */
// ============================================================================

#include "test_config.h"

#include "PSession.h"
#include "ace/INET_Addr.h"

// ***************************************************************
//  PSession - base class for all Read/Write Asynch handlers
// ***************************************************************

PSession::PSession (void)
: lock_      ()
, manager_   (0)
, index_     (-1)
, ref_cnt_r_ (0)
, ref_cnt_w_ (0)
, total_snd_ (0)
, total_rcv_ (0)
, total_w_   (0)
, total_r_   (0)
, last_op_time_ (ACE_OS::gettimeofday ())
, timeout_      (ACE_Time_Value::zero)
, stream_    ()
, file_      ()
{
}

PSession::~PSession (void)
{
}

void
PSession::set_manager(PSessionManager * manager, int index)
{
    ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->lock_);
  
    this->manager_ = manager;
    this->index_ = index;
}

int 
PSession::is_safe_to_delete (void) const
{
    return !this->has_pending_io();
}


void
PSession::addresses (const ACE_Addr& peer, const ACE_Addr& local)
{
  ACE_TCHAR str_peer [256];
  ACE_TCHAR str_local[256];

  TRB_Sock_Addr::to_string (peer, str_peer, sizeof (str_peer)/sizeof (ACE_TCHAR));
  TRB_Sock_Addr::to_string (local, str_local, sizeof (str_local)/sizeof (ACE_TCHAR));
  
  ACE_DEBUG ((LM_DEBUG,
              ACE_TEXT ("(%t) %s=%d Remote=%s Local=%s\n"),
              this->get_name(),
              this->index(),
              str_peer,
              str_local));
}

void
PSession::cancel ()
{
    ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->mutex());
    do_cancel ();
}

void 
PSession::check (void)
{
    ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->mutex());
 
    if (this->timeout_ == ACE_Time_Value::zero)
      return;

    ACE_Time_Value diff_time = ACE_OS::gettimeofday () - this->last_op_time_;

    if (diff_time < this->timeout_)
      return;
    
    this->do_cancel();
}


void 
PSession::do_cancel ()
{
    this->stream_.cancel_and_close();
    this->file_.cancel_and_close ();
}

int
PSession::initiate_read (u_long offset_low, u_long offset_high)
{
    //if (this->get_ref_cnt_r () != 0)
    //    return 0;

    ACE_Message_Block *mb = 0;

    u_int blksize = this->config().r_blksize();

    ACE_NEW_RETURN (mb,
        ACE_Message_Block (blksize),
        -1);

    if (mb == 0)
    {
        do_cancel ();
        return -1;
    }
    mb->reset ();

    return this->initiate_read (offset_low, offset_high, *mb);
}

int
PSession::initiate_read (u_long offset_low, u_long offset_high, ACE_Message_Block & mb)
{
    size_t nbytes = mb.space();

    if (nbytes == 0)
    {
        mb.release ();
        this->do_cancel ();

        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT ("(%t) %s Attempt to read 0 bytes\n"),
            this->get_name()),
            -1);
    }

    int rc = -1;
    if (offset_low == (u_long) -1L  &&
        offset_high == (u_long) -1L )
    {
        rc = this->stream_.read (mb, nbytes);
    }
    else
    {
        rc = this->file_.read (mb, nbytes, offset_low, offset_high);
    }

    if (rc < 0)
    {
      mb.release();
      this->do_cancel ();
      return -1;
    }

    this->ref_cnt_r_++;
    this->total_r_++;
    return 0;
}

int
PSession::initiate_write (u_long offset_low, u_long offset_high,
                          const char * data, size_t len)
{
    ACE_Message_Block *mb = 0;

    u_int blksize = this->config().s_blksize();

    if (blksize < len)
    {
        blksize=len;
    }
    ACE_NEW_RETURN (mb,
        ACE_Message_Block (blksize),
        -1);

    if (mb == 0)
    {
        this->do_cancel ();
        return -1;
    }

    mb->reset ();
    mb->copy (data, len);
    //mb->wr_ptr (mb->space());
    return this->initiate_write (offset_low, offset_high, *mb);
}
  
int
PSession::initiate_write(u_long offset_low, u_long offset_high, ACE_Message_Block &mb)
{
    size_t nbytes = mb.length();

    if (nbytes == 0)
    {
        mb.release ();
        this->do_cancel ();

        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT ("(%t) %s Attempt to write 0 bytes\n"),
            this->get_name()),
            -1);
    }

    int rc = -1;
    if (offset_low == (u_long) -1L  &&
        offset_high == (u_long) -1L )
    {
        rc = this->stream_.write (mb, nbytes);
    }
    else
    {
        rc = this->file_.write (mb, nbytes, offset_low, offset_high);
    }

    if (rc < 0)
    {
        mb.release ();
        this->do_cancel ();
        return -1;
    }

    this->ref_cnt_w_++;
    this->total_w_++;
    return 0;
}

int
PSession::initiate_transmit (ACE_HANDLE h,
                             ACE_Message_Block * header,
                             ACE_Message_Block * trailer)
{
    if (this->stream_.transmit (h, header, trailer) < 0)
    {
        this->do_cancel ();
        return -1;
    }

    this->ref_cnt_w_++;
    this->total_w_++;
    return 0;
}

void
PSession::trace_read_completion (const TRB_Asynch_Read_Stream::Result &result,
                                 const char * type)
{
    {
        ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->mutex ());

        this->update_last_time();

        if (result.success())
            this->total_rcv_ += result.bytes_transferred ();
    }

    int loglevel = this->config().loglevel ();

    ACE_Message_Block & mb = result.message_block ();

    char * last  = mb.wr_ptr();
    char * first = last - result.bytes_transferred();

    if (loglevel == 0)
    {
        LogLocker log_lock;

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) **** %s=%d handle_read_%s() ****\n"),
            this->get_name(),
            this->index(),
            type));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_to_read = %d\n"),
            result.bytes_to_read ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("handle = %d\n"),
            result.handle ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_transfered = %d\n"),
            result.bytes_transferred ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("error = %d\n"),
            result.error ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("act = %@\n"),
        //            result.act ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("success = %d\n"),
        //            result.success ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("completion_key = %@\n"),
        //            result.completion_key ()));


        ACE_HEX_DUMP ((LM_DEBUG, first, result.bytes_transferred(),
            ACE_TEXT ("message_block ")));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("**** end of message ****************\n")));
    }
    else if (result.error () != 0 )
    {
        LogLocker log_lock;

        ACE_Log_Msg::instance ()->errnum (result.error ());
        ACE_OS::last_error (result.error ());
        ACE_Log_Msg::instance ()->log (LM_ERROR,
            ACE_TEXT ("(%t) %s=%d read_%s err=%d %p\n"),
            this->get_name (),
            this->index (),
            type,
            (int) result.error(),
            ACE_TEXT ("ERROR"));
    }
    else if ( loglevel ==1 )
    {
        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) %s=%d read_%s=%d OK\n"),
            this->get_name (),
            this->index (),
            type,
            result.bytes_transferred ()));
    }
}

void
PSession::trace_write_completion (const TRB_Asynch_Write_Stream::Result &result,
                                  const char * type)
{
    {
        ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->mutex ());

        this->update_last_time();

        if (result.success())
            this->total_snd_ += result.bytes_transferred ();
    }

    int loglevel = this->config().loglevel ();

    ACE_Message_Block & mb = result.message_block ();

    char * last  = mb.rd_ptr();
    char * first = last - result.bytes_transferred();

    if (loglevel == 0)
    {
        LogLocker log_lock;

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) **** %s=%d handle_write_%s() ****\n"),
            this->get_name(),
            this->index(),
            type));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_to_write = %d\n"),
            result.bytes_to_write ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("handle = %d\n"),
            result.handle ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_transfered = %d\n"),
            result.bytes_transferred ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("error = %d\n"),
            result.error ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("act = %@\n"),
        //            result.act ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("success = %d\n"),
        //            result.success ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("completion_key = %@\n"),
        //            result.completion_key ()));


        ACE_HEX_DUMP ((LM_DEBUG, first, result.bytes_transferred(),
            ACE_TEXT ("message_block ")));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("**** end of message ****************\n")));
    }
    else if (result.error () != 0 )
    {
        LogLocker log_lock;

        ACE_Log_Msg::instance ()->errnum (result.error ());
        ACE_OS::last_error (result.error ());
        ACE_Log_Msg::instance ()->log (LM_ERROR,
            ACE_TEXT ("(%t) %s=%d write_%s err=%d %p\n"),
            this->get_name (),
            this->index (),
            type,
            (int) result.error(),
            ACE_TEXT ("ERROR"));
    }
    else if ( loglevel ==1 )
    {
        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) %s=%d write_%s=%d OK\n"),
            this->get_name (),
            this->index (),
            type,
            result.bytes_transferred ()));
    }
}

void
PSession::trace_transmit_completion (const TRB_Asynch_Transmit_File::Result &result)
{
    {
        ACE_GUARD (ACE_SYNCH_MUTEX, monitor, this->mutex ());

        this->update_last_time();

        if (result.success())
            this->total_snd_ += result.bytes_transferred ();
    }

    int loglevel = this->config().loglevel ();

 
    if (loglevel == 0)
    {
        LogLocker log_lock;

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) **** %s=%d handle_transmit_file() ****\n"),
            this->get_name(),
            this->index()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_to_write = %d\n"),
            result.bytes_to_write ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_per_send = %d\n"),
            result.bytes_per_send ()));

          ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("handle = %d\n"),
            result.file ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("bytes_transfered = %d\n"),
            result.bytes_transferred ()));

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("error = %d\n"),
            result.error ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("act = %@\n"),
        //            result.act ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("success = %d\n"),
        //            result.success ()));

        //ACE_DEBUG ((LM_DEBUG,
        //            ACE_TEXT ("completion_key = %@\n"),
        //            result.completion_key ()));


        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("**** end of message ****************\n")));
    }
    else if (result.error () != 0 )
    {
        LogLocker log_lock;

        ACE_Log_Msg::instance ()->errnum (result.error ());
        ACE_OS::last_error (result.error ());
        ACE_Log_Msg::instance ()->log (LM_ERROR,
            ACE_TEXT ("(%t) %s=%d transmit_file err=%d %p\n"),
            this->get_name (),
            this->index (),
            (int) result.error(),
            ACE_TEXT ("ERROR"));
    }
    else if ( loglevel ==1 )
    {
        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) %s=%d transmit_file=%d OK\n"),
            this->get_name (),
            this->index (),
            result.bytes_transferred ()));
    }

}

// *************************************************************
//  PSessionManager  - container and object manager
//  for PSession
// *************************************************************

PSessionManager::PSessionManager (ProactorTask &    task,
                                  PSessionFactory & factory,
                                  const ACE_TCHAR * name)
: lock_         ()
, task_         (task)
, factory_      (factory)
, name_         (name)
, total_snd_    (0)
, total_rcv_    (0)
, total_w_      (0)
, total_r_      (0)
, flg_cancel_   (0)
, num_connections_ (0)
, peak_connections_(0)
, timeout_      (ACE_Time_Value::zero)
, timer_id_     (-1)
{
    for (u_int i = 0; i < MAX_CONNECTIONS; ++i)
        this->list_connections_[i] = 0;
}

PSessionManager::~PSessionManager (void)
{
    //ACE_ASSERT(this->num_connections_ == 0);

    ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

    for (u_int i = 0; i < MAX_CONNECTIONS; ++i)
    {
        this->destroy_session (i);
    }
}

void 
PSessionManager::print_statistic(void)
{
    //Print statistic
    ACE_TCHAR bufs [256];
    ACE_TCHAR bufr [256];

    ACE_OS::sprintf (bufs, ACE_TEXT ("%lu(%lu)"),
        this->get_total_snd (),
        this->get_total_w ());

    ACE_OS::sprintf (bufr, ACE_TEXT ("%lu(%lu)"),
        this->get_total_rcv (),
        this->get_total_r ());

    ACE_DEBUG ((LM_DEBUG,
        ACE_TEXT ("(%t) %s total bytes(operations): snd=%s rcv=%s\n"),
        this->get_name(),
        bufs,
        bufr));
}

int
PSessionManager::is_safe_to_delete (void)
{
    ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_,0);

    return (this->num_connections_ == 0 && this->timer_id_ == -1);
}


void
PSessionManager::cancel (void)
{
    ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

    this->flg_cancel_ = 1;

    this->cancel_timer ();

    // Propagate cancel to all sessions
    for (u_int i = 0; i < MAX_CONNECTIONS; ++i)
    {
        if (this->list_connections_[i] != 0)
            this->list_connections_[i]->cancel ();
    }
}

void
PSessionManager::check (void)
{
    //ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

    // Propagate cancel to all sessions
    for (u_int i = 0; i < MAX_CONNECTIONS; ++i)
    {
        if (this->list_connections_[i] != 0)
            this->list_connections_[i]->check ();
    }
}

void
PSessionManager::cancel_timer (void)
{
    if (this->timer_id_ != -1)
    {
        int rc = this->task().get_proactor(0)->cancel_timer (this->timer_id_);

        if (rc != 0)
            this->timer_id_ = -1;
    }
}

void
PSessionManager::start_timer (void) 
{
    if (this->should_finish())
    {
        if (this->is_safe_to_delete())
        {
            this->task().signal_main();
        }
        return;
    }

    if (this->timer_id_ != -1 ||
        this->timeout_ == ACE_Time_Value::zero)
    {
        return;
    }

    ACE_Time_Value abs_time = this->timeout_;

    this->timer_id_ = 
        this->task().get_proactor(0)->schedule_timer (*this,
                                                      0,
                                                      abs_time);
}

void
PSessionManager::handle_time_out ( const ACE_Time_Value & /* tv */,
                                   const void *   /* pArg */)
{
    ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

    //ACE_DEBUG((LM_DEBUG,
    //              ACE_TEXT ("(%t) %s handle timeout\n"),
    //              this->get_name ()));

    
    this->check();
    
    this->timer_id_ = -1;

    this->start_timer ();
}

void
PSessionManager::set_timeout (const ACE_Time_Value & timeout)
{
    ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

    this->timeout_ = timeout;

    this->start_timer ();
}

PSession *
PSessionManager::create_session (void)
{
    ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_, 0);

    if (this->num_connections_ >= MAX_CONNECTIONS || 
        this->flg_cancel_ != 0)
    {
        return 0;
    }

    PSession * session = 0;

    for (u_int i = 0; i < MAX_CONNECTIONS; ++i)
    {
        if (this->list_connections_[i] != 0)
            continue;

        session = factory_.create_session(*this);

        if (session == 0)
            break;

        this->list_connections_[i] = session;
        this->num_connections_++;

        if (this->num_connections_ > this->peak_connections_)
            this->peak_connections_ = this->num_connections_;


        session->set_manager (this,i);
        session->set_timeout (this->timeout_);

        ACE_DEBUG ((LM_DEBUG,
            ACE_TEXT ("(%t) %s=%d NEW %s=%d\n"),
            this->get_name(),
            this->num_connections_,
            session->get_name(),
            i));

        break;
    }

    return session;
}

void
PSessionManager::destroy_session(PSession * session)
{
  if (!session)
    return;

  u_int i = session->index();

  ACE_ASSERT(session->manager() == this);
  ACE_ASSERT(i < MAX_CONNECTIONS);
  ACE_ASSERT(session == this->list_connections_[i]);

  this->destroy_session (i);
}
  


void
PSessionManager::destroy_session(int index)
{
  ACE_GUARD (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->lock_);

  ACE_ASSERT(ACE_static_cast(u_int,index) < MAX_CONNECTIONS);

  PSession * session = this->list_connections_[index];

  if (!session)
    return;
  
  this->total_snd_ += session->get_total_snd();
  this->total_rcv_ += session->get_total_rcv();
  this->total_w_   += session->get_total_w();
  this->total_r_   += session->get_total_r();

  this->num_connections_--;
  this->list_connections_[index] = 0;


    ACE_DEBUG ((LM_DEBUG,
              ACE_TEXT ("(%t) %s=%d: DEL %s=%d S=%d(%d+%d) R=%d(%d+%d)\n"),
              this->get_name(),
              this->num_connections_,
              session->get_name(),
              index,
              session->get_total_snd(),
              session->get_total_w(),
              session->get_ref_cnt_w(),
              session->get_total_rcv(),
              session->get_total_r(),
              session->get_ref_cnt_r()));

  session->set_manager (0,-1);
  delete session;

  if (this->is_safe_to_delete())
    this->task().signal_main();
}

// *************************************************************
//  Acceptor
// *************************************************************
Acceptor::Acceptor  (PSessionManager & manager)
  : manager_        (manager),
    ref_cnt_        (0),
    flg_cancel_     (0)
{
}

Acceptor::~Acceptor (void)
{
}

int
Acceptor::start(const ACE_Addr & addr)
{
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),-1);

  int num_initial_accepts = 50;
  int rc = -1;

  if (this->should_finish ())
    return rc;

  rc = this->open(addr,
                    0,   //size_t bytes_to_read = 0,
                    1,   //int pass_addresses = 0,
                    ACE_DEFAULT_BACKLOG, //int backlog ,
                    1,   //int reuse_addr = 1,
                    manager().task().get_proactor(0), // TRB_Proactor *proactor = 0,
                    1,   //int validate_new_connection = 0,
                    0,   //int reissue_accept = 1,
                    0);  //int number_of_initial_accepts = -1);

  if ( rc < 0)
    return -1;

  for(; this->ref_cnt_ < num_initial_accepts; ++this->ref_cnt_)
    {
      rc =this->accept (0,0);

      if (rc < 0)
        return -1;
    }

  return 0;
}


int
Acceptor::cancel(void)
{
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),-1);
  
  this->flg_cancel_ = 1;
  
  // stop future accepts
  this->reissue_accept (0); 

  // cancel outstanding accepts
  return this->TRB_Asynch_Acceptor<PSession>::cancel ();         
}


int
Acceptor::validate_connection(const TRB_Asynch_Accept::Result&  result,
                              const ACE_Addr& /*remote*/,
                              const ACE_Addr& /*local*/)
{
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),-1);

  --this->ref_cnt_;

  int rc = 0;

  if (!result.success())
    {
      ACE_DEBUG ((LM_DEBUG,
                  ACE_TEXT ("(%t) Acceptor: %p\n"),
                  ACE_TEXT ("accept failed")));
      rc = -1;
    }
  
  if (this->should_finish ())
    {
      rc = -1;
    }

  return rc;
}


int
Acceptor::should_reissue_accept (void)
{ 
  // we provide manual control for reiussue accept
  // to manage number outstanding accepts properly
  // see validate_connection(...) method 

  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),0);

  if (!this->should_finish ())
    {
      if (this->accept (0,0) < 0)
          ACE_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%t) Acceptor: %p\n"),
                      ACE_TEXT ("reissue Accept failed")));
      else
         ++this->ref_cnt_ ;

    }


  if(this->is_safe_to_delete ())
    this->task().signal_main ();
  
  return 0;
}

int
Acceptor::is_safe_to_delete (void)
{ 
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),0);

  return (this->get_ref_cnt () == 0);
}

PSession *
Acceptor::make_handler (void)
{
  return this->manager ().create_session ();
}

// *************************************************************
//  Connector
// *************************************************************
Connector::Connector (PSessionManager & manager)
  : manager_        (manager),
    ref_cnt_        (0),
    flg_cancel_     (0)
{
}

Connector::~Connector (void)
{
}

int
Connector::start (const ACE_Addr& addr, u_int num)
{

  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex (), 0);

  
  if (should_finish ())
    return  0;
    
  if (num > MAX_CONNECTIONS)
    num = MAX_CONNECTIONS;

  u_int rc = 0;

  if (this->open (1,  // int pass_addresses = 0,
                  manager().task().get_proactor(0), //TRB_Proactor *proactor = 0,
                  1    // int validate_new_connection = 0 );
                 )!= 0)
  {
     ACE_ERROR ((LM_ERROR,
                 ACE_TEXT ("(%t) Connector %p\n"),
                 ACE_TEXT ("open failed")));
     return rc;
  }

  for (; rc < num;  ++rc, ++this->ref_cnt_ )
    {

      if (this->connect (addr,
                         ACE_Addr::sap_any,
                         1,
                         (const void *) rc) != 0)
        {
          ACE_ERROR ((LM_ERROR,
                      ACE_TEXT ("(%t) Connector %p\n"),
                      ACE_TEXT ("connect failed")));
          break;
        }
    }
  return (u_int) rc;
}


int
Connector::cancel(void)
{
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),-1);
  
  this->flg_cancel_ = 1;
  
  // cancel outstanding connects
  return this->TRB_Asynch_Connector<PSession>::cancel ();           
}



int
Connector::validate_connection(const TRB_Asynch_Connect::Result&  result,
                               const ACE_Addr& /*remote*/,
                               const ACE_Addr& /*local*/)
{
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex (), -1);

  --this->ref_cnt_;

  int rc = 0;
  if (!result.success() || this->should_finish())
    {
      rc = -1;

      ACE_DEBUG ((LM_DEBUG,
                  ACE_TEXT ("(%t) Connector: Connect failed err=%d\n"),
                  (int) result.error()));
    }

  if(this->is_safe_to_delete())
    this->task().signal_main();

  return rc;
}

int
Connector::is_safe_to_delete (void)
{ 
  ACE_GUARD_RETURN (ACE_SYNCH_RECURSIVE_MUTEX, monitor, this->mutex(),0);

  return (this->get_ref_cnt () == 0);
}

PSession *
Connector::make_handler (void)
{
  return this->manager ().create_session ();
}



