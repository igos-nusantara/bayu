# Epson's CUPS driver for its LQ-2190, LQ-2680K, LQ-690K and LQ-730K
# dot-matrix printers (issue #165), repackaged from Epson's own build: Epson
# no longer offers its source. The filter goes where EL10's CUPS looks for
# filters, not in /usr/lib64/cups, and each PPD gets the IEEE 1284 device ID
# of its printer, so Settings picks it when the printer is plugged in.
Name:           eplq-cups
Version:        1.1.0
Release:        1%{?dist}
Summary:        Epson LQ series dot-matrix printer driver for CUPS
License:        GPL-2.0-or-later
URL:            https://download.epson-europe.com/pub/download/3273/epson327368eu.zip
Source0:        epson327368eu.zip
ExclusiveArch:  x86_64
BuildRequires:  unzip cpio
Requires:       cups-filesystem
%global debug_package %{nil}

%description
Epson's CUPS raster filter and PPDs for the LQ-2190, LQ-2680K, LQ-690K and
LQ-730K 24-pin dot-matrix printers.

%prep
%setup -q -c -T
unzip -q %{SOURCE0}
rpm2cpio eplq-cups-%{version}/eplq-cups-%{version}-1.x86_64.rpm/eplq-cups-%{version}-1.x86_64.rpm | cpio -idm --quiet

%install
install -Dm0755 usr/lib64/cups/filter/rastertoeplq %{buildroot}%{_prefix}/lib/cups/filter/rastertoeplq
mkdir -p %{buildroot}%{_datadir}/cups/model/eplq-cups
for ppd in usr/share/cups/model/eplq*.ppd; do
  model=$(sed -n 's/^\*ModelName: "EPSON \(.*\)".*/\1/p' "$ppd" | tr -d '\r')
  sed "/^\*ModelName:/a *1284DeviceID: \"MFG:EPSON;CMD:ESCPL2;MDL:$model;CLS:PRINTER;\"" "$ppd" \
    | gzip -9n > %{buildroot}%{_datadir}/cups/model/eplq-cups/$(basename "$ppd").gz
done
install -Dm0644 usr/share/doc/packages/eplq-cups/COPYING %{buildroot}%{_licensedir}/%{name}/COPYING

%files
%license %{_licensedir}/%{name}/COPYING
%{_prefix}/lib/cups/filter/rastertoeplq
%{_datadir}/cups/model/eplq-cups/

%changelog
* Tue Nov 03 2026 Eka Tresna Irawan <anak10thn@gmail.com> - 1.1.0-1
- Epson eplq-cups 1.1.0 for EL10 (issue #165)
