// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include "pagetest.h"
#include "pages/printers/printers.h"

TEST(PrintersPage, Opens)
{
    PageFixture page("printers");
    EXPECT_NE(page.dump().find("\"page:printers\""), std::string::npos);
}

TEST(PrintersPage, MakesTheUriOfAnAddress)
{
    using settings::printers::uriForAddress;
    EXPECT_EQ(uriForAddress("192.168.1.20"), "ipp://192.168.1.20/ipp/print");
    EXPECT_EQ(uriForAddress(" printer.local "), "ipp://printer.local/ipp/print");
    EXPECT_EQ(uriForAddress("ipps://10.0.0.5:631/ipp/print"), "ipps://10.0.0.5:631/ipp/print");
    EXPECT_EQ(uriForAddress("  "), "");
}

TEST(PrintersPage, NamesASecondQueueOfThePrinterApart)
{
    using settings::printers::uniqueName;
    EXPECT_EQ(uniqueName("HP_LaserJet", {}), "HP_LaserJet");
    EXPECT_EQ(uniqueName("HP_LaserJet", {"HP_LaserJet"}), "HP_LaserJet_2");
    EXPECT_EQ(uniqueName("HP_LaserJet", {"HP_LaserJet", "HP_LaserJet_2", "Other"}), "HP_LaserJet_3");
}

TEST(PrintersPage, LeavesOutAUsbPrinterThatIppUsbOffers)
{
    using settings::printers::Printer;
    using settings::printers::throughIppUsb;
    const Printer usb {.name = "Canon G1030 series", .info = "Canon G1030 series", .model = "Canon G1030 series",
                       .uri = "usb://Canon/G1030%20series?serial=123"};
    // As in issue #90: the same printer, through ipp-usb.
    EXPECT_TRUE(throughIppUsb(usb, {{.name = "Canon_G1030_series_USB", .info = "Canon G1030 series (USB)",
                                     .model = "Canon G1030 series",
                                     .uri = "ipps://Canon%20G1030%20series%20(USB)._ipps._tcp.local/"}}));
    // Its queue, once that one is added.
    EXPECT_TRUE(throughIppUsb(usb, {{.name = "Canon_G1030_series_USB", .info = "Canon G1030 series (USB)",
                                     .uri = "ipps://Canon%20G1030%20series%20%28USB%29._ipps._tcp.local/"}}));
    // Another printer, or the same model on the network.
    EXPECT_FALSE(throughIppUsb(usb, {{.info = "HP LaserJet (USB)", .model = "HP LaserJet"}}));
    EXPECT_FALSE(throughIppUsb(usb, {{.info = "Canon G1030 series", .model = "Canon G1030 series",
                                      .uri = "ipp://192.168.1.9/ipp/print"}}));
}

TEST(PrintersPage, MakesQueueNames)
{
    using settings::printers::queueName;
    EXPECT_EQ(queueName("HP LaserJet M110w (USB)"), "HP_LaserJet_M110w_USB");
    EXPECT_EQ(queueName("192.168.1.20"), "192_168_1_20");
    EXPECT_EQ(queueName("printer-2_b"), "printer-2_b");
    EXPECT_EQ(queueName("///"), "Printer");
}

TEST(PrintersPage, ReadsDeviceIdFields)
{
    using settings::printers::deviceIdField;
    const std::string id = "MFG:EPSON;CMD:ESCPL2,BDC;MDL:LQ-2190;CLS:PRINTER;";
    EXPECT_EQ(deviceIdField(id, {"MFG", "MANUFACTURER"}), "EPSON");
    EXPECT_EQ(deviceIdField("MANUFACTURER:Hewlett-Packard;MODEL:LaserJet", {"MFG", "MANUFACTURER"}), "Hewlett-Packard");
    EXPECT_EQ(deviceIdField(id, {"MDL"}), "LQ-2190");
    EXPECT_EQ(deviceIdField(id, {"SN"}), "");
    EXPECT_EQ(deviceIdField("", {"MFG"}), "");
}
