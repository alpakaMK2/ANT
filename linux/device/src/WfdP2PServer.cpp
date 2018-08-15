/* Copyright 2017-2018 All Rights Reserved.
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *  
 * [Contact]
 *  Gyeonghwan Hong (redcarrottt@gmail.com)
 *
 * Licensed under the Apache License, Version 2.0(the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <WfdP2PServer.h>

#include <WfdServerAdapter.h>
#include <Core.h>
#include <Util.h>
#include <Counter.h>
#include <DebugLog.h>

#include <thread>
#include <mutex>
#include <condition_variable>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#define DEFAULT_WFD_IP_ADDRESS "192.168.49.1"
#define UDHCPD_CONFIG_PATH "dhcpd.conf"

using namespace cm;

struct sigaction WfdP2PServer::sSigaction;
struct sigaction WfdP2PServer::sSigactionOld;
bool WfdP2PServer::sDhcpdMonitoring = 0;
int WfdP2PServer::sDhcpdPid = 0;

bool WfdP2PServer::allow_impl(void) {
  char buf[1024];
  int ret;

  // Set Wi-fi WPS Device Name
  ret = this->set_wps_device_name(this->mWfdDeviceName, buf, 1024);
  if (ret < 0)
    return false;

  // Add Wi-fi Direct P2P Group
  ret = this->wfd_add_p2p_group(buf, 1024);
  if (ret < 0)
    return false;

  // Get WPA Device Name from wpa-cli
  if (this->mWpaDevName[0] == '\0') {
    LOG_VERB("%s", buf);
    char *ptrptr;
    char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);
    while (ptr != NULL) {
      if (strstr(ptr, "p2p-dev")) {
        snprintf(this->mWpaDevName, sizeof(this->mWpaDevName), "%s", ptr);
      } else if (strstr(ptr, "FAIL")) {
        LOG_WARN("P2P Group add failed");
        return false;
      } else if (strstr(ptr, "OK")) {
        LOG_VERB("P2P Group added");
      }

      ptr = strtok_r(NULL, "\t \n\'", &ptrptr);
    }
  }

  assert(this->mWpaDevName[0] != '\0');

  // Get WPA Interface Name from wpa-cli
  ret = this->ping_wpa_cli(buf, 1024);
  if (ret < 0)
    return false;
  else {
    char *ptrptr;
    char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);
    while (ptr != NULL) {
      if (strstr(ptr, "p2p-wlan")) {
        snprintf(this->mWpaIntfName, sizeof(this->mWpaIntfName), "%s", ptr);
      } else if (strstr(ptr, "PONG")) {
        LOG_VERB("Added interface:%s", this->mWpaIntfName);
      } else if (strstr(ptr, "FAIL")) {
        LOG_WARN("P2P Group add failed");
        this->mWpaIntfName[0] = '\0';
        return false;
      }
      ptr = strtok_r(NULL, "\t \n\'", &ptrptr);
    }
  }

  // Set Wi-fi Direct IP
  this->set_wfd_ip_addr(DEFAULT_WFD_IP_ADDRESS);
  
  // Set DHCPD config
  WfdP2PServer::sDhcpdPid = this->set_dhcpd_config();

  // Send "WFD Device MAC Address"
  ret = this->get_wfd_p2p_device_addr(buf, 256);
  if (ret < 0) {
    return false;
  }
  LOG_VERB("WFD Server added : %s", buf);
  int adapter_id = ((WfdServerAdapter*)this->mOwner)->get_id();
  Core::get_instance()->send_noti_private_data(adapter_id, buf, strlen(buf));

  // Set & Send "WFD PIN"
  ret = this->reset_wfd_server(buf, 256);
  if (ret < 0) {
    return false;
  }
  LOG_VERB("WFD Server PIN: %s", buf);
  Core::get_instance()->send_noti_private_data(adapter_id, buf, strlen(buf));

  // Get server's IP address and send the "IP address"
  LOG_VERB("Get server's IP\n"); 
  int ip_wait_it;
  for (ip_wait_it = 0; ip_wait_it < 30; ip_wait_it++) {
    ret = this->get_wfd_ip_address(buf, 256);
    if (ret == 0) {
      LOG_VERB("IP: %s", buf);
      break;
    }
    usleep(300000);
  }

  if (ret < 0) {
    LOG_ERR("IP is not set");
    return false;
  }

  char* ip_address = buf;
  Core::get_instance()->send_noti_private_data(adapter_id, ip_address, strlen(ip_address));

  // Notify IP address to the listeners
  for(std::vector<WfdIpAddressListener*>::iterator it = this->mIpAddrListeners.begin();
      it != this->mIpAddrListeners.end();
      it++) {
    WfdIpAddressListener* listener = (*it);
    listener->on_change_ip_address(ip_address);
  }

  return true;
}

int WfdP2PServer::set_wps_device_name(char *wfd_device_name, char ret[], size_t len) {
  char *const params[] = {"wpa_cli", "set", "device_name", wfd_device_name, NULL};

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::wfd_add_p2p_group(char ret[], size_t len) {
  char *const params[] = {"wpa_cli", "p2p_group_add", NULL};

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::ping_wpa_cli(char ret[], size_t len) {
  char *const params[] = {"wpa_cli", "ping", NULL};

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::set_wfd_ip_addr(char* ip_addr) {
  char *const params[] = {"ifconfig",
    this->mWpaIntfName,
    ip_addr,
    NULL};
  char buf[256];

  assert(this->mWpaIntfName[0] != '\0');

  return Util::run_client(IFCONFIG_PATH, params, buf, 256);
}

int WfdP2PServer::set_dhcpd_config(void) {
  if (!WfdP2PServer::sDhcpdMonitoring) {
    WfdP2PServer::sSigaction.sa_flags = SA_SIGINFO;
    WfdP2PServer::sSigaction.sa_sigaction = WfdP2PServer::sighandler_monitor_udhcpd;
    sigaction(SIGCHLD,
        &WfdP2PServer::sSigaction, &WfdP2PServer::sSigactionOld);
    
    WfdP2PServer::sDhcpdMonitoring = true; 
  }

  char *const params[] = {"udhcpd", UDHCPD_CONFIG_PATH, "-f", NULL};
  char buf[256];
  
  assert(this->mWpaIntfName[0] != '\0');

  // Generate dhcp configuration
  int config_fd = open(UDHCPD_CONFIG_PATH, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  char script[512];

  sprintf(script, "start 192.168.49.20\n"\
          "end 192.168.49.40\n"\
          "interface %s\n"\
          "max_leases 20\n"\
          "option subnet 255.255.255.0\n"\
          "option router 192.168.49.1\n"\
          "option lease 864000\n"\
          "option broadcast 192.168.49.255", this->mWpaIntfName);

  write(config_fd, script, strlen(script)+1);
  close(config_fd);

  return Util::run_client(UDHCPD_PATH, params, buf, 256);
}

int WfdP2PServer::get_wfd_p2p_device_addr(char *dev_addr, size_t len) {
  char buf[1024];
  int ret;
  if (this->mWpaIntfName[0] == '\0') {
    LOG_VERB("WFD is not up");
    return -1;
  }

  ret = this->get_wfd_status(buf, 1024);
  if (ret < 0)
    return ret;

  char *ptrptr;
  char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);
  while (ptr != NULL) {
    if (strstr(ptr, "p2p_device_address")) {
      // p2p_device_address=XX
      sscanf(ptr, "%*[^=]=%s", dev_addr);
      break;
    } else if (strstr(ptr, "FAIL")) {
      LOG_WARN("Get p2p device address failed");
      return -1;
    }

    ptr = strtok_r(NULL, "\t \n\'", &ptrptr);
  }
  return 0;
}

int WfdP2PServer::get_wfd_status(char ret[], size_t len) {
  char *const params[] = {"wpa_cli", "status", NULL};

  assert(this->mWpaIntfName[0] != '\0');

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::reset_wfd_server(char *pin, size_t len) {
  char buf[1024] = {0, };
  int ret;
  if (this->mWpaIntfName[0] == '\0') {
    LOG_VERB("WFD is not up");
    return -1;
  }

  ret = this->reset_wps_pin(buf, 1024);
  if (ret < 0)
    return ret;

  // Selected interface 'p2p-wlan0-9' 33996608
  LOG_VERB("Pin parsing %s", buf);
  char *ptrptr;
  char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);  // Selected
  ptr = strtok_r(NULL, "\t \n\'", &ptrptr);  // interface
  ptr = strtok_r(NULL, "\t \n\'", &ptrptr);  // p2p-wlan0-9
  ptr = strtok_r(NULL, "\t \n\'", &ptrptr);  // 33996608

  snprintf(pin, len, "%s", ptr);
  LOG_VERB("Pin:%s", ptr);

  if (WfdP2PServer::sDhcpdPid == 0) {
    LOG_VERB("UDHCP is off > turn it up");
    WfdP2PServer::sDhcpdPid = this->set_dhcpd_config();
  }

  return 0;
}

int WfdP2PServer::reset_wps_pin(char ret[], size_t len) {
  char *const params[] = {"wpa_cli", "wps_pin", "any", "12345670", NULL};

  assert(this->mWpaIntfName[0] != '\0');

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::get_wfd_ip_address(char *buf, size_t len) {
  int ret = get_wfd_status(buf, 1024);
  if (ret < 0)
    return ret;

  char *ptrptr;
  char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);
  while (ptr != NULL) {
    if (strstr(ptr, "ip_address")) {
      sscanf(ptr, "%*[^=]=%s", buf);
      LOG_VERB("IP_Device_Addrress = %s", buf);
      return 0;
    }

    ptr = strtok_r(NULL, "\t \n\'", &ptrptr);
  }

  LOG_WARN("Get IP address failed");
  return -1;
}

void WfdP2PServer::sighandler_monitor_udhcpd(int signo, siginfo_t *sinfo, void *context) {
  do {
    if (signo != SIGCHLD || WfdP2PServer::sDhcpdPid == 0) break;
    
    if (sinfo->si_pid == WfdP2PServer::sDhcpdPid) {
      int status;
      int res = waitpid(WfdP2PServer::sDhcpdPid, &status, WNOHANG);
      if (res == 0)
        WfdP2PServer::sDhcpdPid = 0;
    }    
  } while (0);
}

bool WfdP2PServer::disallow_impl(void) {
  char buf[1024];
  int ret;

  if (this->mWpaIntfName[0] == '\0') {
    LOG_VERB("WFD is not up");
    return false;
  }

  this->kill_dhcpd();

  // Remove Wi-fi Direct P2P Group
  ret = this->wfd_remove_p2p_group(buf, 1024);
  LOG_VERB(buf);

  char *ptrptr;
  char *ptr = strtok_r(buf, "\t \n\'", &ptrptr);
  while (ptr != NULL) {
    if (strstr(ptr, "OK")) {
      LOG_VERB("Succedded to remove p2p group");
      return true;
    } else if (strstr(ptr, "FAIL")) {
      LOG_VERB("Failed to remove p2p group");
      return false;
    }
  }

  LOG_ERR("Assertion: Must not reach here");
  return false;
}

int WfdP2PServer::wfd_remove_p2p_group(char ret[], size_t len) {
  char *const params[] = {"wpa_cli",
    "-i",
    this->mWpaDevName,
    "p2p_group_remove",
    this->mWpaIntfName,
    NULL};

  assert(this->mWpaDevName[0] != '\0' && this->mWpaIntfName[0] != '\0');

  return Util::run_client(WPA_CLI_PATH, params, ret, len);
}

int WfdP2PServer::kill_dhcpd(void) {
  if (unlink(UDHCPD_CONFIG_PATH) != 0)
    LOG_WARN("%s", strerror(errno));

  if (WfdP2PServer::sDhcpdPid == 0) {
    LOG_WARN("udhcpd is not alive");
    return 0;
  }

  kill(WfdP2PServer::sDhcpdPid, SIGKILL);

  return 0;
}
