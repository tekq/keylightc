Name:           keylightc
Version:        0
Release:        1%{?dist}
Summary:        Keyboard backlight daemon for Framework laptops

License:        GPL-2.0-or-later
URL:            https://github.com/tekq/keylightc
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  glibc-devel
BuildRequires:  systemd-rpm-macros

Requires:       systemd
%{?systemd_requires}

%description
keylightc is a small system daemon for Framework laptops that listens to
keyboard and touchpad input, and turns on the keyboard backlight while
either is being used. It uses the sysfs keyboard backlight interface
introduced in Linux 6.11, so at least that kernel version is required.
It has no runtime dependencies beyond glibc and the Linux kernel.

%prep
%autosetup -n %{name}-%{version}

%build
%make_build CC="%{__cc}" CFLAGS="%{optflags}"

%install
%make_install DESTDIR=%{buildroot}

%post
%systemd_post keylightc.service

%preun
%systemd_preun keylightc.service

%postun
%systemd_postun_with_restart keylightc.service

%files
%license gpl-2.0.txt
%license gpl-3.0.txt
%doc README.md
%{_bindir}/keylightc
%{_unitdir}/keylightc.service

%changelog
* Sat Aug 01 2026 asmx2 <hello@asmx2.dev> - 0-1
- Initial packaging
