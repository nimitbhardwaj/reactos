/*
 * PROJECT:        ReactOS Kernel
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        To Implement AHCI Miniport driver targeting storport NT 5.2
 * PROGRAMMERS:    Aman Priyadarshi (aman.eureka@gmail.com)
 */

#include <ntddk.h>
#include <ata.h>
#include <storport.h>

#define DEBUG 1

#define MAXIMUM_AHCI_PORT_COUNT             25
#define MAXIMUM_QUEUE_BUFFER_SIZE           255
#define MAXIMUM_TRANSFER_LENGTH             (128*1024) // 128 KB

// section 3.1.2
#define AHCI_Global_HBA_CONTROL_HR          (1 << 0)
#define AHCI_Global_HBA_CONTROL_IE          (1 << 1)
#define AHCI_Global_HBA_CONTROL_MRSM        (1 << 2)
#define AHCI_Global_HBA_CONTROL_AE          (1 << 31)
#define AHCI_Global_HBA_CAP_S64A            (1 << 31)

// ATA Functions
#define ATA_FUNCTION_ATA_COMMAND            0x100
#define ATA_FUNCTION_ATA_IDENTIFY           0x101

// ATAPI Functions
#define ATA_FUNCTION_ATAPI_COMMAND          0x200

// ATA Flags
#define ATA_FLAGS_DATA_IN                   (1 << 1)
#define ATA_FLAGS_DATA_OUT                  (1 << 2)

#define IsAtaCommand(AtaFunction)           (AtaFunction & ATA_FUNCTION_ATA_COMMAND)
#define IsAtapiCommand(AtaFunction)         (AtaFunction & ATA_FUNCTION_ATAPI_COMMAND)
#define IsDataTransferNeeded(SrbExtension)  (SrbExtension->Flags & (ATA_FLAGS_DATA_IN | ATA_FLAGS_DATA_OUT))
#define IsAdapterCAPS64(CAP)                (CAP & AHCI_Global_HBA_CAP_S64A)

// 3.1.1 NCS = CAP[12:08] -> Align
#define AHCI_Global_Port_CAP_NCS(x)            (((x) & 0xF00) >> 8)

#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))
#if DEBUG
    #define DebugPrint(format, ...) StorPortDebugPrint(0, format, __VA_ARGS__)
#endif

//////////////////////////////////////////////////////////////
//              ---- Support Structures ---                 //
//////////////////////////////////////////////////////////////

// section 3.3.5
typedef union _AHCI_INTERRUPT_STATUS
{
    struct
    {
        ULONG DHRS:1;       //Device to Host Register FIS Interrupt
        ULONG PSS :1;       //PIO Setup FIS Interrupt
        ULONG DSS :1;       //DMA Setup FIS Interrupt
        ULONG SDBS :1;      //Set Device Bits Interrupt
        ULONG UFS :1;       //Unknown FIS Interrupt
        ULONG DPS :1;       //Descriptor Processed
        ULONG PCS :1;       //Port Connect Change Status
        ULONG DMPS :1;      //Device Mechanical Presence Status (DMPS)
        ULONG Reserved :14;
        ULONG PRCS :1;      //PhyRdy Change Status
        ULONG IPMS :1;      //Incorrect Port Multiplier Status
        ULONG OFS :1;       //Overflow Status
        ULONG Reserved2 :1;
        ULONG INFS :1;      //Interface Non-fatal Error Status
        ULONG IFS :1;       //Interface Fatal Error Status
        ULONG HBDS :1;      //Host Bus Data Error Status
        ULONG HBFS :1;      //Host Bus Fatal Error Status
        ULONG TFES :1;      //Task File Error Status
        ULONG CPDS :1;      //Cold Port Detect Status
    };

    ULONG Status;
} AHCI_INTERRUPT_STATUS;

