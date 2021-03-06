/*****************************************************************************
   FILE:          svc_uart
   PROJECT:
   SW PACKAGE:    UART svc_mcu
------------------------------------------------------------------------------
   DESCRIPTION:   UART svc_mcu
------------------------------------------------------------------------------
   COPYRIGHT:     (c) <year> STMicroelectronics, (<group>) <site>
------------------------------------------------------------------------------
   Developers:
      AI:   Author Initials
------------------------------------------------------------------------------
   HISTORY:

   Date      | A.I. | Description
   ----------+------+------------------------------------------------------
   yy.mm.dd  |  AI  | Original version
*****************************************************************************/
//!
//!   \file       svc_uart.c
//!   \copyright (c) STMicroelectronics
//!   \brief      <i><b>Service for LLD UART source file</b></i>
//!   \author
//!   \version    1.0
//!   \date       2007.11.09
//!   \bug        Unknown
//!   \warning    None
//!   \addtogroup SERVICES
//!   \{
//!   \addtogroup UART_SERVICES
//!   \{
//!   \page svc_uart_page UART svc_mcu

/*****************************************************************************
   includes
*****************************************************************************/
//!   \addtogroup UART_SERVICES_defines
//!   \{

#include "typedefs.h"
#include "clibs.h"
#include "gpOS.h"
#include "svc_mcu.h"
#include "svc_uart.h"
#include "svc_pwr.h"
#include "platform.h"

#include "gnss_debug.h"

/*****************************************************************************
   external declarations
*****************************************************************************/

/*****************************************************************************
   defines and macros (scope: module-local)
*****************************************************************************/

#define SVC_UART_HW_FIFO_TX_TRIGGER  LLD_UART_16_BYTES_TRIGGER_LEVEL
#define SVC_UART_HW_FIFO_RX_TRIGGER  LLD_UART_16_BYTES_TRIGGER_LEVEL

#define SVC_UART_TX_IRQS             (LLD_UART_TX_INT_MASK | LLD_UART_TXFE_INT_MASK)
#define SVC_UART_RX_IRQS             (LLD_UART_RX_INT_MASK | LLD_UART_TIMEOUT_INT_MASK | LLD_UART_OVERRUN_ERROR_INT_MASK)

#define SVC_UART_HANDLER_SIZE        sizeof( svc_uart_handler_t)

/*****************************************************************************
   typedefs and structures (scope: module-local)
*****************************************************************************/

/********************************************//**
 * \brief UART FIFO handler type
 ***********************************************/
typedef struct svc_uart_fifo_handler_s {
  // access semaphore to the critical resource
  gpOS_semaphore_t * access_sem;

  // FIFO conditions tools
  gpOS_semaphore_t * irq_sem;
  boolean_t     not_empty_waiting;
  boolean_t     not_full_waiting;

  // FIFO buffer informations
  tU8 *         buf_ptr;
  tU8 *         in_ptr;
  tU8 *         out_ptr;

  tU16          buf_size;
  tU16          len;
} svc_uart_fifo_handler_t;

/********************************************//**
 * \brief UART port handler definition
 ***********************************************/
typedef struct svc_uart_port_handler_s svc_uart_port_handler_t;

struct svc_uart_port_handler_s
{
  svc_uart_port_handler_t *next;

  boolean_t                 open;

  // access mutex to the critical resource
  gpOS_mutex_t *                 access_mutex;

  // Stream config infos
  tUInt                     id;
  svc_mcu_addr_t           phy_addr;
  svc_mcu_irq_line_t       phy_irq_line;
  LLD_UART_BaudRateTy       baud_rate;

  // FIFOs handlers
  svc_uart_fifo_handler_t  fifo_rx;
  svc_uart_fifo_handler_t  fifo_tx;

  // Power handler
  peripherallockid_t        peripherallock_id;       /**< Pwr handler  */
};

/********************************************//**
 * \brief UART handler definition
 ***********************************************/
typedef struct svc_uart_handler_s
{
  svc_mcu_item_t             svc_mcu_item_handler;

  svc_uart_port_handler_t *  port_head;
} svc_uart_handler_t;

/*****************************************************************************
   global variable definitions  (scope: module-exported)
*****************************************************************************/

/*****************************************************************************
   global variable definitions (scope: module-local)
*****************************************************************************/

static svc_uart_handler_t *svc_uart_handler = NULL;

int UART_SW_RX_FIFO_FULL = 0;

/*****************************************************************************
   function prototypes (scope: module-local)
*****************************************************************************/

static void                         svc_uart_receivedata   ( svc_uart_port_handler_t *hdlr_ptr);
static void                         svc_uart_senddata      ( svc_uart_port_handler_t *hdlr_ptr);
static svc_uart_port_handler_t *    svc_uart_get_hdlr_ptr  ( tUInt uart_id);
static void                         svc_uart_callback      ( svc_uart_port_handler_t *hdlr_ptr);

/*****************************************************************************
   function implementations (scope: module-local)
*****************************************************************************/
//!   \}
//!   \addtogroup UART_SERVICES_functions
//!   \{

/********************************************//**
 * \brief Load data onto TX FIFO
 *
 * \param[in] hdlr_ptr Pointer to UART port handler
 * \return void
 *
 ***********************************************/
