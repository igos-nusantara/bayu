// SPDX-License-Identifier: BSD-3-Clause

#include "pages/printers/printers.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <set>
#include <string_view>

#include <cups/adminutil.h>
#include <cups/cups.h>

#include "base/i18n.h"

namespace settings::printers {

namespace {

Printer fromDest(const cups_dest_t& dest)
{
    const auto option = [&dest](const char* name) {
        const char* value = cupsGetOption(name, dest.num_options, dest.options);
        return value ? std::string(value) : std::string();
    };
    Printer printer;
    printer.name = dest.name;
    printer.info = option("printer-info");
    if (printer.info.empty())
        printer.info = printer.name;
    printer.model = option("printer-make-and-model");
    printer.uri = option("device-uri");
    if (const std::string state = option("printer-state"); !state.empty())
        printer.state = std::atoi(state.c_str());
    return printer;
}

// The queues set up, from cupsd itself: cupsGetDests2() lists a printer found
// on the network or through ipp-usb under the name of its queue, in place of
// the queue, which would look like a printer not set up while it is plugged in.
std::vector<Printer> queues()
{
    static const char* const attributes[] = {"printer-name", "printer-info", "printer-make-and-model", "device-uri",
                                             "printer-state"};
    ipp_t* request = ippNewRequest(IPP_OP_CUPS_GET_PRINTERS);
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", int(std::size(attributes)),
                  nullptr, attributes);
    ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/");

    std::vector<Printer> printers;
    Printer printer;
    for (ipp_attribute_t* attribute = ippFirstAttribute(response);; attribute = ippNextAttribute(response)) {
        // A nameless attribute separates two printers.
        if (!attribute || !ippGetName(attribute)) {
            if (!printer.name.empty())
                printers.push_back(std::move(printer));
            printer = {};
            if (!attribute)
                break;
            continue;
        }
        if (ippGetGroupTag(attribute) != IPP_TAG_PRINTER)
            continue;
        const std::string name = ippGetName(attribute);
        const char* text = ippGetString(attribute, 0, nullptr);
        if (name == "printer-name" && text)
            printer.name = text;
        else if (name == "printer-info" && text)
            printer.info = text;
        else if (name == "printer-make-and-model" && text)
            printer.model = text;
        else if (name == "device-uri" && text)
            printer.uri = text;
        else if (name == "printer-state")
            printer.state = ippGetInteger(attribute, 0);
    }
    ippDelete(response);

    for (Printer& queue : printers)
        if (queue.info.empty())
            queue.info = queue.name;
    return printers;
}

// The USB printers CUPS' usb backend sees, those that print through ipp-usb
// aside: CUPS finds those on its own, as network printers.
std::vector<Printer> usbDevices()
{
    std::vector<Printer> devices;
    const auto found = [](const char*, const char* id, const char* info, const char* makeAndModel, const char* uri,
                          const char*, void* data) {
        if (!uri || !std::string_view(uri).starts_with("usb://"))
            return;
        Printer printer;
        printer.uri = uri;
        printer.deviceId = id ? id : "";
        printer.model = makeAndModel && std::string_view(makeAndModel) != "Unknown" ? makeAndModel : "";
        printer.info = printer.model.empty() ? (info ? info : uri) : printer.model;
        printer.name = printer.info;
        static_cast<std::vector<Printer>*>(data)->push_back(std::move(printer));
    };
    cupsGetDevices(CUPS_HTTP_DEFAULT, 5, "usb", CUPS_EXCLUDE_NONE, found, &devices);
    return devices;
}

// The serial number in a USB printer's URI, as both usb:// and HPLIP's
// hp:/usb/ have it; empty without one.
std::string serial(const std::string& uri)
{
    const size_t at = uri.find("serial=");
    if (at == std::string::npos || !(uri.starts_with("usb:") || uri.starts_with("hp:/usb")))
        return {};
    return uri.substr(at + 7, uri.find('&', at) - at - 7);
}

// The driver that matches the device ID best, as cups-driverd ranks them.
std::string driverFor(const std::string& deviceId)
{
    if (deviceId.empty())
        return {};
    ipp_t* request = ippNewRequest(IPP_OP_CUPS_GET_PPDS);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_TEXT, "ppd-device-id", nullptr, deviceId.c_str());
    ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "limit", 1);
    ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes", nullptr, "ppd-name");
    ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/");
    ipp_attribute_t* name = ippFindAttribute(response, "ppd-name", IPP_TAG_NAME);
    std::string driver = name ? ippGetString(name, 0, nullptr) : "";
    ippDelete(response);
    return driver;
}

