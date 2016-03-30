/* ---------------------------------------------------------------------------- */
/*                  Atmel Microcontroller Software Support                      */
/*                       SAM Software Package License                           */
/* ---------------------------------------------------------------------------- */
/* Copyright (c) 2015, Atmel Corporation                                        */
/*                                                                              */
/* All rights reserved.                                                         */
/*                                                                              */
/* Redistribution and use in source and binary forms, with or without           */
/* modification, are permitted provided that the following condition is met:    */
/*                                                                              */
/* - Redistributions of source code must retain the above copyright notice,     */
/* this list of conditions and the disclaimer below.                            */
/*                                                                              */
/* Atmel's name may not be used to endorse or promote products derived from     */
/* this software without specific prior written permission.                     */
/*                                                                              */
/* DISCLAIMER:  THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR   */
/* IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE   */
/* DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR ANY DIRECT, INDIRECT,      */
/* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT */
/* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,  */
/* OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    */
/* LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING         */
/* NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, */
/* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                           */
/* ---------------------------------------------------------------------------- */
/**
 * \file
 *
 * \brief USB host Mass Storage Class interface.
 *
 */
/*
 * Support and FAQ: visit <a href="http://www.atmel.com/design-support/">Atmel Support</a>
 */

#include "conf_usb_host.h"
#include "USBHDriver.h"
#include "USBH.h"
#include "uhi_msc.h"
#include <string.h>

#ifdef USB_HOST_HUB_SUPPORT
	// TODO
	#  error USB HUB support is not implemented on UHI MSC
#endif

#ifndef UHI_MSC_NOTIFY_NEW_LUN_EXT
	#define UHI_MSC_NOTIFY_NEW_LUN_EXT
#endif

/**
 * \ingroup uhi_msc_group
 * \defgroup uhi_msc_group_internal Implementation of UHI Mass Storage Class
 *
 * Class internal implementation
 * @{
 */

//! Timeout on SCSI commands
#define  UHI_MSC_SCSI_TIMEOUT 20000

/**
 * \name Internal defines and variables to manage MSC unit
 */
//@{

//! Flag to secure the UHI MSC resources
static volatile bool b_uhi_msc_free = true;

//! USB MSC device information
typedef struct {
	USBH_device_t *dev;
	USBHS_Ep_t ep_in;
	USBHS_Ep_t ep_out;
	uint8_t iface_num;
	uint8_t nb_lun;
	uhi_msc_lun_t *lun;
} uhi_msc_dev_t;

//! Information about the enumerated USB MSC device
static uhi_msc_dev_t uhi_msc_dev = {
	.dev = NULL,
	.lun = NULL,
	.nb_lun = 0,
};

//! Current USB MSC device selected by the UHI MSC
//@{
#ifdef USB_HOST_HUB_SUPPORT
	static uhi_msc_dev_t *uhi_msc_dev_sel;
#else
	#define uhi_msc_dev_sel (&uhi_msc_dev)
#endif
//@}

//! Current LUN selected in USB MSC device by the UHI MSC
//@{
static uhi_msc_lun_t *uhi_msc_lun_sel;
#define uhi_msc_lun_num_sel uhi_msc_cbw.bCBWLUN
//@}

//! Temporary structures used to read LUN information via a SCSI command
//@{

COMPILER_ALIGNED(32)
static struct {
	SBCReadCapacity10Data uhi_msc_capacity;
	uint8_t test[24];  /* Dummy for alignement*/
}capacity_fill;
COMPILER_ALIGNED(32)
static SBCInquiryData uhi_msc_inquiry;
COMPILER_ALIGNED(32)
static SBCRequestSenseData uhi_msc_sense;
COMPILER_ALIGNED(32)
static struct {
	SBCModeParameterHeader6 header;
	SBCInformationalExceptionsControl sense_data;
} uhi_msc_sense6;
//@}

//! Callback type used by SCSI sense command
typedef void (*uhi_msc_scsi_sense_callback_t) (void);

//! Internal callbacks
//@{
static uhi_msc_scsi_callback_t uhi_msc_scsi_sub_callback;
static uhi_msc_scsi_callback_t uhi_msc_scsi_callback;
static uhi_msc_scsi_sense_callback_t uhi_msc_scsi_sense_callback;

//@}
//@}

/**
 * \name Variables to manage SCSI requests
 */
//! Structure to send a CBW packet
COMPILER_ALIGNED(32)
static MSCbw uhi_msc_cbw =
{.dCBWSignature = MSD_CBW_SIGNATURE };

//! Structure to receive a CSW packet
COMPILER_ALIGNED(32)
static MSCsw uhi_msc_csw;

//! Structure to sent or received DATA packet
static uint8_t *uhi_msc_data;
//@}

/**
 * \name Internal routines
 */
//@{

/**
 * \name Routines to initialize the MSC devices
 */
//@{
static void uhi_msc_enable_step1(void);
static void uhi_msc_enable_step2(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans);
static void uhi_msc_enable_step3(void);
static void uhi_msc_enable_step4(bool b_success);
static void uhi_msc_enable_step5(bool b_success);
//@}

