#!/bin/sh
echo "Do you like to study...? (yes/no)"
read answer
case "$answer" in
	yes | y | Y | Yes | YES)
		echo "impressive"
		echo "keep going! \(^o^)/ ";;
	[nN]*)
		echo "hope you like this too... /(ToT)\ ";;
	*)
		echo "answer with yes or no please"
		exit 1;;
esac
exit 0
