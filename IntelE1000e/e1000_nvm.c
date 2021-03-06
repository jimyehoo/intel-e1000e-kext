/*******************************************************************************
 
 Intel PRO/1000 Linux driver
 Copyright(c) 1999 - 2010 Intel Corporation.
 
 This program is free software; you can redistribute it and/or modify it
 under the terms and conditions of the GNU General Public License,
 version 2, as published by the Free Software Foundation.
 
 This program is distributed in the hope it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 more details.
 
 You should have received a copy of the GNU General Public License along with
 this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 
 The full GNU General Public License is included in this distribution in
 the file called "COPYING".
 
 Contact Information:
 Linux NICS <linux.nics@intel.com>
 e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
 Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 
 *******************************************************************************/

#include "e1000.h"

static void e1000_stop_nvm(struct e1000_hw *hw);
static void e1000e_reload_nvm(struct e1000_hw *hw);

/**
 *  e1000_init_nvm_ops_generic - Initialize NVM function pointers
 *  @hw: pointer to the HW structure
 *
 *  Setups up the function pointers to no-op functions
 **/
void e1000_init_nvm_ops_generic(struct e1000_hw *hw)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	/* Initialize function pointers */
	nvm->ops.reload = e1000e_reload_nvm;
}

/**
 *  e1000_raise_eec_clk - Raise EEPROM clock
 *  @hw: pointer to the HW structure
 *  @eecd: pointer to the EEPROM
 *
 *  Enable/Raise the EEPROM clock bit.
 **/
static void e1000_raise_eec_clk(struct e1000_hw *hw, UInt32 *eecd)
{
	*eecd = *eecd | E1000_EECD_SK;
	ew32(EECD, *eecd);
	e1e_flush();
	udelay(hw->nvm.delay_usec);
}

/**
 *  e1000_lower_eec_clk - Lower EEPROM clock
 *  @hw: pointer to the HW structure
 *  @eecd: pointer to the EEPROM
 *
 *  Clear/Lower the EEPROM clock bit.
 **/
static void e1000_lower_eec_clk(struct e1000_hw *hw, UInt32 *eecd)
{
	*eecd = *eecd & ~E1000_EECD_SK;
	ew32(EECD, *eecd);
	e1e_flush();
	udelay(hw->nvm.delay_usec);
}

/**
 *  e1000_shift_out_eec_bits - Shift data bits our to the EEPROM
 *  @hw: pointer to the HW structure
 *  @data: data to send to the EEPROM
 *  @count: number of bits to shift out
 *
 *  We need to shift 'count' bits out to the EEPROM.  So, the value in the
 *  "data" parameter will be shifted out to the EEPROM one bit at a time.
 *  In order to do this, "data" must be broken down into bits.
 **/
static void e1000_shift_out_eec_bits(struct e1000_hw *hw, UInt16 data, UInt16 count)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	UInt32 eecd = er32(EECD);
	UInt32 mask;
	
	mask = 0x01 << (count - 1);
	if (nvm->type == e1000_nvm_eeprom_spi)
		eecd |= E1000_EECD_DO;
	
	do {
		eecd &= ~E1000_EECD_DI;
		
		if (data & mask)
			eecd |= E1000_EECD_DI;
		
		ew32(EECD, eecd);
		e1e_flush();
		
		udelay(nvm->delay_usec);
		
		e1000_raise_eec_clk(hw, &eecd);
		e1000_lower_eec_clk(hw, &eecd);
		
		mask >>= 1;
	} while (mask);
	
	eecd &= ~E1000_EECD_DI;
	ew32(EECD, eecd);
}

/**
 *  e1000_shift_in_eec_bits - Shift data bits in from the EEPROM
 *  @hw: pointer to the HW structure
 *  @count: number of bits to shift in
 *
 *  In order to read a register from the EEPROM, we need to shift 'count' bits
 *  in from the EEPROM.  Bits are "shifted in" by raising the clock input to
 *  the EEPROM (setting the SK bit), and then reading the value of the data out
 *  "DO" bit.  During this "shifting in" process the data in "DI" bit should
 *  always be clear.
 **/
