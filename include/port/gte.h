#ifndef _PORT_GTE_H
#define _PORT_GTE_H

/** @brief Software implementation of the PS1 Geometry Transformation Engine (COP2).
 *
 * Bit-exact model of the hardware (fixed-point maths, 44-bit MAC overflow checks, saturation and
 * FLAG bits, the UNR division used by RTPS/RTPT), written from the psx-spx documentation
 * ("Geometry Transformation Engine (GTE)"). The interface mirrors the CPU's view of COP2:
 * register moves (MTC2/MFC2/CTC2/CFC2, LWC2/SWC2 are moves through memory) and command words
 * (the 25-bit immediate of a COP2 instruction).
 *
 * Port build only (`SH_PORT`).
 */

#include <stdint.h>

/** @brief Writes data register `reg` (cop2r0..31), as MTC2/LWC2 would. */
void Gte_DataWrite(uint32_t reg, uint32_t value);

/** @brief Reads data register `reg` (cop2r0..31), as MFC2/SWC2 would. */
uint32_t Gte_DataRead(uint32_t reg);

/** @brief Writes control register `reg` (cop2r32..63 as 0..31), as CTC2 would. */
void Gte_CtrlWrite(uint32_t reg, uint32_t value);

/** @brief Reads control register `reg` (cop2r32..63 as 0..31), as CFC2 would. */
uint32_t Gte_CtrlRead(uint32_t reg);

/** @brief Executes a GTE command (COP2 immediate: opcode in bits 0-5, lm bit 10, cv 13-14, v 15-16, mx 17-18, sf 19). */
void Gte_Command(uint32_t cmd);

/** @brief Resets all GTE registers to zero. */
void Gte_Reset(void);

#endif
