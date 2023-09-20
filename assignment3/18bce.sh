#!/bin/sh
echo "starting infinite loop. (b: break, c: continue, e: exit)"
while [ 1 ] ; do
	read input
	case $input in
		b | B)
			break;;
		c | C)
			echo "continue: back to condition of while statement"
			continue;;
		e | E)
			echo "exit: terminate p/g(function)"
			exit 1;;
	esac;
done
echo "break: escape from while loop."
exit 0
