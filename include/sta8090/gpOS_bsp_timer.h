/*******************************************************************************
 *                            (C) 2009 STMicroelectronics
 *    Reproduction and Communication of this document is strictly prohibited
 *      unless specifically authorized in writing by STMicroelectronics.
 *-----------------------------------------------------------------------------
 *                                  APG / CRM / SA&PD
 *                   Software Development Group - SW platform & HW Specific
 ******************************************************************************/
/********************************************//**
 * \file  bsp_timer.h
 * \brief Defines items specific to STA8088 BSP for OS timers
 ***********************************************/

#ifndef BSP_TIMER_H
#define BSP_TIMER_H

/*****************************************************************************
   includes
*****************************************************************************/

#include "lld_mtu.h"

/*****************************************************************************
   defines and macros
*****************************************************************************/

/*****************************************************************************
   typedefs and structures (scope: module-local)
*****************************************************************************/

typedef struct
{
  LLD_MTU_PrescalerTy   prescaler;
  tU32                  ext_freq;
} gpOS_bsp_timer_cfg_t;

/*****************************************************************************
   exported variables
*****************************************************************************/

/*****************************************************************************
   exported function prototypes
*****************************************************************************/

#endif /* BSP_TIMER_H */