static LLD_ISR_UART void svc_uart_senddata( svc_uart_port_handler_t *hdlr_ptr)
{
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;
  tU8 *out_ptr = hdlr_ptr->fifo_tx.out_ptr;
  tU32 buf_size = hdlr_ptr->fifo_tx.buf_size;
  tU8 *buf_limit = hdlr_ptr->fifo_tx.buf_ptr + buf_size;
  tU32 len = hdlr_ptr->fifo_tx.len;

  while( (!LLD_UART_IsTxFifoFull( uart_phy_id)) && (len > 0))
  {
    LLD_UART_WriteTxFifo( uart_phy_id, *out_ptr++);
    len--;

    if( out_ptr == buf_limit)
    {
      out_ptr = hdlr_ptr->fifo_tx.buf_ptr;
    }
  }

  hdlr_ptr->fifo_tx.out_ptr = out_ptr;
  hdlr_ptr->fifo_tx.len = len;

  if( (len == 0) && ( hdlr_ptr->fifo_tx.in_ptr != hdlr_ptr->fifo_tx.out_ptr))
  {
    //while(1);
  }
}

/********************************************//**
 * \brief Load data from RX FIFO
 *
 * \param[in] hdlr_ptr Pointer to UART port handler
 * \return void
 *
 ***********************************************/
static LLD_ISR_UART void svc_uart_receivedata( svc_uart_port_handler_t *hdlr_ptr)
{
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;
  tU8 *in_ptr = hdlr_ptr->fifo_rx.in_ptr;
  tU32 buf_size = hdlr_ptr->fifo_rx.buf_size;
  tU8 *buf_limit = hdlr_ptr->fifo_rx.buf_ptr + buf_size;
  tU32 len;

  len = hdlr_ptr->fifo_rx.len;

  if (hdlr_ptr->id == 0)
  {
    if (len >= buf_size)
    {
      /* If SW FIFO full, disable RX IRQ in case of UART id 0 */
      UART_SW_RX_FIFO_FULL = 1;
      LLD_UART_InterruptDisable( uart_phy_id, SVC_UART_RX_IRQS);
    }
    else
    {
      while(( !LLD_UART_IsRxFifoEmpty( uart_phy_id))&&(UART_SW_RX_FIFO_FULL!=1))
      {
        if( len < buf_size)
        {
          tU8 data = LLD_UART_ReadRxFifo( uart_phy_id);

          *in_ptr++ = data;
          len++;

          if( in_ptr == buf_limit)
          {
            in_ptr = hdlr_ptr->fifo_rx.buf_ptr;
          }
        }
        else
        {
          UART_SW_RX_FIFO_FULL = 1;
          LLD_UART_InterruptDisable( uart_phy_id, SVC_UART_RX_IRQS);
        }
      }
    }
  }
  else
  {
    while( !LLD_UART_IsRxFifoEmpty( uart_phy_id))
    {
      /**< TODO: in case of FIFO filled now we discard. Is it better to overwrite? */

        tU8 data = LLD_UART_ReadRxFifo( uart_phy_id);

        if( len < buf_size)
        {
          *in_ptr++ = data;
          len++;

          if( in_ptr == buf_limit)
          {
            in_ptr = hdlr_ptr->fifo_rx.buf_ptr;
          }
        }
    }
  }

  hdlr_ptr->fifo_rx.in_ptr = in_ptr;
  hdlr_ptr->fifo_rx.len = len;
}

/********************************************//**
 * \brief   Get UART handler for specific ID
 *
 * \param[in]   uart_id   ID of wanted UART handler
 * \return  svc_uart_port_handler_t *  pointer to UART handler, or NULL if not open
 *
 ***********************************************/
GNSS_FAST static svc_uart_port_handler_t *svc_uart_get_hdlr_ptr( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr;

  hdlr_ptr = svc_uart_handler->port_head;

  while( hdlr_ptr != NULL)
  {
    if( hdlr_ptr->id == uart_id)
    {
      return( hdlr_ptr);
    }
    else
    {
      hdlr_ptr = hdlr_ptr->next;
    }
  }

  return( NULL);
}

/********************************************//**
 * \brief Callback for UART svc_mcu to signal the receiver task
 *
 * \param[in] hdlr_ptr  pointer to uart handler associated with
                    svc_mcu uart
 * \return void
 *
 ***********************************************/
static LLD_ISR_UART void svc_uart_callback( svc_uart_port_handler_t *hdlr_ptr)
{
  //! ---
  //! <B>Algorithm</B>

  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;
  LLD_UART_IRQSrcTy irq_status = LLD_UART_GetInterruptStatus( uart_phy_id);

  /** - Handle UART errors */
  if( irq_status & LLD_UART_OVERRUN_ERROR_INT_MASK)
  {
    LLD_UART_ClearInterrupt( uart_phy_id, irq_status & LLD_UART_OVERRUN_ERROR_INT_MASK);
  }

  /** - Handle UART errors */
  if( irq_status & LLD_UART_BREAK_ERROR_INT_MASK)
  {
    LLD_UART_ClearInterrupt( uart_phy_id, irq_status & LLD_UART_BREAK_ERROR_INT_MASK);
  }

  /** - Handle RX interrupts */
  if( irq_status & SVC_UART_RX_IRQS)
  {
    LLD_UART_ClearInterrupt( uart_phy_id, irq_status & SVC_UART_RX_IRQS);

    if (hdlr_ptr->id == 0)
    {
      svc_uart_receivedata( hdlr_ptr);
    }
    else
    {
      svc_uart_receivedata( hdlr_ptr);
    }

    if( hdlr_ptr->fifo_rx.not_empty_waiting == TRUE)
    {
      hdlr_ptr->fifo_rx.not_empty_waiting = FALSE;
      gpOS_semaphore_signal( hdlr_ptr->fifo_rx.irq_sem);
    }
  }

  /** - Handle TX interrupt */
  if( irq_status & LLD_UART_TX_INT_MASK)
  {
    LLD_UART_ClearInterrupt( uart_phy_id, irq_status & LLD_UART_TX_INT_MASK);

    svc_uart_senddata( hdlr_ptr);

    if( hdlr_ptr->fifo_tx.len == 0)
    {
      LLD_UART_InterruptDisable( uart_phy_id, LLD_UART_TX_INT_MASK);

      hdlr_ptr->fifo_tx.not_empty_waiting = TRUE;
    }

    /** - If some task is waiting for some space in tx FIFO, signal it */
    if( (hdlr_ptr->fifo_tx.not_full_waiting == TRUE) && (hdlr_ptr->fifo_tx.len < hdlr_ptr->fifo_tx.buf_size))
    {
      hdlr_ptr->fifo_tx.not_full_waiting = FALSE;
      gpOS_semaphore_signal( hdlr_ptr->fifo_tx.irq_sem);
    }
  }

  /** - Handle TX FIFO EMPTY interrupt */
  if( irq_status & LLD_UART_TXFE_INT_MASK)
  {
    LLD_UART_ClearInterrupt( uart_phy_id, irq_status & LLD_UART_TXFE_INT_MASK);

    LLD_UART_InterruptDisable( uart_phy_id, LLD_UART_TXFE_INT_MASK);

    svc_pwr_isr_peripherallock_release(hdlr_ptr->peripherallock_id);
  }

}

