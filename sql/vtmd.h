#ifndef VTMD_INCLUDED
#define VTMD_INCLUDED

#include "table.h"
#include "unireg.h"
#include <mysqld_error.h>

class THD;

class VTMD_table
{
  TABLE *vtmd;
  const TABLE_LIST &about;
  String vtmd_name;
  String vtmd_new_name;
  handlerton *hton;

private:
  VTMD_table(const VTMD_table&); // prohibit copying references

public:
  enum {
    FLD_START= 0,
    FLD_END,
    FLD_NAME,
    FLD_ARCHIVE_NAME,
    FLD_COL_RENAMES,
    FIELD_COUNT
  };

  enum {
    IDX_END= 0
  };

  VTMD_table(TABLE_LIST &_about) :
    vtmd(NULL),
    about(_about),
    hton(NULL)
  {}

  bool create(THD *thd, String &vtmd_name);
  bool find_record(THD *thd, ulonglong sys_trx_end, bool &found);
  bool write_row(THD *thd, const char* archive_name= NULL);
  bool try_rename(THD *thd, LEX_STRING &new_db, LEX_STRING &new_alias);
  bool revert_rename(THD *thd, LEX_STRING &new_db);
};

#endif // VTMD_INCLUDED