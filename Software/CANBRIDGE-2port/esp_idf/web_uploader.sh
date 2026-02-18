web_upload() {
	local FILE="build/nissan_leaf_battery_upgrade.bin"

	if [ ! -f "$FILE" ]; then
		echo "Firmware file not found: $FILE"
		return 1
	fi

	while true; do
		echo "Uploading $FILE to http://${1}/update..."
		curl -F "file=@$FILE" http://${1}/update && break
		echo "Upload failed, retrying in 1 second..."
		sleep 1
	done

	echo "Upload complete."
}

web_upload "7.7.7.7"