/********************************************//**
 * \brief Delete all OS allocated items
 *
 * \param[in] hdlr_ptr  Pointer to handler to delete
 *
 ***********************************************/
static void svc_uart_com_create_fail( svc_uart_port_handler_t *hdlr_ptr)
{
  gpOS_interrupt_uninstall( svc_mcu_get_irq_line( SVC_MCU_PER_ID_UART, hdlr_ptr->id));

  gpOS_semaphore_delete( hdlr_ptr->fifo_rx.irq_sem);
  gpOS_semaphore_delete( hdlr_ptr->fifo_rx.access_sem);

  gpOS_semaphore_delete( hdlr_ptr->fifo_tx.irq_sem);
  gpOS_semaphore_delete( hdlr_ptr->fifo_tx.access_sem);

  gpOS_mutex_delete( hdlr_ptr->access_mutex);

  svc_mcu_uninstall( SVC_MCU_PER_ID_UART);

  gpOS_memory_deallocate_p( svc_uart_handler->svc_mcu_item_handler.part, hdlr_ptr->fifo_rx.buf_ptr);
  gpOS_memory_deallocate_p( svc_uart_handler->svc_mcu_item_handler.part, hdlr_ptr->fifo_tx.buf_ptr);
  gpOS_memory_deallocate_p( svc_uart_handler->svc_mcu_item_handler.part, hdlr_ptr);
}

/********************************************//**
 * \brief Callback function called by bus service
 *
 * \param[in] cmd_id type of command requested
 * \param[in] param additional parameter for
 *                  the request
 *
 ***********************************************/
static gpOS_error_t svc_uart_cmdcallback_exec( svc_mcu_cmd_id_t cmd_id, void *param)
{

  svc_uart_port_handler_t *curr_port;

  if (cmd_id == SVC_MCU_CMD_ID_CHECK_DATA_RATE_CHANGE)
  {
    tU32 frequency = (tU32)param;
    curr_port = svc_uart_handler->port_head;

    while( curr_port != NULL)
    {
      if (LLD_UART_CheckBaudRateReachability(frequency, curr_port->baud_rate) == false)
      {
        return gpOS_FAILURE;
      }
      curr_port = curr_port->next;
    }
    return gpOS_SUCCESS;
  }
  else if (cmd_id == SVC_MCU_CMD_ID_CHANGE_SPEED)
  {
    tU32 new_freq;
    curr_port = svc_uart_handler->port_head;

    // Updates all baudrates
    svc_mcu_busclk_get( svc_uart_handler->svc_mcu_item_handler.bus_id, &new_freq);

    while( curr_port != NULL)
    {
      LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)curr_port->phy_addr;

      LLD_UART_SetBaudRate( uart_phy_id, new_freq, curr_port->baud_rate);

      curr_port = curr_port->next;
    }
  }
  else if (cmd_id == SVC_MCU_CMD_ID_SUSPEND_TRANSFER)
  {
    curr_port = svc_uart_handler->port_head;

    // Stop all UART ports
    while( curr_port != NULL)
    {
      svc_uart_lock( curr_port->id);

      // Wait for the HW TX buffer to be empty
      while( svc_uart_is_tx_empty( curr_port->id) == FALSE) { gpOS_task_delay(300*gpOS_timer_ticks_per_usec()); }

      curr_port = curr_port->next;
    }
  }
  else if (cmd_id == SVC_MCU_CMD_ID_RESTORE_TRANSFER)
  {
    curr_port = svc_uart_handler->port_head;

    // Restart all UART ports
    while( curr_port != NULL)
    {
      // Releases constraints on RX and TX FIFO
      svc_uart_release( curr_port->id);

      curr_port = curr_port->next;
    }
  }
  return gpOS_SUCCESS;
}

/*****************************************************************************
   function implementations (scope: module-exported)
*****************************************************************************/

/********************************************//**
 * \brief Initalize UART svc_mcu
 *
 * \param[in] partition     Partition used for svc_mcu data
 * \param[in] bus_speed     Bus speed
 * \return gpOS_error_t gpOS_SUCCESS if all is ok
 *
 ***********************************************/
