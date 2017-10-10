#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"
#include "sql_handler.h" // mysql_ha_rm_tables()
#include "sql_table.h"
#include "sql_select.h"
#include "table_cache.h" // tdc_remove_table()
#include "key.h"
#include "sql_show.h"
#include "sql_parse.h"
#include "sql_lex.h"
#include "sp_head.h"
#include "sp_rcontext.h"

const MD_naming VERS_VTMD_NAMING("vtmd_template", "_vtmd");
const MD_naming VERS_CMMD_NAMING("cmmd_template", "_cmmd");

template <const MD_naming &names>
bool
MD_table<names>::create(THD *thd)
{
  Table_specification_st create_info;
  TABLE_LIST src_table, table;
  create_info.init(DDL_options_st::OPT_LIKE);
  create_info.options|= HA_VTMD;
  create_info.alias= md_table_name_;
  table.init_one_table(
    DB_WITH_LEN(about_tl_),
    XSTRING_WITH_LEN(md_table_name_),
    md_table_name_,
    TL_READ);
  src_table.init_one_table(
    LEX_STRING_WITH_LEN(MYSQL_SCHEMA_NAME),
    XSTRING_WITH_LEN(names.templ),
    names.templ,
    TL_READ);

  Open_tables_auto_backup open_tables(thd);
  Query_tables_auto_backup query_tables(thd);
  thd->lex->sql_command= query_tables.get().sql_command;
  thd->lex->add_to_query_tables(&src_table);

  MDL_auto_lock mdl_lock(thd, table);
  if (mdl_lock.acquire_error())
    return true;

  Reprepare_observer *reprepare_observer= thd->m_reprepare_observer;
  partition_info *work_part_info= thd->work_part_info;
  thd->m_reprepare_observer= NULL;
  thd->work_part_info= NULL;
  bool rc= mysql_create_like_table(thd, &table, &src_table, &create_info);
  if (!rc)
  {
    DBUG_ASSERT(src_table.table);
    tc_release_table(src_table.table);
    thd->reset_open_tables_state(thd);
  }
  thd->m_reprepare_observer= reprepare_observer;
  thd->work_part_info= work_part_info;
  return rc;
}

template <const MD_naming &names>
bool
MD_table<names>::open(Local_da &local_da, bool *created)
{
  THD *thd= local_da.thd;

  if (created)
    *created= false;

  if (0 == md_table_name_.length() && update_table_name())
    return true;

  while (true) // max 2 iterations
  {
    md_tl_.init_one_table(
      DB_WITH_LEN(about_tl_),
      XSTRING_WITH_LEN(md_table_name_),
      md_table_name_,
      TL_WRITE_CONCURRENT_INSERT);

    TABLE *res= open_log_table(thd, &md_tl_, &ot_backup_);
    if (res)
      return false;

    if (created && !*created && local_da.is_error() && local_da.sql_errno() == ER_NO_SUCH_TABLE)
    {
      local_da.reset_diagnostics_area();
      if (create(thd))
        break;
      *created= true;
    }
    else
      break;
  }
  if (local_da.is_error() && local_da.sql_errno() == ER_NOT_LOG_TABLE)
  {
    local_da.reset_diagnostics_area();
    my_printf_error(ER_VERS_VTMD_ERROR, "Table `%s.%s` is not a VTMD table",
      MYF(0), md_tl_.db, md_tl_.table_name);
  }
  return true;
}


bool
VTMD_table::find_record(ulonglong sys_trx_end, bool &found)
{
  int error;
  key_buf_t key;
  found= false;
  TABLE *vtmd= md_tl_.table;
  DBUG_ASSERT(vtmd);

  if (key.allocate(vtmd->s->max_unique_length))
    return true;

  DBUG_ASSERT(sys_trx_end);
  vtmd->vers_end_field()->set_notnull();
  vtmd->vers_end_field()->store(sys_trx_end, true);
  key_copy(key, vtmd->record[0], vtmd->key_info + IDX_TRX_END, 0);

  error= vtmd->file->ha_index_read_idx_map(vtmd->record[1], IDX_TRX_END, key,
                                           HA_WHOLE_KEY, HA_READ_KEY_EXACT);
  if (error)
  {
    if (error == HA_ERR_RECORD_DELETED || error == HA_ERR_KEY_NOT_FOUND)
      return false;
    vtmd->file->print_error(error, MYF(0));
    return true;
  }

  restore_record(vtmd, record[1]);

  found= true;
  return false;
}