static UInt16 e1000_shift_in_eec_bits(struct e1000_hw *hw, UInt16 count)
{
	UInt32 eecd;
	UInt32 i;
	UInt16 data;
	
	eecd = er32(EECD);
	eecd &= ~(E1000_EECD_DO | E1000_EECD_DI);
	data = 0;
	
	for (i = 0; i < count; i++) {
		data <<= 1;
		e1000_raise_eec_clk(hw, &eecd);
		
		eecd = er32(EECD);
		
		eecd &= ~E1000_EECD_DI;
		if (eecd & E1000_EECD_DO)
			data |= 1;
		
		e1000_lower_eec_clk(hw, &eecd);
	}
	
	return data;
}

/**
 *  e1000e_poll_eerd_eewr_done - Poll for EEPROM read/write completion
 *  @hw: pointer to the HW structure
 *  @ee_reg: EEPROM flag for polling
 *
 *  Polls the EEPROM status bit for either read or write completion based
 *  upon the value of 'ee_reg'.
 **/
SInt32 e1000e_poll_eerd_eewr_done(struct e1000_hw *hw, int ee_reg)
{
	UInt32 attempts = 100000;
	UInt32 i, reg = 0;
	SInt32 ret_val = -E1000_ERR_NVM;
	
	for (i = 0; i < attempts; i++) {
		if (ee_reg == E1000_NVM_POLL_READ)
			reg = er32(EERD);
		else
			reg = er32(EEWR);
		
		if (reg & E1000_NVM_RW_REG_DONE) {
			ret_val = E1000_SUCCESS;
			break;
		}
		
		udelay(5);
	}
	
	return ret_val;
}

/**
 *  e1000e_acquire_nvm - Generic request for access to EEPROM
 *  @hw: pointer to the HW structure
 *
 *  Set the EEPROM access request bit and wait for EEPROM access grant bit.
 *  Return successful if access grant bit set, else clear the request for
 *  EEPROM access and return -E1000_ERR_NVM (-1).
 **/
SInt32 e1000e_acquire_nvm(struct e1000_hw *hw)
{
	UInt32 eecd = er32(EECD);
	SInt32 timeout = E1000_NVM_GRANT_ATTEMPTS;
	SInt32 ret_val = E1000_SUCCESS;
	
	ew32(EECD, eecd | E1000_EECD_REQ);
	eecd = er32(EECD);
	while (timeout) {
		if (eecd & E1000_EECD_GNT)
			break;
		udelay(5);
		eecd = er32(EECD);
		timeout--;
	}
	
	if (!timeout) {
		eecd &= ~E1000_EECD_REQ;
		ew32(EECD, eecd);
		e_dbg("Could not acquire NVM grant\n");
		ret_val = -E1000_ERR_NVM;
	}
	
	return ret_val;
}

/**
 *  e1000_standby_nvm - Return EEPROM to standby state
 *  @hw: pointer to the HW structure
 *
 *  Return the EEPROM to a standby state.
 **/
static void e1000_standby_nvm(struct e1000_hw *hw)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	UInt32 eecd = er32(EECD);
	
	if (nvm->type == e1000_nvm_eeprom_spi) {
		/* Toggle CS to flush commands */
		eecd |= E1000_EECD_CS;
		ew32(EECD, eecd);
		e1e_flush();
		udelay(nvm->delay_usec);
		eecd &= ~E1000_EECD_CS;
		ew32(EECD, eecd);
		e1e_flush();
		udelay(nvm->delay_usec);
	}
}

/**
 *  e1000_stop_nvm - Terminate EEPROM command
 *  @hw: pointer to the HW structure
 *
 *  Terminates the current command by inverting the EEPROM's chip select pin.
 **/