/**
 * \name SCSI commands
 */
//@{
static bool uhi_msc_select_lun(uint8_t lun);
static void uhi_msc_scsi_inquiry(uhi_msc_scsi_callback_t callback);
static void uhi_msc_scsi_inquiry_done(bool b_cbw_succes);
static void uhi_msc_scsi_test_unit_ready_done(bool b_cbw_succes);
static void uhi_msc_scsi_test_unit_ready_sense(void);
static void uhi_msc_scsi_read_capacity(uhi_msc_scsi_callback_t callback);
static void uhi_msc_scsi_read_capacity_done(bool b_cbw_succes);
static void uhi_msc_scsi_read_capacity_sense(void);
static void uhi_msc_scsi_mode_sense6(uhi_msc_scsi_callback_t callback);
static void uhi_msc_scsi_mode_sense6_done(bool b_cbw_succes);
static void uhi_msc_scsi_read_10_done(bool b_cbw_succes);
static void uhi_msc_scsi_write_10_done(bool b_cbw_succes);
static void uhi_msc_scsi_request_sense(uhi_msc_scsi_sense_callback_t callback);
static void uhi_msc_scsi_request_sense_done(bool b_cbw_succes);
//@}

/**
 * \name SCSI protocol sub routines
 */
//@{
static void uhi_msc_scsi(uhi_msc_scsi_callback_t callback, uint8_t *payload);
static void uhi_msc_cbw_sent(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered);
static void uhi_msc_data_transfered(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered);
static void uhi_msc_csw_wait(void);
static void uhi_msc_csw_received(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered);
static void uhi_msc_cbw_rst_stall(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans);
static void uhi_msc_data_csw_rst_stall(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans);
static void uhi_msc_transfer(USBHS_Ep_t endp,
							 uint8_t *payload,
							 uint16_t payload_size,
							 uhd_callback_trans_t callback_end);
static void uhi_msc_reset_endpoint(
	USBHS_Ep_t endp, uhd_callback_setup_end_t callback);
//@}

//@}


/**
 * \name Interface used by UHC module
 */
//@{

USBH_enum_status_t uhi_msc_install(USBH_device_t *dev)
{
	bool b_iface_supported;
	uint16_t conf_desc_lgt;
	USBInterfaceDescriptor *ptr_iface;

	if (uhi_msc_dev.dev != NULL) {
		return UHC_ENUM_SOFTWARE_LIMIT; // Device already allocated
	}

	conf_desc_lgt = dev->conf_desc->wTotalLength;
	ptr_iface = (USBInterfaceDescriptor *) dev->conf_desc;
	b_iface_supported = false;

	while (conf_desc_lgt) {
		switch (ptr_iface->bDescriptorType) {

		case USBGenericDescriptor_INTERFACE:
			if ((ptr_iface->bInterfaceClass   == MSInterfaceDescriptor_CLASS)
				&& (ptr_iface->bInterfaceSubClass == MSD_SUBCLASS_SCSI)
				&& (ptr_iface->bInterfaceProtocol == MSD_PROTOCOL_BULK_ONLY)) {
				// USB HID Mouse interface found
				b_iface_supported = true;
				uhi_msc_dev_sel->iface_num = ptr_iface->bInterfaceNumber;
				uhi_msc_dev.ep_in = 0;
				uhi_msc_dev.ep_out = 0;
			} else {
				// Stop allocation endpoint(s)
				b_iface_supported = false;
			}

			break;

		case USBGenericDescriptor_ENDPOINT:

			//  Allocate the endpoint
			if (!b_iface_supported)
				break;

			USBEndpointDescriptor *ptr_ep = (USBEndpointDescriptor *) ptr_iface;

			if (ptr_ep->bmAttributes != USBEndpointDescriptor_BULK) {
				// A bad endpoint interrupt may be present on no compliance U-disk
				break;
			}

			if (!USBH_HAL_ConfigurePipe(dev->address, ptr_ep)) {
				return UHC_ENUM_HARDWARE_LIMIT; // Endpoint allocation fail
			}

			if (ptr_ep->bEndpointAddress & USB_EP_DIR_IN)
				uhi_msc_dev.ep_in = ptr_ep->bEndpointAddress;
			else
				uhi_msc_dev.ep_out = ptr_ep->bEndpointAddress;

			if (uhi_msc_dev.ep_out && uhi_msc_dev.ep_in) {
				// All endpoints allocated
				uhi_msc_dev.dev = dev;
				uhi_msc_dev.nb_lun = 0;
				return UHC_ENUM_SUCCESS;
			}

			break;

		}

		assert(conf_desc_lgt >= ptr_iface->bLength);
		conf_desc_lgt -= ptr_iface->bLength;
		ptr_iface = (USBInterfaceDescriptor *)((uint8_t *)ptr_iface +
											   ptr_iface->bLength);
	}

	return UHC_ENUM_UNSUPPORTED; // No interface supported
}