bool
VTMD_table::update(THD *thd, VTMD_update_args args)
{
  bool result= true;
  bool found= false;
  bool created;
  int error;
  size_t an_len= 0;
  ulonglong save_thd_options;
  {
    Local_da local_da(thd, ER_VERS_VTMD_ERROR);

    save_thd_options= thd->variables.option_bits;
    thd->variables.option_bits&= ~OPTION_BIN_LOG;

    if (open(local_da, &created))
      goto no_close;

    TABLE *vtmd= md_tl_.table;
    DBUG_ASSERT(vtmd);
    if (!vtmd->versioned())
    {
      my_message(ER_VERS_VTMD_ERROR, "VTMD is not versioned", MYF(0));
      goto quit;
    }

    if (!created && find_record(ULONGLONG_MAX, found))
      goto quit;

    if ((error= vtmd->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)))
    {
      vtmd->file->print_error(error, MYF(0));
      goto quit;
    }

    /* Honor next number columns if present */
    vtmd->next_number_field= vtmd->found_next_number_field;

    if (vtmd->s->fields != FIELD_COUNT)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` unexpected fields count: %d", MYF(0),
                      vtmd->s->db.str, vtmd->s->table_name.str, vtmd->s->fields);
      goto quit;
    }

    if (found)
    {
      if (args.archive_name)
      {
        an_len= strlen(args.archive_name);
        vtmd->field[FLD_ARCHIVE_NAME]->store(args.archive_name, an_len, table_alias_charset);
        vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
      }
      else
      {
        vtmd->field[FLD_ARCHIVE_NAME]->set_null();
      }
      vtmd->field[FLD_CMMD]->store(args.has_cmmd(), true);
      vtmd->field[FLD_CMMD]->set_notnull();

      if (thd->lex->sql_command == SQLCOM_CREATE_TABLE)
      {
        my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` exists and not empty!", MYF(0),
                        vtmd->s->db.str, vtmd->s->table_name.str);
        goto quit;
      }
      vtmd->mark_columns_needed_for_update(); // not needed?
      if (args.archive_name)
      {
        vtmd->s->versioned= false;
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
        vtmd->s->versioned= true;

        if (!error)
        {
          if (thd->lex->sql_command == SQLCOM_DROP_TABLE)
          {
            error= vtmd->file->ha_delete_row(vtmd->record[0]);
          }
          else
          {
            DBUG_ASSERT(thd->lex->sql_command == SQLCOM_ALTER_TABLE);
            ulonglong sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            store_record(vtmd, record[1]);
            vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about_tl_), system_charset_info);
            vtmd->field[FLD_NAME]->set_notnull();
            vtmd->field[FLD_ARCHIVE_NAME]->set_null();
            vtmd->field[FLD_CMMD]->set_null();

            error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
            if (error)
              goto err;

            DBUG_ASSERT(an_len);
            while (true)
            { // fill archive_name of last sequential renames
              bool found;
              if (find_record(sys_trx_end, found))
                goto quit;
              if (!found || !vtmd->field[FLD_ARCHIVE_NAME]->is_null())
                break;

              store_record(vtmd, record[1]);
              vtmd->field[FLD_ARCHIVE_NAME]->store(args.archive_name, an_len, table_alias_charset);
              vtmd->field[FLD_ARCHIVE_NAME]->set_notnull();
              vtmd->s->versioned= false;
              error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
              vtmd->s->versioned= true;
              if (error)
                goto err;
              sys_trx_end= (ulonglong) vtmd->vers_start_field()->val_int();
            } // while (true)
          } // else (thd->lex->sql_command != SQLCOM_DROP_TABLE)
        } // if (!error)
      } // if (archive_name)
      else
      {
        vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about_tl_), system_charset_info);
        vtmd->field[FLD_NAME]->set_notnull();
        error= vtmd->file->ha_update_row(vtmd->record[1], vtmd->record[0]);
      }
      if (!error && args.has_cmmd())
      {
        ulonglong sys_trx_start= (ulonglong) vtmd->vers_start_field()->val_int();
        close_log_table(thd, &ot_backup_);
        CMMD_table cmmd(about_tl_);
        result= cmmd.update(local_da, sys_trx_start, *args.cmmd);
        goto no_close;
      }
    } // if (found)
    else
    {
      vtmd->field[FLD_NAME]->store(TABLE_NAME_WITH_LEN(about_tl_), system_charset_info);
      vtmd->field[FLD_NAME]->set_notnull();
      vtmd->field[FLD_ARCHIVE_NAME]->set_null();
      vtmd->field[FLD_CMMD]->set_null();
      vtmd->mark_columns_needed_for_insert(); // not needed?
      error= vtmd->file->ha_write_row(vtmd->record[0]);
    }

    if (error)
    {
err:
      vtmd->file->print_error(error, MYF(0));
    }
    else
      result= local_da.is_error();
  }

