%define 	pkgname	authd

Name: 		openpanel-authd
Version: 	1.0.1
Release: 	1%{?dist}
Summary:  	OpenPanel root daemon
License: 	GPLv3
Group: 		Applications/Internet
Source: 	%{name}-%{version}.tar.bz2
Source1:	openpanel-authd.init
Requires:	grace
BuildRequires:	grace-devel
BuildRoot:	%{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%description
The OpenPanel root daemon

%prep
%setup -q -n %{pkgname}
./configure --prefix=%{_prefix} --exec-prefix=%{_bindir} \
            --lib-prefix=%{_libdir} --conf-prefix=%{_sysconfdir} \
	    --include-prefix=%{_includedir}

%build
make

%install
rm -rf %{buildroot}
%makeinstall DESTDIR=%{buildroot}
cp -a %SOURCE1 %{buildroot}/etc/init.d/openpanel-authd

%clean
rm -rf $RPM_BUILD_ROOT

%pre
/usr/sbin/groupadd -f -r openpanel-authd >/dev/null 2>&1 || :

%post
/sbin/chkconfig --add openpanel-authd

%preun
if [ $1 = 0 ]; then
	service openpanel-authd stop >/dev/null 2>&1
	/sbin/chkconfig --del openpanel-authd
fi

%postun
if [ $1 = 0 ]; then
	/usr/sbin/groupdel openpanel-authd >/dev/null 2>&1 || :
fi
if [ "$1" ge "1" ]; then
	service openpanel-authd condrestart >/dev/null 2>&1 || :
fi

%files
%defattr(-,root,root)
%config /etc/init.d/openpanel-authd
%{_localstatedir}/openpanel/bin/openpanel-authd
%{_localstatedir}/openpanel/bin/openpanel-authd.app/
%{_localstatedir}/openpanel/tools/
%dir %attr(0750, root, openpanel-authd) %{_localstatedir}/openpanel/sockets/authd

%changelog
* Wed Jan 18 2011 Igmar Palsenberg <igmar@palsenberg.com>
- Initial packaging