gpOS_error_t svc_uart_init( gpOS_partition_t *partition, tU32 bus_id)
{
  tU32 mem_at_start = gpOS_memory_getheapfree_p( partition);

  if( svc_uart_handler != NULL)
  {
    return gpOS_SUCCESS;
  }

  svc_uart_handler = gpOS_memory_allocate_p( partition, SVC_UART_HANDLER_SIZE);

  if( svc_uart_handler == NULL)
  {
    return gpOS_FAILURE;
  }

  // Install service on main handler and fill physical info
  if( svc_mcu_install( SVC_MCU_PER_ID_UART, bus_id, &svc_uart_handler->svc_mcu_item_handler) == gpOS_FAILURE)
  {
    gpOS_memory_deallocate_p( partition, svc_uart_handler);
    return gpOS_FAILURE;
  }

  // Fill specific fields
  svc_uart_handler->svc_mcu_item_handler.part  = partition;
  svc_uart_handler->svc_mcu_item_handler.mem_used   = mem_at_start - gpOS_memory_getheapfree_p( partition);
  svc_uart_handler->svc_mcu_item_handler.cmdif     = svc_uart_cmdcallback_exec;
  svc_uart_handler->port_head  = NULL;

  return gpOS_SUCCESS;
}

/********************************************//**
 * \brief Updates UART registers for new bus speed
 *
 * \param[in] bus_speed new bus speed
 * \return void
 *
 ***********************************************/
void svc_uart_refresh( tU32 bus_speed)
{
// should not be used anymore and replaced by calls to:
// 1) service_phy_busclk_set(PLATFORM_BUSCLK_ID_MCLK, bus_speed); to update bus speed.
// 2) service_phy_update_data_rates(PLATFORM_BUSCLK_ID_MCLK); to update data rates.
#if 0
  if( svc_uart_handler->bus_speed != bus_speed)
  {
    svc_uart_port_handler_t *port = svc_uart_handler->port_head;

    svc_uart_handler->bus_speed = bus_speed;

    while( port != NULL)
    {
      LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)svc_mcu_get_addr( SVC_MCU_PER_ID_UART, port->id);

      gpOS_mutex_lock( port->access_mutex);

      LLD_UART_SetBaudRate( uart_phy_id, bus_speed, port->baud_rate);

      gpOS_mutex_release( port->access_mutex);

      port = port->next;
    }
  }
#endif
}

/********************************************//**
 * \brief Open a UART port
 *
 * \param[in] uart_id       uart port to open
 * \param[in] irq_pri       uart ID
 * \param[in] baud_rate     uart baud rate
 * \param[in] fifo_tx_size  size of TX FIFO (0 if not needed)
 * \param[in] fifo_rx_size  size of RX FIFO (0 if not needed)
 * \return gpOS_error_t
 * \retval gpOS_SUCCESS if all is ok
 * \retval gpOS_FAILURE otherwise
 *
 ***********************************************/
