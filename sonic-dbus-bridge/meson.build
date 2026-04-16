project(
    'sonic-dbus-bridge',
    'cpp',
    version: '1.0.0',
    meson_version: '>=0.57.0',
    default_options: [
        'warning_level=3',
        'werror=true',
        'cpp_std=c++23',
        'buildtype=debugoptimized',
    ],
)

# Configuration for user management
conf_data = configuration_data()
conf_data.set_quoted(
    'USER_MANAGER_BUSNAME',
    'xyz.openbmc_project.User.Manager',
    description: 'The D-Bus busname for user management.',
)
conf_data.set_quoted(
    'INVENTORY_MANAGER_BUSNAME',
    'xyz.openbmc_project.Inventory.Manager',
    description: 'The D-Bus busname for inventory management.',
)
conf_data.set_quoted(
    'OBJECT_MAPPER_BUSNAME',
    'xyz.openbmc_project.ObjectMapper',
    description: 'The D-Bus busname for object mapper.',
)
conf_data.set_quoted(
    'STATE_HOST_BUSNAME',
    'xyz.openbmc_project.State.Host',
    description: 'The D-Bus busname for host state management.',
)

# Enable root user management by default
conf_data.set('ENABLE_ROOT_USER_MGMT', true)

conf_header = configure_file(output: 'config.h', configuration: conf_data)
# Dependencies
systemd_dep = dependency('libsystemd')
hiredis_dep = dependency('hiredis')
jsoncpp_dep = dependency('jsoncpp')
boost_dep = dependency('boost', modules: ['system'])
threads_dep = dependency('threads')
sdbusplus_dep = dependency('sdbusplus')

# Source files
sources = files(
    'src/main.cpp',
    'src/bridge_app.cpp',
    'src/redis_adapter.cpp',
    'src/redis_state_publisher.cpp',
    'src/redis_state_subscriber.cpp',
    'src/platform_json_adapter.cpp',
    'src/fru_adapter.cpp',
    'src/inventory_model.cpp',
    'src/dbus_exporter.cpp',
    'src/update_engine.cpp',
    'src/config_manager.cpp',
    'src/object_mapper.cpp',
    'src/state_manager.cpp',
    'src/rack_manager_receiver.cpp',
    # User management
    'src/user_mgr.cpp',
    'src/users.cpp',
    'src/state_manager.cpp',
)


# Include directories
inc = include_directories('include')

# Executable
executable(
    'sonic-dbus-bridge',
    sources,
    conf_header,
    include_directories: inc,
    dependencies: [
        systemd_dep,
        hiredis_dep,
        jsoncpp_dep,
        boost_dep,
        threads_dep,
        sdbusplus_dep,
    ],
    link_args: ['-lcrypt'],
    install: true,
    install_dir: get_option('bindir'),
)

# Install configuration file
install_data(
    'config/config.yaml',
    install_dir: get_option('sysconfdir') / 'sonic-dbus-bridge',
)

# sonic-dbus-bridge now runs inside docker-bmcweb container (managed by supervisord)
# No systemd service needed

# Install D-Bus policy files
install_data(
    'dbus/xyz.openbmc_project.Inventory.Manager.conf',
    'dbus/xyz.openbmc_project.ObjectMapper.conf',
    'dbus/xyz.openbmc_project.User.Manager.conf',
    install_dir: get_option('sysconfdir') / 'dbus-1' / 'system.d',
)

# Create state directory
meson.add_install_script('sh', '-c',
    'mkdir -p $DESTDIR' + get_option('prefix') / 'var' / 'lib' / 'sonic-dbus-bridge'
)

