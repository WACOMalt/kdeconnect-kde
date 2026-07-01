ecm_add_qml_module(kdeconnectdeclarativeplugin URI "org.kde.kdeconnect" VERSION 1.0)

# WORKAROUND: KDE Bug 519868 — qmlcachegen AOT miscompiles
# arrow-function signal handlers on C++ QML types (specifically
# DBusAsyncResponse callbacks) and other QML constructs that mix
# arrow-function event handlers with parent-scope writes. The AOT
# code uses storeNameSloppy to write into the parent QML scope,
# but the generated frame management is incorrect, causing a SEGV
# when the handler fires from a D-Bus callback. All three QML
# files in this module exhibit some variant of the pattern:
#
#   DBusProperty.qml      — DBusAsyncResponse callback writing
#                            _value = result into prop scope.
#   PluginChecker.qml     — DBusAsyncResponse callbacks writing
#                            root.available / root.iconName.
#   RemoteKeyboard.qml    — arrow-function Keys.onPressed
#                            handler (line 93) plus dynamic
#                            signal.connect() (line 103).
#
# An initial fix covering only DBusProperty.qml proved insufficient:
# client testing reproduced the same SEGV at identical offsets when
# the device list re-rendered after a peer connected — the AOT
# cache of PluginChecker.qml crashed in the same way. This disables
# AOT for the whole module to eliminate every instance of the
# miscompiled pattern at once. Drop these properties once upstream
# qmlcachegen is fixed.
#
# Performance cost: small. Each QML file is parsed by the
# interpreter instead of bytecode-loaded; the affected components
# are instantiated only a handful of times per Settings dialog. A
# less invasive alternative is to set
# QT_QMLCACHEGEN_ARGUMENTS "--only-bytecode" on the target, which
# keeps the bytecode cache and only disables AOT (requires Qt ≥
# 6.5; the project's minimum is 6.7).
set_source_files_properties(qml/DBusProperty.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN TRUE)
set_source_files_properties(qml/PluginChecker.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN TRUE)
set_source_files_properties(qml/RemoteKeyboard.qml PROPERTIES
    QT_QML_SKIP_CACHEGEN TRUE)

target_sources(kdeconnectdeclarativeplugin PRIVATE
    kdeconnectdeclarativeplugin.cpp
    responsewaiter.cpp
    objectfactory.cpp
    pointerlocker.cpp
)

ecm_target_qml_sources(kdeconnectdeclarativeplugin
    SOURCES
    qml/DBusProperty.qml
    qml/PluginChecker.qml
    qml/RemoteKeyboard.qml
    VERSION 1.0)

if(UNIX AND NOT APPLE)
    if (Qt6_VERSION VERSION_GREATER_EQUAL "6.8.0")
        set(private_code_option  "PRIVATE_CODE")
    endif()
    qt6_generate_wayland_protocol_client_sources(kdeconnectdeclarativeplugin FILES
        ${WaylandProtocols_DATADIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
        ${private_code_option}
    )
    qt6_generate_wayland_protocol_client_sources(kdeconnectdeclarativeplugin FILES
        ${WaylandProtocols_DATADIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
        ${private_code_option}
    )

    target_sources(kdeconnectdeclarativeplugin PRIVATE ${wayland_SRCS})
    target_link_libraries(kdeconnectdeclarativeplugin PRIVATE Wayland::Client Qt::WaylandClient Qt::GuiPrivate)
    target_sources(kdeconnectdeclarativeplugin PUBLIC pointerlockerwayland.cpp)
endif()

target_link_libraries(kdeconnectdeclarativeplugin PRIVATE Qt::Quick kdeconnectdbusinterfaces kdeconnectmodels kdeconnectcore)

ecm_finalize_qml_module(kdeconnectdeclarativeplugin DESTINATION ${KDE_INSTALL_QMLDIR})
