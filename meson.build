project(
    'Telemetry',
    'cpp',
    meson_version: '>=0.57.0',
    default_options: [
        'buildtype=debugoptimized',
        'cpp_std=c++20',
        # TODO: Without RTTI telemetry does not build using Boost 1.74.0
        # https://github.com/chriskohlhoff/asio/issues/533
        #'cpp_rtti=false',
        'warning_level=3',
        'werror=true',
        'b_lto=true',
    ],
    license: 'Apache-2.0',
)

summary({'Fast build with link-time optimizer disabled':
            not get_option('b_lto')}, section: 'General')

cpp = meson.get_compiler('cpp')
add_project_arguments(
    cpp.get_supported_arguments([
        '-DBOOST_ASIO_DISABLE_THREADS',
        '-DBOOST_ALL_NO_LIB',
        '-DBOOST_SYSTEM_NO_DEPRECATED',
        '-DBOOST_ASIO_NO_DEPRECATED',
        '-DBOOST_NO_RTTI',
        '-DBOOST_NO_TYPEID',
        # TODO: Removed below arg after upgrade to Boost 1.75
        '-DBOOST_ALLOW_DEPRECATED_HEADERS',
        '-Wno-unused-parameter',
    ]),
    language: 'cpp'
)

boost = dependency(
    'boost',
    version: '>=1.74.0',
    required: false,
    modules: ['coroutine'])
assert(boost.found(),
       'Missing Boost 1.74.0 or higher, as WA you can set BOOST_ROOT ' +
       'environemt to point at boost build. To build a boost you can use ' +
       'script ./scripts/boost_build_1.74.0.sh')

phosphor_logging = dependency('phosphor-logging')
sdbusplus = dependency('sdbusplus')
systemd = dependency('systemd')

if cpp.has_header('nlohmann/json.hpp')
    nlohmann_json = declare_dependency()
else
    nlohmann_json = dependency('nlohmann_json')
endif

add_project_arguments(
    '-DTELEMETRY_MAX_REPORTS=' + get_option('max-reports').to_string(),
    '-DTELEMETRY_MAX_READING_PARAMS=' +
        get_option('max-reading-parameters').to_string(),
    '-DTELEMETRY_MIN_INTERVAL=' + get_option('min-interval').to_string(),
    '-DTELEMETRY_MAX_TRIGGERS=' + get_option('max-triggers').to_string(),
    '-DTELEMETRY_MAX_DBUS_PATH_LENGTH=' +
        get_option('max-dbus-path-length').to_string(),
    '-DTELEMETRY_MAX_APPEND_LIMIT=' +
        get_option('max-append-limit').to_string(),
    language: 'cpp'
)

executable(
    'telemetry',
    [
        'src/discrete_threshold.cpp',
        'src/main.cpp',
        'src/metric.cpp',
        'src/metrics/collection_data.cpp',
        'src/metrics/collection_function.cpp',
        'src/numeric_threshold.cpp',
        'src/on_change_threshold.cpp',
        'src/persistent_json_storage.cpp',
        'src/report.cpp',
        'src/report_factory.cpp',
        'src/report_manager.cpp',
        'src/sensor.cpp',
        'src/sensor_cache.cpp',
        'src/trigger.cpp',
        'src/trigger_actions.cpp',
        'src/trigger_factory.cpp',
        'src/trigger_manager.cpp',
        'src/types/readings.cpp',
        'src/types/report_types.cpp',
        'src/utils/conversion_trigger.cpp',
        'src/utils/generate_id.cpp',
        'src/utils/messanger_service.cpp',
    ],
    dependencies: [
        boost,
        nlohmann_json,
        sdbusplus,
        phosphor_logging,
    ],
    include_directories: 'src',
    install: true,
    install_dir: get_option('prefix') / get_option('bindir'),
    pie: true,
)

configure_file(
    input: 'xyz.openbmc_project.Telemetry.service.in',
    output: 'xyz.openbmc_project.Telemetry.service',
    configuration: {
        'bindir': get_option('prefix') / get_option('bindir'),
    },
    install: true,
    install_dir: systemd.get_variable(pkgconfig: 'systemdsystemunitdir'),
)

if get_option('buildtest')
    subdir('tests')
endif