gpOS_error_t svc_uart_open_port( tUInt uart_id, gpOS_interrupt_priority_t irq_pri, LLD_UART_BaudRateTy baud_rate, tU16 fifo_tx_size, tU16 fifo_rx_size, tU32 priority)
{
  //! ---
  //! <B>Algorithm</B>

  tU32 mem_at_start;
  svc_uart_port_handler_t *last_hdlr_ptr, *hdlr_ptr;
  tU32 bus_clk;

  LLD_UART_IdTy uart_phy_id       ;
  VicLineTy     uart_phy_irq_line ;

  /** - Check if a port can be open (FIFO sizes, handler initialized, ...) */
  if( (fifo_tx_size == 0) && (fifo_rx_size == 0))
  {
    return gpOS_FAILURE;
  }

  if( svc_uart_handler == NULL)
  {
    return gpOS_FAILURE;
  }

  if( uart_id >= svc_uart_handler->svc_mcu_item_handler.phy_item->number)
  {
    return gpOS_FAILURE;
  }

  uart_phy_id       = (LLD_UART_IdTy)svc_uart_handler->svc_mcu_item_handler.phy_item->addr[uart_id];
  uart_phy_irq_line = (VicLineTy)svc_uart_handler->svc_mcu_item_handler.phy_item->irq_line[uart_id];

  mem_at_start      = gpOS_memory_getheapfree_p( svc_uart_handler->svc_mcu_item_handler.part);

  /** - Check if port was already open */
  last_hdlr_ptr = svc_uart_handler->port_head;
  hdlr_ptr = svc_uart_handler->port_head;

  while( hdlr_ptr != NULL)
  {
    if( hdlr_ptr->id == uart_id)
    {
      if( hdlr_ptr->open == TRUE)
      {
        return( gpOS_FAILURE);
      }
      else
      {
        break;
      }
    }
    else
    {
      last_hdlr_ptr = hdlr_ptr;
      hdlr_ptr = hdlr_ptr->next;
    }
  }

  if( hdlr_ptr == NULL)
  {
    /** - Allocate needed memory */
    hdlr_ptr = gpOS_memory_allocate_p( svc_uart_handler->svc_mcu_item_handler.part, sizeof( svc_uart_port_handler_t));

    if( hdlr_ptr == NULL)
    {
      return gpOS_FAILURE;
    }

    hdlr_ptr->access_mutex = gpOS_mutex_create_p( MUTEX_FIFO, svc_uart_handler->svc_mcu_item_handler.part);

    if( hdlr_ptr->access_mutex == NULL)
    {
      svc_uart_com_create_fail( hdlr_ptr);
      return gpOS_FAILURE;
    }

    /** - Initialize UART device and handler */
    hdlr_ptr->id            = uart_id;
    hdlr_ptr->phy_addr      = (svc_mcu_addr_t)uart_phy_id;
    hdlr_ptr->phy_irq_line  = uart_phy_irq_line;
    hdlr_ptr->baud_rate     = baud_rate;
    hdlr_ptr->next          = NULL;

    hdlr_ptr->fifo_tx.buf_size = 0;
    hdlr_ptr->fifo_rx.buf_size = 0;

    if( fifo_tx_size != 0)
    {
      /** - Initialize tx FIFO access and irq semaphores */
      hdlr_ptr->fifo_tx.irq_sem = gpOS_semaphore_create_p( SEM_FIFO, svc_uart_handler->svc_mcu_item_handler.part, 0);
      hdlr_ptr->fifo_tx.access_sem = gpOS_semaphore_create_p( SEM_FIFO, svc_uart_handler->svc_mcu_item_handler.part, 0);
      hdlr_ptr->fifo_tx.buf_ptr = gpOS_memory_allocate_p( svc_uart_handler->svc_mcu_item_handler.part, fifo_tx_size);

      if( (hdlr_ptr->fifo_tx.buf_ptr == NULL) || (hdlr_ptr->fifo_tx.access_sem == NULL) || (hdlr_ptr->fifo_tx.irq_sem == NULL))
      {
        svc_uart_com_create_fail( hdlr_ptr);
        return( gpOS_FAILURE);
      }

      /** - Initialize tx FIFO buffer */
      hdlr_ptr->fifo_tx.buf_size = fifo_tx_size;
      _clibs_memset( hdlr_ptr->fifo_tx.buf_ptr, 0, fifo_tx_size);

      /** - Initialize tx FIFO buffertools */
      hdlr_ptr->fifo_tx.not_empty_waiting = TRUE;
      hdlr_ptr->fifo_tx.not_full_waiting = FALSE;

      /** - Initialize tx FIFO buffer */
      hdlr_ptr->fifo_tx.in_ptr = hdlr_ptr->fifo_tx.buf_ptr;
      hdlr_ptr->fifo_tx.out_ptr = hdlr_ptr->fifo_tx.buf_ptr;
      hdlr_ptr->fifo_tx.len = 0;
    }

    if( fifo_rx_size != 0)
    {
      /** - Initialize rx FIFO access and irq semaphores */
      hdlr_ptr->fifo_rx.irq_sem = gpOS_semaphore_create_p( SEM_FIFO, svc_uart_handler->svc_mcu_item_handler.part, 0);
      hdlr_ptr->fifo_rx.access_sem = gpOS_semaphore_create_p( SEM_FIFO, svc_uart_handler->svc_mcu_item_handler.part, 0);
      hdlr_ptr->fifo_rx.buf_ptr = gpOS_memory_allocate_p( svc_uart_handler->svc_mcu_item_handler.part, fifo_rx_size);

      hdlr_ptr->fifo_rx.access_sem = gpOS_semaphore_create_p( SEM_FIFO, svc_uart_handler->svc_mcu_item_handler.part, 0);

      if( (hdlr_ptr->fifo_rx.buf_ptr == NULL) || (hdlr_ptr->fifo_rx.access_sem == NULL) || (hdlr_ptr->fifo_rx.irq_sem == NULL))
      {
        svc_uart_com_create_fail( hdlr_ptr);
        return( gpOS_FAILURE);
      }

      /** - Initialize rx FIFO buffer */
      hdlr_ptr->fifo_rx.buf_size = fifo_rx_size;
      _clibs_memset( hdlr_ptr->fifo_rx.buf_ptr, 0, fifo_rx_size);

      /** - Initialize rx FIFO buffertools */
      hdlr_ptr->fifo_rx.not_empty_waiting = FALSE;
      hdlr_ptr->fifo_rx.not_full_waiting = FALSE;

      /** - Initialize tx FIFO buffer */
      hdlr_ptr->fifo_rx.in_ptr = hdlr_ptr->fifo_rx.buf_ptr;
      hdlr_ptr->fifo_rx.out_ptr = hdlr_ptr->fifo_rx.buf_ptr;
      hdlr_ptr->fifo_rx.len = 0;
    }

    if( last_hdlr_ptr == NULL)
    {
      svc_uart_handler->port_head = hdlr_ptr;
    }
    else
    {
      last_hdlr_ptr->next = hdlr_ptr;
    }

    svc_uart_handler->svc_mcu_item_handler.mem_used += mem_at_start - gpOS_memory_getheapfree_p( svc_uart_handler->svc_mcu_item_handler.part);
  }

  svc_mcu_enable( SVC_MCU_PER_ID_UART, uart_id);

  LLD_UART_ResetReg( uart_phy_id);

  /**< Configure UART */
  svc_mcu_busclk_get( svc_uart_handler->svc_mcu_item_handler.bus_id, &bus_clk);
  LLD_UART_Config(
    uart_phy_id,
    bus_clk,
    baud_rate,
    LLD_UART_ONE_STOP_BITS,
    LLD_UART_EIGTH_BITS,
    LLD_UART_NO_PARITY
  );

  /** - Enable UART FIFOs */
  LLD_UART_FifoEnable( uart_phy_id);

  gpOS_interrupt_install( uart_phy_irq_line, irq_pri, (gpOS_interrupt_callback_t)svc_uart_callback, hdlr_ptr);
  gpOS_interrupt_enable( uart_phy_irq_line);

  /** - Program UART TX FIFO registers */
  if( fifo_tx_size != 0)
  {
    /** - Configure FIFO threshold */
    LLD_UART_SetTxFifoTriggerLevel( uart_phy_id, SVC_UART_HW_FIFO_TX_TRIGGER);

    /** - Enable transmitter */
    LLD_UART_TxEnable( uart_phy_id);

    gpOS_semaphore_signal( hdlr_ptr->fifo_tx.access_sem);
  }

  /** - Program UART RX FIFO registers */
  if( fifo_rx_size != 0)
  {
    /** - Configure FIFO threshold */
    LLD_UART_SetRxFifoTriggerLevel( uart_phy_id, SVC_UART_HW_FIFO_RX_TRIGGER);

    /** - Enable UART Rx interrupt */
    LLD_UART_InterruptEnable( uart_phy_id, SVC_UART_RX_IRQS | LLD_UART_BREAK_ERROR_INT_MASK);

    /** - Enable receiver */
    LLD_UART_RxEnable( uart_phy_id);

    gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
  }

  hdlr_ptr->open = TRUE;

  svc_pwr_peripherallock_register(&hdlr_ptr->peripherallock_id);

  if(uart_id == 0)
    {
       LLD_UART_EnableHwFlowControl(uart_phy_id);
    }


  /** - Enable peripheral */
  LLD_UART_Enable( uart_phy_id);

  return gpOS_SUCCESS;
}

