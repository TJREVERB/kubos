/*
 * KubOS HAL
 * Copyright (C) 2016 Kubos Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
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
#include "kubos-hal/i2c.h"
#include "kubos-hal-stm32f4/i2c.h"
#include "kubos-hal/gpio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"

// @TODO: This timeout value might be excessive and could be better tuned.
#define FLAG_CHECK_TIMEOUT 100

/**
 * Fetches i2c bus data structure
 * @param num i2c bus num to fetch
 * @return hal_i2c_handle* pointer to data structure
 */
static hal_i2c_handle * hal_i2c_get_handle(KI2CNum num);

/**
 * Initializes i2c bus structure with data needed to setup hardware
 * @param i2c higher level hal i2c data
 * @return hal_i2c_handle* NULL if bad bus num, otherwise data ready for dev setup
 */
static hal_i2c_handle * hal_i2c_device_init(KI2C * i2c);

/**
 * Initializes the I2C according to the specified parameters
 * in the I2C_InitTypeDef and create the associated handle.
 *
 * @note Derived from STM32CubeF4's HAL_I2C_INIT
 * @param handle pointer to hal_i2c_handle containing config information
 * @return KI2CStatus I2C_OK if success, otherwise a specific error flag
 */
static KI2CStatus hal_i2c_hw_init(hal_i2c_handle * handle);

/**
 * Low level I2C hardware setup
 * @note Derived from STM32CubeF4's HAL_I2C_MspInit
 * @param handle pointer to hal_i2c_handle containing config information
 */
static void hal_i2c_msp_init(hal_i2c_handle * handle);

/**
 * I2C hardware cleanup and disabling
 * @param handle pointer to hal_i2c_handle containing config information
 */
static void hal_i2c_terminate(hal_i2c_handle * handle);

/**
 * Checks for the addr flag to be set, with builtin timeout
 * @note Derived from STM32CubeF4's I2C_WaitOnMasterAddressFlagUntilTimeout
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param flag I2C flag to check
 * @return KI2CStatus I2C_OK if success, otherwise a specific error flag
 */
static KI2CStatus hal_i2c_check_addr_timeout(I2C_HandleTypeDef * handle, uint32_t flag);

/**
 * Checks for special conditions based on the flag
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param flag I2C Flag that should be checked
 * @return KI2CStatus I2C_OK if no special conditions found, specific error otherwise
 */
static KI2CStatus hal_i2c_check_flag_special(I2C_HandleTypeDef * handle, uint32_t flag);

/**
 * Checks specified flag for desired state, with builtin timeout
 * @note Derived from STM32CubeF4's I2C_WaitOnFlagUntilTimeout
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param flag I2C Flag that should be checked
 * @param status Indicates whether to check for flag state of SET or RESET
 * @return KI2CStatus I2C_OK if flag is set to desired value within timout, otherwise I2C_TIMEOUT
 */
static KI2CStatus hal_i2c_check_flag_timeout(I2C_HandleTypeDef * handle, uint32_t flag, uint16_t status);

/**
 * Checks for btf flag to reset, with builtin timeout
 * @note Derived from STM32CubeF4's I2C_WaitOnBTFFlagUntilTimeout
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @return KI2CStatus I2C_OK if btf is reset within timeout, otherwise specific error
 */
static KI2CStatus hal_i2c_check_btf_timeout(I2C_HandleTypeDef * handle);

/**
 * Checks for txe flag to reset, with builtin timeout
 * @note Derived from STM32CubeF4's I2C_WaitOnBTFFlagUntilTimeout
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @return KI2CStatus I2C_OK if txe is reset within timeout, otherwise specific error
 */
static KI2CStatus hal_i2c_check_txe_timeout(I2C_HandleTypeDef * handle);

/**
 * Master sends slave address for read request
 * @note Derived from STM32CubeF4's I2C_MasterRequestRead
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param addr target slave address
 * @return KI2CStatus I2C_OK if success, otherwise specific error
 */
static KI2CStatus hal_i2c_master_request_read(I2C_HandleTypeDef * hal_handle, uint16_t addr);

/**
 * Sends initial receive sequence based on length of data expected
 * @note Partly derived from STM32CubeF4's HAL_I2C_Master_Receive
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param addr target slave address
 * @param len length of data expected to be received
 * @return KI2CStatus I2C_OK if success, otherwise specific error
 */
static KI2CStatus hal_i2c_master_setup_read(I2C_HandleTypeDef * hal_handle, uint16_t addr, int len);