quit:
  close_log_table(thd, &ot_backup_);

no_close:
  thd->variables.option_bits= save_thd_options;
  return result;
}

bool
VTMD_rename::move_archives(THD *thd, LString &new_db)
{
  md_tl_.init_one_table(
    DB_WITH_LEN(about_tl_),
    XSTRING_WITH_LEN(md_table_name_),
    md_table_name_,
    TL_READ);
  int error;
  bool rc= false;
  SString_fs archive;
  bool end_keyread= false;
  bool index_end= false;
  Open_tables_backup open_tables_backup;
  key_buf_t key;

  TABLE *vtmd= open_log_table(thd, &md_tl_, &open_tables_backup);
  if (!vtmd)
    return true;

  if (key.allocate(vtmd->key_info[IDX_ARCHIVE_NAME].key_length))
  {
    close_log_table(thd, &open_tables_backup);
    return true;
  }

  if ((error= vtmd->file->ha_start_keyread(IDX_ARCHIVE_NAME)))
    goto err;
  end_keyread= true;

  if ((error= vtmd->file->ha_index_init(IDX_ARCHIVE_NAME, true)))
    goto err;
  index_end= true;

  error= vtmd->file->ha_index_first(vtmd->record[0]);
  while (!error)
  {
    if (!vtmd->field[FLD_ARCHIVE_NAME]->is_null())
    {
      vtmd->field[FLD_ARCHIVE_NAME]->val_str(&archive);
      key_copy(key, vtmd->record[0], &vtmd->key_info[IDX_ARCHIVE_NAME],
                vtmd->key_info[IDX_ARCHIVE_NAME].key_length, false);
      error= vtmd->file->ha_index_read_map(vtmd->record[0], key,
        vtmd->key_info[IDX_ARCHIVE_NAME].ext_key_part_map, HA_READ_PREFIX_LAST);
      if (!error)
      {
        if ((rc= move_table(thd, archive, new_db)))
          break;

        error= vtmd->file->ha_index_next(vtmd->record[0]);
      }
    }
    else
    {
      archive.length(0);
      error= vtmd->file->ha_index_next(vtmd->record[0]);
    }
  }

  if (error && error != HA_ERR_END_OF_FILE)
  {
err:
    vtmd->file->print_error(error, MYF(0));
    rc= true;
  }

  if (index_end)
    vtmd->file->ha_index_end();
  if (end_keyread)
    vtmd->file->ha_end_keyread();

  close_log_table(thd, &open_tables_backup);
  return rc;
}

