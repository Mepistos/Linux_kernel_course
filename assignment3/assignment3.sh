#!/bin/sh
if [ "$1" -le 0 ] || [ "$2" -le 0 ]
then
	echo "Input must be greater than 0"
	exit 1
fi
echo "valid input"
temp=
for i in $(seq 1 $1)
do
	for j in $(seq 1 $2)
	do
		echo -n "$i * $j = "
		echo -n $(($i * $j))
		echo -n "\t"
	done
	echo ""
done
exit 0