static void e1000_stop_nvm(struct e1000_hw *hw)
{
	UInt32 eecd;
	
	eecd = er32(EECD);
	if (hw->nvm.type == e1000_nvm_eeprom_spi) {
		/* Pull CS high */
		eecd |= E1000_EECD_CS;
		e1000_lower_eec_clk(hw, &eecd);
	}
}

/**
 *  e1000e_release_nvm - Release exclusive access to EEPROM
 *  @hw: pointer to the HW structure
 *
 *  Stop any current commands to the EEPROM and clear the EEPROM request bit.
 **/
void e1000e_release_nvm(struct e1000_hw *hw)
{
	UInt32 eecd;
	
	e1000_stop_nvm(hw);
	
	eecd = er32(EECD);
	eecd &= ~E1000_EECD_REQ;
	ew32(EECD, eecd);
}

/**
 *  e1000_ready_nvm_eeprom - Prepares EEPROM for read/write
 *  @hw: pointer to the HW structure
 *
 *  Setups the EEPROM for reading and writing.
 **/
static SInt32 e1000_ready_nvm_eeprom(struct e1000_hw *hw)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	UInt32 eecd = er32(EECD);
	SInt32 ret_val = E1000_SUCCESS;
	UInt16 timeout = 0;
	UInt8 spi_stat_reg;
	
	if (nvm->type == e1000_nvm_eeprom_spi) {
		/* Clear SK and CS */
		eecd &= ~(E1000_EECD_CS | E1000_EECD_SK);
		ew32(EECD, eecd);
		udelay(1);
		timeout = NVM_MAX_RETRY_SPI;
		
		/*
		 * Read "Status Register" repeatedly until the LSB is cleared.
		 * The EEPROM will signal that the command has been completed
		 * by clearing bit 0 of the internal status register.  If it's
		 * not cleared within 'timeout', then error out.
		 */
		while (timeout) {
			e1000_shift_out_eec_bits(hw, NVM_RDSR_OPCODE_SPI,
			                         hw->nvm.opcode_bits);
			spi_stat_reg = (UInt8)e1000_shift_in_eec_bits(hw, 8);
			if (!(spi_stat_reg & NVM_STATUS_RDY_SPI))
				break;
			
			udelay(5);
			e1000_standby_nvm(hw);
			timeout--;
		}
		
		if (!timeout) {
			e_dbg("SPI NVM Status error\n");
			ret_val = -E1000_ERR_NVM;
			goto out;
		}
	}
	
out:
	return ret_val;
}

/**
 *  e1000e_read_nvm_eerd - Reads EEPROM using EERD register
 *  @hw: pointer to the HW structure
 *  @offset: offset of word in the EEPROM to read
 *  @words: number of words to read
 *  @data: word read from the EEPROM
 *
 *  Reads a 16 bit word from the EEPROM using the EERD register.
 **/
SInt32 e1000e_read_nvm_eerd(struct e1000_hw *hw, UInt16 offset, UInt16 words, UInt16 *data)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	UInt32 i, eerd = 0;
	SInt32 ret_val = E1000_SUCCESS;
	
	/*
	 * A check for invalid values:  offset too large, too many words,
	 * too many words for the offset, and not enough words.
	 */
	if ((offset >= nvm->word_size) || (words > (nvm->word_size - offset)) ||
	    (words == 0)) {
		e_dbg("nvm parameter(s) out of bounds\n");
		ret_val = -E1000_ERR_NVM;
		goto out;
	}
	
	for (i = 0; i < words; i++) {
		eerd = ((offset+i) << E1000_NVM_RW_ADDR_SHIFT) +
		E1000_NVM_RW_REG_START;
		
		ew32(EERD, eerd);
		ret_val = e1000e_poll_eerd_eewr_done(hw, E1000_NVM_POLL_READ);
		if (ret_val)
			break;
		
		data[i] = (er32(EERD) >>
		           E1000_NVM_RW_REG_DATA);
	}
	
