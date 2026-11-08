# Captive-portal DNS server.
#
# Vendored from the official ESP-IDF example
#   examples/protocols/http_server/captive_portal/components/dns_server
# (Unlicense OR CC0-1.0), unchanged except for one indexing fix noted in
# dns_server.c.  It is here rather than pulled from the example tree because
# examples/ is not on the component search path of an ordinary project.
idf_component_register(
    SRCS "dns_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip
)
