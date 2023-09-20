#!/bin/sh
sum=0
i=1
while [ $i -le 10 ]
do
	sum=`expr $sum + $i`
	i=`expr $i + 1`
done
echo "sum of 1 to 10: "$sum
exit 0