struct Search
{
    std::set<std::string> known;
    std::vector<Printer>* found;
};

int foundDest(void* data, unsigned flags, cups_dest_t* dest)
{
    auto* search = static_cast<Search*>(data);
    if (flags & CUPS_DEST_FLAGS_REMOVED || dest->instance)
        return 1;
    // CUPS lists the driverless printers it finds too, to print on without
    // setting them up; they are offered to be set up.
    Printer printer = fromDest(*dest);
    // A printer set up already, under its own name or its address.
    if (printer.uri.empty() || search->known.contains(printer.name) || search->known.contains(printer.uri))
        return 1;
    search->known.insert(printer.name);
    search->found->push_back(std::move(printer));
    return 1;
}

std::string quoted(const std::string& argument)
{
    std::string result = "'";
    for (char c : argument)
        result += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return result + "'";
}

} // namespace

Printers read(int searchMs)
{
    Printers printers;
    Search search {.found = &printers.found};

    // The users default, or else the systems.
    cups_dest_t* fallback = cupsGetNamedDest(CUPS_HTTP_DEFAULT, nullptr, nullptr);
    const std::string defaultName = fallback ? fallback->name : "";
    cupsFreeDests(fallback ? 1 : 0, fallback);

    for (Printer& printer : queues()) {
        printer.isDefault = printer.name == defaultName;
        cups_job_t* jobs = nullptr;
        printer.jobs = std::max(0, cupsGetJobs2(CUPS_HTTP_DEFAULT, &jobs, printer.name.c_str(), 0,
                                                CUPS_WHICHJOBS_ACTIVE));
        cupsFreeJobs(printer.jobs, jobs);
        search.known.insert(printer.name);
        search.known.insert(printer.uri);
        printers.queues.push_back(std::move(printer));
    }

    if (searchMs > 0) {
        cupsEnumDests(CUPS_DEST_FLAGS_NONE, searchMs, nullptr, 0, 0, foundDest, &search);
        // USB printers too: one removed is offered again, as udev set it up
        // only when it was plugged in.
        std::set<std::string> serials;
        for (const Printer& queue : printers.queues)
            if (const std::string number = serial(queue.uri); !number.empty())
                serials.insert(number);
        for (Printer& device : usbDevices()) {
            const std::string number = serial(device.uri);
            if (search.known.contains(device.uri) || (!number.empty() && serials.contains(number))
                || throughIppUsb(device, printers.found) || throughIppUsb(device, printers.queues))
                continue;
            search.known.insert(device.uri);
            printers.found.push_back(std::move(device));
        }
    }
    std::ranges::sort(printers.found, {}, &Printer::info);
    return printers;
}

std::string uriForAddress(const std::string& address)
{
    std::string text = address;
    std::erase_if(text, [](unsigned char c) { return std::isspace(c); });
    if (text.empty() || text.find("://") != std::string::npos)
        return text;
    return "ipp://" + text + "/ipp/print";
}

std::string queueName(const std::string& text)
{
    std::string name;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_')
            name += char(c);
        else if (!name.empty() && name.back() != '_')
            name += '_';
    }
    while (!name.empty() && name.back() == '_')
        name.pop_back();
    return name.empty() ? std::string("Printer") : name.substr(0, 127);
}