out:
	return ret_val;
}

/**
 *  e1000e_write_nvm_spi - Write to EEPROM using SPI
 *  @hw: pointer to the HW structure
 *  @offset: offset within the EEPROM to be written to
 *  @words: number of words to write
 *  @data: 16 bit word(s) to be written to the EEPROM
 *
 *  Writes data to EEPROM at offset using SPI interface.
 *
 *  If e1000_update_nvm_checksum is not called after this function , the
 *  EEPROM will most likely contain an invalid checksum.
 **/
SInt32 e1000e_write_nvm_spi(struct e1000_hw *hw, UInt16 offset, UInt16 words, UInt16 *data)
{
	struct e1000_nvm_info *nvm = &hw->nvm;
	SInt32 ret_val;
	UInt16 widx = 0;
	
	/*
	 * A check for invalid values:  offset too large, too many words,
	 * and not enough words.
	 */
	if ((offset >= nvm->word_size) || (words > (nvm->word_size - offset)) ||
	    (words == 0)) {
		e_dbg("nvm parameter(s) out of bounds\n");
		ret_val = -E1000_ERR_NVM;
		goto out;
	}
	
	ret_val = nvm->ops.acquire(hw);
	if (ret_val)
		goto out;
	
	while (widx < words) {
		UInt8 write_opcode = NVM_WRITE_OPCODE_SPI;
		
		ret_val = e1000_ready_nvm_eeprom(hw);
		if (ret_val)
			goto release;
		
		e1000_standby_nvm(hw);
		
		/* Send the WRITE ENABLE command (8 bit opcode) */
		e1000_shift_out_eec_bits(hw, NVM_WREN_OPCODE_SPI,
		                         nvm->opcode_bits);
		
		e1000_standby_nvm(hw);
		
		/*
		 * Some SPI eeproms use the 8th address bit embedded in the
		 * opcode
		 */
		if ((nvm->address_bits == 8) && (offset >= 128))
			write_opcode |= NVM_A8_OPCODE_SPI;
		
		/* Send the Write command (8-bit opcode + addr) */
		e1000_shift_out_eec_bits(hw, write_opcode, nvm->opcode_bits);
		e1000_shift_out_eec_bits(hw, (UInt16)((offset + widx) * 2),
		                         nvm->address_bits);
		
		/* Loop to allow for up to whole page write of eeprom */
		while (widx < words) {
			UInt16 word_out = data[widx];
			word_out = (word_out >> 8) | (word_out << 8);
			e1000_shift_out_eec_bits(hw, word_out, 16);
			widx++;
			
			if ((((offset + widx) * 2) % nvm->page_size) == 0) {
				e1000_standby_nvm(hw);
				break;
			}
		}
	}
	
	msleep(10);
release:
	nvm->ops.release(hw);
	
out:
	return ret_val;
}

/**
 *  e1000e_read_pba_num - Read device part number
 *  @hw: pointer to the HW structure
 *  @pba_num: pointer to device part number
 *
 *  Reads the product board assembly (PBA) number from the EEPROM and stores
 *  the value in pba_num.
 **/
SInt32 e1000e_read_pba_num(struct e1000_hw *hw, UInt32 *pba_num)
{
	SInt32  ret_val;
	UInt16 nvm_data;
	
	ret_val = e1000_read_nvm(hw, NVM_PBA_OFFSET_0, 1, &nvm_data);
	if (ret_val) {
		e_dbg("NVM Read Error\n");
		goto out;
	}
	*pba_num = (UInt32)(nvm_data << 16);
	
	ret_val = e1000_read_nvm(hw, NVM_PBA_OFFSET_1, 1, &nvm_data);
	if (ret_val) {
		e_dbg("NVM Read Error\n");
		goto out;
	}
	*pba_num |= nvm_data;
	
out:
	return ret_val;
}

