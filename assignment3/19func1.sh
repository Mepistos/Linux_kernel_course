#!/bin/sh
myFunction () {
	echo "~function~"
	return
}
echo "function call"
myFunction
echo "terminate function"
exit 0
