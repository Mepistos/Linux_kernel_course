#!/bin/sh
# modification of 15while2.sh
# change while statement into until statement
sum=0
i=1
until [ $i -gt 10 ]
do
	sum=`expr $sum + $i`
	i=`expr $i + 1`
done
echo "sum of 1 to 1: "$sum
exit 0
