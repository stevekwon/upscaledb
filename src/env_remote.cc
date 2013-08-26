/*
 * Copyright (C) 2005-2013 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#ifdef HAM_ENABLE_REMOTE

#include "config.h"

#include "txn.h"
#include "mem.h"
#include "os.h"
#include "cursor.h"
#include "db_remote.h"
#include "env_remote.h"

#include "protocol/protocol.h"

namespace hamsterdb {

RemoteEnvironment::~RemoteEnvironment()
{
  (void)os_socket_close(&m_socket);
}

ham_status_t
RemoteEnvironment::perform_request(Protocol *request, Protocol **reply)
{
  *reply = 0;

  // use ByteArray to avoid frequent reallocs!
  m_buffer.clear();

  if (!request->pack(&m_buffer)) {
    ham_log(("protoype Protocol::pack failed"));
    return (HAM_INTERNAL_ERROR);
  }

  ham_status_t st = os_socket_send(m_socket, (ham_u8_t *)m_buffer.get_ptr(),
          m_buffer.get_size());
  if (st)
    return (st);

  // now block and wait for the reply; first read the header, then the
  // remaining data
  st = os_socket_recv(m_socket, (ham_u8_t *)m_buffer.get_ptr(), 8);
  if (st) {
    os_socket_close(&m_socket);
    return (HAM_IO_ERROR);
  }

  // no need to check the magic; it's verified in Protocol::unpack
  ham_u32_t size = *(ham_u32_t *)((char *)m_buffer.get_ptr() + 4);
  m_buffer.resize(ham_db2h32(size) + 8);
  st = os_socket_recv(m_socket, (ham_u8_t *)m_buffer.get_ptr() + 8, size);
  if (st) {
    os_socket_close(&m_socket);
    return (HAM_IO_ERROR);
  }

  *reply = Protocol::unpack((const ham_u8_t *)m_buffer.get_ptr(), size + 8);

  return (0);
}

ham_status_t
RemoteEnvironment::create(const char *url, ham_u32_t flags,
        ham_u32_t mode, ham_size_t pagesize, ham_size_t cachesize,
        ham_u16_t maxdbs)
{
  // the 'create' operation is identical to 'open'
  return (open(url, flags, cachesize));
}

ham_status_t
RemoteEnvironment::open(const char *url, ham_u32_t flags,
        ham_size_t cachesize)
{
  (void)cachesize;

  if (m_socket != HAM_INVALID_FD)
    (void)os_socket_close(&m_socket);

  ham_assert(url != 0);
  ham_assert(::strstr(url, "ham://") == url);
  const char *ip = url + 6;
  const char *port_str = strstr(ip, ":");
  if (!port_str) {
    ham_trace(("remote uri does not include port - expected "
                "`ham://<ip>:<port>`"));
    return (HAM_INV_PARAMETER);
  }
  ham_u16_t port = (ham_u16_t)atoi(port_str + 1);
  if (!port) {
    ham_trace(("remote uri includes invalid port - expected "
                "`ham://<ip>:<port>`"));
    return (HAM_INV_PARAMETER);
  }

  const char *filename = strstr(port_str, "/");

  std::string hostname(ip, port_str);
  ham_status_t st = os_socket_connect(hostname.c_str(), port,
          m_timeout, &m_socket);
  if (st) {
    (void)os_socket_close(&m_socket);
    return (HAM_NETWORK_ERROR);
  }

  Protocol request(Protocol::CONNECT_REQUEST);
  request.mutable_connect_request()->set_path(filename);

  Protocol *reply = 0;
  st = perform_request(&request, &reply);
  if (st) {
    (void)os_socket_close(&m_socket);
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->type() == Protocol::CONNECT_REPLY);

  st = reply->connect_reply().status();
  if (st == 0) {
    m_filename = url;
    set_flags(flags | reply->connect_reply().env_flags());
    m_remote_handle = reply->connect_reply().env_handle();
  }

  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::rename_db( ham_u16_t oldname, ham_u16_t newname,
        ham_u32_t flags)
{
  ham_status_t st;
  Protocol *reply = 0;

  Protocol request(Protocol::ENV_RENAME_REQUEST);
  request.mutable_env_rename_request()->set_env_handle(m_remote_handle);
  request.mutable_env_rename_request()->set_oldname(oldname);
  request.mutable_env_rename_request()->set_newname(newname);
  request.mutable_env_rename_request()->set_flags(flags);

  st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_rename_reply());

  st = reply->env_rename_reply().status();

  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::erase_db(ham_u16_t name, ham_u32_t flags)
{
  ham_status_t st;
  Protocol *reply = 0;

  Protocol request(Protocol::ENV_ERASE_DB_REQUEST);
  request.mutable_env_erase_db_request()->set_env_handle(m_remote_handle);
  request.mutable_env_erase_db_request()->set_name(name);
  request.mutable_env_erase_db_request()->set_flags(flags);

  st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_erase_db_reply());

  st = reply->env_erase_db_reply().status();

  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::get_database_names(ham_u16_t *names, ham_size_t *count)
{
  Protocol *reply = 0;

  Protocol request(Protocol::ENV_GET_DATABASE_NAMES_REQUEST);
  request.mutable_env_get_database_names_request();
  request.mutable_env_get_database_names_request()->set_env_handle(m_remote_handle);

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_get_database_names_reply());

  st = reply->env_get_database_names_reply().status();
  if (st) {
    delete reply;
    return (st);
  }

  /* copy the retrieved names */
  ham_size_t i;
  for (i = 0;
      i < (ham_size_t)reply->env_get_database_names_reply().names_size()
        && i < *count;
      i++) {
    names[i] = (ham_u16_t)*(reply->mutable_env_get_database_names_reply()->mutable_names()->mutable_data() + i);
  }

  *count = i;

  delete reply;
  return (0);
}