bool
VTMD_rename::move_table(THD *thd, SString_fs &table_name, LString &new_db)
{
  handlerton *table_hton= NULL;
  if (!ha_table_exists(thd, about_tl_.db, table_name, &table_hton) || !table_hton)
  {
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` archive doesn't exist",
        about_tl_.db, table_name.ptr());
    return false;
  }

  if (ha_table_exists(thd, new_db, table_name))
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` archive already exists!", MYF(0),
                    new_db.ptr(), table_name.ptr());
    return true;
  }

  TABLE_LIST tl;
  tl.init_one_table(
    DB_WITH_LEN(about_tl_),
    XSTRING_WITH_LEN(table_name),
    table_name,
    TL_WRITE_ONLY);
  tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &tl);
  if (lock_table_names(thd, &tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about_tl_.db, table_name, false);

  bool rc= mysql_rename_table(
    table_hton,
    about_tl_.db, table_name,
    new_db, table_name,
    NO_FK_CHECKS);
  if (!rc)
    query_cache_invalidate3(thd, &tl, 0);

  return rc;
}

bool
VTMD_rename::try_rename(THD *thd, LString new_db, LString new_alias, VTMD_update_args args)
{
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  TABLE_LIST new_table;

  if (check_exists(thd))
    return true;

  new_table.init_one_table(
    XSTRING_WITH_LEN(new_db),
    XSTRING_WITH_LEN(new_alias),
    new_alias, TL_READ);

  if (add_suffix(new_table, vtmd_new_name))
    return true;

  if (ha_table_exists(thd, new_db, vtmd_new_name))
  {
    if (exists)
    {
      my_printf_error(ER_VERS_VTMD_ERROR, "`%s.%s` table already exists!", MYF(0),
                          new_db.ptr(), vtmd_new_name.ptr());
      return true;
    }
    push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "`%s.%s` table already exists!",
        new_db.ptr(), vtmd_new_name.ptr());
    return false;
  }

  if (!exists)
    return false;

  bool same_db= true;
  if (LString_fs(DB_WITH_LEN(about_tl_)) != LString_fs(new_db))
  {
    // Move archives before VTMD so if the operation is interrupted, it could be continued.
    if (move_archives(thd, new_db))
      return true;
    same_db= false;
  }

  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    DB_WITH_LEN(about_tl_),
    XSTRING_WITH_LEN(md_table_name_),
    md_table_name_,
    TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);

  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, about_tl_.db, md_table_name_, false);
  if (local_da.is_error()) // just safety check
    return true;
  bool rc= mysql_rename_table(hton, about_tl_.db, md_table_name_, new_db,
                              vtmd_new_name, NO_FK_CHECKS);
  if (!rc)
  {
    query_cache_invalidate3(thd, &vtmd_tl, 0);
    if (same_db || args.archive_name || new_alias != LString(TABLE_NAME_WITH_LEN(about_tl_)))
    {
      local_da.finish();
      VTMD_table new_vtmd(new_table);
      rc= new_vtmd.update(thd, args);
    }
  }
  return rc;
}

bool
VTMD_rename::revert_rename(THD *thd, LString new_db)
{
  DBUG_ASSERT(hton);
  Local_da local_da(thd, ER_VERS_VTMD_ERROR);

  TABLE_LIST vtmd_tl;
  vtmd_tl.init_one_table(
    DB_WITH_LEN(about_tl_),
    XSTRING_WITH_LEN(vtmd_new_name),
    vtmd_new_name,
    TL_WRITE_ONLY);
  vtmd_tl.mdl_request.set_type(MDL_EXCLUSIVE);
  mysql_ha_rm_tables(thd, &vtmd_tl);
  if (lock_table_names(thd, &vtmd_tl, 0, thd->variables.lock_wait_timeout, 0))
    return true;
  tdc_remove_table(thd, TDC_RT_REMOVE_ALL, new_db, vtmd_new_name, false);

  bool rc= mysql_rename_table(
    hton,
    new_db, vtmd_new_name,
    new_db, md_table_name_,
    NO_FK_CHECKS);

  if (!rc)
    query_cache_invalidate3(thd, &vtmd_tl, 0);

  return rc;
}

