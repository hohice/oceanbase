/*
 * ob_admin_server_executor.cpp
 *
 *  Created on: Aug 29, 2017
 *      Author: yuzhong.zhao
 */


#include "ob_admin_server_executor.h"
#include <getopt.h>
#include <stddef.h>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <iterator>
#include <iomanip>
#include "observer/ob_srv_network_frame.h"
#include "share/ob_encrypt_kms.h" 
#include "lib/utility/ob_print_utils.h"
#include "ob_admin_routine.h"
using namespace std;
using namespace oceanbase::common;
using namespace oceanbase::tools;

namespace oceanbase
{
namespace tools
{
const std::string ObAdminServerExecutor::DEFAULT_HOST = "127.1";
const int ObAdminServerExecutor::DEFAULT_PORT = 2500;
const int64_t ObAdminServerExecutor::DEFAULT_TIMEOUT = 3000000; /* 3s */

static const char *optstring = "h:p:t:s:m:";
static struct option long_options[] = {
  {"host", 1, NULL, 'h'},
  {"port", 1, NULL, 'p'},
  {"timeout", 1, NULL, 't'},
  {"ssl-mode", 1, NULL, 's'},
  {"ssl-cfg-mode", 1, NULL, 'm'},
  {NULL, 0, NULL, 0}
};


ObAdminServerExecutor::ObAdminServerExecutor()
    : inited_(false),
      timeout_(DEFAULT_TIMEOUT),
      ssl_mode_(SSL_MODE_NONE),
      ssl_cfg_mode_(0)
{}

ObAdminServerExecutor::~ObAdminServerExecutor()
{
  g_routines.clear();
}

bool ObAdminServerExecutor::parse_command(int argc, char *argv[])
{
  bool ret = true;
  int option_index = 0;
  int c;
  string host = DEFAULT_HOST;
  int port = DEFAULT_PORT;
  bool has_ssl_opt = false;
  string ssl_str;
  string ssl_cfg_mode_str = "local";
  while (-1 != (c = getopt_long(argc, argv, optstring, long_options, &option_index)))
  {
    switch (c)
    {
      case 'h':
        host = optarg;
        break;
      case 'p':
        port = static_cast<int>(strtol(optarg, NULL, 10));
        break;
      case 't':
        timeout_ = strtol(optarg, NULL, 10);
        break;
      case 's':
        ssl_str = optarg;
        has_ssl_opt = true;
        break;
      case 'm':
        ssl_cfg_mode_str = optarg;
        break;
      case '?':
      case ':':
        ret = false;
        break;
      default:
        break;
    }
  }

  if (!ret)
  {
    return ret;
  }
  if (optind > argc)
  {
    return false;
  }
  if (optind == argc)
  {
    cerr << "no command specified!" << endl;
    return false;
  }
  if (port <= 0 || port >= 65536)
  {
    cerr << "port not valid: " << port << endl;
    return false;
  }
  if (has_ssl_opt) {
    if (0 == strcasecmp(ssl_str.c_str(), "INTL")) {
      ssl_mode_ = SSL_MODE_INTL;
    } else if (0 == strcasecmp(ssl_str.c_str(), "SM")) {
      ssl_mode_ = SSL_MODE_SM;
    } else {
      ssl_mode_ = SSL_MODE_INTL;
    }
  } else {
    ssl_mode_ = SSL_MODE_NONE;
  }

  if (0 == strcasecmp(ssl_cfg_mode_str.c_str(), "local")) {
    ssl_cfg_mode_= 0;
  } else if (0 == strcasecmp(ssl_cfg_mode_str.c_str(), "bkmi")) {
    ssl_cfg_mode_ = 1;
  } 
  dst_server_.set_ip_addr(host.c_str(), port);

  ostringstream ss;
  copy(argv + optind, argv + argc, ostream_iterator<char*>(ss, " "));
  cmd_ = ss.str();
  cmd_.erase(cmd_.end() - 1);

  return ret;
}

void ObAdminServerExecutor::usage() const
{
  cerr << "============================================================================" << endl;
  cerr << "[USAGE]" << endl;
  cerr << "\tob_admin [OPTION] COMMAND" << endl;
  cerr << "[OPTION]" << endl;
  cerr << "\t-h host  default 127.1" << endl;
  cerr << "\t-p port  default 2500" << endl;
  cerr << "\t-t timeout  default 3000000 (3s)" << endl;
  cerr << "\t-s ssl-mode intl or sm, default intl" << endl;
  cerr << "\t-m ssl-cfg-mode bkmi or local, default local" << endl;
  cerr << "[COMMAND]" << endl;
  for (vector<ObAdminRoutine*>::iterator it = g_routines.begin();
       it != g_routines.end();
       it++)
  {
    cerr << "\t" << left << setw(6) << (*it)->target() << ": " << (*it)->usage() << endl;
  }
}

static int ob_admin_server_read_bkmi_cfg(const char* buf, int64_t &sz)
{
  int ret = OB_SUCCESS;
  FILE *fp = NULL;
  const char* path = "obadmin_ssl_bkmi.cfg";
  if (OB_ISNULL(fp = fopen(path, "rb"))) {
    if (ENOENT == errno) {
      COMMON_LOG(ERROR, "obadmin_ssl_bkmi_cfg.txt is not exist");
      ret = OB_FILE_NOT_EXIST;
    } else {
      ret = OB_IO_ERROR;
      COMMON_LOG(ERROR, "cannot open file", K(path), K(errno));
    }
  } else {
    sz = fread((void *)buf, 1, OB_MAX_CONFIG_VALUE_LEN, fp);

    if (OB_UNLIKELY(0 != ferror(fp))) {
      ret = OB_IO_ERROR;
      COMMON_LOG(ERROR, "read config file error", K(path), K(ret));
    } else if (OB_UNLIKELY(0 == feof(fp))) {
      ret = OB_BUF_NOT_ENOUGH;
      COMMON_LOG(ERROR, "config file is too long", K(path), K(ret));
    } else {
      COMMON_LOG(INFO, "read config file succ", K(path));
    }

    if (OB_UNLIKELY(0 != fclose(fp))) {
      ret = OB_IO_ERROR;
      COMMON_LOG(ERROR, "Close config file failed", K(ret));
    }
  }

  return ret;
}

int ObAdminServerExecutor::load_ssl_config()
{
  int ret = OB_SUCCESS;
  const bool enable_ssl_client_authentication  = (SSL_MODE_NONE == ssl_mode_) ? false : true;
  if (enable_ssl_client_authentication) {
    bool use_bkmi = (0 == ssl_cfg_mode_) ? false : true;
    bool is_sm = (SSL_MODE_SM == ssl_mode_) ? true : false;
    int64_t ssl_key_expired_time = 0;
    const char *ca_cert = NULL;
    const char *public_cert = NULL;
    const char *private_key = NULL;
    if (!use_bkmi) {
      ca_cert = OB_CLIENT_SSL_CA_FILE;
      public_cert = OB_CLIENT_SSL_CERT_FILE;
      private_key = OB_CLIENT_SSL_KEY_FILE;
    } else {
        share::ObSSLClient ssl_client;
        char ssl_kms_info[OB_MAX_CONFIG_VALUE_LEN];
        memset(ssl_kms_info, 0, sizeof(ssl_kms_info));
        int64_t sz = 0;
        if (OB_FAIL(ob_admin_server_read_bkmi_cfg(ssl_kms_info, sz))) {
          COMMON_LOG(ERROR, "read from bkmi config file failed", K(ret));
        } else {
          ObString ssl_config(ssl_kms_info);
          if (OB_FAIL(ssl_client.init(ssl_config.ptr(), ssl_config.length()))) {
            COMMON_LOG(ERROR, "ssl_client_init failed", K(ret), K(ssl_config));
          } else if (OB_FAIL(ssl_client.check_param_valid())) {
            COMMON_LOG(ERROR, "kms client param is not valid", K(ret));
          } else {
            ca_cert = ssl_client.get_root_ca().ptr();
            public_cert = ssl_client.public_cert_.content_.ptr();
            private_key = ssl_client.private_key_.content_.ptr();
          }
        }
    }

    if (EASY_OK != easy_ssl_ob_config_check(ca_cert, public_cert, private_key,  !use_bkmi, is_sm)) {
      COMMON_LOG(ERROR, "Local file mode: key and cert not match");
      ret = OB_INVALID_CONFIG;
    } else if (OB_FAIL(observer::ObSrvNetworkFrame::extract_expired_time(OB_CLIENT_SSL_CERT_FILE, 
      ssl_key_expired_time))) {
      COMMON_LOG(ERROR, "extract_expired_time failed", KR(ret));
    } else if (OB_FAIL(client_.load_ssl_config(ca_cert, public_cert, private_key))) {
      COMMON_LOG(ERROR, "ObNetClient load_ssl_config failed", K(ret), K(is_sm), K(ssl_key_expired_time));
    }
  } else {
    COMMON_LOG(INFO, "no need to open ssl");
  }

  return ret;
}

int ObAdminServerExecutor::execute(int argc, char *argv[])
{
  int ret = OB_SUCCESS;
  if (!parse_command(argc, argv)) {
    usage();
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(client_.init())) {
    COMMON_LOG(WARN, "client init failed", K(ret));
  } else if (OB_FAIL(load_ssl_config())) {
    COMMON_LOG(WARN, "client load_ssl_config failed", K(ret));
  } else if (OB_FAIL(client_.get_proxy(srv_proxy_))) {
    COMMON_LOG(WARN, "get_proxy failed", K(ret));
  } else {
    srv_proxy_.set_server(dst_server_);
    srv_proxy_.set_timeout(timeout_);
    int64_t tenant_id = atoll(getenv("tenant")?:"0")?:OB_DIAG_TENANT_ID;
    srv_proxy_.set_tenant(tenant_id);
    inited_ = true;
    COMMON_LOG(INFO, "process", K(cmd_.c_str()), K_(timeout), K(tenant_id));
    vector<ObAdminRoutine*>::iterator it = find_if(g_routines.begin(), g_routines.end(), RoutineComparer(cmd_));
    if (it == g_routines.end()) {
      cerr << "Unknow command: " << cmd_ << endl;
      return false;
    }
    (*it)->set_timeout(timeout_);
    (*it)->set_command(cmd_);
    (*it)->set_client(&srv_proxy_);
    int ret = (*it)->process();
    COMMON_LOG(INFO, "process result:", K(ret));
  }
  return ret;
}


}
}