/**
 * Master sends slave address for write request
 * @note Derived from STM32CubeF4's I2C_MasterRequestWrite
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param addr target slave address
 * @return KI2CStatus I2C_OK if success, otherwise specific error
 */
static KI2CStatus hal_i2c_master_request_write(I2C_HandleTypeDef * hal_handle, uint16_t addr);

/**
 * Sends initial transmit sequence
 * @note Derived from STM32CubeF4's HAL_I2C_Master_Transmit
 * @param handle Pointer to STM32CubeF4 HAL defined structure for I2C data
 * @param addr target slave address
 * @return KI2CStatus I2C_OK if success, otherwise specific error
 */
static KI2CStatus hal_i2c_master_setup_write(I2C_HandleTypeDef * hal_handle, uint16_t addr);

/**
 * Static array of i2c bus handles
 */
static hal_i2c_handle hal_i2c_bus[K_NUM_I2CS];

/** Functions implemented from Kubos-HAL interface **/

void kprv_i2c_dev_init(KI2CNum i2c_num)
{
    KI2C * i2c = kprv_i2c_get(i2c_num);
    hal_i2c_handle * handle = hal_i2c_device_init(i2c);
    hal_i2c_hw_init(handle);
}

void kprv_i2c_dev_terminate(KI2CNum i2c)
{
    hal_i2c_handle * handle = hal_i2c_get_handle(i2c);
    hal_i2c_terminate(handle);
}

KI2CStatus kprv_i2c_master_write(KI2CNum i2c, uint16_t addr, uint8_t *ptr, int len)
{
    KI2CStatus ret = I2C_OK;
    hal_i2c_handle * handle = hal_i2c_get_handle(i2c);
    if (handle == NULL)
    {
        return I2C_ERROR_NULL_HANDLE;
    }

    I2C_HandleTypeDef * hal_handle = &(handle->hal_handle);
    if ((ret = hal_i2c_master_setup_write(hal_handle, addr)) != I2C_OK)
    {
        return ret;
    }
    while (len > 0)
    {
        /* Wait until TXE is set */
        ret = hal_i2c_check_txe_timeout(hal_handle);
        if (ret != I2C_OK)
        {
            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;
            return ret;
        }

        /* Write data */
        hal_handle->Instance->DR = (*ptr++);
        len--;

        if ((__HAL_I2C_GET_FLAG(hal_handle, I2C_FLAG_BTF) == SET) && (len != 0))
        {
            /* Write data */
            hal_handle->Instance->DR = (*ptr++);
            len--;
        }

        /* Wait for BTF flag */
        ret = hal_i2c_check_btf_timeout(hal_handle);
        if (ret != I2C_OK)
        {
            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;
            return ret;
        }
    }

    /* Generate Stop */
    hal_handle->Instance->CR1 |= I2C_CR1_STOP;

    return ret;
}

KI2CStatus kprv_i2c_master_read(KI2CNum i2c, uint16_t addr, uint8_t *ptr, int len)
{
    KI2CStatus ret = I2C_OK;
    hal_i2c_handle * handle = hal_i2c_get_handle(i2c);
    if (handle == NULL)
    {
        return I2C_ERROR_NULL_HANDLE;
    }

    I2C_HandleTypeDef * hal_handle = &(handle->hal_handle);
    if ((ret = hal_i2c_master_setup_read(hal_handle, addr, len)) != I2C_OK)
    {
        return ret;
    }

    /* Data reading process */
    while (len > 0)
    {
        if (len == 1)
        {
            /* Wait for RXNE */
            if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_RXNE, SET)) != I2C_OK)
            {
                return ret;
            }

            /* Read data1 */
            uint8_t val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;
        }
        else if (len == 2)
        {
            /* Wait for BTF */
            if ((ret = hal_i2c_check_btf_timeout(hal_handle)) != I2C_OK)
            {
                return ret;
            }

            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;

            /* Read data1 */
            uint8_t val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;

            /* Read data2 */
            val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;
        }
        else if (len == 3)
        {
            /* Wait for BTF to be set */
            if ((ret = hal_i2c_check_btf_timeout(hal_handle)) != I2C_OK)
            {
                return ret;
            }

            /* Disable Acknowledge */
            hal_handle->Instance->CR1 &= ~I2C_CR1_ACK;

            /* Read data */
            uint8_t val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;

            /* Wait for BTF to be set */
            if ((ret = hal_i2c_check_btf_timeout(hal_handle)) != I2C_OK)
            {
                return ret;
            }

            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;

            /* Read data */
            val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;

            /* Read data */
            val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;
        }
        else
        {
            /* Wait for RXNE to be set */
            if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_RXNE, SET)) != I2C_OK)
            {
                return ret;
            }

            /* Read dataN */
            uint8_t val = hal_handle->Instance->DR;
            (*ptr++) = val;
            len--;

            /* Read again if BTF is set */
            if (__HAL_I2C_GET_FLAG(hal_handle, I2C_FLAG_BTF) == SET)
            {
                val = hal_handle->Instance->DR;
                (*ptr++) = val;
                len--;
            }
        }
    }
    return ret;
}

