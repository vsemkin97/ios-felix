#pragma once
#include <stdint.h>

uint64_t counter_freq(void);
int ufs_cold_init(void);
void ufs_init_host_dma(uint64_t dma_pa);
int64_t ufs_query_set_fdeviceinit(uint64_t dma_pa);
int64_t ufs_query_poll_fdeviceinit(uint64_t dma_pa);
int64_t ufs_scsi_start_stop(uint64_t dma_pa, uint8_t lun, int start);
int64_t ufs_scsi_request_sense(uint64_t dma_pa, uint8_t lun, void *sense_buf);
int64_t ufs_scsi_tur(uint64_t dma_pa, uint8_t lun);
int64_t ufs_scsi_read(uint64_t dma_pa, uint8_t lun, uint64_t lba, uint32_t count_blocks, void *bounce_ptr);