void uhi_msc_enable(USBH_device_t *dev)
{
	if (uhi_msc_dev.dev != dev) {
		return; // No interface to enable
	}

	// ** Process to enable device **
	// Setup request - Get Number of LUN
	// For each LUN {
	//   MSC request - Inquiry
	//   MSC request - Read capacity*
	// }
	// *It is required by specific U-disk which does not respect USB MSC norm.

#ifdef USB_HOST_HUB_SUPPORT
	uhi_msc_dev_sel = &uhi_msc_dev[];

	if (!b_uhi_msc_free) {
		// Install must be postponed
	}

#else
	assert(uhi_msc_dev.dev != NULL);
#endif
	b_uhi_msc_free = false;
	uhi_msc_enable_step1();
}

void uhi_msc_uninstall(USBH_device_t *dev)
{
	if (uhi_msc_dev.dev != dev) {
		return; // Device not enabled in this interface
	}

	uhi_msc_dev.dev = NULL;

	if (uhi_msc_dev_sel->lun != NULL)
		free(uhi_msc_dev_sel->lun);

	UHI_MSC_CHANGE(dev, false);
}

//@}

/**
 * \name Routines to initialize the MSC devices
 */
//@{

/**
 * \brief Step 1 of the initialization of a USB MSC device
 * Sends the setup request GET_MAX_LUN.
 */
static void uhi_msc_enable_step1(void)
{
	// Default value is 0, because the Get LUN number request
	// can be stalled if the device have only 1 LUN
	uhi_msc_dev_sel->nb_lun = 0;

	// Request Get LUN number
	USBGenericRequest req;
	req.bmRequestType = USB_REQ_RECIP_INTERFACE | USB_REQ_TYPE_CLASS |
						USB_REQ_DIR_IN;
	req.bRequest = MSD_GET_MAX_LUN;
	req.wValue = 0;
	req.wIndex = uhi_msc_dev_sel->iface_num;
	req.wLength = 1;
	USBH_HAL_SetupReq(uhi_msc_dev_sel->dev->address,
					  &req,
					  &(uhi_msc_dev_sel->nb_lun),
					  1,
					  NULL,
					  uhi_msc_enable_step2);
}

/**
 * \brief Step 2 of the initialization of a USB MSC device
 * Decode the setup request GET_MAX_LUN and allocs LUN structures.
 *
 * \param add           USB address of the setup request
 * \param status        Transfer status
 * \param payload_trans Number of data transfered during DATA phase
 */
static void uhi_msc_enable_step2(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans)
{
	UNUSED(add);

	// Compute number of LUN
	if (status == UHD_TRANS_NOERROR) {
		if (payload_trans) {
			// The received value is the maximum index
			uhi_msc_dev_sel->nb_lun++;
		} else
			uhi_msc_dev_sel->nb_lun = 1;
	} else if (status == UHD_TRANS_STALL)
		uhi_msc_dev_sel->nb_lun = 1;
	else {
		// Error set no LUN
		b_uhi_msc_free = true;
		return;
	}

	// Alloc LUN structures
	uhi_msc_dev_sel->lun = malloc(uhi_msc_dev_sel->nb_lun * sizeof(uhi_msc_lun_t));

	if (uhi_msc_dev_sel->lun == NULL) {
		assert(false);
		b_uhi_msc_free = true;
		return;
	}

	// Initialize each LUN
	uhi_msc_lun_num_sel = ((uint8_t)-1) & 0xF;
	uhi_msc_enable_step3();
}

/**
 * \brief Step 3 of the initialization of a USB MSC device
 * Selects the next LUN.
 * Sends the SCSI INQUIRY request on the LUN.
 */
static void uhi_msc_enable_step3(void)
{
	if (!uhi_msc_select_lun(++uhi_msc_lun_num_sel)) {
		// End of enable MSC interface
		b_uhi_msc_free = true;
		UHI_MSC_CHANGE(uhi_msc_dev.dev, true);
		return;
	}

	uhi_msc_lun_sel->status = LUN_NOT_PRESENT;
	uhi_msc_scsi_inquiry(uhi_msc_enable_step4);
}

/**
 * \brief Step 4 of the initialization of a USB MSC device
 * Sends the SCSI READ CAPACITY request on the current LUN.
 *
 * \param b_success true, if the scsi command is successful
 */
static void uhi_msc_enable_step4(bool b_success)
{
	if (!b_success) {
		uhi_msc_enable_step3();
		return;
	}

	uhi_msc_scsi_read_capacity(uhi_msc_enable_step5);
}

/**
 * \brief Step 5 of the initialization of a USB MSC device
 * Go to the initialization of the next LUN.
 *
 * \param b_success true, if the scsi command is successful
 */
static void uhi_msc_enable_step5(bool b_success)
{
	UNUSED(b_success);
	// Initialize next LUN
	uhi_msc_enable_step3();
}

//@}

/**
 * \name External SCSI commands
 */