/** Private hal functions **/

static hal_i2c_handle* hal_i2c_get_handle(KI2CNum num)
{
    return &hal_i2c_bus[num];
}

static hal_i2c_handle * hal_i2c_device_init(KI2C * i2c)
{
    hal_i2c_handle * handle = NULL;
    if (i2c != NULL)
    {
        handle = hal_i2c_get_handle(i2c->bus_num);
        if (handle != NULL)
        {
            KI2CConf config = i2c->conf;
            handle->ki2c = i2c;
            handle->hal_handle.Init.AddressingMode  = config.addressing_mode;
            handle->hal_handle.Init.ClockSpeed      = config.clock_speed;
            handle->hal_handle.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
            handle->hal_handle.Init.DutyCycle       = I2C_DUTYCYCLE_2;
            handle->hal_handle.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
            handle->hal_handle.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
            handle->hal_handle.Init.OwnAddress1     = 0x00;
            handle->hal_handle.Init.OwnAddress2     = 0x00;

            switch(i2c->bus_num)
            {
#ifdef YOTTA_CFG_HARDWARE_I2C_I2C1
                case K_I2C1:
                {
                    handle->hal_handle.Instance = I2C1;
                    /* gpio pins */
                    handle->pins.scl = YOTTA_CFG_HARDWARE_I2C_I2C1_SCL_PIN;
                    handle->pins.scl_mode = YOTTA_CFG_HARDWARE_I2C_I2C1_SCL_MODE;
                    handle->pins.scl_pullup = YOTTA_CFG_HARDWARE_I2C_I2C1_SCL_PULLUP;
                    handle->pins.scl_speed = YOTTA_CFG_HARDWARE_I2C_I2C1_SCL_SPEED;
                    handle->pins.sda = YOTTA_CFG_HARDWARE_I2C_I2C1_SDA_PIN;
                    handle->pins.sda_mode = YOTTA_CFG_HARDWARE_I2C_I2C1_SDA_MODE;
                    handle->pins.sda_pullup = YOTTA_CFG_HARDWARE_I2C_I2C1_SDA_PULLUP;
                    handle->pins.sda_speed = YOTTA_CFG_HARDWARE_I2C_I2C1_SDA_SPEED;
                    handle->pins.alt = YOTTA_CFG_HARDWARE_I2C_I2C1_ALT;
                    handle->pins.gpio_port = GPIOB;
                    handle->pins.ev_irqn = I2C1_EV_IRQn;
                    handle->pins.er_irqn = I2C1_ER_IRQn;
                    break;
                }
#endif
#ifdef YOTTA_CFG_HARDWARE_I2C_I2C2
                case K_I2C2:
                {
                    handle->hal_handle.Instance = I2C2;
                    /* gpio pins */
                    handle->pins.scl = YOTTA_CFG_HARDWARE_I2C_I2C2_SCL_PIN;
                    handle->pins.scl_mode = YOTTA_CFG_HARDWARE_I2C_I2C2_SCL_MODE;
                    handle->pins.scl_pullup = YOTTA_CFG_HARDWARE_I2C_I2C2_SCL_PULLUP;
                    handle->pins.scl_speed = YOTTA_CFG_HARDWARE_I2C_I2C2_SCL_SPEED;
                    handle->pins.sda = YOTTA_CFG_HARDWARE_I2C_I2C2_SDA_PIN;
                    handle->pins.sda_mode = YOTTA_CFG_HARDWARE_I2C_I2C2_SDA_MODE;
                    handle->pins.sda_pullup = YOTTA_CFG_HARDWARE_I2C_I2C2_SDA_PULLUP;
                    handle->pins.sda_speed = YOTTA_CFG_HARDWARE_I2C_I2C2_SDA_SPEED;
                    handle->pins.alt = YOTTA_CFG_HARDWARE_I2C_I2C2_ALT;
                    handle->pins.gpio_port = GPIOB;
                    handle->pins.ev_irqn = I2C2_EV_IRQn;
                    handle->pins.er_irqn = I2C2_ER_IRQn;
                    break;
                }
#endif
                default:
                {
                    handle = NULL;
                }
            }
        }
    }
    return handle;
}

