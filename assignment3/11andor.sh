#!/bin/sh
echo "type the file name, which you want to check"
read fname
if [ -f $fname ] && [ -s $fname ] ; then
	head -5 $fname
else
	echo "there is no such file or file is empty."
fi
exit 0