//@{
bool uhi_msc_is_available(void)
{
	return b_uhi_msc_free;
}

uint8_t uhi_msc_get_lun(void)
{
	if (uhi_msc_dev.dev == NULL)
		return 0;

	return uhi_msc_dev.nb_lun;
}

uhi_msc_lun_t *uhi_msc_get_lun_desc(uint8_t lun)
{
	if (lun >= uhi_msc_dev.nb_lun)
		return NULL;

	return &uhi_msc_dev.lun[lun];
}

bool uhi_msc_scsi_test_unit_ready(uint8_t lun, uhi_msc_scsi_callback_t callback)
{
	if (!uhi_msc_select_lun(lun))
		return false;

	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength = 0;
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_HOST_TO_DEVICE;
	uhi_msc_cbw.bCBWCBLength = 6;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));
	uhi_msc_cbw.pCommand[0] = SBC_TEST_UNIT_READY;

	uhi_msc_scsi(uhi_msc_scsi_test_unit_ready_done, NULL);
	return true;
}

bool uhi_msc_scsi_read_10(uint8_t lun, uint32_t addr, uint8_t *ram,
						  uint8_t nb_sector, uhi_msc_scsi_callback_t callback)
{
	uint32_t addrTmp = addr;
	uint32_t *pBlockLen = (uint32_t *)
						  &uhi_msc_lun_sel->capacity.pLogicalBlockLength[0];

	if (!uhi_msc_select_lun(lun))
		return false;

	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength =
		(*pBlockLen) * nb_sector;
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_DEVICE_TO_HOST;
	uhi_msc_cbw.bCBWCBLength = 10;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));

	/**pCommand++ = SBC_WRITE10;
	memcpy(pCommand, &addr, sizeof(addr));*/
	// CBWCB0 - Operation Code
	uhi_msc_cbw.pCommand[0] = SBC_READ_10;

	// CBWCB1 - RDPROTECT, DPO, FUA, Obsolete (0x00) (done by previous memset())

	addrTmp = CPU_TO_BE32(addr);
	// CBWCB2 to 5 - Logical Block Address (BE16)
	memcpy(&uhi_msc_cbw.pCommand[2], &addrTmp, sizeof(addr));

	// CBWCW6 - Reserved (0x00) (done by previous memset())
	// CBWCW7 to 8 - Transfer Length
	// uhi_msc_cbw.pCommand[7] = 0x00; // MSB (done by previous memset())
	uhi_msc_cbw.pCommand[8] = nb_sector; // LSB

	// CBWCW9 - Control (0x00) (done by previous memset())
	uhi_msc_scsi(uhi_msc_scsi_read_10_done, ram);
	return true;
}

bool uhi_msc_scsi_write_10(uint8_t lun, uint32_t addr, const uint8_t *ram,
						   uint8_t nb_sector, uhi_msc_scsi_callback_t callback)
{
	uint32_t addrTmp = addr;
	uint32_t *pBlockLen = (uint32_t *)
						  &uhi_msc_lun_sel->capacity.pLogicalBlockLength[0];

	if (!uhi_msc_select_lun(lun))
		return false;

	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength =
		(*pBlockLen) * nb_sector;
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_HOST_TO_DEVICE;
	uhi_msc_cbw.bCBWCBLength = 10;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));

	// CBWCB0 - Operation Code
	uhi_msc_cbw.pCommand[0] = SBC_WRITE_10;

	addrTmp = CPU_TO_BE32(addr);
	// CBWCB1 - RDPROTECT, DPO, FUA, Obsolete (0x00) (done by previous memset())

	// CBWCB2 to 5 - Logical Block Address (BE16)
	memcpy(&uhi_msc_cbw.pCommand[2], &addrTmp, sizeof(addr));

	// CBWCW6 - Reserved (0x00) (done by previous memset())

	// CBWCW7 to 8 - Transfer Length
	// uhi_msc_cbw.pCommand[7] = 0x00; // MSB (done by previous memset())
	uhi_msc_cbw.pCommand[8] = nb_sector; // LSB

	// CBWCW9 - Control (0x00) (done by previous memset())
	uhi_msc_scsi(uhi_msc_scsi_write_10_done, (uint8_t *) ram);
	return true;
}

//@}


/**
 * \name Internal SCSI commands and sub routines
 */
//@{

/**
 * \brief Selects a LUN
 *
 * \param lun   LUN number to select
 *
 * \return  true, if the LUN number is correct
 */
static bool uhi_msc_select_lun(uint8_t lun)
{
	if (lun >= uhi_msc_dev_sel->nb_lun)
		return false;

	uhi_msc_lun_num_sel = lun;
	uhi_msc_lun_sel = &uhi_msc_dev_sel->lun[lun];
	return true;
}

/**
 * \brief Sends the CBW packet of the scsi INQUIRY command
 *
 * \param callback  Callback to call at the end of scsi command
 */
