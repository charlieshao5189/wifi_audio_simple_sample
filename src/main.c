/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 * @brief WiFi station sample
 */

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(wifi_to_i2s, CONFIG_LOG_DEFAULT_LEVEL);

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/iterable_sections.h>
#include "socket_util.h"

const struct device *dev_i2s = DEVICE_DT_GET(DT_ALIAS(i2s_tx));

#define NUM_BLOCKS 10
#define BLOCK_SIZE BUFFER_MAX_SIZE

#define I2S_TIMEOUT_MS 100

#define I2S_THREAD_STACKSIZE 8192
#define I2S_THREAD_PRIORITY 8

#ifdef CONFIG_NOCACHE_MEMORY
	#define MEM_SLAB_CACHE_ATTR __nocache
#else
	#define MEM_SLAB_CACHE_ATTR
#endif /* CONFIG_NOCACHE_MEMORY */

static char MEM_SLAB_CACHE_ATTR __aligned(WB_UP(32))
	_k_mem_slab_buf_tx_0_mem_slab[(NUM_BLOCKS) * WB_UP(BLOCK_SIZE)];

static STRUCT_SECTION_ITERABLE(k_mem_slab, tx_0_mem_slab) =
	Z_MEM_SLAB_INITIALIZER(tx_0_mem_slab, _k_mem_slab_buf_tx_0_mem_slab,
				WB_UP(BLOCK_SIZE), NUM_BLOCKS);

static uint32_t i2s_running = false;

int i2s_recover(void)
{
    LOG_INF("I2S driver is likely in error state, trying to recover");
    int ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
    if (ret < 0) {
        LOG_ERR("Failed to recover I2S. Please reset board");
    }
    return ret;
}


int i2s_drain_tx(void)
{
    /* Drain TX queue */
	LOG_INF("Triggered DRAIN");
    int ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
    i2s_running = false;
	if (ret < 0) {
		LOG_ERR("I2S DRAIN not triggered correctly: %d", ret);
        //Try to fix by reconfiguring driver
        ret = i2s_recover();
        if(ret <0)
        {
            LOG_ERR("Could not recover I2S, please reset board");
            return ret;
        }
        LOG_INF("Recover successful!");
		return ret;
	}
    
}

int i2s_send_data(uint8_t *data, uint16_t len)
{
    int ret;
    int curr_block_idx = 0;
    void *tx_block[NUM_BLOCKS];

    if(!i2s_running)
    {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &tx_block[curr_block_idx], K_FOREVER);
        if(ret < 0)
        {
            LOG_ERR("Could not allocate memory slab: %d", ret);
            return ret;
        }
        memset(tx_block[curr_block_idx],0x00, BLOCK_SIZE);
        memcpy(tx_block[curr_block_idx], data, MIN(len, BLOCK_SIZE));
        /* Send first block */
        ret = i2s_write(dev_i2s, tx_block[curr_block_idx], BLOCK_SIZE);
        if (ret < 0) {
            LOG_ERR("Could not write I2S TX buffer %d", ret);
            return ret;
        }
        /* Trigger the I2S transmission */
        ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
        if (ret < 0) {
            LOG_ERR("Could not trigger I2S tx");
            return ret;
	    }
        curr_block_idx++;
        i2s_running = true;
    }
    
    while(len > BLOCK_SIZE * curr_block_idx)
    {
        ret = k_mem_slab_alloc(&tx_0_mem_slab, &tx_block[curr_block_idx], K_FOREVER);
        if(ret < 0)
        {
            LOG_ERR("Could not allocate memory slab: %d", ret);
            return ret;
        }
        memset(tx_block[curr_block_idx], 0x00, BLOCK_SIZE);
        memcpy(tx_block[curr_block_idx], (data + (BLOCK_SIZE * curr_block_idx)), MIN(len-curr_block_idx*BLOCK_SIZE, BLOCK_SIZE));
        /* Send remaining blocks */
        ret = i2s_write(dev_i2s, tx_block[curr_block_idx], MIN(len-curr_block_idx*BLOCK_SIZE, BLOCK_SIZE));
        if(ret == -EIO)
        {
            //If I2S write fails due to an internal error (buffer provided too later or drain triggered too early), try to recover to continue
            ret = i2s_recover();
            if(ret < 0)
            {
                LOG_ERR("Could not recover I2S, please reset board");
                return ret;
            }
            ret = i2s_write(dev_i2s, tx_block[curr_block_idx], MIN(len-curr_block_idx*BLOCK_SIZE, BLOCK_SIZE));
            if (ret < 0) {
                LOG_ERR("Could not recover I2S, please reset board");
                return ret;
            }
            /* Trigger the I2S transmission */
            ret = i2s_trigger(dev_i2s, I2S_DIR_TX, I2S_TRIGGER_START);
            if (ret < 0) {
                LOG_ERR("Could not recover I2S, please reset board");
                return ret;
            }
            
        }
        else if (ret < 0) {
            LOG_ERR("Could not write I2S TX buffer %d", ret);
            return ret;
        }
        
        curr_block_idx++;
    }
    return 0;
}



// void socket_rx_callback(uint8_t *data, uint16_t len)
// {
   
//     // Process data here before outputting on I2S.
//     i2s_send_data(data, len);

// }

int i2s_config(void)
{
	struct i2s_config i2s_cfg;
	int ret;	

	if (!device_is_ready(dev_i2s)) {
		printf("I2S device not ready\n");
		return -ENODEV;
	}
	/* Configure I2S stream */
	i2s_cfg.word_size = 16U;
	i2s_cfg.channels = 2U;
	i2s_cfg.format = I2S_FMT_DATA_FORMAT_I2S;
	i2s_cfg.frame_clk_freq = 44100;
	i2s_cfg.block_size = BLOCK_SIZE;
	i2s_cfg.timeout = SYS_FOREVER_MS;
	/* Configure the Transmit port as Master */
	i2s_cfg.options = I2S_OPT_FRAME_CLK_MASTER
			| I2S_OPT_BIT_CLK_MASTER;
	i2s_cfg.mem_slab = &tx_0_mem_slab;
	ret = i2s_configure(dev_i2s, I2S_DIR_TX, &i2s_cfg);
	if (ret < 0) {
		printf("Failed to configure I2S stream\n");
		return ret;
	}

    return 0;
}

int main(void)
{
        LOG_INF("WiFi to I2S streamer start");

        i2s_config();
        
}

void i2s_data_handler(void)
{
    static socket_receive_t socket_receive;
    while(1)
    {
        while (k_msgq_get(&socket_recv_queue, &socket_receive, K_MSEC(I2S_TIMEOUT_MS)) == 0) {
            i2s_send_data(socket_receive.buf, socket_receive.len);
        }
        
        if(i2s_running)
        {
            // If I2S is running and no incomming packet during timeout, trigger drain and stop I2S
            i2s_drain_tx();
        }
    }
}

K_THREAD_DEFINE(i2s_data_handler_id, I2S_THREAD_STACKSIZE,
                i2s_data_handler, NULL, NULL, NULL,
                I2S_THREAD_PRIORITY, 0, 0);