static KI2CStatus hal_i2c_hw_init(hal_i2c_handle * handle)
{
    I2C_HandleTypeDef * hi2c;
    uint32_t freqrange = 0;
    uint32_t pclk1 = 0;

    if(handle == NULL)
    {
        return I2C_ERROR_NULL_HANDLE;
    }

    hi2c = &(handle->hal_handle);

    /* Allocate lock resource and initialize it */
    hi2c->Lock = HAL_UNLOCKED;
    /* Init the low level hardware : GPIO, CLOCK, NVIC */
    hal_i2c_msp_init(handle);

    /* Disable the selected I2C peripheral */
    __HAL_I2C_DISABLE(hi2c);

    /* Get PCLK1 frequency */
    pclk1 = HAL_RCC_GetPCLK1Freq();

    /* Calculate frequency range */
    freqrange = I2C_FREQRANGE(pclk1);

    /*---------------------------- I2Cx CR2 Configuration ----------------------*/
    /* Configure I2Cx: Frequency range */
    hi2c->Instance->CR2 = freqrange;

    /*---------------------------- I2Cx TRISE Configuration --------------------*/
    /* Configure I2Cx: Rise Time */
    hi2c->Instance->TRISE = I2C_RISE_TIME(freqrange, hi2c->Init.ClockSpeed);

    /*---------------------------- I2Cx CCR Configuration ----------------------*/
    /* Configure I2Cx: Speed */
    hi2c->Instance->CCR = I2C_SPEED(pclk1, hi2c->Init.ClockSpeed, hi2c->Init.DutyCycle);

    /*---------------------------- I2Cx CR1 Configuration ----------------------*/
    /* Configure I2Cx: Generalcall and NoStretch mode */
    hi2c->Instance->CR1 = (hi2c->Init.GeneralCallMode | hi2c->Init.NoStretchMode);

    /*---------------------------- I2Cx OAR1 Configuration ---------------------*/
    /* Configure I2Cx: Own Address1 and addressing mode */
    hi2c->Instance->OAR1 = (hi2c->Init.AddressingMode | hi2c->Init.OwnAddress1);

    /*---------------------------- I2Cx OAR2 Configuration ---------------------*/
    /* Configure I2Cx: Dual mode and Own Address2 */
    hi2c->Instance->OAR2 = (hi2c->Init.DualAddressMode | hi2c->Init.OwnAddress2);

    /* Enable the selected I2C peripheral */
    __HAL_I2C_ENABLE(hi2c);

    return I2C_OK;
}

static void hal_i2c_msp_init(hal_i2c_handle * handle)
{
    GPIO_InitTypeDef  GPIO_InitStruct;

    /*##-1- Enable GPIO Clocks #################################################*/
    /* Enable GPIO clock */
    switch(handle->ki2c->bus_num)
    {
        case K_I2C1:
        case K_I2C2:
        {
            __HAL_RCC_GPIOB_CLK_ENABLE();
            break;
        }
    }

    /*##-2- Configure peripheral GPIO ##########################################*/
    /* I2C SCL TX GPIO pin configuration  */
    GPIO_InitStruct.Pin       = handle->pins.scl;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull      = GPIO_NOPULL;
    GPIO_InitStruct.Speed     = GPIO_SPEED_MEDIUM;
    GPIO_InitStruct.Alternate = handle->pins.alt;
    HAL_GPIO_Init(handle->pins.gpio_port, &GPIO_InitStruct);

    /* I2C SDA RX GPIO pin configuration  */
    GPIO_InitStruct.Pin       = handle->pins.sda;
    GPIO_InitStruct.Mode      = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull      = GPIO_PULLUP;
    GPIO_InitStruct.Alternate = handle->pins.alt;
    HAL_GPIO_Init(handle->pins.gpio_port, &GPIO_InitStruct);

    /*##-3- Enable I2C peripherals Clock #######################################*/
    /* Enable I2C1 clock */
    switch(handle->ki2c->bus_num)
    {
        case K_I2C1:
        {
            __HAL_RCC_I2C1_CLK_ENABLE();
            break;
        }
        case K_I2C2:
        {
            __HAL_RCC_I2C2_CLK_ENABLE();
            break;
        }
    }

    /*##-4- Configure the NVIC for I2C #########################################*/
    /* NVIC for I2C1 */
    HAL_NVIC_SetPriority(handle->pins.er_irqn, 1, 0);
    HAL_NVIC_EnableIRQ(handle->pins.er_irqn);
    HAL_NVIC_SetPriority(handle->pins.ev_irqn, 2, 0);
    HAL_NVIC_EnableIRQ(handle->pins.ev_irqn);
}

