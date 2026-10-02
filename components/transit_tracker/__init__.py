import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.display import Display, DisplayRef
from esphome.components.font import Font
from esphome.components.time import RealTimeClock
from esphome.components import color
from esphome.const import (
    CONF_ID,
    CONF_DISPLAY_ID,
    CONF_TIME_ID,
    CONF_SHOW_UNITS,
    CONF_LAMBDA,
    CONF_URL,
    CONF_UPDATE_INTERVAL,
    CONF_TIMEOUT,
    __version__ as ESPHOME_VERSION,
)
from esphome.types import ConfigType
from esphome.components import esp32
from esphome.components.esp32 import (
    add_idf_component,
)

_MINIMUM_ESPHOME_VERSION = "2025.11.0"

DEPENDENCIES = ["network", "display", "font", "time", "esp32"]
AUTO_LOAD = ["json"]

transit_tracker_ns = cg.esphome_ns.namespace("transit_tracker")
TransitTracker = transit_tracker_ns.class_("TransitTracker", cg.Component)

UnitDisplay = transit_tracker_ns.enum("UnitDisplay")
Alert = transit_tracker_ns.struct("Alert")
AlertConstRef = Alert.operator("ref").operator("const")
AlertHeaderStyle = transit_tracker_ns.enum("AlertHeaderStyle")
ALERT_HEADER_STYLES = {
    "filled": AlertHeaderStyle.ALERT_HEADER_FILLED,
    "text": AlertHeaderStyle.ALERT_HEADER_TEXT,
}
ClockAnimation = transit_tracker_ns.enum("ClockAnimation")
CLOCK_ANIMATIONS = {
    "none": ClockAnimation.CLOCK_ANIMATION_NONE,
    "slide": ClockAnimation.CLOCK_ANIMATION_SLIDE,
    "fade": ClockAnimation.CLOCK_ANIMATION_FADE,
}
UNIT_DISPLAY_VALUES = {
    "long": UnitDisplay.UNIT_DISPLAY_LONG,
    "short": UnitDisplay.UNIT_DISPLAY_SHORT,
    "none": UnitDisplay.UNIT_DISPLAY_NONE,
}

CONF_ROUTES = "routes"
CONF_STOPS = "stops"
CONF_BASE_URL = "base_url"
CONF_FONT_ID = "font_id"
CONF_LIMIT = "limit"
CONF_ABBREVIATIONS = "abbreviations"
CONF_STYLES = "styles"
CONF_FEED_CODE = "feed_code"
CONF_DEFAULT_ROUTE_COLOR = "default_route_color"
CONF_REALTIME_COLOR = "realtime_color"
CONF_TIME_DISPLAY = "time_display"
CONF_LIST_MODE = "list_mode"
CONF_SCROLL_HEADSIGNS = "scroll_headsigns"
CONF_HEADERS = "headers"
CONF_HEADER_TEXT = "header_text"
CONF_ALERTS = "alerts"
CONF_KEYS = "keys"
CONF_LANGUAGE = "language"
CONF_MAX_RESPONSE_SIZE = "max_response_size"
CONF_SCHEDULE_DURATION = "schedule_duration"
CONF_ALERT_DURATION = "alert_duration"
CONF_PAGE_DURATION = "page_duration"
CONF_HEADER_STYLE = "header_style"
CONF_INFO_TITLE = "info_title"
CONF_WARNING_TITLE = "warning_title"
CONF_SEVERE_TITLE = "severe_title"
CONF_HEADER_TEXT_SOURCE = "header"
CONF_SHOW_COUNTER = "show_counter"
CONF_INFO_COLOR = "info_color"
CONF_WARNING_COLOR = "warning_color"
CONF_SEVERE_COLOR = "severe_color"
CONF_TEXT_COLOR = "text_color"
CONF_CANCELLED = "cancelled"
CONF_SHOW = "show"
CONF_RAIL_TEXT = "rail_text"
CONF_BUS_TEXT = "bus_text"
CONF_COLOR = "color"
CONF_HEADSIGN_COLOR = "headsign_color"
CONF_ALTERNATE_INTERVAL = "alternate_interval"
CONF_UNKNOWN_ROUTE_TYPE = "unknown_route_type"
CONF_RAIL_ROUTES = "rail_routes"
CONF_CLOCK = "clock"
CONF_INTERVAL = "interval"
CONF_DURATION = "duration"
CONF_DATE_COLOR = "date_color"
CONF_TIME_COLOR = "time_color"
CONF_ANIMATION = "animation"
CONF_VLILLE = "vlille"
CONF_STATIONS = "stations"
CONF_LOGO_COLOR = "logo_color"
ALERT_KEY_NAMES = [
    "list", "id", "title", "message", "severity", "route", "color", "active", "start", "end", "important",
]