static void uhi_msc_scsi_inquiry(uhi_msc_scsi_callback_t callback)
{
	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength = sizeof(SBCInquiryData);
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_DEVICE_TO_HOST;
	uhi_msc_cbw.bCBWCBLength = 6;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));
	uhi_msc_cbw.pCommand[0] = SBC_INQUIRY;
	uhi_msc_cbw.pCommand[4] = sizeof(SBCInquiryData);
	uhi_msc_scsi(uhi_msc_scsi_inquiry_done, (uint8_t *) & uhi_msc_inquiry);
}

/**
 * \brief Call the callback at the end of scsi INQUIRY command
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_inquiry_done(bool b_cbw_succes)
{
	SCB_InvalidateDCache_by_Addr((uint32_t*)(&uhi_msc_inquiry),sizeof(uhi_msc_inquiry));

	if ((!b_cbw_succes) || uhi_msc_csw.dCSWDataResidue) {
		uhi_msc_scsi_callback(false);
		return;
	}

	// Inquiry successful
	uhi_msc_scsi_callback(true);
}

/**
 * \brief Decodes the result of scsi TEST UNIT READY command
 * Launches READ CAPACITY command, if a new LUN has been detected.
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_test_unit_ready_done(bool b_cbw_succes)
{
	if (!b_cbw_succes) {
		uhi_msc_lun_sel->status = LUN_FAIL;
		uhi_msc_scsi_callback(false);
		return;
	}

	// Test unit ready successful
	if (uhi_msc_csw.bCSWStatus != MSD_CSW_COMMAND_PASSED) {
		uhi_msc_lun_sel->status = LUN_FAIL; // By default
		// LUN is not ready
		// Read a sense code
		uhi_msc_scsi_request_sense(uhi_msc_scsi_test_unit_ready_sense);
		return;
	}

	if (uhi_msc_lun_sel->status != LUN_GOOD) {
		// It is a new LUN
		if (uhi_msc_lun_sel->status == LUN_BUSY) {
			// To initialize the new LUN read capacity and write protection flag
			uhi_msc_lun_sel->status = LUN_GOOD;
			uhi_msc_scsi_read_capacity(uhi_msc_scsi_callback);
			return;
		}

		// The USB device has not returned a BUSY
		// to notify the LUN change to high level,
		// then add the notify
		uhi_msc_lun_sel->status = LUN_BUSY;
	}

	uhi_msc_scsi_callback(true);
}

/**
 * \brief Decodes the scsi sense code after an error on TEST UNIT READY command
 */
static void uhi_msc_scsi_test_unit_ready_sense(void)
{
	uint16_t sense_key = uhi_msc_sense.bSenseKey;
	uint16_t add_sense = (uint16_t)uhi_msc_sense.bAdditionalSenseCode << 8
						 | uhi_msc_sense.bAdditionalSenseCodeQualifier;

	// Decode data sense
	if ((uhi_msc_sense.bResponseCode
		 & SBC_SENSE_RESPONSE_CODE_MASK) == SBC_SENSE_DATA_FIXED_CURRENT) {
		// Valid data
		if ((sense_key == SBC_SENSE_KEY_NOT_READY)
			&& (add_sense == (SBC_ASC_MEDIUM_NOT_PRESENT << 8)))
			uhi_msc_lun_sel->status = LUN_NOT_PRESENT;

		if ((sense_key == SBC_SENSE_KEY_UNIT_ATTENTION)
			&& (add_sense == (SBC_ASC_NOT_READY_TO_READY_CHANGE << 8)))
			uhi_msc_lun_sel->status = LUN_BUSY;
	}

	uhi_msc_scsi_callback(true);
}

/**
 * \brief Sends the CBW packet of the scsi READ CAPACITY command
 *
 * \param callback  Callback to call at the end of scsi command
 */
static void uhi_msc_scsi_read_capacity(uhi_msc_scsi_callback_t callback)
{
	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength = sizeof(SBCReadCapacity10Data);
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_DEVICE_TO_HOST;
	uhi_msc_cbw.bCBWCBLength = 10;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));
	uhi_msc_cbw.pCommand[0] = SBC_READ_CAPACITY_10;
	uhi_msc_scsi(uhi_msc_scsi_read_capacity_done, (uint8_t *)&(capacity_fill.uhi_msc_capacity));
}

