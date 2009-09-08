%define version 0.8.17

%define libpath /usr/lib
%ifarch x86_64
  %define libpath /usr/lib64
%endif

Summary: authd
Name: openpanel-authd
Version: %version
Release: 1
License: GPLv2
Group: Development
Source: http://packages.openpanel.com/archive/openpanel-authd-%{version}.tar.gz
BuildRoot: /var/tmp/%{name}-buildroot
Requires: libgrace

%description
authd

%prep
%setup -q -n openpanel-authd-%version

%build
BUILD_ROOT=$RPM_BUILD_ROOT
./configure
make
make -C fcat

%install
BUILD_ROOT=$RPM_BUILD_ROOT
rm -rf ${BUILD_ROOT}
mkdir -p ${BUILD_ROOT}/var/opencore/tools
install -m 750 -d ${BUILD_ROOT}/var/opencore/log
mkdir -p ${BUILD_ROOT}/var/opencore/bin
mkdir -p ${BUILD_ROOT}/var/opencore/sockets/authd
cp -rf authd.app ${BUILD_ROOT}/var/opencore/bin/
ln -sf authd.app/exec ${BUILD_ROOT}/var/opencore/bin/authd
cp -rf opencore-tools/* ${BUILD_ROOT}/var/opencore/tools/
cp -f fcat/fcat ${BUILD_ROOT}/var/opencore/tools/
mkdir -p ${BUILD_ROOT}/etc/rc.d/init.d
install -m 755 contrib/redhat.init ${BUILD_ROOT}/etc/rc.d/init.d/openpanel-authd

%post
groupadd -f paneluser > /dev/null
groupadd -f authd > /dev/null
chown -R root:authd /var/opencore/sockets
chmod -R 775 /var/opencore/sockets
chmod 755 /var/opencore/tools
chkconfig --level 2345 openpanel-authd on

%files
%defattr(-,root,root)
/