def validate_ws_url(value):
    url = cv.url(value)
    if not value.startswith("ws://") and not value.startswith("wss://"):
        raise cv.Invalid("URL must start with 'ws://' or 'wss://")

    return url


def validate_esphome_version(obj):
    if cv.Version.parse(ESPHOME_VERSION) < cv.Version.parse(_MINIMUM_ESPHOME_VERSION):
        raise cv.Invalid(
            "The transit_tracker component requires ESPHome version " +
            f"{_MINIMUM_ESPHOME_VERSION} or later."
        )
    return obj


def _consume_transit_tracker_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for transit_tracker component."""
    from esphome.components import socket
    socket.consume_sockets(1, "transit_tracker")(config)
    return config


COLOR_SCHEMA = cv.All(
    cv.requires_component("color"),
    cv.use_id(color.ColorStruct)
)


def validate_http_url(value):
    value = cv.string_strict(value)
    if value and not (value.startswith("http://") or value.startswith("https://")):
        raise cv.Invalid("Alerts URL must start with 'http://' or 'https://'")
    return value


ALERTS_SCHEMA = cv.Schema(
    {
        # Empty URL is allowed so it can be set at runtime (e.g. from a text entity)
        cv.Optional(CONF_URL, default=""): validate_http_url,
        # Font for the alert screen; defaults to the schedule font
        cv.Optional(CONF_FONT_ID): cv.use_id(Font),
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=5)),
        ),
        cv.Optional(CONF_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MAX_RESPONSE_SIZE, default=8192): cv.int_range(min=256, max=65536),
        cv.Optional(CONF_LANGUAGE, default=""): cv.string,
        cv.Optional(CONF_HEADERS): cv.ensure_list(
            cv.Schema(
                {
                    cv.Required("name"): cv.string,
                    cv.Required("value"): cv.string,
                }
            )
        ),
        cv.Optional(CONF_KEYS, default={}): cv.Schema(
            {cv.Optional(key): cv.string for key in ALERT_KEY_NAMES}
        ),
        # Rotation: schedule is shown for schedule_duration, then each alert in turn.
        # schedule_duration: 0s shows only alerts while there are any.
        cv.Optional(CONF_SCHEDULE_DURATION, default="20s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_ALERT_DURATION, default="8s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=1)),
        ),
        cv.Optional(CONF_PAGE_DURATION, default="4s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=500)),
        ),
        cv.Optional(CONF_HEADER_STYLE, default="filled"): cv.enum(ALERT_HEADER_STYLES, lower=True),
        # Header shows the severity label ("severity") or the alert title ("title")
        cv.Optional(CONF_HEADER_TEXT_SOURCE, default="severity"): cv.one_of("severity", "title", lower=True),
        cv.Optional(CONF_INFO_TITLE, default="Information"): cv.string,
        cv.Optional(CONF_WARNING_TITLE, default="Perturbation"): cv.string,
        cv.Optional(CONF_SEVERE_TITLE, default="Perturbation"): cv.string,
        cv.Optional(CONF_SHOW_COUNTER, default=True): cv.boolean,
        cv.Optional(CONF_INFO_COLOR): COLOR_SCHEMA,
        cv.Optional(CONF_WARNING_COLOR): COLOR_SCHEMA,
        cv.Optional(CONF_SEVERE_COLOR): COLOR_SCHEMA,
        cv.Optional(CONF_TEXT_COLOR): COLOR_SCHEMA,
        # Fully custom rendering: void(Display &it, const Alert &alert, int index, int count, uint32_t elapsed)
        cv.Optional(CONF_LAMBDA): cv.lambda_,
    }
)


CANCELLED_SCHEMA = cv.Schema(
    {
        # Ask the API to keep cancelled trips (flagged) instead of dropping them
        cv.Optional(CONF_SHOW, default=True): cv.boolean,
        # Metro, tram, train, ferry... (any non-bus GTFS route_type)
        cv.Optional(CONF_RAIL_TEXT, default="Interrompu"): cv.string,
        # Bus, trolleybus, coach
        cv.Optional(CONF_BUS_TEXT, default="Non desservi"): cv.string,
        cv.Optional(CONF_COLOR): COLOR_SCHEMA,
        cv.Optional(CONF_HEADSIGN_COLOR): COLOR_SCHEMA,
        # The cross and the label alternate at this interval (rows out of phase)
        cv.Optional(CONF_ALTERNATE_INTERVAL, default="2s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(milliseconds=250)),
        ),
        # Label to use when the API doesn't report the route type
        cv.Optional(CONF_UNKNOWN_ROUTE_TYPE, default="bus"): cv.one_of("bus", "rail", lower=True),
        # Route IDs that always use the rail label
        cv.Optional(CONF_RAIL_ROUTES, default=[]): cv.ensure_list(cv.string),
    }
)


CLOCK_SCHEMA = cv.Schema(
    {
        # Initial state; can be toggled at runtime with set_clock_enabled()
        cv.Optional(CONF_SHOW, default=True): cv.boolean,
        # Time between two appearances of the date/time screen
        cv.Optional(CONF_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=5)),
        ),
        # How long the date/time screen stays on
        cv.Optional(CONF_DURATION, default="5s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=1)),
        ),
        cv.Optional(CONF_DATE_COLOR): COLOR_SCHEMA,
        cv.Optional(CONF_TIME_COLOR): COLOR_SCHEMA,
        # Enter/exit transition: slide (date from the top, time from the bottom), fade or none
        cv.Optional(CONF_ANIMATION, default="slide"): cv.enum(CLOCK_ANIMATIONS, lower=True),
    }
)


VLILLE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_URL, default="https://api.mobilille.fr/v1/vlille/"): validate_http_url,
        # Names (or parts of names) or IDs, separated by commas; one screen per station.
        # Usually set at runtime from a text entity instead.
        cv.Optional(CONF_STATIONS, default=""): cv.string,
        # Initial state; can be toggled at runtime with set_vlille_enabled()
        cv.Optional(CONF_SHOW, default=True): cv.boolean,
        cv.Optional(CONF_UPDATE_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=15)),
        ),
        cv.Optional(CONF_TIMEOUT, default="10s"): cv.positive_time_period_milliseconds,
        # Time between two passes over the stations
        cv.Optional(CONF_INTERVAL, default="60s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=5)),
        ),
        # How long each station stays on screen
        cv.Optional(CONF_DURATION, default="6s"): cv.All(
            cv.positive_time_period_milliseconds,
            cv.Range(min=cv.TimePeriod(seconds=1)),
        ),
        cv.Optional(CONF_LOGO_COLOR): COLOR_SCHEMA,
    }
)


CONFIG_SCHEMA = cv.All(
    validate_esphome_version,
    cv.only_on_esp32,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TransitTracker),
            cv.GenerateID(CONF_DISPLAY_ID): cv.use_id(Display),
            cv.GenerateID(CONF_FONT_ID): cv.use_id(Font),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(RealTimeClock),
            cv.Optional(CONF_BASE_URL): validate_ws_url,
            cv.Optional(CONF_LIMIT, default=3): cv.positive_int,
            cv.Optional(CONF_FEED_CODE, default=""): cv.string,
            cv.Optional(CONF_TIME_DISPLAY, default="departure"): cv.one_of(
                "departure", "arrival"
            ),
            cv.Optional(CONF_LIST_MODE, default="sequential"): cv.one_of(
                "sequential", "nextPerRoute"
            ),
            cv.Optional(CONF_SCROLL_HEADSIGNS, default=False) : cv.boolean,
            cv.Optional(CONF_STOPS, default=[]): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("stop_id"): cv.string,
                        cv.Optional("time_offset", default="0s"): cv.time_period,
                        cv.Required(CONF_ROUTES): cv.ensure_list(cv.string),
                    }
                )
            ),
            cv.Optional(CONF_HEADER_TEXT, default=""): cv.string,
            cv.Optional(CONF_SHOW_UNITS, default="long"): cv.enum(UNIT_DISPLAY_VALUES),
            cv.Optional(CONF_DEFAULT_ROUTE_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_REALTIME_COLOR): COLOR_SCHEMA,
            cv.Optional(CONF_STYLES): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("route_id"): cv.string,
                        cv.Required("name"): cv.string,
                        cv.Required("color"): COLOR_SCHEMA,
                    }
                )
            ),
            cv.Optional(CONF_ABBREVIATIONS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("from"): cv.string,
                        cv.Required("to"): cv.string,
                    }
                )
            ),
            cv.Optional(CONF_HEADERS): cv.ensure_list(
                cv.Schema(
                    {
                        cv.Required("name"): cv.string,
                        cv.Required("value"): cv.string,
                    }
                )
            ),
            cv.Optional(CONF_ALERTS): ALERTS_SCHEMA,
            cv.Optional(CONF_CANCELLED, default={}): CANCELLED_SCHEMA,
            cv.Optional(CONF_CLOCK): CLOCK_SCHEMA,
            cv.Optional(CONF_VLILLE): VLILLE_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_transit_tracker_sockets,
)


def _generate_schedule_string(stops):
    return ";".join(
        [
            f"{route},{stop['stop_id']},{stop['time_offset'].total_seconds}"
            for stop in stops
            for route in stop[CONF_ROUTES]
        ]
    )


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])

    drawing_display = await cg.get_variable(config[CONF_DISPLAY_ID])
    cg.add(var.set_display(drawing_display))

    font = await cg.get_variable(config[CONF_FONT_ID])
    cg.add(var.set_font(font))

    time = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_rtc(time))

    if CONF_BASE_URL in config:
        cg.add(var.set_base_url(config[CONF_BASE_URL]))

    cg.add(var.set_feed_code(config[CONF_FEED_CODE]))
    cg.add(var.set_schedule_string(_generate_schedule_string(config[CONF_STOPS])))

    display_departure_times = config[CONF_TIME_DISPLAY] == "departure"
    cg.add(var.set_display_departure_times(display_departure_times))

    cg.add(var.set_list_mode(config[CONF_LIST_MODE]))
    cg.add(var.set_scroll_headsigns(config[CONF_SCROLL_HEADSIGNS]))

    cg.add(var.set_limit(config[CONF_LIMIT]))

    if CONF_HEADER_TEXT in config:
        cg.add(var.set_header_text(config[CONF_HEADER_TEXT]))

    cg.add(var.set_unit_display(config[CONF_SHOW_UNITS]))

    if CONF_HEADERS in config:
        for header in config[CONF_HEADERS]:
            cg.add(var.add_header(header["name"], header["value"]))

    if CONF_ABBREVIATIONS in config:
        for abbreviation in config[CONF_ABBREVIATIONS]:
            cg.add(var.add_abbreviation(abbreviation["from"], abbreviation["to"]))

    if CONF_DEFAULT_ROUTE_COLOR in config:
        cg.add(
            var.set_default_route_color(
                await cg.get_variable(config[CONF_DEFAULT_ROUTE_COLOR])
            )
        )

    if CONF_REALTIME_COLOR in config:
        cg.add(
            var.set_realtime_color(
                await cg.get_variable(config[CONF_REALTIME_COLOR])
            )
        )

    if CONF_STYLES in config:
        for style in config[CONF_STYLES]:
            color_struct = await cg.get_variable(style["color"])
            cg.add(var.add_route_style(style["route_id"], style["name"], color_struct))

    if CONF_ALERTS in config:
        await _alerts_to_code(var, config[CONF_ALERTS])

    await _cancelled_to_code(var, config[CONF_CANCELLED])

    if CONF_CLOCK in config:
        await _clock_to_code(var, config[CONF_CLOCK])

    if CONF_VLILLE in config:
        await _vlille_to_code(var, config[CONF_VLILLE])

    await cg.register_component(var, config)

    add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )

    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)


async def _alerts_to_code(var, conf):
    # esp_http_client is excluded from the build by default unless http_request is used
    if hasattr(esp32, "include_builtin_idf_component"):
        esp32.include_builtin_idf_component("esp_http_client")

    cg.add(var.set_alerts_update_interval(conf[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_alerts_timeout(conf[CONF_TIMEOUT]))
    if CONF_FONT_ID in conf:
        cg.add(var.set_alerts_font(await cg.get_variable(conf[CONF_FONT_ID])))
    cg.add(var.set_alerts_max_response_size(conf[CONF_MAX_RESPONSE_SIZE]))
    cg.add(var.set_alerts_language(conf[CONF_LANGUAGE]))

    for header in conf.get(CONF_HEADERS, []):
        cg.add(var.add_alerts_header(header["name"], header["value"]))

    for key, value in conf[CONF_KEYS].items():
        cg.add(cg.RawExpression(f"{var}->alerts_keys().{key} = {cg.safe_exp(value)}"))

    cg.add(var.set_alerts_schedule_duration(conf[CONF_SCHEDULE_DURATION]))
    cg.add(var.set_alerts_alert_duration(conf[CONF_ALERT_DURATION]))
    cg.add(var.set_alerts_page_duration(conf[CONF_PAGE_DURATION]))
    cg.add(var.set_alerts_header_style(conf[CONF_HEADER_STYLE]))
    cg.add(var.set_alerts_header_shows_severity(conf[CONF_HEADER_TEXT_SOURCE] == "severity"))
    cg.add(var.set_alerts_info_title(conf[CONF_INFO_TITLE]))
    cg.add(var.set_alerts_warning_title(conf[CONF_WARNING_TITLE]))
    cg.add(var.set_alerts_severe_title(conf[CONF_SEVERE_TITLE]))
    cg.add(var.set_alerts_show_counter(conf[CONF_SHOW_COUNTER]))

    for conf_key, setter in (
        (CONF_INFO_COLOR, var.set_alerts_info_color),
        (CONF_WARNING_COLOR, var.set_alerts_warning_color),
        (CONF_SEVERE_COLOR, var.set_alerts_severe_color),
        (CONF_TEXT_COLOR, var.set_alerts_text_color),
    ):
        if conf_key in conf:
            cg.add(setter(await cg.get_variable(conf[conf_key])))

    if CONF_LAMBDA in conf:
        lambda_ = await cg.process_lambda(
            conf[CONF_LAMBDA],
            [
                (DisplayRef, "it"),
                (AlertConstRef, "alert"),
                (cg.int_, "index"),
                (cg.int_, "count"),
                (cg.uint32, "elapsed"),
            ],
            return_type=cg.void,
        )
        cg.add(var.set_alerts_draw_lambda(lambda_))

    # Set last: this is what enables the feature
    cg.add(var.set_alerts_url(conf[CONF_URL]))
    if not conf[CONF_URL]:
        # Enable the feature so the URL can be provided later at runtime
        cg.add(var.set_alerts_configured())


async def _vlille_to_code(var, conf):
    cg.add(var.set_vlille_configured())
    cg.add(var.set_vlille_url(conf[CONF_URL]))
    cg.add(var.set_vlille_update_interval(conf[CONF_UPDATE_INTERVAL]))
    cg.add(var.set_vlille_timeout(conf[CONF_TIMEOUT]))
    cg.add(var.set_vlille_interval(conf[CONF_INTERVAL]))
    cg.add(var.set_vlille_duration(conf[CONF_DURATION]))
    if conf[CONF_STATIONS]:
        cg.add(var.set_vlille_stations(conf[CONF_STATIONS]))
    if CONF_LOGO_COLOR in conf:
        cg.add(var.set_vlille_logo_color(await cg.get_variable(conf[CONF_LOGO_COLOR])))
    cg.add(var.set_vlille_enabled(conf[CONF_SHOW]))


async def _clock_to_code(var, conf):
    cg.add(var.set_clock_interval(conf[CONF_INTERVAL]))
    cg.add(var.set_clock_duration(conf[CONF_DURATION]))
    cg.add(var.set_clock_animation(conf[CONF_ANIMATION]))
    if CONF_DATE_COLOR in conf:
        cg.add(var.set_clock_date_color(await cg.get_variable(conf[CONF_DATE_COLOR])))
    if CONF_TIME_COLOR in conf:
        cg.add(var.set_clock_time_color(await cg.get_variable(conf[CONF_TIME_COLOR])))
    cg.add(var.set_clock_enabled(conf[CONF_SHOW]))


async def _cancelled_to_code(var, conf):
    cg.add(var.set_show_cancelled(conf[CONF_SHOW]))
    cg.add(var.set_cancelled_rail_text(conf[CONF_RAIL_TEXT]))
    cg.add(var.set_cancelled_bus_text(conf[CONF_BUS_TEXT]))
    cg.add(var.set_cancelled_alternate_interval(conf[CONF_ALTERNATE_INTERVAL]))
    cg.add(var.set_cancelled_unknown_is_rail(conf[CONF_UNKNOWN_ROUTE_TYPE] == "rail"))
    for route_id in conf[CONF_RAIL_ROUTES]:
        cg.add(var.add_cancelled_rail_route(route_id))
    if CONF_COLOR in conf:
        cg.add(var.set_cancelled_color(await cg.get_variable(conf[CONF_COLOR])))
    if CONF_HEADSIGN_COLOR in conf:
        cg.add(var.set_cancelled_headsign_color(await cg.get_variable(conf[CONF_HEADSIGN_COLOR])))
