CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I. -I.. -I../..

.PHONY: build
build: all

all: aws

aws: aws.o sock_util.o http_parser.o

http_parser.o: http_parser.c 

aws.o: aws.c sock_util.h debug.h util.h aws.h http_parser.h

sock_util.o: sock_util.c sock_util.h debug.h util.h

.PHONY: clean
clean:
	-rm -f *.o
	-rm -f aws.o aws
	-rm -f sock_util.o