/**
 * \brief Decodes the result of scsi READ CAPACITY command
 * Launches MODE SENSE 6 command.
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_read_capacity_done(bool b_cbw_succes)
{
	uint32_t *pBlockLen, *pBlockAddr;
	pBlockLen = (uint32_t *)&uhi_msc_lun_sel->capacity.pLogicalBlockLength[0];
	pBlockAddr = (uint32_t *)&uhi_msc_lun_sel->capacity.pLogicalBlockAddress[0];

	SCB_InvalidateDCache_by_Addr((uint32_t*)(&capacity_fill.uhi_msc_capacity),
					sizeof(capacity_fill.uhi_msc_capacity));

	if ((!b_cbw_succes) || (uhi_msc_csw.bCSWStatus != MSD_CSW_COMMAND_PASSED)
		|| uhi_msc_csw.dCSWDataResidue) {
		// Read capacity has failed
		uhi_msc_lun_sel->status = LUN_FAIL;

		// (WA-Udisk) Read request sense
		uhi_msc_scsi_request_sense(uhi_msc_scsi_read_capacity_sense);
		return;
	}

	// Format capacity data
	*pBlockLen = be32_to_cpu(*((uint32_t *)
								&capacity_fill.uhi_msc_capacity.pLogicalBlockLength[0]));
	*pBlockAddr = be32_to_cpu(*((uint32_t *)
								 &capacity_fill.uhi_msc_capacity.pLogicalBlockAddress[0]));

	// Now, read flag write protection
	uhi_msc_scsi_mode_sense6(uhi_msc_scsi_callback);
}

/**
 * \brief Decodes the scsi sense code after a scsi READ CAPACITY command failed
 */
static void uhi_msc_scsi_read_capacity_sense(void)
{
	uhi_msc_scsi_callback(false);
}

/**
 * \brief Sends the CBW packet of the scsi MODE SENSE 6 command
 * This function returns the write-protected mode.
 * Field optional provided only by the LUN with a write protection feature.
 *
 * \param callback  Callback to call at the end of scsi command
 */
static void uhi_msc_scsi_mode_sense6(uhi_msc_scsi_callback_t callback)
{
	uhi_msc_scsi_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength = sizeof(uhi_msc_sense6);
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_DEVICE_TO_HOST;
	uhi_msc_cbw.bCBWCBLength = 6;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));
	uhi_msc_cbw.pCommand[0] = SBC_MODE_SENSE_6;
	uhi_msc_cbw.pCommand[2] = SBC_PAGE_INFORMATIONAL_EXCEPTIONS_CONTROL;
	uhi_msc_cbw.pCommand[4] = sizeof(uhi_msc_sense6);
	uhi_msc_scsi(uhi_msc_scsi_mode_sense6_done,
				 (uint8_t *) & uhi_msc_sense6);
}

/**
 * \brief Decodes the result of scsi MODE SENSE 6 command
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_mode_sense6_done(bool b_cbw_succes)
{
	if ((!b_cbw_succes) || (uhi_msc_csw.bCSWStatus != MSD_CSW_COMMAND_PASSED)
		|| (uhi_msc_csw.dCSWDataResidue < 4)) {
		// Sense6 command is not supported,
		// The device must be not write protected.
		uhi_msc_lun_sel->b_write_protected = false;
		uhi_msc_scsi_callback(true);
		return;
	}
	SCB_InvalidateDCache_by_Addr((uint32_t *)&uhi_msc_sense6, sizeof(uhi_msc_sense6));

	// Decode field
	uhi_msc_lun_sel->b_write_protected =
		(uhi_msc_sense6.header.isWP);
	uhi_msc_scsi_callback(true);
}

/**
 * \brief Decodes the result of scsi READ 10 command
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_read_10_done(bool b_cbw_succes)
{
	if ((!b_cbw_succes) || (uhi_msc_csw.bCSWStatus != MSD_CSW_COMMAND_PASSED)
		|| uhi_msc_csw.dCSWDataResidue) {
		// Read10 has failed
		uhi_msc_lun_sel->status = LUN_FAIL;
		uhi_msc_scsi_callback(false);
		return;
	}

	uhi_msc_scsi_callback(true);
}

/**
 * \brief Decodes the result of scsi WRITE 10 command
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_write_10_done(bool b_cbw_succes)
{
	if ((!b_cbw_succes) || (uhi_msc_csw.bCSWStatus != MSD_CSW_COMMAND_PASSED)
		|| uhi_msc_csw.dCSWDataResidue) {
		// Write10 has failed
		uhi_msc_lun_sel->status = LUN_FAIL;
		uhi_msc_scsi_callback(false);
		return;
	}

	uhi_msc_scsi_callback(true);
}

/**
 * \brief Sends the CBW packet of the scsi REQUESRT SENSE command
 * Called by TEST UNIT READY and READ CAPACITY command in case of error.
 *
 * \param callback  Callback to call at the end of scsi command
 */
static void uhi_msc_scsi_request_sense(uhi_msc_scsi_sense_callback_t callback)
{
	uhi_msc_scsi_sense_callback = callback;

	// Prepare specific value of CBW packet
	uhi_msc_cbw.dCBWDataTransferLength = sizeof(SBCRequestSenseData);
	uhi_msc_cbw.bmCBWFlags = MSD_CBW_DEVICE_TO_HOST;
	uhi_msc_cbw.bCBWCBLength = 6;
	memset(uhi_msc_cbw.pCommand, 0, sizeof(uhi_msc_cbw.pCommand));
	uhi_msc_cbw.pCommand[0] = SBC_REQUEST_SENSE;
	uhi_msc_cbw.pCommand[4] = sizeof(SBCRequestSenseData);

	uhi_msc_scsi(uhi_msc_scsi_request_sense_done, (uint8_t *)&uhi_msc_sense);
}