void
VTMD_table::archive_name(
  THD* thd,
  const char* table_name,
  char* new_name,
  size_t new_name_size)
{
  const MYSQL_TIME now= thd->query_start_TIME();
  my_snprintf(new_name, new_name_size, "%s_%04d%02d%02d_%02d%02d%02d_%06d",
              table_name, now.year, now.month, now.day, now.hour, now.minute,
              now.second, now.second_part);
}

bool
VTMD_table::find_archive_name(THD *thd, String &out)
{
  READ_RECORD info;
  int error;
  SQL_SELECT *select= NULL;
  COND *conds= NULL;
  List<TABLE_LIST> dummy;
  SELECT_LEX &select_lex= thd->lex->select_lex;

  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  if (open(local_da))
    return true;

  TABLE *vtmd= md_tl_.table;
  DBUG_ASSERT(vtmd);
  Name_resolution_context &ctx= thd->lex->select_lex.context;
  TABLE_LIST *table_list= ctx.table_list;
  TABLE_LIST *first_name_resolution_table= ctx.first_name_resolution_table;
  table_map map = vtmd->map;
  ctx.table_list= &md_tl_;
  ctx.first_name_resolution_table= &md_tl_;
    vtmd->map= 1;

    md_tl_.vers_conditions= about_tl_.vers_conditions;
  if ((error= vers_setup_select(thd, &md_tl_, &conds, &select_lex)) ||
      (error= setup_conds(thd, &md_tl_, dummy, &conds)))
    goto err;

  select= make_select(vtmd, 0, 0, conds, NULL, 0, &error);
  if (error)
    goto loc_err;

  error= init_read_record(&info, thd, vtmd, select, NULL,
                          1 /* use_record_cache */, true /* print_error */,
                          false /* disable_rr_cache */);
  if (error)
    goto loc_err;

  while (!(error= info.read_record(&info)) && !thd->killed && !thd->is_error())
  {
    if (!select || select->skip_record(thd) > 0)
    {
      vtmd->field[FLD_ARCHIVE_NAME]->val_str(&out);
      break;
    }
  }

  if (error < 0)
    my_error(ER_NO_SUCH_TABLE, MYF(0), about_tl_.db, about_tl_.alias);

loc_err:
  end_read_record(&info);
err:
  delete select;
  ctx.table_list= table_list;
  ctx.first_name_resolution_table= first_name_resolution_table;
    vtmd->map= map;
  close_log_table(thd, &ot_backup_);
  DBUG_ASSERT(!error || local_da.is_error());
  return error;
}

static
bool
get_vtmd_tables(THD *thd, const char *db,
                size_t db_length, Dynamic_array<LEX_STRING *> &table_names)
{
  LOOKUP_FIELD_VALUES lookup_field_values= {
    *thd->make_lex_string(db, db_length),
    *thd->make_lex_string(C_STRING_WITH_LEN("%_vtmd")), false, true
  };

  int res= make_table_name_list(thd, &table_names, thd->lex, &lookup_field_values,
                           &lookup_field_values.db_value);
  return res;
}

