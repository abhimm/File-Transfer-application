CC = gcc
CFLAGS = -I/home/abhinav/Documents/STONY_BROOK/FALL_2014/NETWORK_PROGRAMMING/ASSIGNMENT_2/unpv13e/lib/ -g -O2 -D_REENTRANT -Wall -D__EXTENSIONS__
LIBS = /home/abhinav/Documents/STONY_BROOK/FALL_2014/NETWORK_PROGRAMMING/ASSIGNMENT_2/unpv13e/libunp.a -lresolv -lnsl -lpthread -lm -lc



CLEANFILES = core core.* *.core *.o temp.* *.out typescript* \
                *.lc *.lh *.bsdi *.sparc *.uw


PROGS = client server

all:    ${PROGS}

get_ifi_info_plus.o: get_ifi_info_plus.c
	${CC} ${CFLAGS} -c get_ifi_info_plus.c

client:    client.o get_ifi_info_plus.o
	${CC} ${CFLAGS} -o client client.o get_ifi_info_plus.o ${LIBS} 

server:    server.o get_ifi_info_plus.o
	${CC} ${CFLAGS} -o server server.o get_ifi_info_plus.o ${LIBS}  

server.o: server.c
	${CC} ${CFLAGS} -c server.c

client.o: client.c
	${CC} ${CFLAGS} -c client.c


clean:
	rm -f ${PROGS} ${CLEANFILES}