/********************************************//**
 * \brief  Close UART port
 *
 * \param[in] uart_id tUInt
 * \return gpOS_error_t
 *
 ***********************************************/
gpOS_error_t svc_uart_close_port( tUInt uart_id)
{
  LLD_UART_IdTy uart_phy_id;
  VicLineTy     uart_phy_irq_line;

  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);

  if( hdlr_ptr == NULL)
  {
    return gpOS_FAILURE;
  }

  if( hdlr_ptr->open == FALSE)
  {
    return( gpOS_FAILURE);
  }

  gpOS_mutex_lock( hdlr_ptr->access_mutex);
  gpOS_semaphore_wait( hdlr_ptr->fifo_rx.access_sem);
  gpOS_semaphore_wait( hdlr_ptr->fifo_tx.access_sem);

  uart_phy_id       = (LLD_UART_IdTy)hdlr_ptr->phy_addr;
  uart_phy_irq_line = (VicLineTy)hdlr_ptr->phy_irq_line;

  LLD_UART_ResetReg( uart_phy_id);

  svc_mcu_disable( SVC_MCU_PER_ID_UART, uart_id);

  gpOS_interrupt_uninstall( uart_phy_irq_line);
  gpOS_interrupt_disable( uart_phy_irq_line);

  hdlr_ptr->open = FALSE;

  gpOS_semaphore_signal( hdlr_ptr->fifo_tx.access_sem);
  gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
  gpOS_mutex_release( hdlr_ptr->access_mutex);

  return gpOS_SUCCESS;
}

/********************************************//**
 * \brief Reads characters from a stream
 *
 * \param[in] hdlr_ptr  handler of the stream
 * \param[in] out_buf   buffer where to write chars
 * \param[in] max_chars bytes to read
 * \param[in] timeout   timeout
 * \return read chars
 *
 ***********************************************/
tU32 svc_uart_read( tUInt uart_id, tU8 *out_buf, tU32 max_chars, gpOS_clock_t *timeout)
{
  //! ---
  //! <B>Algorithm</B>

  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  tU32 read_chars = 0;

  if( (hdlr_ptr == NULL) || (max_chars == 0))
  {
    return 0;
  }

  if( hdlr_ptr->open == FALSE)
  {
    return 0;
  }

  if( hdlr_ptr->fifo_rx.buf_size != 0)
  {
    gpOS_semaphore_wait( hdlr_ptr->fifo_rx.access_sem);

    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    /** - loop until all characters are read */
    while( read_chars < max_chars)
    {
      tU32 len;
      tU32 curr_rx_chars;

      /** - if no characters are in rx FIFO, wait for some */
      gpOS_interrupt_lock();

      if( hdlr_ptr->fifo_rx.len == 0)
      {
        hdlr_ptr->fifo_rx.not_empty_waiting = TRUE;
        gpOS_interrupt_unlock();

        gpOS_mutex_release( hdlr_ptr->access_mutex);

        if( gpOS_semaphore_wait_timeout( hdlr_ptr->fifo_rx.irq_sem, timeout) == gpOS_FAILURE)
        {
          gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
          return read_chars;
        }

        gpOS_mutex_lock( hdlr_ptr->access_mutex);
      }
      else
      {
        gpOS_interrupt_unlock();
      }

      len = hdlr_ptr->fifo_rx.len;
      curr_rx_chars = read_chars;

      /** - read characters from RX FIFO */
      {
        tU8 *out_ptr = hdlr_ptr->fifo_rx.out_ptr;
        tU8 *buf_limit = hdlr_ptr->fifo_rx.buf_ptr + hdlr_ptr->fifo_rx.buf_size;

        while( (read_chars < max_chars) && (len > 0))
        {
          *out_buf++ = *out_ptr++;
          read_chars++;
          len--;

          if( out_ptr == buf_limit)
          {
            out_ptr = hdlr_ptr->fifo_rx.buf_ptr;
          }
        }

        hdlr_ptr->fifo_rx.out_ptr = out_ptr;
      }

      gpOS_interrupt_lock();
      hdlr_ptr->fifo_rx.len -= (read_chars - curr_rx_chars);

      /** - if rx FIFO process was waiting for some space, signal it */
      if( hdlr_ptr->fifo_rx.not_full_waiting == TRUE)
      {
        hdlr_ptr->fifo_rx.not_full_waiting = FALSE;
      }
      gpOS_interrupt_unlock();

    }

    gpOS_interrupt_lock();

    if ((UART_SW_RX_FIFO_FULL == 1)&&(hdlr_ptr->id == 0)&&(hdlr_ptr->fifo_rx.len == 0))
    {
      LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)svc_mcu_get_addr(SVC_MCU_PER_ID_UART, hdlr_ptr->id);
      UART_SW_RX_FIFO_FULL = 0;
      LLD_UART_InterruptEnable( uart_phy_id, SVC_UART_RX_IRQS);
    }

    gpOS_interrupt_unlock();

    gpOS_mutex_release( hdlr_ptr->access_mutex);

    gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
  }

  return read_chars;
}

