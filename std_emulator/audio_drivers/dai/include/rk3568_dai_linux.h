/*
 * Copyright (C) 2022 HiHope Open Source Organization .
 *
 * HDF is dual licensed: you can use it either under the terms of
 * the GPL, or the BSD license, at your option.
 * See the LICENSE file in the root of this repository for complete details.
 */

#ifndef RK3568_DAI_LINUX_H
#define RK3568_DAI_LINUX_H

#include <linux/dmaengine.h>

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* __cplusplus */

/* I2S REGS */
#define I2S_TXCR    (0x0000)
#define I2S_RXCR    (0x0004)
#define I2S_CKR        (0x0008)
#define I2S_TXFIFOLR    (0x000c)
#define I2S_DMACR    (0x0010)
#define I2S_INTCR    (0x0014)
#define I2S_INTSR    (0x0018)
#define I2S_XFER    (0x001c)
#define I2S_CLR        (0x0020)
#define I2S_TXDR    (0x0024)
#define I2S_RXDR    (0x0028)
#define I2S_RXFIFOLR    (0x002c)
#define I2S_TDM_TXCR    (0x0030)
#define I2S_TDM_RXCR    (0x0034)
#define I2S_CLKDIV    (0x0038)


/*
 * TXCR
 * transmit operation control register
 */
#define I2S_TXCR_PATH_SHIFT(x)    (23 + (x) * 2)
#define I2S_TXCR_PATH_MASK(x)    (0x3 << I2S_TXCR_PATH_SHIFT(x))
#define I2S_TXCR_PATH(x, v)    ((v) << I2S_TXCR_PATH_SHIFT(x))
#define I2S_TXCR_RCNT_SHIFT    17
#define I2S_TXCR_RCNT_MASK    (0x3f << I2S_TXCR_RCNT_SHIFT)
#define I2S_TXCR_CSR_SHIFT    15
#define I2S_TXCR_CSR(x)        ((x) << I2S_TXCR_CSR_SHIFT)
#define I2S_TXCR_CSR_MASK    (3 << I2S_TXCR_CSR_SHIFT)
#define I2S_TXCR_HWT        BIT(14)
#define I2S_TXCR_SJM_SHIFT    12
#define I2S_TXCR_SJM_R        (0 << I2S_TXCR_SJM_SHIFT)
#define I2S_TXCR_SJM_L        (1 << I2S_TXCR_SJM_SHIFT)
#define I2S_TXCR_FBM_SHIFT    11
#define I2S_TXCR_FBM_MSB    (0 << I2S_TXCR_FBM_SHIFT)
#define I2S_TXCR_FBM_LSB    (1 << I2S_TXCR_FBM_SHIFT)
#define I2S_TXCR_IBM_SHIFT    9
#define I2S_TXCR_IBM_NORMAL    (0 << I2S_TXCR_IBM_SHIFT)
#define I2S_TXCR_IBM_LSJM    (1 << I2S_TXCR_IBM_SHIFT)
#define I2S_TXCR_IBM_RSJM    (2 << I2S_TXCR_IBM_SHIFT)
#define I2S_TXCR_IBM_MASK    (3 << I2S_TXCR_IBM_SHIFT)
#define I2S_TXCR_PBM_SHIFT    7
#define I2S_TXCR_PBM_MODE(x)    ((x) << I2S_TXCR_PBM_SHIFT)
#define I2S_TXCR_PBM_MASK    (3 << I2S_TXCR_PBM_SHIFT)
#define I2S_TXCR_TFS_SHIFT    5
#define I2S_TXCR_TFS_I2S    (0 << I2S_TXCR_TFS_SHIFT)
#define I2S_TXCR_TFS_PCM    (1 << I2S_TXCR_TFS_SHIFT)
#define I2S_TXCR_TFS_TDM_PCM    (2 << I2S_TXCR_TFS_SHIFT)
#define I2S_TXCR_TFS_TDM_I2S    (3 << I2S_TXCR_TFS_SHIFT)
#define I2S_TXCR_TFS_MASK    (3 << I2S_TXCR_TFS_SHIFT)
#define I2S_TXCR_VDW_SHIFT    0
#define I2S_TXCR_VDW(x)        (((x) - 1) << I2S_TXCR_VDW_SHIFT)
#define I2S_TXCR_VDW_MASK    (0x1f << I2S_TXCR_VDW_SHIFT)