ham_status_t
RemoteEnvironment::get_parameters(ham_parameter_t *param)
{
  static char filename[1024]; // TODO not threadsafe!!
  Protocol *reply = 0;
  ham_parameter_t *p = param;

  if (!param)
    return (HAM_INV_PARAMETER);

  Protocol request(Protocol::ENV_GET_PARAMETERS_REQUEST);
  request.mutable_env_get_parameters_request()->set_env_handle(m_remote_handle);
  while (p && p->name != 0) {
    request.mutable_env_get_parameters_request()->add_names(p->name);
    p++;
  }

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_get_parameters_reply());

  st = reply->env_get_parameters_reply().status();
  if (st) {
    delete reply;
    return (st);
  }

  p = param;
  while (p && p->name) {
    switch (p->name) {
    case HAM_PARAM_CACHESIZE:
      ham_assert(reply->env_get_parameters_reply().has_cachesize());
      p->value = reply->env_get_parameters_reply().cachesize();
      break;
    case HAM_PARAM_PAGESIZE:
      ham_assert(reply->env_get_parameters_reply().has_pagesize());
      p->value = reply->env_get_parameters_reply().pagesize();
      break;
    case HAM_PARAM_MAX_DATABASES:
      ham_assert(reply->env_get_parameters_reply().has_max_env_databases());
      p->value = reply->env_get_parameters_reply().max_env_databases();
      break;
    case HAM_PARAM_FLAGS:
      ham_assert(reply->env_get_parameters_reply().has_flags());
      p->value = reply->env_get_parameters_reply().flags();
      break;
    case HAM_PARAM_FILEMODE:
      ham_assert(reply->env_get_parameters_reply().has_filemode());
      p->value = reply->env_get_parameters_reply().filemode();
      break;
    case HAM_PARAM_FILENAME:
      if (reply->env_get_parameters_reply().has_filename()) {
        strncpy(filename, reply->env_get_parameters_reply().filename().c_str(),
              sizeof(filename));
        p->value = (ham_u64_t)(&filename[0]);
      }
      break;
    default:
      ham_trace(("unknown parameter %d", (int)p->name));
      break;
    }
    p++;
  }

  delete reply;
  return (0);
}

ham_status_t
RemoteEnvironment::flush(ham_u32_t flags)
{
  Protocol *reply = 0;

  Protocol request(Protocol::ENV_FLUSH_REQUEST);
  request.mutable_env_flush_request()->set_flags(flags);
  request.mutable_env_flush_request()->set_env_handle(m_remote_handle);

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_flush_reply());

  st = reply->env_flush_reply().status();
  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::create_db(Database **pdb, ham_u16_t dbname, ham_u32_t flags,
        const ham_parameter_t *param)
{
  Protocol *reply = 0;
  const ham_parameter_t *p;

  Protocol request(Protocol::ENV_CREATE_DB_REQUEST);
  request.mutable_env_create_db_request()->set_env_handle(m_remote_handle);
  request.mutable_env_create_db_request()->set_dbname(dbname);
  request.mutable_env_create_db_request()->set_flags(flags);

  p = param;
  if (p) {
    for (; p->name; p++) {
      request.mutable_env_create_db_request()->add_param_names(p->name);
      request.mutable_env_create_db_request()->add_param_values(p->value);
    }
  }

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_create_db_reply());

  st = reply->env_create_db_reply().status();
  if (st) {
    delete reply;
    return (st);
  }

  RemoteDatabase *rdb = new RemoteDatabase(this, dbname,
          reply->env_create_db_reply().db_flags());

  rdb->set_remote_handle(reply->env_create_db_reply().db_handle());
  *pdb = rdb;

  delete reply;

  /*
   * on success: store the open database in the environment's list of
   * opened databases
   */
  get_database_map()[dbname] = *pdb;

  return (0);
}