/**
 *  e1000e_read_mac_addr_generic - Read device MAC address
 *  @hw: pointer to the HW structure
 *
 *  Reads the device MAC address from the EEPROM and stores the value.
 *  Since devices with two ports use the same EEPROM, we increment the
 *  last bit in the MAC address for the second port.
 **/
SInt32 e1000e_read_mac_addr_generic(struct e1000_hw *hw)
{
	UInt32 rar_high;
	UInt32 rar_low;
	UInt16 i;
	
	rar_high = er32(RAH(0));
	rar_low = er32(RAL(0));
	
	for (i = 0; i < E1000_RAL_MAC_ADDR_LEN; i++)
		hw->mac.perm_addr[i] = (UInt8)(rar_low >> (i*8));
	
	for (i = 0; i < E1000_RAH_MAC_ADDR_LEN; i++)
		hw->mac.perm_addr[i+4] = (UInt8)(rar_high >> (i*8));
	
	for (i = 0; i < ETH_ADDR_LEN; i++)
		hw->mac.addr[i] = hw->mac.perm_addr[i];
	
	return E1000_SUCCESS;
}

/**
 *  e1000e_validate_nvm_checksum_generic - Validate EEPROM checksum
 *  @hw: pointer to the HW structure
 *
 *  Calculates the EEPROM checksum by reading/adding each word of the EEPROM
 *  and then verifies that the sum of the EEPROM is equal to 0xBABA.
 **/
SInt32 e1000e_validate_nvm_checksum_generic(struct e1000_hw *hw)
{
	SInt32 ret_val = E1000_SUCCESS;
	UInt16 checksum = 0;
	UInt16 i, nvm_data;
	
	for (i = 0; i < (NVM_CHECKSUM_REG + 1); i++) {
		ret_val = e1000_read_nvm(hw, i, 1, &nvm_data);
		if (ret_val) {
			e_dbg("NVM Read Error\n");
			goto out;
		}
		checksum += nvm_data;
	}
	
	if (checksum != (UInt16) NVM_SUM) {
		e_dbg("NVM Checksum Invalid\n");
		ret_val = -E1000_ERR_NVM;
		goto out;
	}
	
out:
	return ret_val;
}

/**
 *  e1000e_update_nvm_checksum_generic - Update EEPROM checksum
 *  @hw: pointer to the HW structure
 *
 *  Updates the EEPROM checksum by reading/adding each word of the EEPROM
 *  up to the checksum.  Then calculates the EEPROM checksum and writes the
 *  value to the EEPROM.
 **/
SInt32 e1000e_update_nvm_checksum_generic(struct e1000_hw *hw)
{
	SInt32  ret_val;
	UInt16 checksum = 0;
	UInt16 i, nvm_data;
	
	for (i = 0; i < NVM_CHECKSUM_REG; i++) {
		ret_val = e1000_read_nvm(hw, i, 1, &nvm_data);
		if (ret_val) {
			e_dbg("NVM Read Error while updating checksum.\n");
			goto out;
		}
		checksum += nvm_data;
	}
	checksum = (UInt16) NVM_SUM - checksum;
	ret_val = e1000_write_nvm(hw, NVM_CHECKSUM_REG, 1, &checksum);
	if (ret_val)
		e_dbg("NVM Write Error while updating checksum.\n");
	
out:
	return ret_val;
}

/**
 *  e1000e_reload_nvm - Reloads EEPROM
 *  @hw: pointer to the HW structure
 *
 *  Reloads the EEPROM by setting the "Reinitialize from EEPROM" bit in the
 *  extended control register.
 **/
static void e1000e_reload_nvm(struct e1000_hw *hw)
{
	UInt32 ctrl_ext;
	
	udelay(10);
	ctrl_ext = er32(CTRL_EXT);
	ctrl_ext |= E1000_CTRL_EXT_EE_RST;
	ew32(CTRL_EXT, ctrl_ext);
	e1e_flush();
}