#define I2S_RXCR_PATH_SHIFT(x)    (17 + (x) * 2)
#define I2S_RXCR_PATH_MASK(x)    (0x3 << I2S_RXCR_PATH_SHIFT(x))
#define I2S_RXCR_PATH(x, v)    ((v) << I2S_RXCR_PATH_SHIFT(x))
#define I2S_RXCR_CSR_SHIFT    15
#define I2S_RXCR_CSR(x)        ((x) << I2S_RXCR_CSR_SHIFT)
#define I2S_RXCR_CSR_MASK    (3 << I2S_RXCR_CSR_SHIFT)
#define I2S_RXCR_HWT        BIT(14)
#define I2S_RXCR_SJM_SHIFT    12
#define I2S_RXCR_SJM_R        (0 << I2S_RXCR_SJM_SHIFT)
#define I2S_RXCR_SJM_L        (1 << I2S_RXCR_SJM_SHIFT)
#define I2S_RXCR_FBM_SHIFT    11
#define I2S_RXCR_FBM_MSB    (0 << I2S_RXCR_FBM_SHIFT)
#define I2S_RXCR_FBM_LSB    (1 << I2S_RXCR_FBM_SHIFT)
#define I2S_RXCR_IBM_SHIFT    9
#define I2S_RXCR_IBM_NORMAL    (0 << I2S_RXCR_IBM_SHIFT)
#define I2S_RXCR_IBM_LSJM    (1 << I2S_RXCR_IBM_SHIFT)
#define I2S_RXCR_IBM_RSJM    (2 << I2S_RXCR_IBM_SHIFT)
#define I2S_RXCR_IBM_MASK    (3 << I2S_RXCR_IBM_SHIFT)
#define I2S_RXCR_PBM_SHIFT    7
#define I2S_RXCR_PBM_MODE(x)    ((x) << I2S_RXCR_PBM_SHIFT)
#define I2S_RXCR_PBM_MASK    (3 << I2S_RXCR_PBM_SHIFT)
#define I2S_RXCR_TFS_SHIFT    5
#define I2S_RXCR_TFS_I2S    (0 << I2S_RXCR_TFS_SHIFT)
#define I2S_RXCR_TFS_PCM    (1 << I2S_RXCR_TFS_SHIFT)
#define I2S_RXCR_TFS_TDM_PCM    (2 << I2S_RXCR_TFS_SHIFT)
#define I2S_RXCR_TFS_TDM_I2S    (3 << I2S_RXCR_TFS_SHIFT)
#define I2S_RXCR_TFS_MASK    (3 << I2S_RXCR_TFS_SHIFT)
#define I2S_RXCR_VDW_SHIFT    0
#define I2S_RXCR_VDW(x)        (((x) - 1) << I2S_RXCR_VDW_SHIFT)
#define I2S_RXCR_VDW_MASK    (0x1f << I2S_RXCR_VDW_SHIFT)

#define I2S_CSR_SHIFT    15
#define I2S_CHN_2    (0 << I2S_CSR_SHIFT)
#define I2S_CHN_4    (1 << I2S_CSR_SHIFT)
#define I2S_CHN_6    (2 << I2S_CSR_SHIFT)
#define I2S_CHN_8    (3 << I2S_CSR_SHIFT)

#define I2S_CLKDIV_TXM_SHIFT    0
#define I2S_CLKDIV_TXM(x)        (((x) - 1) << I2S_CLKDIV_TXM_SHIFT)
#define I2S_CLKDIV_TXM_MASK    (0xff << I2S_CLKDIV_TXM_SHIFT)
#define I2S_CLKDIV_RXM_SHIFT    8
#define I2S_CLKDIV_RXM(x)        (((x) - 1) << I2S_CLKDIV_RXM_SHIFT)
#define I2S_CLKDIV_RXM_MASK    (0xff << I2S_CLKDIV_RXM_SHIFT)


