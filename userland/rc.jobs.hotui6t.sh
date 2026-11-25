# rc.jobs.sh ON|off (as "hotspot status" says them): the hotspot switched in Settings > Personal Hotspot, as a person does it (Settings opened, its
# Personal Hotspot pane, Allow Others to Join tapped), and checked with "hotspot status"; up to three tries, since the
# first opening of Settings after a start can come before its list is there (25 November, hotui-1 and -2).
want=$1
for t in 1 2 3; do
	case "$(/redwood/bin/hotspot status)" in
	"hotspot: $want"*) echo "hotui: $want (try $t)"; exit 0 ;;
	esac
	/redwood/bin/rwtouch button home wait 2 notify com.hakios.unlock wait 3 notify com.hakios.open.prefs wait 12
	/redwood/bin/battery_test open prefs:root=INTERNET_TETHERING
	/redwood/bin/rwtouch wait 6 tap 0.86 0.183 wait 5
done
case "$(/redwood/bin/hotspot status)" in
"hotspot: $want"*) echo "hotui: $want (after 3 tries)"; exit 0 ;;
esac
echo "hotui: NOT $want after 3 tries"
exit 1
