#include <zephyr/linker/iterable_sections.h>

ITERABLE_SECTION_ROM(http_resource_desc_qat_ws_http,      Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(http_resource_desc_qat_ws_https,     Z_LINK_ITERABLE_SUBALIGN)
#if defined(CONFIG_NET_IPV6)
ITERABLE_SECTION_ROM(http_resource_desc_qat_ws_http_v6,   Z_LINK_ITERABLE_SUBALIGN)
ITERABLE_SECTION_ROM(http_resource_desc_qat_ws_https_v6,  Z_LINK_ITERABLE_SUBALIGN)
#endif
