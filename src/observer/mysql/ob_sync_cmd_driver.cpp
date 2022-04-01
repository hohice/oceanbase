/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX SERVER

#include "ob_sync_cmd_driver.h"

#include "lib/profile/ob_perf_event.h"
#include "obsm_row.h"
#include "sql/resolver/cmd/ob_variable_set_stmt.h"
#include "observer/mysql/obmp_query.h"
#include "rpc/obmysql/packet/ompk_row.h"
#include "rpc/obmysql/packet/ompk_eof.h"

namespace oceanbase {
using namespace common;
using namespace sql;
using namespace obmysql;
using namespace share;
namespace observer {

ObSyncCmdDriver::ObSyncCmdDriver(const ObGlobalContext &gctx, const ObSqlCtx &ctx, sql::ObSQLSessionInfo &session,
    ObQueryRetryCtrl &retry_ctrl, ObIMPPacketSender &sender)
    : ObQueryDriver(gctx, ctx, session, retry_ctrl, sender)
{}

ObSyncCmdDriver::~ObSyncCmdDriver()
{}

int ObSyncCmdDriver::response_result(ObMySQLResultSet &result)
{
  int ret = OB_SUCCESS;
  bool process_ok = false;
  // for select SQL
  bool ac = true;
  if (OB_FAIL(session_.get_autocommit(ac))) {
    LOG_WARN("fail to get autocommit", K(ret));
  } else if (OB_FAIL(result.sync_open())) {
    int cret = OB_SUCCESS;
    int cli_ret = OB_SUCCESS;
    if (ObStmt::is_ddl_stmt(result.get_stmt_type(), result.has_global_variable())) {
      // even failed, still need update lsv, as drop multi tables are not in one trx.
      cret = process_schema_version_changes(result);
      if (OB_SUCCESS != cret) {
        LOG_WARN("failed to set schema version changes", K(cret));
      }
    }

    cret = result.close();
    if (cret != OB_SUCCESS) {
      LOG_WARN("close result set fail", K(cret));
    }

    retry_ctrl_.test_and_save_retry_state(gctx_, ctx_, result, ret, cli_ret);
    LOG_WARN("result set open failed, check if need retry", K(ret), K(cli_ret), K(retry_ctrl_.need_retry()));
    ret = cli_ret;

  } else if (FALSE_IT(THIS_WORKER.disable_retry())) {
  } else if (result.is_with_rows()) {
    if (!result.is_pl_stmt(result.get_stmt_type())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("Not SELECT, should not have any row!!!", K(ret));
    } else if (OB_FAIL(response_query_result(result))) {
      LOG_WARN("response query result fail", K(ret));
      int cret = result.close();
      if (cret != OB_SUCCESS) {
        LOG_WARN("close result set fail", K(cret));
      }
    } else {
      OMPKEOF eofp;
      const ObWarningBuffer *warnings_buf = common::ob_get_tsi_warning_buffer();
      uint16_t warning_count = 0;
      if (OB_ISNULL(warnings_buf)) {
        LOG_WARN("can not get thread warnings buffer");
      } else {
        warning_count = static_cast<uint16_t>(warnings_buf->get_readable_warning_count());
      }
      eofp.set_warning_count(warning_count);
      ObServerStatusFlags flags = eofp.get_server_status();
      flags.status_flags_.OB_SERVER_STATUS_IN_TRANS = (session_.is_server_status_in_transaction() ? 1 : 0);
      flags.status_flags_.OB_SERVER_STATUS_AUTOCOMMIT = (ac ? 1 : 0);
      flags.status_flags_.OB_SERVER_MORE_RESULTS_EXISTS = result.has_more_result();
      // flags.status_flags_.OB_SERVER_PS_OUT_PARAMS = 1;
      if (!session_.is_obproxy_mode()) {
        // in java client or others, use slow query bit to indicate partition hit or not
        flags.status_flags_.OB_SERVER_QUERY_WAS_SLOW = !session_.partition_hit().get_bool();
      }

      eofp.set_server_status(flags);

      // for proxy
      // in multi-stmt, send extra ok packet in the last stmt(has no more result)
      if (!result.has_more_result()) {
        sender_.update_last_pkt_pos();
      }
      if (OB_FAIL(sender_.response_packet(eofp))) {
        LOG_WARN("response packet fail", K(ret));
      }
    }
  } else { /*do nothing*/
  }

  if (OB_SUCC(ret)) {
    // for CRUD SQL
    // must be called before result.close()
    process_schema_version_changes(result);

    if (OB_FAIL(result.close())) {
      LOG_WARN("close result set fail", K(ret));
    } else if (!result.is_with_rows() || (sender_.need_send_extra_ok_packet() && !result.has_more_result())) {
      process_ok = true;
      ObOKPParam ok_param;
      ok_param.message_ = const_cast<char *>(result.get_message());
      ok_param.affected_rows_ = result.get_affected_rows();
      ok_param.lii_ = result.get_last_insert_id_to_client();
      const ObWarningBuffer *warnings_buf = common::ob_get_tsi_warning_buffer();
      if (OB_ISNULL(warnings_buf)) {
        LOG_WARN("can not get thread warnings buffer");
      } else {
        ok_param.warnings_count_ = static_cast<uint16_t>(warnings_buf->get_readable_warning_count());
      }
      ok_param.is_partition_hit_ = session_.partition_hit().get_bool();
      ok_param.has_more_result_ = result.has_more_result();
      if (OB_FAIL(sender_.send_ok_packet(session_, ok_param))) {
        LOG_WARN("send ok packet fail", K(ok_param), K(ret));
      }
    } else { /*do nothing*/
    }
  }

  if (!OB_SUCC(ret) && !process_ok && !retry_ctrl_.need_retry()) {
    int sret = OB_SUCCESS;
    bool is_partition_hit = session_.partition_hit().get_bool();
    if (OB_SUCCESS != (sret = sender_.send_error_packet(ret, NULL, is_partition_hit))) {
      LOG_WARN("send error packet fail", K(sret), K(ret));
    }
  }

  return ret;
}

// must be called before result.close()
// two aspects:
// - set session last_schema_version to proxy for part DDL
// - promote local schema up to target version if last_schema_version is set
int ObSyncCmdDriver::process_schema_version_changes(const ObMySQLResultSet &result)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(gctx_.schema_service_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_ERROR("invalid schema service", K(ret));
  } else {
    uint64_t tenant_id = session_.get_effective_tenant_id();
    // - set session last_schema_version to proxy for DDL
    if (ObStmt::is_ddl_stmt(result.get_stmt_type(), result.has_global_variable())) {
      if (OB_FAIL(ObSQLUtils::update_session_last_schema_version(*gctx_.schema_service_, session_))) {
        LOG_WARN("fail to update session last schema_version", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      // - promote local schema up to target version if last_schema_version is set
      if (result.get_stmt_type() == stmt::T_VARIABLE_SET) {
        const ObVariableSetStmt *set_stmt = static_cast<const ObVariableSetStmt *>(result.get_cmd());
        if (NULL != set_stmt) {
          ObVariableSetStmt::VariableNamesSetNode tmp_node;  // just for init node
          for (int64_t i = 0; OB_SUCC(ret) && i < set_stmt->get_variables_size(); ++i) {
            ObVariableSetStmt::VariableNamesSetNode &var_node = tmp_node;
            ObString set_var_name(OB_SV_LAST_SCHEMA_VERSION);
            if (OB_FAIL(set_stmt->get_variable_node(i, var_node))) {
              LOG_WARN("fail to get_variable_node", K(i), K(ret));
            } else if (var_node.is_set_variable_) {
              if (ObCharset::case_insensitive_equal(var_node.var_set_node_.variable_name_, set_var_name)) {
                if (OB_FAIL(check_and_refresh_schema(tenant_id))) {
                  LOG_WARN("failed to check_and_refresh_schema", K(ret), K(tenant_id));
                }
                break;
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObSyncCmdDriver::check_and_refresh_schema(uint64_t tenant_id)
{
  int ret = OB_SUCCESS;
  int64_t local_version = 0;
  int64_t last_version = 0;

  if (OB_ISNULL(gctx_.schema_service_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("null schema service", K(ret), K(gctx_));
  } else {
    if (OB_FAIL(gctx_.schema_service_->get_tenant_refreshed_schema_version(tenant_id, local_version))) {
      LOG_WARN("fail to get tenant refreshed schema version", K(ret));
    } else if (OB_FAIL(session_.get_ob_last_schema_version(last_version))) {
      LOG_WARN("failed to get_sys_variable", K(OB_SV_LAST_SCHEMA_VERSION));
    } else if (local_version >= last_version) {
      // skip
    } else if (OB_FAIL(gctx_.schema_service_->async_refresh_schema(tenant_id, last_version))) {
      LOG_WARN("failed to refresh schema", K(ret), K(tenant_id), K(last_version));
    }
  }
  return ret;
}

int ObSyncCmdDriver::response_query_result(ObMySQLResultSet &result)
{
  int ret = OB_SUCCESS;
  session_.get_trans_desc().consistency_wait();

  const common::ObNewRow *row = NULL;
  if (OB_FAIL(result.next_row(row))) {
    LOG_WARN("fail to get next row", K(ret));
  } else if (OB_FAIL(response_query_header(result, result.has_more_result(), true))) {
    LOG_WARN("fail to response query header", K(ret));
  } else if (OB_ISNULL(ctx_.session_info_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("session info is null", K(ret));
  } else {
    ObNewRow *tmp_row = const_cast<ObNewRow *>(row);
    for (int64_t i = 0; OB_SUCC(ret) && i < tmp_row->get_count(); i++) {
      ObObj &value = tmp_row->get_cell(i);
      if (ob_is_string_type(value.get_type()) && CS_TYPE_INVALID != value.get_collation_type()) {
        OZ(convert_string_value_charset(value, result));
      } else if (value.is_clob_locator() && OB_FAIL(convert_lob_value_charset(value, result))) {
        LOG_WARN("convert lob value charset failed", K(ret));
      }
      if (OB_SUCC(ret) && OB_FAIL(convert_lob_locator_to_longtext(value, result))) {
        LOG_WARN("convert lob locator to longtext failed", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      MYSQL_PROTOCOL_TYPE protocol_type = result.is_ps_protocol() ? BINARY : TEXT;
      const ObSQLSessionInfo *tmp_session = result.get_exec_context().get_my_session();
      const ObDataTypeCastParams dtc_params = ObBasicSessionInfo::create_dtc_params(tmp_session);
      ObSMRow sm_row(protocol_type,
          *row,
          dtc_params,
          result.get_field_columns(),
          ctx_.schema_guard_,
          tmp_session->get_effective_tenant_id());
      OMPKRow rp(sm_row);
      if (OB_FAIL(sender_.response_packet(rp))) {
        LOG_WARN("response packet fail", K(ret), KP(row));
      }
    }
  }
  return ret;
}

}  // namespace observer
}  // namespace oceanbase
