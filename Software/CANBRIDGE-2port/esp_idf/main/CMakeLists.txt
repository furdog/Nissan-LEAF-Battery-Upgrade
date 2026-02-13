idf_component_register(
	SRCS "main.c" "web/rescue.c" "web/ws_server.c" "web/dns_server.c" "web/ota.c" "../../source/Src/can-bridge-firmware.c"
	INCLUDE_DIRS "." "web/" "../../source/Inc"
	EMBED_FILES "web/index.html" "web/qrcode.min.js"
)