ham_status_t
RemoteEnvironment::open_db(Database **pdb, ham_u16_t dbname, ham_u32_t flags,
        const ham_parameter_t *param)
{
  Protocol *reply = 0;
  const ham_parameter_t *p;

  /* make sure that this database is not yet open */
  if (get_database_map().find(dbname) !=  get_database_map().end())
    return (HAM_DATABASE_ALREADY_OPEN);

  Protocol request(Protocol::ENV_OPEN_DB_REQUEST);
  request.mutable_env_open_db_request()->set_env_handle(m_remote_handle);
  request.mutable_env_open_db_request()->set_dbname(dbname);
  request.mutable_env_open_db_request()->set_flags(flags);

  p = param;
  if (p) {
    for (; p->name; p++) {
      request.mutable_env_open_db_request()->add_param_names(p->name);
      request.mutable_env_open_db_request()->add_param_values(p->value);
    }
  }

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_env_open_db_reply());

  st = reply->env_open_db_reply().status();
  if (st) {
    delete reply;
    return (st);
  }

  RemoteDatabase *rdb = new RemoteDatabase(this, dbname,
          reply->env_open_db_reply().db_flags());
  rdb->set_remote_handle(reply->env_open_db_reply().db_handle());
  *pdb = rdb;

  delete reply;

  // on success: store the open database in the environment's list of
  // opened databases
  get_database_map()[dbname] = *pdb;

  return (0);
}

ham_status_t
RemoteEnvironment::close(ham_u32_t flags)
{
  ham_status_t st = 0;
  (void)flags;

  /* close all databases */
  Environment::DatabaseMap::iterator it = get_database_map().begin();
  while (it != get_database_map().end()) {
    Environment::DatabaseMap::iterator it2 = it; it++;
    Database *db = it2->second;
    if (flags & HAM_AUTO_CLEANUP)
      st = ham_db_close((ham_db_t *)db, flags | HAM_DONT_LOCK);
    else
      st = db->close(flags);
    if (st)
      return (st);
  }

  Protocol request(Protocol::DISCONNECT_REQUEST);
  request.mutable_disconnect_request()->set_env_handle(m_remote_handle);

  Protocol *reply = 0;
  st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->type() == Protocol::DISCONNECT_REPLY);

  st = reply->disconnect_reply().status();
  if (st == 0) {
    os_socket_close(&m_socket);
    m_remote_handle = 0;
  }

  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::txn_begin(Transaction **txn, const char *name,
                ham_u32_t flags)
{
  ham_status_t st;
  Protocol *reply = 0;

  Protocol request(Protocol::TXN_BEGIN_REQUEST);
  request.mutable_txn_begin_request()->set_env_handle(m_remote_handle);
  request.mutable_txn_begin_request()->set_flags(flags);
  if (name)
    request.mutable_txn_begin_request()->set_name(name);

  st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_txn_begin_reply());

  st = reply->txn_begin_reply().status();
  if (st) {
    delete reply;
    return (st);
  }

  *txn = new Transaction(this, name, flags);
  (*txn)->set_remote_handle(reply->txn_begin_reply().txn_handle());
    append_txn(*txn);

  delete reply;
  return (0);
}

ham_status_t
RemoteEnvironment::txn_commit(Transaction *txn, ham_u32_t flags)
{
  Protocol *reply = 0;

  Protocol request(Protocol::TXN_COMMIT_REQUEST);
  request.mutable_txn_commit_request()->set_txn_handle(txn->get_remote_handle());
  request.mutable_txn_commit_request()->set_flags(flags);

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_txn_commit_reply());

  st = reply->txn_commit_reply().status();
  if (st == 0) {
    remove_txn(txn);
    delete txn;
  }

  delete reply;
  return (st);
}

ham_status_t
RemoteEnvironment::txn_abort(Transaction *txn, ham_u32_t flags)
{
  Protocol *reply = 0;

  Protocol request(Protocol::TXN_ABORT_REQUEST);
  request.mutable_txn_abort_request()->set_txn_handle(txn->get_remote_handle());
  request.mutable_txn_abort_request()->set_flags(flags);

  ham_status_t st = perform_request(&request, &reply);
  if (st) {
    delete reply;
    return (st);
  }

  ham_assert(reply != 0);
  ham_assert(reply->has_txn_abort_reply());

  st = reply->txn_abort_reply().status();
  if (st == 0) {
    remove_txn(txn);
    delete txn;
  }

  delete reply;
  return (st);
}

} // namespace hamsterdb

#endif // HAM_ENABLE_REMOTE
