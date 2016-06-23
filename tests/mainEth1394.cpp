/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

#include <iostream>
#include <stdio.h>

#include <Amp1394/AmpIORevision.h>
#if Amp1394_HAS_RAW1394
#include <termios.h>
#include "FirewirePort.h"
#endif
#include "Eth1394Port.h"
#include "AmpIO.h"

#ifdef _MSC_VER
#include <stdlib.h>   // for byteswap functions
inline uint16_t bswap_16(uint16_t data) { return _byteswap_ushort(data); }
inline uint32_t bswap_32(uint32_t data) { return _byteswap_ulong(data); }
#else
#include <byteswap.h>
#endif

const AmpIO_UInt32 VALID_BIT        = 0x80000000;  /*!< High bit of 32-bit word */
const AmpIO_UInt32 DAC_MASK         = 0x0000ffff;  /*!< Mask for 16-bit DAC values */

AmpIO_UInt32 KSZ8851CRC(const unsigned char *data, size_t len)
{
    AmpIO_UInt32 crc = 0xffffffff;
    for (size_t i = 0; i < len; i++) {
        for (size_t j = 0; j < 8; j++) {
            if (((crc >> 31) ^ (data[i] >> j)) & 0x01)
                crc = (crc << 1) ^ 0x04c11db7;
            else
                crc = crc << 1;
        }
    }
    return crc;    
}

// Compute parameters to initialize multicast hash table
void ComputeMulticastHash(unsigned char *MulticastMAC, AmpIO_UInt8 &regAddr, AmpIO_UInt16 &regData)
{
    AmpIO_UInt32 crc = KSZ8851CRC(MulticastMAC, 6);
    int regOffset = (crc >> 29) & 0x0006;  // first 2 bits of CRC (x2)
    int regBit = (crc >> 26) & 0x00F;      // next 4 bits of CRC
    regAddr = 0xA0 + regOffset;            // 0xA0 --> MAHTR0 (MAC Address Hash Table Register 0)
    regData = (1 << regBit);
}

#if Amp1394_HAS_RAW1394
// Ethernet debug
void PrintEthernetDebug(AmpIO &Board)
{
    AmpIO_UInt16 status = Board.ReadKSZ8851Status();
    if (!(status&0x8000)) {
        std::cout << "   No Ethernet controller, status = " << std::hex << status << std::endl;
        return;
    }
    std::cout << "Status: ";
    if (status&0x4000) std::cout << "error ";
    if (status&0x2000) std::cout << "initOK ";
    if (status&0x1000) std::cout << "initReq ";
    if (status&0x0800) std::cout << "ethIoErr ";
    if (status&0x0400) std::cout << "cmdReq ";
    if (status&0x0200) std::cout << "cmdAck ";
    if (status&0x0100) std::cout << "qRead ";
    if (status&0x0080) std::cout << "qWrite ";
    if (status&0x0040) std::cout << "bRead ";
    if (status&0x0020) std::cout << "bWrite ";
    //if (status&0x0020) std::cout << "PME ";
    //if (!(status&0x0010)) std::cout << "IRQ ";
    if ((status&0x0010)) std::cout << "multicast ";
    if (status&0x0008) std::cout << "KSZ-idle ";
    if (status&0x0004) std::cout << "ETH-idle ";
    int waitInfo = status&0x0003;
    if (waitInfo == 0) std::cout << "wait-none";
    else if (waitInfo == 1) std::cout << "wait-ack";
    else if (waitInfo == 2) std::cout << "wait-ack-clear";
    else std::cout << "wait-flush";
    std::cout << std::endl;
}

// Ethernet status, as reported by KSZ8851 on FPGA board
void PrintEthernetStatus(AmpIO &Board)
{
    AmpIO_UInt16 reg;
    Board.ReadKSZ8851Reg(0xF8, reg);
    std::cout << "Port 1 status:";
    if (reg & 0x0020) std::cout << " link-good";
    if (reg & 0x0040) std::cout << " an-done";
    if (reg & 0x0200) std::cout << " full-duplex";
    else std::cout << " half-duplex";
    if (reg & 0x0400) std::cout << " 100Mbps";
    else std::cout << " 10Mbps";
    if (reg & 0x2000) std::cout << " polarity-reversed";
    std::cout << std::endl;
}

