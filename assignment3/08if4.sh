#!/bin/sh
fname=./rbtree.c
if [ -f $fname ]
then
	echo "look! I found this! \(^.^)/ "
	head -5 $fname
else
	echo "there is no file named as '$fname' /(T.T)\ "
fi
exit 0