/**
 * \brief Decodes the result of scsi REQUEST SENSE command
 *
 * \param b_cbw_succes true, if the scsi command is successful
 */
static void uhi_msc_scsi_request_sense_done(bool b_cbw_succes)
{
	if ((!b_cbw_succes) || uhi_msc_csw.dCSWDataResidue) {
		uhi_msc_scsi_callback(false);
		return;
	}
	SCB_InvalidateDCache_by_Addr((uint32_t *)&uhi_msc_sense, sizeof(uhi_msc_sense));

	// Request sense successful
	uhi_msc_scsi_sense_callback();
}

//@}

/**
 * \name Internal SCSI protocol routines
 */
//@{

/**
 * \brief Sends the CBW packet
 *
 * \param callback  Callback to call at the end of scsi protocol
 * \param payload   Pointer on the data to transfer (Optional)
 */
static void uhi_msc_scsi(uhi_msc_scsi_callback_t callback, uint8_t *payload)
{
	// Save context
	uhi_msc_scsi_sub_callback = callback;
	uhi_msc_data = payload;

	// Prepare CBW
	uhi_msc_cbw.dCBWTag++;
	uhi_msc_cbw.dCBWDataTransferLength =
		(uhi_msc_cbw.dCBWDataTransferLength);
	// CBWCB0 - Operation Code
	// CBWCB1 - Obsolete, EVPD = 0
	// CBWCB2 - Page Code = 0
	// CBWCB3 - MSB(Allocation Length)
	// CBWCB4 - LSB(Allocation Length)
	// CBWCW5 - Control = 0

	SCB_CleanDCache_by_Addr((uint32_t *)(&uhi_msc_cbw),sizeof(uhi_msc_cbw));
	// Start transfer of CBW packet on bulk endpoint OUT
	uhi_msc_transfer(uhi_msc_dev_sel->ep_out, (uint8_t *) &uhi_msc_cbw,
					 sizeof(uhi_msc_cbw), uhi_msc_cbw_sent);
}

/**
 * \brief Checks the CBW packet transfer and launch the next step
 * The next step can be a DATA phase or a CSW packet.
 *
 * \param add           USB address used by the transfer
 * \param status        Transfer status
 * \param nb_transfered Number of data transfered
 */
static void uhi_msc_cbw_sent(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered)
{
	USBHS_Ep_t endp;
	UNUSED(add);
	UNUSED(ep);
	UNUSED(nb_transfered);
//	printf("Cbw ");
	// Checks the result of CBW transfer
	if (status != UHD_TRANS_NOERROR) {
		if (status == UHD_TRANS_STALL) {
			uhi_msc_reset_endpoint(uhi_msc_dev_sel->ep_out,
								   uhi_msc_cbw_rst_stall);
			return;
		}

		uhi_msc_scsi_sub_callback(false);
		return;
	}

	assert(nb_transfered == sizeof(uhi_msc_cbw));
	uhi_msc_cbw.dCBWDataTransferLength =
		(uhi_msc_cbw.dCBWDataTransferLength);
	// Here CBW is success

	if (!uhi_msc_cbw.dCBWDataTransferLength) {
		// Start CSW phase
		uhi_msc_csw_wait();
		return;
	}

	// Start DATA phase
	if (uhi_msc_cbw.bmCBWFlags & MSD_CBW_DEVICE_TO_HOST)
		endp = uhi_msc_dev_sel->ep_in;
	else
		{
		endp = uhi_msc_dev_sel->ep_out;
		SCB_CleanDCache_by_Addr((uint32_t *)uhi_msc_data,uhi_msc_cbw.dCBWDataTransferLength);

		}

	uhi_msc_transfer(endp, uhi_msc_data, uhi_msc_cbw.dCBWDataTransferLength,
					 uhi_msc_data_transfered);
}

/**
 * \brief Checks the DATA phase transfer and launch the next step
 * The next step can be a CSW packet or a endpoint reset in case of STALL.
 *
 * \param add           USB address used by the transfer
 * \param status        Transfer status
 * \param nb_transfered Number of data transfered
 */
static void uhi_msc_data_transfered(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered)
{
	USBHS_Ep_t endp;
	UNUSED(add);
	UNUSED(ep);
	UNUSED(nb_transfered);

	if (status != UHD_TRANS_NOERROR) {
		if (status == UHD_TRANS_STALL) {
			if (uhi_msc_cbw.bmCBWFlags & MSD_CBW_DEVICE_TO_HOST)
				endp = uhi_msc_dev_sel->ep_in;
			else
				endp = uhi_msc_dev_sel->ep_out;

			uhi_msc_reset_endpoint(endp, uhi_msc_data_csw_rst_stall);
			return;
		}

		uhi_msc_scsi_sub_callback(false);
		return;
	}

	// DATA phase complete
	if (uhi_msc_cbw.bmCBWFlags & MSD_CBW_DEVICE_TO_HOST)
		{
		SCB_InvalidateDCache_by_Addr((uint32_t *)uhi_msc_data,uhi_msc_cbw.dCBWDataTransferLength);
		}

	// Start CSW phase
	uhi_msc_csw_wait();
	return;
}