typedef struct _AHCI_FIS_DMA_SETUP
{
    ULONG ULONG0_1;         // FIS_TYPE_DMA_SETUP
                            // Port multiplier
                            // Reserved
                            // Data transfer direction, 1 - device to host
                            // Interrupt bit
                            // Auto-activate. Specifies if DMA Activate FIS is needed
    UCHAR Reserved[2];      // Reserved
    ULONG DmaBufferLow;     // DMA Buffer Identifier. Used to Identify DMA buffer in host memory. SATA Spec says host specific and not in Spec. Trying AHCI spec might work.
    ULONG DmaBufferHigh;
    ULONG Reserved2;        // More reserved
    ULONG DmaBufferOffset;  // Byte offset into buffer. First 2 bits must be 0
    ULONG TranferCount;     // Number of bytes to transfer. Bit 0 must be 0
    ULONG Reserved3;        // Reserved
} AHCI_FIS_DMA_SETUP;

typedef struct _AHCI_PIO_SETUP_FIS
{
    UCHAR FisType;
    UCHAR Reserved1 :5;
    UCHAR D :1;
    UCHAR I :1;
    UCHAR Reserved2 :1;
    UCHAR Status;
    UCHAR Error;

    UCHAR SectorNumber;
    UCHAR CylLow;
    UCHAR CylHigh;
    UCHAR Dev_Head;

    UCHAR SectorNumb_Exp;
    UCHAR CylLow_Exp;
    UCHAR CylHigh_Exp;
    UCHAR Reserved3;

    UCHAR SectorCount;
    UCHAR SectorCount_Exp;
    UCHAR Reserved4;
    UCHAR E_Status;

    USHORT TransferCount;
    UCHAR Reserved5[2];
} AHCI_PIO_SETUP_FIS;

typedef struct _AHCI_D2H_REGISTER_FIS
{
    UCHAR FisType;
    UCHAR Reserved1 :6;
    UCHAR I:1;
    UCHAR Reserved2 :1;
    UCHAR Status;
    UCHAR Error;

    UCHAR SectorNumber;
    UCHAR CylLow;
    UCHAR CylHigh;
    UCHAR Dev_Head;

    UCHAR SectorNum_Exp;
    UCHAR CylLow_Exp;
    UCHAR CylHigh_Exp;
    UCHAR Reserved;

    UCHAR SectorCount;
    UCHAR SectorCount_Exp;
    UCHAR Reserved3[2];

    UCHAR Reserved4[4];
} AHCI_D2H_REGISTER_FIS;

typedef struct _AHCI_SET_DEVICE_BITS_FIS
{
    UCHAR FisType;

    UCHAR PMPort: 4;
    UCHAR Reserved1 :2;
    UCHAR I :1;
    UCHAR N :1;

    UCHAR Status_Lo :3;
    UCHAR Reserved2 :1;
    UCHAR Status_Hi :3;
    UCHAR Reserved3 :1;

    UCHAR Error;

    UCHAR Reserved5[4];
} AHCI_SET_DEVICE_BITS_FIS;

typedef struct _AHCI_QUEUE
{
    PVOID Buffer[MAXIMUM_QUEUE_BUFFER_SIZE];  // because Storahci hold Srb queue of 255 size
    ULONG Head;
    ULONG Tail;
} AHCI_QUEUE, *PAHCI_QUEUE;

//////////////////////////////////////////////////////////////
//              ---------------------------                 //
//////////////////////////////////////////////////////////////

typedef union _AHCI_COMMAND_HEADER_DESCRIPTION
{
    struct
    {
        ULONG CFL :5;       // Command FIS Length
        ULONG A :1;         // IsATAPI
        ULONG W :1;         // Write
        ULONG P :1;         // Prefetchable

        ULONG R :1;         // Reset
        ULONG B :1;         // BIST
        ULONG C :1;         //Clear Busy upon R_OK
        ULONG DW0_Reserved :1;
        ULONG PMP :4;       //Port Multiplier Port

        ULONG PRDTL :16;    //Physical Region Descriptor Table Length
    };

    ULONG Status;
} AHCI_COMMAND_HEADER_DESCRIPTION;