bool throughIppUsb(const Printer& usb, const std::vector<Printer>& printers)
{
    const auto lower = [](std::string text) {
        for (char& c : text)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    const std::string model = lower(usb.model.empty() ? usb.info : usb.model);
    if (model.empty())
        return false;
    constexpr std::string_view suffix = " (usb)";
    for (const Printer& printer : printers) {
        const std::string info = lower(printer.info);
        const std::string uri = lower(printer.uri);
        if (!info.ends_with(suffix) && uri.find("(usb)") == std::string::npos
            && uri.find("%28usb%29") == std::string::npos)
            continue;
        if (lower(printer.model) == model || info == model + std::string(suffix))
            return true;
    }
    return false;
}

std::string uniqueName(const std::string& name, const std::set<std::string>& taken)
{
    std::string unique = name;
    for (int n = 2; taken.contains(unique); ++n)
        unique = name + "_" + std::to_string(n);
    return unique;
}

std::string matchingDriver(const Printer& printer)
{
    return printer.uri.starts_with("usb://") ? driverFor(printer.deviceId) : std::string("everywhere");
}

std::string deviceIdField(const std::string& deviceId, const std::vector<std::string>& keys)
{
    for (const std::string& key : keys) {
        for (size_t at = 0; at < deviceId.size();) {
            const size_t end = std::min(deviceId.find(';', at), deviceId.size());
            const std::string_view field(deviceId.data() + at, end - at);
            if (const size_t colon = field.find(':');
                colon != std::string_view::npos && field.substr(0, colon) == key)
                return std::string(field.substr(colon + 1));
            at = end + 1;
        }
    }
    return {};
}

std::vector<Driver> drivers(const Printer& printer)
{
    const auto list = [](const std::string& make) {
        std::vector<Driver> found;
        ipp_t* request = ippNewRequest(IPP_OP_CUPS_GET_PPDS);
        if (!make.empty())
            ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_TEXT, "ppd-make", nullptr, make.c_str());
        static const char* const attributes[] = {"ppd-name", "ppd-make-and-model"};
        ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
                      int(std::size(attributes)), nullptr, attributes);
        ipp_t* response = cupsDoRequest(CUPS_HTTP_DEFAULT, request, "/");
        Driver driver;
        for (ipp_attribute_t* attribute = ippFirstAttribute(response);; attribute = ippNextAttribute(response)) {
            if (!attribute || !ippGetName(attribute)) {
                // Driverless ones print only on printers that need no driver.
                if (!driver.name.empty() && driver.name != "everywhere" && !driver.name.starts_with("driverless")
                    && driver.name != "raw")
                    found.push_back(std::move(driver));
                driver = {};
                if (!attribute)
                    break;
                continue;
            }
            const std::string name = ippGetName(attribute);
            const char* text = ippGetString(attribute, 0, nullptr);
            if (name == "ppd-name" && text)
                driver.name = text;
            else if (name == "ppd-make-and-model" && text)
                driver.description = text;
        }
        ippDelete(response);
        return found;
    };
    std::vector<Driver> found = list(deviceIdField(printer.deviceId, {"MFG", "MANUFACTURER"}));
    if (found.empty())
        found = list({});
    std::ranges::sort(found, {}, &Driver::description);
    found.insert(found.begin(), {"raw", bayu::i18n::tr("Raw (the application sends the printer's own commands)")});
    return found;
}

std::optional<std::string> add(const Printer& printer, std::string driver)
{
    std::set<std::string> taken;
    for (const Printer& queue : queues())
        taken.insert(queue.name);
    if (driver.empty())
        driver = matchingDriver(printer);
    if (driver.empty())
        return std::string(bayu::i18n::tr("No driver for this printer was found."));
    // A second queue of the same printer is told apart by its number, in its
    // name and in what it is called.
    const std::string base = queueName(printer.name);
    const std::string name = uniqueName(base, taken);
    const std::string info = name == base ? printer.info : printer.info + " (" + name.substr(base.size() + 1) + ")";
    return run({"lpadmin", "-p", name, "-D", info, "-E", "-v", printer.uri, "-m", driver});
}

std::optional<std::string> run(const std::vector<std::string>& command)
{
    std::string line;
    for (const std::string& argument : command)
        line += quoted(argument) + ' ';
    // No terminal to ask a password on: lpadmin fails at once for a user who
    // may not manage printers.
    line += "</dev/null 2>&1";
    FILE* pipe = popen(line.c_str(), "r");
    if (!pipe)
        return std::string("Cannot run ") + command.front();
    std::string output;
    char buffer[512];
    while (const size_t size = fread(buffer, 1, sizeof buffer, pipe))
        output.append(buffer, size);
    if (pclose(pipe) == 0)
        return std::nullopt;
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back())))
        output.pop_back();
    return output.empty() ? command.front() + " failed" : output;
}

} // namespace settings::printers