/********************************************//**
 * \brief Writes characters from a stream

 *
 * \param[in] hdlr_ptr  handler of the stream
 * \param[in] in_buf    buffer from where to read chars
 * \param[in] max_chars bytes to write
 * \param[in] timeout   timeout
 * \return remaining chars
 *
 ***********************************************/
GNSS_FAST tU32 svc_uart_write( tUInt uart_id, tU8 *in_buf, tU32 max_chars, gpOS_clock_t *timeout)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  tU32 written_chars = 0;

  if( (hdlr_ptr == NULL) || (max_chars == 0))
  {
    return 0;
  }

  if( hdlr_ptr->open == FALSE)
  {
    return 0;
  }

  if( hdlr_ptr->fifo_tx.buf_size != 0)
  {
    gpOS_semaphore_wait( hdlr_ptr->fifo_tx.access_sem);

    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    /** - loop until all characters are written */
    while( written_chars < max_chars)
    {
      tU32 len;
      tU32 curr_tx_chars;
      LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

      /** - if no space is available in tx FIFO, wait for some */
      gpOS_interrupt_lock();
      if( hdlr_ptr->fifo_tx.len == hdlr_ptr->fifo_tx.buf_size)
      {
        hdlr_ptr->fifo_tx.not_full_waiting = TRUE;
        gpOS_interrupt_unlock();

        gpOS_mutex_release( hdlr_ptr->access_mutex);

        if( gpOS_semaphore_wait_timeout( hdlr_ptr->fifo_tx.irq_sem, timeout) == gpOS_FAILURE)
        {
          gpOS_semaphore_signal( hdlr_ptr->fifo_tx.access_sem);
          return written_chars;
        }

        gpOS_mutex_lock( hdlr_ptr->access_mutex);
      }
      else
      {
        gpOS_interrupt_unlock();
      }

      len = hdlr_ptr->fifo_tx.len;
      curr_tx_chars = written_chars;

      /** - Fill SW FIFO */
      {
        tU8 *in_ptr = hdlr_ptr->fifo_tx.in_ptr;
        tU8 *buf_limit = hdlr_ptr->fifo_tx.buf_ptr + hdlr_ptr->fifo_tx.buf_size;
        tU32 buf_size = hdlr_ptr->fifo_tx.buf_size;

        while( (written_chars < max_chars) && (len < buf_size))
        {
          *in_ptr++ = *in_buf++;
          written_chars++;
          len++;

          if( in_ptr == buf_limit)
          {
            in_ptr = hdlr_ptr->fifo_tx.buf_ptr;
          }
        }

        hdlr_ptr->fifo_tx.in_ptr = in_ptr;
      }

      gpOS_interrupt_lock();
      LLD_UART_InterruptDisable( uart_phy_id, SVC_UART_TX_IRQS);
      hdlr_ptr->fifo_tx.len += (written_chars - curr_tx_chars);

      /** - if tx FIFO process was waiting for some character, signal it */
      if( (hdlr_ptr->fifo_tx.not_empty_waiting == TRUE) && (hdlr_ptr->fifo_tx.len > 0))
      {
        hdlr_ptr->fifo_tx.not_empty_waiting = FALSE;
      }

//      LLD_UART_InterruptDisable( svc_mcu_get_addr( SVC_MCU_PER_ID_UART, hdlr_ptr->id), SVC_UART_TX_IRQS);
      gpOS_interrupt_unlock();

      svc_pwr_peripherallock_acquire(hdlr_ptr->peripherallock_id);

      /** - Send data if any */
      svc_uart_senddata( hdlr_ptr);

      LLD_UART_InterruptEnable( uart_phy_id, SVC_UART_TX_IRQS);
//     LLD_UART_InterruptEnable( svc_mcu_get_addr( SVC_MCU_PER_ID_UART, hdlr_ptr->id), SVC_UART_TX_IRQS);
    }

    gpOS_mutex_release( hdlr_ptr->access_mutex);

    gpOS_semaphore_signal( hdlr_ptr->fifo_tx.access_sem);
  }

  return written_chars;
}

/********************************************//**
 * \brief Lock UART for a specific task
 *
 * \param[in] uart_id UART port
 * \return void
 *
 ***********************************************/
void svc_uart_lock( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);

  if( hdlr_ptr != NULL)
  {
    gpOS_mutex_lock( hdlr_ptr->access_mutex);
  }
}

/********************************************//**
 * \brief Release UART from a specific task
 *
 * \param[in] uart_id UART port
 * \return void
 *
 ***********************************************/
void svc_uart_release( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);

  if( hdlr_ptr != NULL)
  {
    gpOS_mutex_release( hdlr_ptr->access_mutex);
  }
}

/********************************************//**
 * \brief Check if RX FIFO of given stream is empty
 *
 * \param[in] hdlr_ptr Pointer to stream to check for
 * \return boolean_t
 * \retval TRUE if RX FIFO is empty
 * \retval FALSE otherwise
 *
 ***********************************************/
boolean_t svc_uart_is_rx_empty( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  boolean_t result = FALSE;

  if( hdlr_ptr != NULL)
  {
    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    if( hdlr_ptr->fifo_rx.buf_size == 0)
    {
      result = FALSE;
    }
    else
    {
      gpOS_semaphore_wait( hdlr_ptr->fifo_rx.access_sem);

      if( hdlr_ptr->fifo_rx.len == 0)
      {
        result = TRUE;
      }

      gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
    }

    gpOS_mutex_release( hdlr_ptr->access_mutex);
  }

  return result;
}