// Check contents of KSZ8851 register
bool CheckRegister(AmpIO &Board, AmpIO_UInt8 regNum, AmpIO_UInt16 mask, AmpIO_UInt16 value)
{
    AmpIO_UInt16 reg;
    Board.ReadKSZ8851Reg(regNum, reg);
    if ((reg&mask) != value) {
        std::cout << "Register " << std::hex << (int)regNum << ": read = " << reg
                  << ", expected = " << value << " (mask = " << mask << ")" << std::endl;
        return false;
    }
    return true;
}

// Check whether Ethernet initialized correctly
bool CheckEthernet(AmpIO &Board)
{
    bool ret = true;
    ret &= CheckRegister(Board, 0x10, 0xffff, 0x9400);  // MAC address low = 0x94nn (nn = board id)
    ret &= CheckRegister(Board, 0x12, 0xffff, 0x0E13);  // MAC address middle = 0xOE13
    ret &= CheckRegister(Board, 0x14, 0xffff, 0xFA61);  // MAC address high = 0xFA61
    ret &= CheckRegister(Board, 0x84, 0x4000, 0x4000);  // Enable QMU transmit frame data pointer auto increment
    ret &= CheckRegister(Board, 0x70, 0x01fe, 0x01EE);  // Enable QMU transmit flow control, CRC, and padding
    ret &= CheckRegister(Board, 0x86, 0x4000, 0x4000);  // Enable QMU receive frame data pointer auto increment
    ret &= CheckRegister(Board, 0x9C, 0x00ff, 0x0001);  // Configure receive frame threshold for 1 frame
    ret &= CheckRegister(Board, 0x74, 0xfffe, 0x7CE0);
    // Check multicast hash table
    unsigned char MulticastMAC[6];
    Eth1394Port::GetDestMulticastMacAddr(MulticastMAC);
    AmpIO_UInt8 HashReg;
    AmpIO_UInt16 HashValue;
    ComputeMulticastHash(MulticastMAC, HashReg, HashValue);
    ret &= CheckRegister(Board, HashReg, 0xffff, HashValue);
    ret &= CheckRegister(Board, 0x82, 0x03f7, 0x0020);  // Enable QMU frame count threshold (1), no auto-dequeue
    ret &= CheckRegister(Board, 0x90, 0xffff, 0x2000);  // Enable receive interrupts (TODO: also consider link change interrupt)
    ret &= CheckRegister(Board, 0x70, 0x0001, 0x0001);
    ret &= CheckRegister(Board, 0x74, 0x0001, 0x0001);
    return ret;
}

bool InitEthernet(AmpIO &Board)
{
    if (Board.GetFirmwareVersion() < 5) {
        std::cout << "   No Ethernet controller, firmware version = " << Board.GetFirmwareVersion() << std::endl;
        return false;
    }

    AmpIO_UInt16 status = Board.ReadKSZ8851Status();
    if (!(status&0x8000)) {
        std::cout << "   No Ethernet controller, status = " << std::hex << status << std::endl;
        return false;
    }
    std::cout << "   Ethernet controller status = " << std::hex << status << std::endl;
    PrintEthernetDebug(Board);

    // Reset the board
    Board.ResetKSZ8851();
    // Wait 100 msec
    usleep(100000L);

    // Read the status
    status = Board.ReadKSZ8851Status();
    std::cout << "   After reset, status = " << std::hex << status << std::endl;

    if (!(status&0x2000)) {
        std::cout << "   Ethernet failed initialization" << std::endl;
        PrintEthernetDebug(Board);
        return false;
    }

    // Read the Chip ID (16-bit read)
    AmpIO_UInt16 chipID = Board.ReadKSZ8851ChipID();
    std::cout << "   Chip ID = " << std::hex << chipID << std::endl;
    if ((chipID&0xfff0) != 0x8870)
        return false;


    // Check that KSZ8851 registers are as expected
    if (!CheckEthernet(Board)) {
        PrintEthernetDebug(Board);
        return false;
    }

    // Display the MAC address
    AmpIO_UInt16 regLow, regMid, regHigh;
    Board.ReadKSZ8851Reg(0x10, regLow);   // MAC address low = 0x94nn (nn = board id)
    Board.ReadKSZ8851Reg(0x12, regMid);   // MAC address middle = 0xOE13
    Board.ReadKSZ8851Reg(0x14, regHigh);  // MAC address high = 0xFA61
    std::cout << "   MAC address = " << std::hex << regHigh << ":" << regMid << ":" << regLow << std::endl;

    // Wait 2.5 sec
    usleep(2500000L);
    return true;
}