// 4.2.2 Command Header
typedef struct _AHCI_COMMAND_HEADER
{
    AHCI_COMMAND_HEADER_DESCRIPTION DI;   // DW 0
    ULONG PRDBC;                // DW 1
    ULONG CTBA0;                // DW 2
    ULONG CTBA_U0;              // DW 3
    ULONG Reserved[4];          // DW 4-7
} AHCI_COMMAND_HEADER, *PAHCI_COMMAND_HEADER;

// Received FIS
typedef struct _AHCI_RECEIVED_FIS
{
    struct _AHCI_FIS_DMA_SETUP          DmaSetupFIS;      // 0x00 -- DMA Setup FIS
    ULONG                               pad0;             // 4 BYTE padding
    struct _AHCI_PIO_SETUP_FIS          PioSetupFIS;      // 0x20 -- PIO Setup FIS
    ULONG                               pad1[3];          // 12 BYTE padding
    struct _AHCI_D2H_REGISTER_FIS       RegisterFIS;      // 0x40 -- Register – Device to Host FIS
    ULONG                               pad2;             // 4 BYTE padding
    struct _AHCI_SET_DEVICE_BITS_FIS    SetDeviceFIS;     // 0x58 -- Set Device Bit FIS
    ULONG                               UnknowFIS[16];    // 0x60 -- Unknown FIS
    ULONG                               Reserved[24];     // 0xA0 -- Reserved
} AHCI_RECEIVED_FIS, *PAHCI_RECEIVED_FIS;

// Holds Port Information
typedef struct _AHCI_PORT
{
    ULONG   CLB;                                // 0x00, command list base address, 1K-byte aligned
    ULONG   CLBU;                               // 0x04, command list base address upper 32 bits
    ULONG   FB;                                 // 0x08, FIS base address, 256-byte aligned
    ULONG   FBU;                                // 0x0C, FIS base address upper 32 bits
    ULONG   IS;                                 // 0x10, interrupt status
    ULONG   IE;                                 // 0x14, interrupt enable
    ULONG   CMD;                                // 0x18, command and status
    ULONG   RSV0;                               // 0x1C, Reserved
    ULONG   TFD;                                // 0x20, task file data
    ULONG   SIG;                                // 0x24, signature
    ULONG   SSTS;                               // 0x28, SATA status (SCR0:SStatus)
    ULONG   SCTL;                               // 0x2C, SATA control (SCR2:SControl)
    ULONG   SERR;                               // 0x30, SATA error (SCR1:SError)
    ULONG   SACT;                               // 0x34, SATA active (SCR3:SActive)
    ULONG   CI;                                 // 0x38, command issue
    ULONG   SNTF;                               // 0x3C, SATA notification (SCR4:SNotification)
    ULONG   FBS;                                // 0x40, FIS-based switch control
    ULONG   RSV1[11];                           // 0x44 ~ 0x6F, Reserved
    ULONG   Vendor[4];                          // 0x70 ~ 0x7F, vendor specific
} AHCI_PORT, *PAHCI_PORT;

typedef struct _AHCI_MEMORY_REGISTERS
{
    // 0x00 - 0x2B, Generic Host Control
    ULONG CAP;                                  // 0x00, Host capability
    ULONG GHC;                                  // 0x04, Global host control
    ULONG IS;                                   // 0x08, Interrupt status
    ULONG PI;                                   // 0x0C, Port implemented
    ULONG VS;                                   // 0x10, Version
    ULONG CCC_CTL;                              // 0x14, Command completion coalescing control
    ULONG CCC_PTS;                              // 0x18, Command completion coalescing ports
    ULONG EM_LOC;                               // 0x1C, Enclosure management location
    ULONG EM_CTL;                               // 0x20, Enclosure management control
    ULONG CAP2;                                 // 0x24, Host capabilities extended
    ULONG BOHC;                                 // 0x28, BIOS/OS handoff control and status
    ULONG Reserved[0xA0-0x2C];                  // 0x2C - 0x9F, Reserved
    ULONG VendorSpecific[0x100-0xA0];           // 0xA0 - 0xFF, Vendor specific registers
    AHCI_PORT PortList[MAXIMUM_AHCI_PORT_COUNT];

} AHCI_MEMORY_REGISTERS, *PAHCI_MEMORY_REGISTERS;

