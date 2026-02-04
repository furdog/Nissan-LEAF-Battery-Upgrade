idf_component_register(
	SRCS "rescue.c" "main.c" "ws_server.c" "../../source/Src/can-bridge-firmware.c"
	INCLUDE_DIRS "." "../../source/Inc"
	EMBED_FILES "index.html" "qrcode.min.js"
)