bool
VTMD_table::get_archive_tables(THD *thd, const char *db, size_t db_length,
                               Dynamic_array<String> &result)
{
  Dynamic_array<LEX_STRING *> vtmd_tables;
  if (get_vtmd_tables(thd, db, db_length, vtmd_tables))
    return true;

  Local_da local_da(thd, ER_VERS_VTMD_ERROR);
  for (uint i= 0; i < vtmd_tables.elements(); i++)
  {
    const LEX_STRING &table_name= *vtmd_tables.at(i);

    Open_tables_backup open_tables_backup;
    TABLE_LIST table_list;
    table_list.init_one_table(db, db_length, LEX_STRING_WITH_LEN(table_name),
                              table_name.str, TL_READ);

    TABLE *table= open_log_table(thd, &table_list, &open_tables_backup);
    if (!table || !table->vers_vtmd())
    {
      if (table)
        close_log_table(thd, &open_tables_backup);
      else
      {
        if (local_da.is_error() && local_da.sql_errno() == ER_NOT_LOG_TABLE)
          local_da.reset_diagnostics_area();
        else
          return true;
      }
      push_warning_printf(
        thd, Sql_condition::WARN_LEVEL_WARN,
        ER_VERS_VTMD_ERROR,
        "Table `%s.%s` is not a VTMD table",
        db, table_name.str);
      continue;
    }

    READ_RECORD read_record;
    int error= 0;
    SQL_SELECT *sql_select= make_select(table, 0, 0, NULL, NULL, 0, &error);
    if (error)
    {
      close_log_table(thd, &open_tables_backup);
      return true;
    }
    error= init_read_record(&read_record, thd, table, sql_select, NULL, 1, 1, false);
    if (error)
    {
      delete sql_select;
      close_log_table(thd, &open_tables_backup);
      return true;
    }

    while (!(error= read_record.read_record(&read_record)))
    {
      Field *field= table->field[FLD_ARCHIVE_NAME];
      if (field->is_null())
        continue;

      String archive_name;
      field->val_str(&archive_name);
      archive_name.set_ascii(strmake_root(thd->mem_root, archive_name.c_ptr(),
                                          archive_name.length()),
                             archive_name.length());
      result.push(archive_name);
    }
    // check for EOF
    if (!thd->is_error())
      error= 0;

    end_read_record(&read_record);
    delete sql_select;
    close_log_table(thd, &open_tables_backup);
  }

  return false;
}

bool
VTMD_table::setup_select(THD* thd)
{
  SString archive_name;
  if (find_archive_name(thd, archive_name))
    return true;

  if (archive_name.length() == 0)
    return false;

  about_tl_.table_name= (char *) thd->memdup(archive_name.c_ptr_safe(), archive_name.length() + 1);
  about_tl_.table_name_length= archive_name.length();
  DBUG_ASSERT(!about_tl_.mdl_request.ticket);
  about_tl_.mdl_request.init(MDL_key::TABLE, about_tl_.db, about_tl_.table_name,
                          about_tl_.mdl_request.type, about_tl_.mdl_request.duration);
  about_tl_.vers_force_alias= true;
  // Since we modified SELECT_LEX::table_list, we need to invalidate current SP
  if (thd->spcont)
  {
    DBUG_ASSERT(thd->spcont->sp);
    thd->spcont->sp->set_sp_cache_version(ULONG_MAX);
  }
  return false;
}


bool
CMMD_table::update(Local_da &local_da, ulonglong sys_trx_start, CMMD_map& cmmd_map)
{
  THD* thd= local_da.thd;
  bool result= true;
  bool created;

  if (open(local_da, &created))
    goto open_error;

  {
    TABLE *cmmd= md_tl_.table;
    DBUG_ASSERT(cmmd);

    for (uint i= 0; i < cmmd_map.elements(); i++)
    {
      const IntPair &map= cmmd_map.at(i);
      cmmd->field[FLD_START]->store(sys_trx_start, true);
      cmmd->field[FLD_START]->set_notnull();
      cmmd->field[FLD_CURRENT]->store(map.first, true);
      cmmd->field[FLD_CURRENT]->set_notnull();
      cmmd->field[FLD_LATER]->store(map.second, true);
      cmmd->field[FLD_LATER]->set_notnull();
      cmmd->mark_columns_needed_for_insert(); // not needed?
      int error= cmmd->file->ha_write_row(cmmd->record[0]);
      if (error)
      {
        cmmd->file->print_error(error, MYF(0));
        goto quit;
      }
    }

    result= local_da.is_error();
  }

quit:
  close_log_table(thd, &ot_backup_);

open_error:
  return result;
}
