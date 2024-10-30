default:
	@echo "Building default target"
	gcc -o scheduler scheduler.c -lpthread -lrt
	gcc -o shellsched shellsched.c -lpthread -lrt
	gcc -o fib fib.c
clean:
	@echo "Cleaning up"
	rm -f scheduler shellsched
# debug:
# 	@echo "Building debug target"
# 	gcc -o scheduler scheduler.c -lpthread -lrt -g
# 	gcc -o shellsched shellsched.c -lpthread -lrt -g
# 	gcc -o fib fib.c -g