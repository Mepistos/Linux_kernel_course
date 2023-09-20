#!/bin/sh
sum=0
for i in 1 2 3 4 5 6 7 8 9 10
do
	sum=`expr $sum + $i`
done
echo "sum of 1 to 10: " $sum
exit 0
