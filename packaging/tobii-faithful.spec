%global debug_package %{nil}
%{!?tf_version: %global tf_version 0.1.0}
%{!?tf_release: %global tf_release 1}
%{!?tf_url:     %global tf_url https://github.com/SciFi-Bob/tobii-faithful}
%{!?tf_date:    %global tf_date Fri Oct 09 2026}
%global __provides_exclude_from ^%{_libdir}/tobii-faithful/.*$
%if %{defined tf_deps}
%global __requires_exclude ^libonnxruntime\\.so.*$
%endif

Name:           tobii-faithful
Version:        %{tf_version}
Release:        %{tf_release}%{?dist}
Summary:        Linux runtime for the Tobii Eye Tracker 5: head tracking and gaze for games
License:        Apache-2.0 AND MIT
URL:            %{tf_url}
Source0:        %{name}-%{version}.tar.gz
ExclusiveArch:  x86_64

BuildRequires:  cmake >= 3.20
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  mingw64-gcc
BuildRequires:  pkgconfig(eigen3)
BuildRequires:  pkgconfig(libusb-1.0)
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(glfw3)
BuildRequires:  pkgconfig(libsystemd)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  pkgconfig(gl)
%if %{undefined tf_deps}
BuildRequires:  opencv-devel
BuildRequires:  onnxruntime-devel
%endif
BuildRequires:  systemd-rpm-macros

Requires:       xrandr
Requires:       hicolor-icon-theme
Requires:       systemd-udev
%{?systemd_requires}
Suggests:       wine

%description
A user service owns the Tobii Eye Tracker 5 and serves head pose and gaze over
a local socket. A game adapter answers games that ship Tobii's own integration
DLL, a manager calibrates and shows the camera, and a tray icon shows whether
the tracker is awake. The tracker is opened only while something needs it.

%prep
%autosetup

%build
%cmake -G Ninja %{?tf_deps:-C %{tf_deps}/deps.cmake} \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DTOBII_REQUIRE_BRIDGE=ON \
    -DTOBII_INSTALL_UDEV_RULES=OFF \
    -DTOBII_SYSTEMD_USER_UNIT_DIR=%{_userunitdir} \
    -DTOBII_PROJECT_URL=%{tf_url}
%cmake_build

%install
%cmake_install
for f in %{buildroot}%{_bindir}/*; do
    [ "$(head -c4 "$f")" = $'\x7fELF' ] && strip --strip-all "$f"
done
install -Dpm0644 configs/udev/60-tobii-eyetracker5.rules \
    %{buildroot}%{_udevrulesdir}/60-tobii-eyetracker5.rules
install -dm0755 %{buildroot}%{_userpresetdir}
printf 'enable tobiid.service\nenable tobii-adapter.service\n' \
    > %{buildroot}%{_userpresetdir}/80-tobii-faithful.preset

%post
udevadm control --reload-rules >/dev/null 2>&1 || :
udevadm trigger --subsystem-match=usb --attr-match=idVendor=2104 >/dev/null 2>&1 || :
%systemd_user_post tobiid.service tobii-adapter.service
if [ $1 -eq 1 ]; then
    mkdir -p /run/tobii-faithful && touch /run/tobii-faithful/first-install
fi

%posttrans
if [ -e /run/tobii-faithful/first-install ]; then
    rm -rf /run/tobii-faithful
    for uid in $(loginctl list-users --no-legend 2>/dev/null | awk '{print $1}'); do
        user="$(id -nu "$uid" 2>/dev/null)" || continue
        systemctl is-active --quiet "user@$uid.service" || continue
        systemctl --user --machine="$user@" daemon-reload >/dev/null 2>&1 || continue
        systemctl --user --machine="$user@" start tobiid.service tobii-adapter.service >/dev/null 2>&1 || :
        if systemctl --user --machine="$user@" show-environment 2>/dev/null \
                | grep -qE '^(WAYLAND_DISPLAY|DISPLAY)='; then
            systemd-run --user --machine="$user@" --quiet --collect \
                --unit=tobii-tray-first-start \
                --property=PartOf=graphical-session.target \
                --property=After=graphical-session.target \
                %{_bindir}/tobii-tray >/dev/null 2>&1 || :
        fi
    done
fi

%preun
%systemd_user_preun tobiid.service tobii-adapter.service
if [ $1 -eq 0 ]; then
    for p in tobii-tray tobii-manager tobii-adapter tobiid; do
        pkill -x "$p" >/dev/null 2>&1 || :
    done
    pkill -f tobii-sesp-bridge.exe >/dev/null 2>&1 || :
fi

%postun
%udev_rules_update
%systemd_user_postun_with_restart tobiid.service tobii-adapter.service

%files
%license LICENSE THIRD-PARTY-NOTICES.md licenses/
%license third_party/imgui/LICENSE.txt
%{_bindir}/tobiid
%{_bindir}/tobiictl
%{_bindir}/tobii-adapter
%{_bindir}/tobii-manager
%{_bindir}/tobii-tray
%{_bindir}/tobii-faithful-uninstall
%{_libdir}/tobii-faithful/
%{_datadir}/tobii-faithful/
%{_datadir}/applications/tobii-faithful.desktop
%{_datadir}/applications/tobii-manager.desktop
%{_datadir}/icons/hicolor/scalable/apps/tobii-faithful.svg
%{_sysconfdir}/xdg/autostart/tobii-tray.desktop
%{_userunitdir}/tobiid.service
%{_userunitdir}/tobii-adapter.service
%{_userpresetdir}/80-tobii-faithful.preset
%{_udevrulesdir}/60-tobii-eyetracker5.rules

%changelog
* %{tf_date} tobii-faithful - %{tf_version}-%{tf_release}
- Release %{tf_version}