static char QuadletReadCallbackBoardId = 0;

bool QuadletReadCallback(Eth1394Port &, unsigned char boardId, std::ostream &debugStream)
{
    if ((QuadletReadCallbackBoardId != boardId) && (boardId != 0xff)) {
        debugStream << "Warning: QuadletReadCallback called for board " << (unsigned int) boardId
                    << ", expected board " << (unsigned int) QuadletReadCallbackBoardId << std::endl;
        return false;
    }
    return true;
}
#endif

int main()
{
    // Compute the hash table values used by the KSZ8851 chip to filter for multicast packets.
    // The results (RegAddr and RegData) are hard-coded in the FPGA code (EthernetIO.v).
    unsigned char MulticastMAC[6];
    Eth1394Port::GetDestMulticastMacAddr(MulticastMAC);
    AmpIO_UInt8 RegAddr;
    AmpIO_UInt16 RegData;
    ComputeMulticastHash(MulticastMAC, RegAddr, RegData);
    std::cout << "Multicast hash table: register " << std::hex << (int)RegAddr << ", data = " << RegData << std::endl;

    // Hard-coded for board #0
    AmpIO board1(0);
    AmpIO board2(0);

#if Amp1394_HAS_RAW1394
    FirewirePort FwPort(0, std::cout);
    if (!FwPort.IsOK()) {
        std::cout << "Failed to initialize firewire port" << std::endl;
        return 0;
    }
    FwPort.AddBoard(&board1);
    if (!InitEthernet(board1)) {
        std::cout << "Failed to initialize Ethernet chip" << std::endl;
        FwPort.RemoveBoard(0);
        return 0;
    }

    QuadletReadCallbackBoardId = board1.GetBoardId();
    Eth1394Port EthPort(0, std::cout, QuadletReadCallback);
    if (!EthPort.IsOK()) {
        std::cout << "Failed to initialize ethernet port" << std::endl;
        FwPort.RemoveBoard(0);
        return 0;
    }
#else
    Eth1394Port EthPort(0, std::cout);
    if (!EthPort.IsOK()) {
        std::cout << "Failed to initialize ethernet port" << std::endl;
        return 0;
    }
#endif
    EthPort.AddBoard(&board2);

    // For now, nothing more can be done without FireWire (RAW1394)
#if Amp1394_HAS_RAW1394
    // Turn off buffered I/O for keyboard
    struct termios oldTerm, newTerm;
    tcgetattr(0, &oldTerm);
    newTerm = oldTerm;
    newTerm.c_lflag &= ~ICANON;
    newTerm.c_lflag &= ECHO;
    tcsetattr(0, TCSANOW, &newTerm);

    bool done = false;
    while (!done) {

        std::cout << std::endl << "Ethernet Test Program" << std::endl;
        std::cout << "  0) Quit" << std::endl;
        std::cout << "  1) Quadlet write to board" << std::endl;
        std::cout << "  2) Quadlet read from board" << std::endl;
        std::cout << "  3) Block read from board" << std::endl;
        std::cout << "  4) Block write to board" << std::endl;
        std::cout << "  5) Ethernet port status" << std::endl;
        std::cout << "  6) Initialize Ethernet port" << std::endl;
        std::cout << "  7) Ethernet debug info" << std::endl;
        std::cout << "  8) Multicast quadlet read" << std::endl;
        std::cout << "Select option: ";
        
        int c = getchar();
        std::cout << std::endl << std::endl;

        nodeid_t boardid;
        nodeaddr_t addr;
        unsigned int tcode;
        quadlet_t quad_data;
        AmpIO_UInt16 srcAddress[3];
        quadlet_t read_data, write_data;
        quadlet_t fw_block_data[16];
        quadlet_t eth_block_data[16];
        quadlet_t write_block[4] = { 0x11111111, 0x22222222, 0x33333333, 0x44444444 };
        int i;
        char buf[5];

        switch (c) {
        case '0':   // Quit
                done = true;
                break;

        case '1':   // Write quadlet from PC to FPGA
                write_data = 0x0;
                if (!EthPort.WriteQuadlet(0, 0, write_data))
                    std::cout << "Failed to write quadlet via Ethernet port" << std::endl;
                break;

        case '2':   // Read request from PC via Ethernet (note that QuadletReadCallback is called)
                read_data = 0;
                addr = 4;  // Return QLA1
                if (EthPort.ReadQuadlet(0, addr, read_data))
                    std::cout << "Read quadlet data: " << std::hex << bswap_32(read_data) << std::endl;
                else
                    std::cout << "Failed to read quadlet via Ethernet port" << std::endl;
                memcpy(buf, (char *)(&read_data), 4);
                buf[4] = 0;
                std::cout << "  as string: " << buf << std::endl;
                break;

        case '3':
                if (!FwPort.ReadBlock(0, 0, fw_block_data, sizeof(fw_block_data))) {
                    std::cout << "Failed to read block data via FireWire port" << std::endl;
                    break;
                }
                if (!EthPort.ReadBlock(0, 0, eth_block_data, sizeof(eth_block_data))) {
                    std::cout << "Failed to read block data via Ethernet port" << std::endl;
                    break;
                }
                for (i = 0; i < sizeof(fw_block_data)/sizeof(quadlet_t); i++)
                    std::cout << std::dec << i << ": " << std::hex << bswap_32(fw_block_data[i])
                              << ", " << bswap_32(eth_block_data[i]) << std::endl;
                break;

        case '4':
                // Read from DAC (quadlet reads), modify values, write them using
                // a block write, then read them again to check.
                // Note that test can be done using FireWire by changing EthPort to FwPort.
                for (i = 0; i < 4; i++) {
                    addr = 0x0001 | ((i+1) << 4);  // channel 1-4, DAC Control
                    EthPort.ReadQuadlet(0, addr, write_block[i]);
                }
                std::cout << "Read from DAC: " << std::hex << bswap_32(write_block[0]) << ", "
                          << bswap_32(write_block[1]) << ", " << bswap_32(write_block[2]) << ", "
                          << bswap_32(write_block[3]) << std::endl;
                for (i = 0; i < 4; i++) {
                    write_block[i] = bswap_32(VALID_BIT | ((bswap_32(write_block[i])+(i+1)*0x100) & DAC_MASK));
                }
#if 1
                if (!EthPort.WriteBlock(0, 0, write_block, sizeof(write_block))) {
                    std::cout << "Failed to write block data via Ethernet port" << std::endl;
                    break;
                }
                PrintEthernetDebug(board1);
#else
                //For testing (first write fails)
                //addr = 0x0001 | ((1) << 4);  // channel 1-4, DAC Control
                //EthPort.WriteQuadlet(0, addr, write_block[0]);
                for (i = 0; i < 4; i++) {
                    addr = 0x0001 | ((i+1) << 4);  // channel 1-4, DAC Control
                    EthPort.WriteQuadlet(0, addr, write_block[i]);
                }
#endif
                for (i = 0; i < 4; i++) {
                    addr = 0x0001 | ((i+1) << 4);  // channel 1-4, DAC Control
                    EthPort.ReadQuadlet(0, addr, write_block[i]);
                }
                std::cout << "Read from DAC: " << std::hex << bswap_32(write_block[0]) << ", "
                          << bswap_32(write_block[1]) << ", " << bswap_32(write_block[2]) << ", "
                          << bswap_32(write_block[3]) << std::endl;
                break;

        case '5':
                PrintEthernetStatus(board1);
                break;

        case '6':
                InitEthernet(board1);
                break;

        case '7':
                PrintEthernetDebug(board1);
                break;

        case '8':   // Read request via Ethernet multicast
                read_data = 0;
                addr = 0;  // Return status register
                if (EthPort.ReadQuadlet(0xff, addr, read_data))
                    std::cout << "Read quadlet data: " << std::hex << bswap_32(read_data) << std::endl;
                else
                    std::cout << "Failed to read quadlet via Ethernet port" << std::endl;
                break;

        }
    }

    tcsetattr(0, TCSANOW, &oldTerm);  // Restore terminal I/O settings
    FwPort.RemoveBoard(0);
#endif
    EthPort.RemoveBoard(0);
    return 0;
}