/********************************************//**
 * \brief Check if TX FIFO of given stream is full
 *
 * \param[in] hdlr_ptr Pointer to stream to check for
 * \return boolean_t
 * \retval TRUE if TX FIFO is full
 * \retval FALSE otherwise
 *
 ***********************************************/
boolean_t svc_uart_is_tx_full( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  boolean_t result = FALSE;

  if( hdlr_ptr != NULL)
  {
    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    if( hdlr_ptr->fifo_tx.buf_size == 0)
    {
      result = FALSE;
    }
    else
    {
      gpOS_semaphore_wait( hdlr_ptr->fifo_rx.access_sem);

      if( hdlr_ptr->fifo_tx.len == hdlr_ptr->fifo_tx.buf_size)
      {
        result = TRUE;
      }

      gpOS_semaphore_signal( hdlr_ptr->fifo_rx.access_sem);
    }

    gpOS_mutex_release( hdlr_ptr->access_mutex);
  }

  return result;
}

/********************************************//**
 * \brief   Enable Sw Flow Control
 *
 * \param[in] uart_id UART port
 * \param[in]   txmode Sw Transmit Flow Control mode
 * \param[in]   rxmode Sw Receive Flow Control mode
 * \return  void
 *
 ***********************************************/
void svc_uart_enable_swflowcontrol( tUInt uart_id, LLD_UART_SwFlowCtrlModeTy txmode, LLD_UART_SwFlowCtrlModeTy rxmode, tU32 xon1, tU32 xoff1, tU32 xon2, tU32 xoff2)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr(uart_id);
  LLD_UART_IdTy uart_phy_id;

  if( hdlr_ptr != NULL)
  {
    uart_phy_id = (LLD_UART_IdTy)svc_mcu_get_addr(SVC_MCU_PER_ID_UART, hdlr_ptr->id);

    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    LLD_UART_SetXonXoffValue(uart_phy_id, LLD_UART_XONXOFF_XON1, xon1);
    LLD_UART_SetXonXoffValue(uart_phy_id, LLD_UART_XONXOFF_XOFF1, xoff1);
    LLD_UART_SetXonXoffValue(uart_phy_id, LLD_UART_XONXOFF_XON2,  xon2);
    LLD_UART_SetXonXoffValue(uart_phy_id, LLD_UART_XONXOFF_XOFF2, xoff2);

    LLD_UART_EnableSwFlowControl(uart_phy_id, txmode, rxmode);

    gpOS_mutex_release( hdlr_ptr->access_mutex);
  }
}

/********************************************//**
 * \brief   Disable Sw Flow Control
 *
 * \param[in] uart_id UART port
 * \return  void
 *
 ***********************************************/
void svc_uart_disable_swflowcontrol( tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr(uart_id);
  LLD_UART_IdTy uart_phy_id;

  if( hdlr_ptr != NULL)
  {
    uart_phy_id = (LLD_UART_IdTy)svc_mcu_get_addr(SVC_MCU_PER_ID_UART, hdlr_ptr->id);

    gpOS_mutex_lock( hdlr_ptr->access_mutex);

    LLD_UART_DisableSwFlowControl(uart_phy_id);

    gpOS_mutex_release( hdlr_ptr->access_mutex);
  }
}
/********************************************//**
 * \brief Check if TX FIFO is empty
 *
 * \param uart_id UART ID
 * \return TRUE if full, FALSE otherwise
 *
 ***********************************************/
boolean_t svc_uart_is_tx_empty( const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  boolean_t result = FALSE;
  if( hdlr_ptr != NULL)
  {
    if( hdlr_ptr->fifo_tx.buf_size == 0)
    {
      result = TRUE;
    }
    else
    {
      if( hdlr_ptr->fifo_tx.len == 0)
      {
        while(LLD_UART_IsBusy( (LLD_UART_IdTy)svc_mcu_get_addr(SVC_MCU_PER_ID_UART, hdlr_ptr->id)));
        result = TRUE;
      }
    }
  }
  return result;
}

/********************************************//**
 * \brief Reset RX UART part
 *
 * \param uart_id UART ID
 * \return none
 *
 ***********************************************/
tVoid svc_uart_reset_rx(const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

  LLD_UART_RxReset(uart_phy_id);
}

#if 0
/********************************************//**
 * \brief �et UART RTS to 1
 *
 * \param uart_id UART ID
 * \return none
 *
 ***********************************************/
tVoid svc_uart_set_RTS(const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

  LLD_UART_RTS_set(uart_phy_id);
}

/********************************************//**
 * \brief �et UART RTS to 0
 *
 * \param uart_id UART ID
 * \return none
 *
 ***********************************************/
tVoid svc_uart_clear_RTS(const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

  LLD_UART_RTS_clear(uart_phy_id);
}

/********************************************//**
 * \brief Enable HW Flow Control
 *
 * \param uart_id UART ID
 * \return none
 *
 ***********************************************/
tVoid svc_uart_enable_HW_Flow_Control(const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

  LLD_UART_EnableHwFlowControl(uart_phy_id);
}

/********************************************//**
 * \brief Disable HW Flow Control
 *
 * \param uart_id UART ID
 * \return none
 *
 ***********************************************/
tVoid svc_uart_disable_HW_Flow_Control(const tUInt uart_id)
{
  svc_uart_port_handler_t *hdlr_ptr = svc_uart_get_hdlr_ptr( uart_id);
  LLD_UART_IdTy uart_phy_id = (LLD_UART_IdTy)hdlr_ptr->phy_addr;

  LLD_UART_DisableHwFlowControl(uart_phy_id);
}
#endif
/* End of file */