// Holds information for each attached attached port to a given adapter.
typedef struct _AHCI_PORT_EXTENSION
{
    ULONG PortNumber;
    ULONG OccupiedSlots;                                // slots to which we have already assigned task
    BOOLEAN IsActive;
    PAHCI_PORT Port;                                    // AHCI Port Infomation
    AHCI_QUEUE SrbQueue;
    PAHCI_RECEIVED_FIS ReceivedFIS;
    PAHCI_COMMAND_HEADER CommandList;
    STOR_DEVICE_POWER_STATE DevicePowerState;           // Device Power State
    struct _AHCI_ADAPTER_EXTENSION* AdapterExtension;   // Port's Adapter Information
} AHCI_PORT_EXTENSION, *PAHCI_PORT_EXTENSION;

// Holds Adapter Information
typedef struct _AHCI_ADAPTER_EXTENSION
{
    ULONG   SystemIoBusNumber;
    ULONG   SlotNumber;
    ULONG   AhciBaseAddress;
    PULONG  IS;// Interrupt Status, In case of MSIM == `1`
    ULONG   PortImplemented;// bit-mapping of ports which are implemented
    ULONG   PortCount;

    USHORT  VendorID;
    USHORT  DeviceID;
    USHORT  RevisionID;

    ULONG   Version;
    ULONG   CAP;
    ULONG   CAP2;
    ULONG   LastInterruptPort;
    ULONG   CurrentCommandSlot;

    PVOID NonCachedExtension;// holds virtual address to noncached buffer allocated for Port Extension

    struct
    {
        // Message per port or shared port?
        ULONG MessagePerPort : 1;
        ULONG Removed : 1;
        ULONG Reserved : 30; // not in use -- maintain 4 byte alignment
    } StateFlags;

    PAHCI_MEMORY_REGISTERS ABAR_Address;
    AHCI_PORT_EXTENSION PortExtension[MAXIMUM_AHCI_PORT_COUNT];
} AHCI_ADAPTER_EXTENSION, *PAHCI_ADAPTER_EXTENSION;

typedef struct _ATA_REGISTER
{
    UCHAR CommandReg;
    ULONG Reserved;
} ATA_REGISTER;

typedef struct _AHCI_SRB_EXTENSION
{
    ULONG AtaFunction;
    ULONG Flags;
    ATA_REGISTER Task;
    ULONG SlotIndex;
    ULONG Reserved[4];
} AHCI_SRB_EXTENSION, *PAHCI_SRB_EXTENSION;

//////////////////////////////////////////////////////////////
//                       Declarations                       //
//////////////////////////////////////////////////////////////

BOOLEAN
AhciAdapterReset (
    __in PAHCI_ADAPTER_EXTENSION AdapterExtension
    );

__inline
VOID
AhciZeroMemory (
    __out PCHAR Buffer,
    __in ULONG BufferSize
    );

__inline
BOOLEAN
IsPortValid (
    __in PAHCI_ADAPTER_EXTENSION AdapterExtension,
    __in UCHAR pathId
    );

ULONG
DeviceInquiryRequest (
    __in PAHCI_ADAPTER_EXTENSION AdapterExtension,
    __in PSCSI_REQUEST_BLOCK Srb,
    __in PCDB Cdb
    );

__inline
BOOLEAN
AddQueue (
    __inout PAHCI_QUEUE Queue,
    __in PVOID Srb
    );

__inline
PVOID
RemoveQueue (
    __inout PAHCI_QUEUE Queue
    );