/*
 * CKR
 * clock generation register
 */
#define I2S_CKR_TRCM_SHIFT    28
#define I2S_CKR_TRCM(x)    ((x) << I2S_CKR_TRCM_SHIFT)
#define I2S_CKR_TRCM_TXRX    (0 << I2S_CKR_TRCM_SHIFT)
#define I2S_CKR_TRCM_TXONLY    (1 << I2S_CKR_TRCM_SHIFT)
#define I2S_CKR_TRCM_RXONLY    (2 << I2S_CKR_TRCM_SHIFT)
#define I2S_CKR_TRCM_MASK    (3 << I2S_CKR_TRCM_SHIFT)
#define I2S_CKR_MSS_SHIFT    27
#define I2S_CKR_MSS_MASTER    (0 << I2S_CKR_MSS_SHIFT)
#define I2S_CKR_MSS_SLAVE    (1 << I2S_CKR_MSS_SHIFT)
#define I2S_CKR_MSS_MASK    (1 << I2S_CKR_MSS_SHIFT)
#define I2S_CKR_CKP_SHIFT    26
#define I2S_CKR_CKP_NORMAL    (0 << I2S_CKR_CKP_SHIFT)
#define I2S_CKR_CKP_INVERTED    (1 << I2S_CKR_CKP_SHIFT)
#define I2S_CKR_CKP_MASK    (1 << I2S_CKR_CKP_SHIFT)
#define I2S_CKR_RLP_SHIFT    25
#define I2S_CKR_RLP_NORMAL    (0 << I2S_CKR_RLP_SHIFT)
#define I2S_CKR_RLP_INVERTED    (1 << I2S_CKR_RLP_SHIFT)
#define I2S_CKR_RLP_MASK    (1 << I2S_CKR_RLP_SHIFT)
#define I2S_CKR_TLP_SHIFT    24
#define I2S_CKR_TLP_NORMAL    (0 << I2S_CKR_TLP_SHIFT)
#define I2S_CKR_TLP_INVERTED    (1 << I2S_CKR_TLP_SHIFT)
#define I2S_CKR_TLP_MASK    (1 << I2S_CKR_TLP_SHIFT)
#define I2S_CKR_MDIV_SHIFT    16
#define I2S_CKR_MDIV(x)        (((x) - 1) << I2S_CKR_MDIV_SHIFT)
#define I2S_CKR_MDIV_MASK    (0xff << I2S_CKR_MDIV_SHIFT)
#define I2S_CKR_RSD_SHIFT    8
#define I2S_CKR_RSD(x)        (((x) - 1) << I2S_CKR_RSD_SHIFT)
#define I2S_CKR_RSD_MASK    (0xff << I2S_CKR_RSD_SHIFT)
#define I2S_CKR_TSD_SHIFT    0
#define I2S_CKR_TSD(x)        (((x) - 1) << I2S_CKR_TSD_SHIFT)
#define I2S_CKR_TSD_MASK    (0xff << I2S_CKR_TSD_SHIFT)


#define I2S_DMACR_RDE_SHIFT    24
#define I2S_DMACR_RDE_DISABLE    (0 << I2S_DMACR_RDE_SHIFT)
#define I2S_DMACR_RDE_ENABLE    (1 << I2S_DMACR_RDE_SHIFT)
#define I2S_DMACR_RDL_SHIFT    16
#define I2S_DMACR_RDL(x)    (((x) - 1) << I2S_DMACR_RDL_SHIFT)
#define I2S_DMACR_RDL_MASK    (0x1f << I2S_DMACR_RDL_SHIFT)
#define I2S_DMACR_TDE_SHIFT    8
#define I2S_DMACR_TDE_DISABLE    (0 << I2S_DMACR_TDE_SHIFT)
#define I2S_DMACR_TDE_ENABLE    (1 << I2S_DMACR_TDE_SHIFT)
#define I2S_DMACR_TDL_SHIFT    0
#define I2S_DMACR_TDL(x)    ((x) << I2S_DMACR_TDL_SHIFT)
#define I2S_DMACR_TDL_MASK    (0x1f << I2S_DMACR_TDL_SHIFT)