static void hal_i2c_terminate(hal_i2c_handle * handle)
{
  /*##-1- Reset peripherals ##################################################*/
  switch(handle->ki2c->bus_num)
  {
      case K_I2C1:
      {
          __HAL_RCC_I2C1_FORCE_RESET();
          __HAL_RCC_I2C1_RELEASE_RESET();
          break;
      }
      case K_I2C2:
      {
          __HAL_RCC_I2C2_FORCE_RESET();
          __HAL_RCC_I2C2_RELEASE_RESET();
          break;
      }
  }

  /*##-2- Disable peripherals and GPIO Clocks ################################*/
  /* Configure I2C Tx as alternate function  */
  HAL_GPIO_DeInit(handle->pins.gpio_port, handle->pins.scl);
  /* Configure I2C Rx as alternate function  */
  HAL_GPIO_DeInit(handle->pins.gpio_port, handle->pins.sda);

  /*##-3- Disable the NVIC for I2C ###########################################*/
  HAL_NVIC_DisableIRQ(handle->pins.er_irqn);
  HAL_NVIC_DisableIRQ(handle->pins.ev_irqn);
}

static KI2CStatus hal_i2c_check_addr_timeout(I2C_HandleTypeDef * handle, uint32_t flag)
{
    KI2CStatus ret = hal_i2c_check_flag_timeout(handle, flag, RESET);
    switch(ret)
    {
        case I2C_OK: return I2C_OK;
        case I2C_ERROR_TIMEOUT: return I2C_ERROR_ADDR_TIMEOUT;
        default: return ret;
    }
}

static KI2CStatus hal_i2c_check_btf_timeout(I2C_HandleTypeDef * handle)
{
    KI2CStatus ret = hal_i2c_check_flag_timeout(handle, I2C_FLAG_BTF, RESET);
    switch(ret)
    {
        case I2C_OK: return I2C_OK;
        case I2C_ERROR_TIMEOUT: return I2C_ERROR_BTF_TIMEOUT;
        default: return ret;
    }
}

static KI2CStatus hal_i2c_check_txe_timeout(I2C_HandleTypeDef * handle)
{
    KI2CStatus ret = hal_i2c_check_flag_timeout(handle, I2C_FLAG_TXE, RESET);
    switch(ret)
    {
        case I2C_OK: return I2C_OK;
        case I2C_ERROR_TIMEOUT: return I2C_ERROR_TXE_TIMEOUT;
        default: return ret;
    }
}

static KI2CStatus hal_i2c_check_flag_special(I2C_HandleTypeDef * handle, uint32_t flag)
{
    if ((flag == I2C_FLAG_BTF) || (flag == I2C_FLAG_TXE))
    {
        /* Check for NACK */
        if (__HAL_I2C_GET_FLAG(handle, I2C_FLAG_AF) == SET)
        {
            __HAL_I2C_CLEAR_FLAG(handle, I2C_FLAG_AF);
            return I2C_ERROR_NACK;
        }
    }
    else if (flag == I2C_FLAG_ADDR)
    {
        if (__HAL_I2C_GET_FLAG(handle, I2C_FLAG_AF) == SET)
        {
            /* Generate Stop */
            handle->Instance->CR1 |= I2C_CR1_STOP;

            /* Clear AF */
            __HAL_I2C_CLEAR_FLAG(handle, I2C_FLAG_AF);

            return I2C_ERROR_AF;
        }
    }
    return I2C_OK;
}

static KI2CStatus hal_i2c_check_flag_timeout(I2C_HandleTypeDef * handle, uint32_t flag, uint16_t status)
{
    uint16_t count = 0;
    KI2CStatus ret;

    while((__HAL_I2C_GET_FLAG(handle, flag) ? SET : RESET) == status)
    {
        if ((ret = hal_i2c_check_flag_special(handle, flag)) != I2C_OK)
        {
            return ret;
        }
        if (count >= FLAG_CHECK_TIMEOUT)
        {
            return I2C_ERROR_TIMEOUT;
        }
        count++;
        vTaskDelay(50);
    }
    return I2C_OK;
}


