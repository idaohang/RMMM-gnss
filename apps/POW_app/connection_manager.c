/*
 * connection_manager.c
 *
 *  Created on: 24 mai 2017
 *      Author: jsanchez
 */


#include "at_module.h"
#include "gnss_data.h"
#include "POW_app.h"
#include "svc_uart.h"
#include "clibs.h"
#include "gnssapp.h"      /* to configure power services  */
#include "gnss_debug.h"
#include "gnss_api.h"     /* to use GNSS api              */
#include "http_module.h"

/* //Just to be used in debugging

#define IMEI "357353080001193"
#define FLEET_ID "5721489412194304"
*/


boolean_t openSocket(){

  // Just need to config the sockets once
  if(!at_configure_socket(SOCKET_ID,PDN,0,0,600,50)){
    report_error(" - sqnscfg failed");
    return FALSE;
  }
  if(!at_extended_configure_socket(SOCKET_ID,1,0,0)){
    report_error(" - sqnscfgext failed");
    return FALSE;
  }

  if(!send_at_wait_response(_AT("at+sqnsd=1,0,80,\"cloud-logger-159118.appspot.com\",0,8000,1"),"OK", 100)){
    report_error(" - sqnsd failed");
    return FALSE;
  }
  return TRUE;
}

void send_control_z(){
  send_at_get_response("\032", gpOS_TIMEOUT_IMMEDIATE);
  //send_at( "\032");
}

boolean_t activate_tracker(){


  gpOS_clock_t socekt_at_timeout_ms = 5000;
  gpOS_clock_t std_at_timeout_ms = 100;

  flush_rx();

  at_check_signal_quality();
  at_check_pin_status();

  gpOS_task_delay(5*1000*1000*gpOS_timer_ticks_per_usec()); //1 segundo?

  if(!at_check_eps_registration_status()){
    report_error(" - No network");
    return FALSE;
  }

  if(!at_check_pdp_context(PDN)){
    report_error(" - No PDN");
    at_activate_pdp(PDN);
    return FALSE;
  }

  if(at_is_socket_closed(SOCKET_ID)){
    if(!openSocket()){
      report_error(" - open socket failed");
      return FALSE;
    }
  }

  at_send_data(SOCKET_ID);
  send_packet_activate();
  send_control_z();

  if(!at_listen_server()){
    report_error(" - No reply from server");
  }else{

    //need to flush rx since the OK\r\n in HTTP response triggered the AT parser to exit too soon
    flush_rx();

  }
  return TRUE;
}

boolean_t send_positions()
{

  gpOS_clock_t socekt_at_timeout_ms = 5000;
  gpOS_clock_t std_at_timeout_ms = 100;

  flush_rx();

  at_check_signal_quality();
  at_check_pin_status();

  if(!at_check_eps_registration_status()){
    report_error(" - No network");
    return FALSE;
  }

  if(!at_check_pdp_context(PDN)){
    report_error(" - No PDN");
    at_activate_pdp(PDN);
    return FALSE;
  }

  if(at_is_socket_closed(SOCKET_ID)){
    if(!openSocket()){
      report_error(" - open socket failed");
      return FALSE;
    }
  }

  at_send_data(SOCKET_ID);
  send_http_package();
  send_control_z();

  if(!at_listen_server()){
    report_error(" - No reply from server");
  }else{
    //need to flush rx since the OK\r\n in HTTP response triggered the AT parser to exit too soon
    flush_rx();

  }

  return TRUE;
}
