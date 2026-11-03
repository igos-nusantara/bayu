// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace settings::printers {

struct Printer
{
    // The CUPS queue's name, or for a printer found, the one CUPS gives it.
    std::string name;
    // What people call it, as it describes itself.
    std::string info;
    std::string model;
    std::string uri;
    // A USB printer's IEEE 1284 device ID, which picks its driver.
    std::string deviceId;
    // IPP's printer-state: 3 idle, 4 printing, 5 stopped.
    int state = 3;
    int jobs = 0;
    bool isDefault = false;
};

struct Printers
{
    // The printers set up.
    std::vector<Printer> queues;
    // Those on the network, and on USB, not set up yet.
    std::vector<Printer> found;
};

// Waits up to `searchMs` for the printers that answer on the network; off the
// UI thread.
Printers read(int searchMs);

// The URI of a printer typed in as a host name or an address (IPP Everywhere's
// usual path), or the URI itself when one is typed in.
std::string uriForAddress(const std::string& address);
// A CUPS queue name from a printer's name or address: letters, digits, "-"
// and "_".
std::string queueName(const std::string& text);
// Whether a USB printer is one of `printers` reached through ipp-usb, which
// names its printers "<model> (USB)": the same printer, printing without a
// driver, so the USB one is left out.
bool throughIppUsb(const Printer& usb, const std::vector<Printer>& printers);

// `name`, or with "_2", "_3"... after it while a queue has it.
std::string uniqueName(const std::string& name, const std::set<std::string>& taken);

struct Driver
{
    // What lpadmin -m takes.
    std::string name;
    std::string description;
};

// The driver a printer is set up with: none for one printing without a
// driver, on the network or through ipp-usb ("everywhere"); for a USB
// printer, the one its device ID matches best, or empty when none does.
// Blocks.
std::string matchingDriver(const Printer& printer);
// The drivers to choose from for a printer none matches: those of its maker,
// or all when CUPS has none of its maker, "raw" first. Blocks.
std::vector<Driver> drivers(const Printer& printer);
// A field of an IEEE 1284 device ID, such as "MFG" or "MANUFACTURER".
std::string deviceIdField(const std::string& deviceId, const std::vector<std::string>& keys);

// Sets the printer up as a new queue with `driver`, or with its matching one
// when empty. Nothing when it worked, else why not. Blocks.
std::optional<std::string> add(const Printer& printer, std::string driver = {});

// Runs a CUPS command (lpadmin, lp, lpoptions, cancel): nothing when it
// succeeded, else what it said. Blocks, so off the UI thread.
std::optional<std::string> run(const std::vector<std::string>& command);

} // namespace settings::printers
