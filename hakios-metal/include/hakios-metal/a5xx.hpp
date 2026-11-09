/*
 * a5xx.hpp: the Adreno 506 (A5XX) register and command-packet layer of
 * hakios-metal. The A6XX analog is a6xx.hpp; every constant here is checked
 * against Mesa's generated A5XX headers by tools/check-a5xx.py (the comment on
 * each line is the header and line it comes from), under Mesa's own names.
 *
 * The 506 is msm8953's GPU: qcom,kgsl-3d0@1c00000, qcom,chipid 0x5000600.
 * The command-processor packet codes are shared with A6XX (same type-4/type-7
 * packets) but the A5XX hardware generation has no CP_MEMCPY; copies use
 * CP_BLIT or CP_MEM_TO_MEM (see pm4.hpp's note on kMemcpy).
 */
#pragma once

#include <cstdint>

namespace hakios_metal {

/* ---- command processor packets (adreno_pm4.xml.h, enum adreno_pm4_type3_packets) ---- */

constexpr uint32_t CP_NOP = 16;                // CP_NOP                  adreno_pm4.xml.h:189
constexpr uint32_t CP_WAIT_MEM_WRITES = 18;    // CP_WAIT_MEM_WRITES     adreno_pm4.xml.h:259
constexpr uint32_t CP_WAIT_FOR_ME = 19;        // CP_WAIT_FOR_ME         adreno_pm4.xml.h:246
constexpr uint32_t CP_WAIT_FOR_IDLE = 38;      // CP_WAIT_FOR_IDLE       adreno_pm4.xml.h:195
constexpr uint32_t CP_BLIT = 44;               // CP_BLIT                adreno_pm4.xml.h:281
constexpr uint32_t CP_MEM_WRITE = 61;          // CP_MEM_WRITE           adreno_pm4.xml.h:205
constexpr uint32_t CP_INDIRECT_BUFFER = 63;    // CP_INDIRECT_BUFFER     adreno_pm4.xml.h:192
constexpr uint32_t CP_EVENT_WRITE = 70;        // CP_EVENT_WRITE         adreno_pm4.xml.h:210
constexpr uint32_t CP_SET_MARKER = 101;        // CP_SET_MARKER          adreno_pm4.xml.h:266
constexpr uint32_t CP_MEM_TO_MEM = 115;        // CP_MEM_TO_MEM          adreno_pm4.xml.h:280
/* CP_MEMCPY = 117 exists in the shared enum but is A6XX-and-later on the hardware. */

/* ---- A5XX registers (a5xx.xml.h) ---- */

constexpr uint32_t REG_A5XX_RBBM_STATUS = 0x04f5;        // REG_A5XX_RBBM_STATUS           a5xx.xml.h:2238
constexpr uint32_t REG_A5XX_CP_RB_BASE = 0x0800;          // REG_A5XX_CP_RB_BASE           a5xx.xml.h:898
constexpr uint32_t REG_A5XX_CP_RB_BASE_HI = 0x0801;        // REG_A5XX_CP_RB_BASE_HI        a5xx.xml.h:901
constexpr uint32_t REG_A5XX_CP_RB_CNTL = 0x0802;          // REG_A5XX_CP_RB_CNTL           a5xx.xml.h:904
constexpr uint32_t REG_A5XX_CP_RB_RPTR = 0x0806;          // REG_A5XX_CP_RB_RPTR           a5xx.xml.h:913
constexpr uint32_t REG_A5XX_CP_RB_WPTR = 0x0807;          // REG_A5XX_CP_RB_WPTR           a5xx.xml.h:916
constexpr uint32_t REG_A5XX_CP_INTERRUPT_STATUS = 0x0b18; // REG_A5XX_CP_INTERRUPT_STATUS   a5xx.xml.h:1048
constexpr uint32_t REG_A5XX_CP_HW_FAULT = 0x0b1a;         // REG_A5XX_CP_HW_FAULT          a5xx.xml.h:1051
constexpr uint32_t REG_A5XX_CP_PROTECT_STATUS = 0x0b1c;   // REG_A5XX_CP_PROTECT_STATUS     a5xx.xml.h:1054

}  // namespace hakios_metal
