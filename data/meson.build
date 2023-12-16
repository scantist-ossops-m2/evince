subdir('icons')

# Desktop file [input filename, output filename]
desktops = [['org.gnome.Evince.desktop',
             '@0@.desktop'.format(ev_app_id)]]

if enable_previewer
  desktops += [['org.gnome.Evince-previewer.desktop',
                '@0@-previewer.desktop'.format(ev_app_id)]]
endif

foreach desktop: desktops
  desktop_in = configure_file(
    input: desktop[0] + '.in.in',
    output: desktop[1] + '.in',
    configuration: mime_types_conf,
  )

  i18n.merge_file(
    type: 'desktop',
    input: desktop_in,
    output: desktop[1],
    po_dir: po_dir,
    install: true,
    install_dir: join_paths(ev_datadir, 'applications'),
  )
endforeach

# DBus service file
if enable_dbus
  service_conf = configuration_data()
  service_conf.set('libexecdir', ev_libexecdir)

  # The D-Bus interface names are in lowercase, which does not match
  # the application name nor the namespace. However, we keep it in
  # lowercase for backward compatibility.
  services = [[ev_namespace.to_lower() + '.Daemon.service', dbus_service_dir]]

  if install_systemd_user_unit_dir
    services += [[ev_namespace + '.service', systemd_user_unit_dir]]
  endif

  foreach service: services
    configure_file(
      input: service[0] + '.in',
      output: service[0],
      configuration: service_conf,
      install: true,
      install_dir: service[1],
    )
  endforeach
endif

# GSettings schema
gschema_file = 'org.gnome.Evince.gschema.xml'
install_data(
  gschema_file,
  install_dir: join_paths(ev_datadir, 'glib-2.0', 'schemas'),
)
if ev_debug
  gnome.compile_schemas(
    depend_files: gschema_file,
  )
endif

# man file
man_pages = ['evince.1', 'evince-previewer.1', 'evince-thumbnailer.1']
foreach man_page: man_pages
  install_data(
    man_page,
    install_dir: join_paths(ev_mandir, 'man1'),
  )
endforeach

# Appdata file
appdata_config = configuration_data()
appdata_config.set('app_id', ev_app_id)
appdata_file_in = configure_file(
          input: 'org.gnome.Evince.appdata.xml.in.in',
         output: 'org.gnome.Evince.appdata.xml.in',
  configuration: appdata_config,
)
appdata_file = i18n.merge_file(
  input: appdata_file_in,
  output: '@0@.appdata.xml'.format(ev_app_id),
  po_dir: po_dir,
  install: true,
  install_dir: ev_appstreamdir,
)

appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  validate_args = ['validate-relax', appdata_file]
endif