/*
 * XFER
 * Transfer start register
 */
#define I2S_XFER_RXS_SHIFT    1
#define I2S_XFER_RXS_STOP    (0 << I2S_XFER_RXS_SHIFT)
#define I2S_XFER_RXS_START    (1 << I2S_XFER_RXS_SHIFT)
#define I2S_XFER_TXS_SHIFT    0
#define I2S_XFER_TXS_STOP    (0 << I2S_XFER_TXS_SHIFT)
#define I2S_XFER_TXS_START    (1 << I2S_XFER_TXS_SHIFT)

/*
 * CLR
 * clear SCLK domain logic register
 */
#define I2S_CLR_RXC    BIT(1)
#define I2S_CLR_TXC    BIT(0)
#define HIWORD_UPDATE(v, h, l)    (((v) << (l)) | (GENMASK((h), (l)) << 16))
#define RK3568_I2S1_MCLK_OUT_SRC_FROM_TX    HIWORD_UPDATE(1, 5, 5)
#define RK3568_I2S1_MCLK_OUT_SRC_FROM_RX    HIWORD_UPDATE(0, 5, 5)

#define RK3568_I2S1_CLK_TXONLY \
    RK3568_I2S1_MCLK_OUT_SRC_FROM_TX

#define RK3568_I2S1_CLK_RXONLY \
    RK3568_I2S1_MCLK_OUT_SRC_FROM_RX

#define CH_GRP_MAX    4
struct rk3568_snd_dmaengine_dai_dma_data {
    dma_addr_t addr;
    enum dma_slave_buswidth addr_width;
    u32 maxburst;
    unsigned int slave_id;
    void *filter_data;
    const char *chan_name;
    unsigned int fifo_size;
    unsigned int flags;
};
struct rk3568_i2s_tdm_dev {
    struct device *dev;
    struct clk *hclk;
    struct clk *mclk_tx;
    struct clk *mclk_rx;
    /* The mclk_tx_src is parent of mclk_tx */
    struct clk *mclk_tx_src;
    /* The mclk_rx_src is parent of mclk_rx */
    struct clk *mclk_rx_src;
    /*
     * The mclk_root0 and mclk_root1 are root parent and supplies for
     * the different FS.
     *
     * e.g:
     * mclk_root0 is VPLL0, used for FS=48000Hz
     * mclk_root0 is VPLL1, used for FS=44100Hz
     */
    struct clk *mclk_root0;
    struct clk *mclk_root1;
    struct regmap *regmap;
    struct regmap *grf;
    struct rk3568_snd_dmaengine_dai_dma_data capture_dma_data;
    struct rk3568_snd_dmaengine_dai_dma_data playback_dma_data;
    struct reset_control *tx_reset;
    struct reset_control *rx_reset;
    const struct rk_i2s_soc_data *soc_data;
#ifdef HAVE_SYNC_RESET
    void __iomem *cru_base;
    int tx_reset_id;
    int rx_reset_id;
#endif
    bool is_master_mode;
    bool io_multiplex;
    bool mclk_calibrate;
    bool tdm_mode;
    bool tdm_fsync_half_frame;
    unsigned int mclk_rx_freq;
    unsigned int mclk_tx_freq;
    unsigned int mclk_root0_freq;
    unsigned int mclk_root1_freq;
    unsigned int mclk_root0_initial_freq;
    unsigned int mclk_root1_initial_freq;
    unsigned int bclk_fs;
    unsigned int clk_trcm;
    unsigned int i2s_sdis[CH_GRP_MAX];
    unsigned int i2s_sdos[CH_GRP_MAX];
    int clk_ppm;
    atomic_t refcount;
    spinlock_t lock; /* xfer lock */
    bool txStart;
    bool rxStart;
};


#ifdef __cplusplus
#if __cplusplus
        }
#endif
#endif /* __cplusplus */
        
#endif