static KI2CStatus hal_i2c_master_request_read(I2C_HandleTypeDef * hal_handle, uint16_t addr)
{
    KI2CStatus ret = I2C_OK;

    /* Enable Acknowledge */
    hal_handle->Instance->CR1 |= I2C_CR1_ACK;

    /* Generate Start */
    hal_handle->Instance->CR1 |= I2C_CR1_START;

    /* Wait for SB */
    if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_SB, RESET)) != I2C_OK)
    {
        return ret;
    }
    /* Send slave address */
    hal_handle->Instance->DR = I2C_7BIT_ADD_READ(addr);

    /* Wait for ADDR */
    ret = hal_i2c_check_addr_timeout(hal_handle, I2C_FLAG_ADDR);

    return ret;
}

static KI2CStatus hal_i2c_master_setup_read(I2C_HandleTypeDef * hal_handle, uint16_t addr, int len)
{
    KI2CStatus ret = I2C_OK;
    // I2C_7BIT_ADD_READ expects an address already shifted
    uint16_t slave_addr = addr << 1;

    /* Check if I2C Busy */
    if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_BUSY, SET)) != I2C_OK)
    {
        return ret;
    }
    /* Disable pos */
    hal_handle->Instance->CR1 &= ~I2C_CR1_POS;

    /* Send Slave Address */
    if (( ret = hal_i2c_master_request_read(hal_handle, slave_addr)) != I2C_OK)
    {
        return ret;
    }
    switch(len)
    {
        case 0:
        {
            /* Clear ADDR */
            __HAL_I2C_CLEAR_ADDRFLAG(hal_handle);

            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;
            break;
        }
        case 1:
        {
            /* Disable Acknowledge */
            hal_handle->Instance->CR1 &= ~I2C_CR1_ACK;

            /* Clear ADDR */
            __HAL_I2C_CLEAR_ADDRFLAG(hal_handle);

            /* Generate Stop */
            hal_handle->Instance->CR1 |= I2C_CR1_STOP;
            break;
        }
        case 2:
        {
            /* Disable Acknowledge */
            hal_handle->Instance->CR1 &= ~I2C_CR1_ACK;

            /* Enable POS */
            hal_handle->Instance->CR1 |= I2C_CR1_POS;

            /* Clear ADDR */
            __HAL_I2C_CLEAR_ADDRFLAG(hal_handle);
            break;
        }
        default:
        {
            /* Enable Acknowledge */
            hal_handle->Instance->CR1 |= I2C_CR1_ACK;

            /* Clear ADDR */
            __HAL_I2C_CLEAR_ADDRFLAG(hal_handle);
            break;
        }
    }

    return ret;
}


static KI2CStatus hal_i2c_master_request_write(I2C_HandleTypeDef * hal_handle, uint16_t addr)
{
    KI2CStatus ret = I2C_OK;

    /* Generate Start */
    hal_handle->Instance->CR1 |= I2C_CR1_START;

    /* Wait for SB */
    if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_SB, RESET)) != I2C_OK)
    {
        return ret;
    }

    /* Send slave address */
    hal_handle->Instance->DR = I2C_7BIT_ADD_WRITE(addr);

    /* Wait for ADDR */
    ret = hal_i2c_check_addr_timeout(hal_handle, I2C_FLAG_ADDR);

    return ret;
}

static KI2CStatus hal_i2c_master_setup_write(I2C_HandleTypeDef * hal_handle, uint16_t addr)
{
    KI2CStatus ret = I2C_OK;
    // I2C_7BIT_ADD_WRITE expects address already shifted
    uint16_t slave_addr = addr << 1;

    /* Check if I2C is busy */
    if ((ret = hal_i2c_check_flag_timeout(hal_handle, I2C_FLAG_BUSY, SET)) != I2C_OK)
    {
        return ret;
    }
    /* Disable Pos */
    hal_handle->Instance->CR1 &= ~I2C_CR1_POS;

    /* Send Slave Address */
    ret = hal_i2c_master_request_write(hal_handle, slave_addr);

    if (ret == I2C_OK)
    {
        /* Clear ADDR flag */
        __HAL_I2C_CLEAR_ADDRFLAG(hal_handle);
    }

    return ret;
}
