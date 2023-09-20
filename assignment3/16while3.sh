#!/bin/sh
echo "Enter your password."
read mypass
while [ $mypass != "1234" ]
do
	echo "incorrect, please re-enter."
	read mypass
done
echo "pass"
exit 0
