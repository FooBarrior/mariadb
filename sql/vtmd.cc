#include "vtmd.h"
#include "sql_base.h"
#include "sql_class.h"

bool VTMD_table::write_row(THD *thd, ulonglong alter_trx_id)
{
  TABLE_LIST table_list;
  TABLE *table;
  bool result= true;
  bool need_close= false;
  bool need_rnd_end= false;
  int ret;
  Open_tables_backup open_tables_backup;
  ulonglong save_thd_options;
  Diagnostics_area new_stmt_da(thd->query_id, false, true);
  Diagnostics_area *save_stmt_da= thd->get_stmt_da();
  thd->set_stmt_da(&new_stmt_da);

  save_thd_options= thd->variables.option_bits;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;

  table_list.init_one_table(MYSQL_SCHEMA_NAME.str, MYSQL_SCHEMA_NAME.length,
                            VERS_VTD_NAME.str, VERS_VTD_NAME.length,
                            VERS_VTD_NAME.str,
                            TL_WRITE_CONCURRENT_INSERT);

  if (!(table= open_log_table(thd, &table_list, &open_tables_backup)))
    goto err;

  need_close= true;

  if ((ret= table->file->extra(HA_EXTRA_MARK_AS_LOG_TABLE)) ||
      (ret= table->file->ha_rnd_init(0)))
  {
    table->file->print_error(ret, MYF(0));
    goto err;
  }

  need_rnd_end= true;

  /* Honor next number columns if present */
  table->next_number_field= table->found_next_number_field;

  if (table->s->fields != 6)
  {
    my_printf_error(ER_VERS_VTMD_ERROR, "unexpected fields count: %d", MYF(0), table->s->fields);
    goto err;
  }

  table->field[TRX_ID_START]->store(alter_trx_id, true);
  table->field[TRX_ID_START]->set_notnull();
  table->field[TRX_ID_END]->store((longlong) 0, true);
  table->field[TRX_ID_END]->set_notnull();
  table->field[OLD_NAME]->set_null();
  table->field[NAME]->store(STRING_WITH_LEN("name"), system_charset_info);
  table->field[NAME]->set_notnull();
  table->field[FRM_IMAGE]->set_null();
  table->field[COL_RENAMES]->set_null();

  if ((ret= table->file->ha_write_row(table->record[0]))  )
  {
    table->file->print_error(ret, MYF(0));
    goto err;
  }

  result= false;

err:
  thd->set_stmt_da(save_stmt_da);

  if (result)
    my_error(ER_VERS_VTMD_ERROR, MYF(0), new_stmt_da.message());

  if (need_rnd_end)
  {
    table->file->ha_rnd_end();
    table->file->ha_release_auto_increment();
  }

  if (need_close)
    close_log_table(thd, &open_tables_backup);

  thd->variables.option_bits= save_thd_options;
  return result;
}
