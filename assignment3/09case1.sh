#!/bin/sh
case "$1" in
	start)
		echo "starting";;
	stop)
		echo "halt";;
	restart)
		echo "restarting";;
	*)
		echo "idk... :(";;
esac
exit 0