/**
 * \brief Start the transfer of the CSW packet
 */
static void uhi_msc_csw_wait(void)
{
	// Start transfer of CSW packet on bulk endpoint IN
	uhi_msc_transfer(uhi_msc_dev_sel->ep_in, (uint8_t *) & uhi_msc_csw,
					 sizeof(uhi_msc_csw), uhi_msc_csw_received);
}

/**
 * \brief Checks the CSW packet transfer
 *
 * \param add           USB address used by the transfer
 * \param status        Transfer status
 * \param nb_transfered Number of data transfered
 */
static void uhi_msc_csw_received(
	USBHS_Add_t add,
	USBHS_Ep_t ep,
	USBH_XfrStatus_t status,
	uint32_t nb_transfered)
{
	UNUSED(add);
	UNUSED(ep);

	if (status != UHD_TRANS_NOERROR) {
		if (status == UHD_TRANS_STALL) {
			uhi_msc_reset_endpoint(uhi_msc_dev_sel->ep_in,
								   uhi_msc_data_csw_rst_stall);
			return;
		}

		uhi_msc_scsi_sub_callback(false);
		return;
	}
	SCB_InvalidateDCache_by_Addr((uint32_t *)&uhi_msc_csw, sizeof(uhi_msc_csw));

	if ((nb_transfered != sizeof(uhi_msc_csw))
		|| (uhi_msc_csw.dCSWTag != uhi_msc_cbw.dCBWTag)
		|| (uhi_msc_csw.dCSWSignature != MSD_CSW_SIGNATURE)) {
		// Error in CSW fields
		uhi_msc_scsi_sub_callback(false);
		return;
	}

	// CSW is success
	uhi_msc_csw.dCSWDataResidue = uhi_msc_csw.dCSWDataResidue;
	uhi_msc_scsi_sub_callback(true);
}

/**
 * \brief Manages the end of setup request RESET ENDPOINT after a CBW packet
 *
 * \param add           USB address of the setup request
 * \param status        Transfer status
 * \param payload_trans Number of data transfered during DATA phase
 */
static void uhi_msc_cbw_rst_stall(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans)
{
	UNUSED(add);
	UNUSED(status);
	UNUSED(payload_trans);
	uhi_msc_scsi_sub_callback(false);
}

/**
 * \brief Manages the end of RESET ENDPOINT request after a DATA or CSW packet
 *
 * \param add           USB address of the setup request
 * \param status        Transfer status
 * \param payload_trans Number of data transfered during DATA phase
 */
static void uhi_msc_data_csw_rst_stall(
	USBHS_Add_t add,
	USBH_XfrStatus_t status,
	uint16_t payload_trans)
{
	UNUSED(add);
	UNUSED(payload_trans);

	if (status != UHD_TRANS_NOERROR) {
		uhi_msc_scsi_sub_callback(false);
		return;
	}

	// DATA stalled but CSW must be received after clear of STALL
	uhi_msc_csw_wait();
}

/**
 * \brief Start a transfer on an endpoint of current USB MSC device
 * Used to send a CBW packet, DATA packet, or a CSW packet.
 *
 * \param endp          Endpoint to use for this transfer
 * \param payload       Pointer on the data to transfer
 * \param payload_size  Size of the data to transfer
 * \param callback_end  Callback to call at the end of transfer
 */
static void uhi_msc_transfer(USBHS_Ep_t endp,
							 uint8_t *payload,
							 uint16_t payload_size,
							 uhd_callback_trans_t callback_end)
{
	if (!USBH_HAL_RunEndpoint(uhi_msc_dev_sel->dev->address,
							  endp, false, payload, payload_size,
							  UHI_MSC_SCSI_TIMEOUT, callback_end))
		uhi_msc_scsi_sub_callback(false);
}

/**
 * \brief Sends a setup request RESET ENDPOINT
 * Used after a STALL received during a scsi command.
 *
 * \param endp          Endpoint to reset
 * \param callback_end  Callback to call at the end of request
 */
static void uhi_msc_reset_endpoint(
	USBHS_Ep_t endp, uhd_callback_setup_end_t callback)
{
	// Setup Request to reset endpoint
	USBGenericRequest req;

	req.bmRequestType = USB_REQ_RECIP_ENDPOINT
						| USB_REQ_TYPE_STANDARD | USB_REQ_DIR_OUT;
	req.bRequest = USB_REQ_CLEAR_FEATURE;
	req.wValue = USB_EP_FEATURE_HALT;
	req.wIndex = endp;
	req.wLength = 0;

	if (!USBH_HAL_SetupReq(uhi_msc_dev_sel->dev->address,
						   &req, NULL, 0, NULL, callback))
		callback(uhi_msc_dev_sel->dev->address, UHD_TRANS_DISCONNECT, 0);
}

//@}

//@}
