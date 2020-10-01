#!/bin/sh
	TAP_TXG=`fw_printenv -n hnap_tx_gain 2> /dev/null || echo 10`
	TAP_RXG=`fw_printenv -n hnap_rx_gain 2> /dev/null || echo 70`
	TAP_U=`fw_printenv -n hnap_u_mode 2> /dev/null || echo 0`
	TAP_D=`fw_printenv -n hnap_d_mode 2> /dev/null || echo 4`

[ -f /root/basestation ] || exit 0

# Autostart the basestation service

start() {
  	if [ `fw_printenv -n hnap_bs_autostart` = 1 ]
	then
		printf "Starting basestation application"
		/root/./basestation -t -$TAP_TXG -g $TAP_RXG &
	fi
  	if [ `fw_printenv -n hnap_bs_autostart` = 2 ]
	then
		printf "Starting client application"
		/root/./client -d $TAP_D -u $TAP_U &
	fi
	echo "done"
}


stop() {
	printf "Stopping basestation: "
	killall basestation
	killall client
	echo "done"
}

restart() {
	stop
	start
}

# See how we were called.
case "$1" in
  start)
	start
	;;
  stop)
	stop
	;;
  restart|reload)
	restart
	;;
  *)
	echo "Usage: $0 {start|stop|reload|restart}"
	exit 1
esac

exit $?
