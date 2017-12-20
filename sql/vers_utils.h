#ifndef VERS_UTILS_INCLUDED
#define VERS_UTILS_INCLUDED

#include "table.h"
#include "sql_class.h"
#include "sql_base.h"
#include "vers_string.h"

class MDL_auto_lock
{
  THD *thd;
  TABLE_LIST &table;
  bool error;

public:
  MDL_auto_lock(THD *_thd, TABLE_LIST &_table, Open_table_context *ot_ctx= NULL) :
    thd(_thd), table(_table)
  {
    DBUG_ASSERT(thd);
    MDL_request protection_request;
    MDL_deadlock_handler mdl_deadlock_handler(ot_ctx);
    if (thd->global_read_lock.can_acquire_protection())
    {
      error= true;
      return;
    }
    protection_request.init(MDL_key::GLOBAL, "", "", MDL_INTENTION_EXCLUSIVE,
                            MDL_EXPLICIT);
    if (ot_ctx)
      thd->push_internal_handler(&mdl_deadlock_handler);
    error= thd->mdl_context.acquire_lock(&protection_request, thd->variables.lock_wait_timeout);
    if (ot_ctx)
      thd->pop_internal_handler();
    if (error)
      return;

    table.mdl_request.init(MDL_key::TABLE, table.db, table.table_name, MDL_EXCLUSIVE, MDL_EXPLICIT);
    error= thd->mdl_context.acquire_lock(&table.mdl_request, thd->variables.lock_wait_timeout);
    thd->mdl_context.release_lock(protection_request.ticket);
  }
  ~MDL_auto_lock()
  {
    if (!error)
    {
      DBUG_ASSERT(table.mdl_request.ticket);
      thd->mdl_context.release_lock(table.mdl_request.ticket);
      table.mdl_request.ticket= NULL;
    }
  }
  bool acquire_error() const { return error; }
};


class Local_da : public Diagnostics_area
{
  THD *thd;
  uint sql_error;
  Diagnostics_area *saved_da;

public:
  Local_da(THD *_thd, uint _sql_error= 0) :
    Diagnostics_area(_thd->query_id, false, true),
    thd(_thd),
    sql_error(_sql_error),
    saved_da(_thd->get_stmt_da())
  {
    thd->set_stmt_da(this);
  }
  ~Local_da()
  {
    if (saved_da)
      finish();
  }
  void finish()
  {
    DBUG_ASSERT(saved_da && thd);
    thd->set_stmt_da(saved_da);
    if (is_error())
      my_error(sql_error ? sql_error : sql_errno(), MYF(0), message());
    if (warn_count() > error_count())
      saved_da->copy_non_errors_from_wi(thd, get_warning_info());
    saved_da= NULL;
  }
};


#endif // VERS_UTILS_INCLUDED
