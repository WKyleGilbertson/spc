#ifndef _SPC_H
#define _SPC_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define SPC_NUM_CHANNELS 8

// =============================================================================
// 1. Ingest Telemetry (Hardware -> Software via read())
// =============================================================================

struct spc_channel_data {
    // 24-bit signed accumulators (sign-extended to int32_t)
    int32_t in, qn;           // Noise arm (Very Early) for C/N0 estimation
    int32_t ie, qe;           // Early arm
    int32_t ip, qp;           // Prompt arm
    int32_t il, ql;           // Late arm
    
    // Carrier & Code phase timing
    uint32_t carrier_phase;   // Full 32-bit NCO phase
    uint16_t epoch_idx;       // Fractional sample clock counter
    
    // Status & slip detection
    uint8_t  epoch_cnt;       // 2-bit C/A rollover counter
    uint8_t  data_ready;      // 1 = Coherent PDI complete
} __attribute__((packed));

struct spc_epoch_data {
    uint32_t system_epoch;                         // Global hardware millisecond counter
    struct spc_channel_data ch[SPC_NUM_CHANNELS];  // Latched 8-channel snapshot
} __attribute__((packed));

// Typedef aliases for user-space daemon compatibility
typedef struct spc_channel_data channel_harvest_t;
typedef struct spc_epoch_data   epoch_harvest_t;

// =============================================================================
// 2. Channel Configuration & Control (Software -> Hardware)
// =============================================================================

#define SPC_STROBE_CARR_FREQ  (1 << 0)
#define SPC_STROBE_CODE_FREQ  (1 << 1)
#define SPC_STROBE_CA_SEEDS   (1 << 2)
#define SPC_STROBE_CONFIG     (1 << 3)
#define SPC_STROBE_ARM_ALL    (0x0F)

typedef struct {
    uint8_t  channel_idx;      // Target channel (0 to 7)
    uint8_t  ctrl_strobe;      // Shadow register transfer mask
    uint8_t  spacing;          // Early-Late spacing (default 8 = 0.5 chip)
    uint8_t  nms;              // Coherent integration: 0=1ms, 1=2ms, 2=5ms, 3=10ms
    
    uint32_t carr_freq;        // 32-bit Carrier NCO word (4.092 MHz IF + Doppler)
    uint32_t code_freq;        // 32-bit Code NCO word (1.023 MHz + Doppler scale)
    
    uint16_t g1_seed;          // 10-bit G1 LFSR seed
    uint16_t g2_seed;          // 10-bit G2 LFSR seed
    uint16_t code_phase;       // Initial integer chip phase (0 to 1022)
} channel_config_t;

typedef struct {
    uint8_t  channel_idx;
    uint32_t carr_freq;
    uint32_t code_freq;
} channel_nco_update_t;

// =============================================================================
// 3. Timebase & PPS Steering (Software -> Hardware)
// =============================================================================

typedef struct {
    uint32_t period_ticks;     // Nominal 16,368,000 +/- slew offset
    uint32_t pulse_width;      // High time in cycles (default 327 = 20 us)
    uint32_t jam_target_val;   // Phase counter target for coarse alignment
    uint8_t  trigger_jam;      // 1 = Strobe instantaneous jam-sync
} pps_config_t;

// =============================================================================
// 4. IOCTL Interface Definitions
// =============================================================================

#define SPC_IOC_MAGIC 'S'

#define SPC_IOC_CONFIG_CH      _IOW(SPC_IOC_MAGIC, 1, channel_config_t)
#define SPC_IOC_UPDATE_NCO     _IOW(SPC_IOC_MAGIC, 2, channel_nco_update_t)
#define SPC_IOC_STEER_PPS      _IOW(SPC_IOC_MAGIC, 3, pps_config_t)
#define SPC_IOC_RESET_TIMEBASE _IO(SPC_IOC_MAGIC, 4)

#endif // _SPC_H