#include <stdio.h>
#include <math.h>
#include "unpifiplus.h"
#include <sys/time.h>
#include <setjmp.h>
#define INITIAL_SEQ_NO 1


/* define RTT related macros */
#define RTT_RXTMIN 1000
#define RTT_RXTMAX 3000
#define RTT_MAXNREXMT 12



/*****************************************VARIABLES FOR CHILD PROCESS*****************************************/
sigjmp_buf jmpbuf;

struct tcp_header{
    uint32_t    seq_num;
    uint32_t    ack_num;
    uint16_t     ack;
    uint16_t     syn;
    uint16_t     fin;
    uint32_t    rcv_wnd_size;
};

// datagram payload : size 512 bytes
struct tcp_segment{
    struct tcp_header head;
    char data[368];
};

struct win_segment{
    struct tcp_segment segment;
    struct win_segment *next_segment;         /* next segment in the window */
    uint32_t no_of_sends;                     /* no of retransmission */
    uint64_t time_of_send;                    /* time msec since LINUX initial time */
    uint32_t no_of_ack_rcvd;                  /* no of ack received for segement */
};

struct sliding_win
{
    uint32_t advt_rcv_win_size;             /* receiving window size */
    uint32_t cwin_size;                     /* congestion window size */
    struct win_segment *buffer_head;        /* sliding window head pointer */
    uint32_t no_of_seg_in_transit;          /* no of segments in transit */
    uint32_t no_ack_in_rtt;                 /* no of acknowledgement received in an RTT */
    uint32_t ssthresh;                      /* slow start threshold */
    uint32_t fin_ack_received;
};

struct rtt_info
{
	int 				rtt;			/* current recent rtt in msec */
	int 				srtt;			/* smoothed rtt estimator multiplied by 8 */
	int 				rttvar; 		/* estimated mean deviator multiplied  by 4*/
	int 				rto;			/* current rto in msec */
};

struct win_probe_rto_info
{
    int win_probe_time_out;
};
/*****************************************VARIABLES FOR CHILD PROCESS*****************************************/




/*****************************************VARIABLES FOR PARENT PROCESS*****************************************/
struct client{
	struct client* next_client;
	uint32_t ip;
	uint16_t port_num;
	int pipefd;
	pid_t pid;
};

struct client *head;

 /***********************************VARIABLES FOR PARENT PROCESS*************************************/



 /***********************************VARIABLES COMMON*************************************/
struct nw_interface{
	int 				sockfd;	        // socket file descriptor
	struct	sockaddr  	*ifi_addr;		//IP address bound to socket
	struct	sockaddr  	*ifi_maskaddr;  //network mask
	struct	sockaddr	*ifi_subaddr;	//subnet address
};
 /***********************************VARIABLES COMMON*************************************/



 /***********************************CLIENT QUEUE HANDLING FUNCTIONS*************************************/
struct client* is_client_present(struct client* r,uint32_t ip,uint16_t port_num);

struct client* create_cli(int32_t ip,uint16_t port_num,int pipefd,pid_t pid);

void add_cli(struct client** r,int32_t ip,uint16_t port_num,int pipefd,pid_t pid);

void rm_cli(pid_t child_pid);
 /***********************************CLIENT QUEUE HANDLING FUNCTIONS*************************************/



/***********************************SLIDING WINDOW HANDLING FUNCTIONS*************************************/

void add_win_seg_to_sld_win(struct sliding_win* sld_win_buff, struct win_segment* win_seg);

void send_tcp_segment(struct sliding_win* sld_win_buff_ptr, struct win_segment *win_seg_ptr, int conn_sock_fd, struct sockaddr_in* cliaddr, struct rtt_info *rtti_ptr, struct win_probe_rto_info* win_prob_rti_ptr, int is_set_alarm_req);

void remove_win_seg_from_sld_win(struct sliding_win* sld_win_buff, struct win_segment* win_seg_ptr);
/***********************************SLIDING WINDOW HANDLING FUNCTIONS*************************************/



/***********************************RTT RELATED FUNCTIONS*************************************/
int rtt_minmax(int rto);
void determine_rto(struct rtt_info* rtti_ptr);
void set_alarm(struct sliding_win *sld_win_buff, struct rtt_info* rtti_ptr, struct win_probe_rto_info* win_prob_rti_ptr);
void sig_alrm(int signo);
/***********************************RTT RELATED FUNCTIONS*************************************/



/***********************************FILE TRANSFER FUNCTION*************************************/

void file_transfer(char *filename, struct nw_interface* list,int num, int i, struct sockaddr_in *clientaddr,int winsize, int pipefd);
void process_client_ack(struct sliding_win *sld_win_buff, struct tcp_segment *tcp_seg_ptr,
                        struct rtt_info *rtti, struct win_probe_rto_info *win_probe_rti,
                        int conn_sock_fd, struct sockaddr_in* cliaddr);
void sig_child(int sig_no);
/***********************************FILE TRANSFER FUNCTION*************************************/


int main()
{

	int serv_port_num, advt_win_size;
	int i = 0;
	const char input_file[] = "server.in";
	char ip_str[16];
	int sockfd,maxfd, pid;
	struct ifi_info *ifi;
	struct sockaddr_in *sa = NULL, clientaddr, *clientaddr_ptr = NULL;
	fd_set rset;
	FD_ZERO(&rset);
	struct tcp_segment rcvd_seg;
    int no_of_interfaces = 0;

    /* Initilizing the client list*/
    head = NULL;

    FILE *file = fopen ( input_file, "r" ); //'r' is just for reading



    /* Get server input parameters*/
	if ( file != NULL )
	{
		char line [128];
		while ( fgets ( line, sizeof line, file ) != NULL )
		{
			if(i==0)
				sscanf(line, "%d", &serv_port_num);
			else
				sscanf(line, "%d", &advt_win_size);
			i++;
			//fputs ( line, stdout );
		}
		fclose ( file );
	}
	else
	{
		perror ( input_file ); // file open is wrong
	}



	for(ifi = get_ifi_info_plus(AF_INET,1); ifi != NULL; ifi = ifi->ifi_next)
		no_of_interfaces++;

    struct nw_interface nw_interfaces[no_of_interfaces];
    /* Print and bind all unicast IP interface at server*/
	i = 0;
	for(ifi = get_ifi_info_plus(AF_INET,1); ifi != NULL; ifi = ifi->ifi_next){
		if(ifi->ifi_flags != IFF_BROADCAST){ // make sure the program only bind to unicast IP addresses
			sockfd = Socket(AF_INET,SOCK_DGRAM,0);

			sa = (struct sockaddr_in *) ifi->ifi_addr;
			sa->sin_family = AF_INET;
			sa->sin_port = htons(serv_port_num);
			Bind(sockfd,(SA *)sa, sizeof(*sa));

			nw_interfaces[i].sockfd = sockfd;
			nw_interfaces[i].ifi_addr = ifi->ifi_addr;      //Obtain IP address
			nw_interfaces[i].ifi_maskaddr = ifi->ifi_ntmaddr;	//Obtain network mask

			//calculate the subnet address using bit-wise calculation
			//between IP address and its network mask
			struct sockaddr_in *ip = (struct sockaddr_in *)ifi->ifi_addr;
			struct sockaddr_in *ntm = (struct sockaddr_in *)ifi->ifi_ntmaddr;
			uint32_t ipint = ip->sin_addr.s_addr;
			uint32_t ntmint = ntm->sin_addr.s_addr;

			uint32_t subint = ipint & ntmint; //bit-wise calculation
			struct sockaddr_in sub;
			bzero(&sub,sizeof(sub));
			sub.sin_addr.s_addr = subint;
			sub.sin_family  = AF_INET;
			nw_interfaces[i].ifi_subaddr = (SA*)&sub;

			//now since everything inside nw_interfaces is obtained
			//let's print out the info in the required format
			printf("\nInterface No.%d is %s.\n",i+1,ifi->ifi_name);
			printf("IP address is %s.\n",Sock_ntop_host(nw_interfaces[i].ifi_addr, sizeof(*nw_interfaces[i].ifi_addr)));
			printf("Network mask is %s.\n",Sock_ntop_host(nw_interfaces[i].ifi_maskaddr, sizeof(*nw_interfaces[i].ifi_maskaddr)));
			printf("Subnet address is %s.\n",Sock_ntop_host(nw_interfaces[i].ifi_subaddr, sizeof(*nw_interfaces[i].ifi_subaddr)));
		}
		i++;
	}

 	while(1)
	{

		//First of all, let's find the biggest file descriptor
		maxfd = 0;
		for(i=0;i<no_of_interfaces;i++){
			FD_SET(nw_interfaces[i].sockfd,&rset);
			if(nw_interfaces[i].sockfd > maxfd)
				maxfd = nw_interfaces[i].sockfd;
		}
		maxfd = maxfd+1;

		if(select(maxfd,&rset,NULL,NULL,NULL) < 0)
		{
            if(errno == EINTR)
            {
				continue;
			}
			else
			{
        		fprintf(stdout, "\nError on select: %s\n", strerror(errno));
				exit(1);
			}
		}

		for(i=0; i < no_of_interfaces; i++)
		{

			socklen_t len = sizeof(*clientaddr_ptr);

			if(FD_ISSET(nw_interfaces[i].sockfd,&rset)){//if this FD is available, three-way handshake

				/* first handshake from client, requesting the file name*/
                clientaddr_ptr = malloc(sizeof(struct sockaddr_in));
				Recvfrom(nw_interfaces[i].sockfd, &rcvd_seg, 512, 0,(SA*)clientaddr_ptr,&len);

                memset(&clientaddr,0,sizeof(clientaddr));
                memcpy(&clientaddr,clientaddr_ptr,sizeof(*clientaddr_ptr));

				char filename[strlen(rcvd_seg.data) + 1];
				strcpy(filename,rcvd_seg.data);

				/* check if this client has been put onto the list, if yes tell corresponding child to trigger the
				   second handshake again*/

				struct client *node;
				char msg[] = "Client Requests File Again.";
				node = NULL;
				if((node = is_client_present(head,clientaddr.sin_addr.s_addr, clientaddr.sin_port)) != NULL)
				{
					if(write(node->pipefd,msg,strlen(msg)) < 0)
					{
						printf("\nError writing client existance check:%s\n",strerror(errno));
						exit(1);
					}
					printf("This client is already present in the processing queue...\n");
                    /* Enter the code to tell client to send the second handshake again*/
				}
				else
				{
                    fprintf(stdout, "\nFirst handshake received...\n");
                    fprintf(stdout, "File requested from client: \"%s\"\n", filename);
                    fprintf(stdout, "Client Port no: %d\n", ntohs(clientaddr.sin_port));
                    fprintf(stdout, "Client IP address: %s\n\n", inet_ntop(AF_INET, &clientaddr.sin_addr, ip_str, sizeof(ip_str))) ;

					int pipefd[2];
					if(pipe(pipefd) < 0)
					{
						printf("Pipe Error; %s\n", strerror(errno));
						exit(1);
					}

                    /* Create a chile to process client request*/
					if((pid = fork()) < 0)
					{
						printf("Fork Error; %s\n", strerror(errno));
						exit(1);
					}
					else if(pid > 0)
					{
                        /* parent process */
                        /* close read end of child process, the child will just read*/
						close(pipefd[0]);
						add_cli(&head, clientaddr.sin_addr.s_addr, clientaddr.sin_port, pipefd[1], pid);
					}
					else
					{
                        /* child process */
                        /* close write end of child process, the child will just read*/
						close(pipefd[1]); //
                        /* Intiate file transfer */
						file_transfer(filename,nw_interfaces,no_of_interfaces,i,&clientaddr,min(advt_win_size, ntohl(rcvd_seg.head.rcv_wnd_size)),pipefd[0]);
                        /*File transfer done, exit the process */
                        close(pipefd[0]);
                        exit(0);
					}

				}
			}
		}

	}

	return 0;
}



/********************************** start of sig_chld handling function *******************************/
void sig_child(int sig_no)
{
    pid_t pid;
	int stat;
    /* when the child server process complete the processing of the client, remove that client from processing list*/
	while( (pid = waitpid(-1, &stat, WNOHANG)) > 0)
	{
      rm_cli(pid);
	}
	return;
}
/********************************** end of sig_chld handling function *******************************/

/***********************************Start of file_transfer function*************************************/

/* Performs handshaking as well as file transfer*/
void file_transfer(char *filename, struct nw_interface* list,int no_of_interfaces,
					int n, struct sockaddr_in *clientaddr,int winsize, int pipefd){

	int i,islocal = 0,j;
	int on = 1;
	int conn_socket_fd;
	struct sockaddr_in conn_socket_addr_temp,conn_socket_addr;
	struct tcp_segment tcp_seg;
    uint64_t factor = 1000;
    int second_hshake_sent = 0;
    fd_set rset;
    char local_buffer[128];
    struct timeval wait_time;
    int maxfd = 0;
    uint32_t len;
    uint32_t curr_recv_wnd_size = 0;

    struct sliding_win sld_win_buff;
    FILE *file_req = NULL;
    uint32_t tcp_seg_seq_num = INITIAL_SEQ_NO;
    struct win_segment *win_seg_ptr = NULL;

    struct itimerval test_itime_val;

    int effective_win_size = 0;
    uint16_t is_file_read_complete;

    struct rtt_info rtti;
    sigset_t alarm_mask;
    struct timeval time_val;
    struct win_probe_rto_info win_prob_rti;
    int flight_size;

    /* check if the incoming client is local */
	for(i=0; i < no_of_interfaces; i++)
	{
		if(i != n)
		{
			close(list[i].sockfd); // closing other listening sockets
		}

		// Remeber all ip addr in struct nw_interface are generic
		uint32_t serip = (((struct sockaddr_in*)list[i].ifi_addr)->sin_addr.s_addr) &
						(((struct sockaddr_in*)list[i].ifi_maskaddr)->sin_addr.s_addr);

		uint32_t cliip = (clientaddr->sin_addr.s_addr) &
						(((struct sockaddr_in*)list[i].ifi_maskaddr)->sin_addr.s_addr);

		if((cliip ^ serip )== 0){
			if(setsockopt(list[i].sockfd,SOL_SOCKET, SO_DONTROUTE,&on,sizeof(on)) < 0){
				printf("Setsockopt listening socket error:%s\n",strerror(errno));
				exit(1);
			}

			islocal = 1;
		}
	}

	/* create the connection socket */
	conn_socket_fd = Socket(AF_INET,SOCK_DGRAM,0);


	if(islocal == 0)
	{
		printf("Client doesn't belong to local network...\n");

		/* Set this connection socket option to address reusable */
		if(setsockopt(conn_socket_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0 ){
			printf("Setsockopt connecting socket error:%s\n",strerror(errno));
			exit(1);
		}
	}
	else
    {
		printf("\nClient belongs to local network...\n");

		//Set this connection socket option to DONTROUTE.
		if(setsockopt(conn_socket_fd, SOL_SOCKET, SO_DONTROUTE, &on, sizeof(on)) < 0 ){
			printf("Setsockopt connecting socket error:%s\n",strerror(errno));
			exit(1);
		}
	}


	conn_socket_addr_temp = *((struct sockaddr_in *) list[n].ifi_addr);
	conn_socket_addr_temp.sin_family = AF_INET;
	conn_socket_addr_temp.sin_port = htons(0);

    /* bind the connection socket */
	Bind(conn_socket_fd,(SA *)&conn_socket_addr_temp, sizeof(conn_socket_addr_temp));


	len = sizeof(conn_socket_addr);
	if(getsockname(conn_socket_fd,(SA *)&conn_socket_addr,&len)<0){
		printf("\ngetsockname error:%s\n",strerror(errno));
		exit(1);
	}
	else
	{
		printf("\nThe IP address of child server  is: %s\n",Sock_ntop_host((SA *)&conn_socket_addr, sizeof(conn_socket_addr)));
		printf("The connection port number of child server is: %d\n", ntohs(conn_socket_addr.sin_port));
	}


	Connect(conn_socket_fd,(SA *)clientaddr,sizeof(*clientaddr));
	fprintf(stdout, "\nChild server process successfully connected to client...\n");




	len = sizeof(*clientaddr);
	/***************************************Handshaking processing intiated***********************************/
    while(1)
    {
        tcp_seg.head.ack = 1;
        tcp_seg.head.syn = 1;
        tcp_seg.head.seq_num = INITIAL_SEQ_NO;
        sprintf(tcp_seg.data, "%d", conn_socket_addr.sin_port);

        if(second_hshake_sent == 1)
        {
            if(send(conn_socket_fd, &tcp_seg, sizeof(tcp_seg), 0) == -1 )
            {
                fprintf(stderr, "\nError sending second handshake: %s\n", strerror(errno));
                exit(1);
            }
            fprintf(stdout, "\nSecond handshake sent successfully from connecting socket with new port no...\n");
        }

        if(sendto(list[n].sockfd, &tcp_seg, sizeof(tcp_seg), 0, (SA *)clientaddr, sizeof(*clientaddr)) == -1 )
        {
            fprintf(stdout, "\nError sending second handshake: %s\n", strerror(errno));
            exit(1);
        }

        fprintf(stdout, "\nSecond handshake sent successfully from listening socket...\n");
        second_hshake_sent = 1;
    read_again:
        memset(&wait_time, 0, sizeof(wait_time));
        FD_ZERO(&rset);
        FD_SET( pipefd, &rset);
        FD_SET( conn_socket_fd, &rset);

        wait_time.tv_sec = 1;
        wait_time.tv_usec = 300000;

        maxfd = max(conn_socket_fd, pipefd) + 1;
        if(select(maxfd, &rset, NULL, NULL, &wait_time) < 0)
        {
            fprintf(stderr, "\nError while select: %s\n", strerror(errno));
            exit(1);
        }

        if(FD_ISSET(pipefd, &rset))
        {
            if( read(pipefd, local_buffer, 128) < 0)
            {
                fprintf(stderr, "\nError while reading messsage from parent server through pipe:%s\n", strerror(errno));
            }
            fprintf(stdout,"\nMessage from parent server: %s\n", local_buffer);

            continue;
        }

        if( FD_ISSET(conn_socket_fd, &rset))
        {
            memset(&tcp_seg, 0, sizeof(tcp_seg));
            len = sizeof(*clientaddr);
            // recvfrom(conn_socket_fd, &tcp_seg, sizeof(tcp_seg), 0, (SA *)clientaddr, &len)
            if( recvfrom(conn_socket_fd, &tcp_seg, sizeof(tcp_seg), 0, NULL, &len)< 0 )
            {
                if(errno == 111 || errno == 146)
                 goto read_again;
                else
                {
                    fprintf(stderr, "\nError while receiving third handshake from client:%d\n", errno);
                    exit(1);
                }
            }
            curr_recv_wnd_size = tcp_seg.head.rcv_wnd_size;
            fprintf(stdout, "\nThird handshake from client received...\n");
            fprintf(stdout, "File transder will be initiated...\n");
            fprintf(stdout, "Received window size: %d\n", curr_recv_wnd_size);
            break;
        }
    }

    close(list[n].sockfd);
    /***************************************Handshaking processing complete***********************************/

    /***************************************Initiate File Transfer Processing**********************************************/
    /*Intialize variables related to file transfer*/

     /* Here intialization values are taken from steven's Unix Programming book */
    rtti.rtt = 0;
    rtti.rttvar = 3000; /* in fraction it would be 3/4 and since it is using msec scale thus after multiplication by 4
                        and millisec scale rttvar = 3000*/
    rtti.srtt = 0;
    rtti.rto = 3000;
    sld_win_buff.ssthresh  = 512;
    is_file_read_complete = 0;

    win_prob_rti.win_probe_time_out = 1500;

    sld_win_buff.advt_rcv_win_size = curr_recv_wnd_size;
    sld_win_buff.buffer_head = NULL;
    sld_win_buff.cwin_size = 1 ; /* SLOW START*/
    sld_win_buff.no_of_seg_in_transit = 0;


    if((file_req = fopen(filename, "r")) == NULL )
    {
        fprintf(stderr, "\nError opening requested file: %s\n", strerror(errno));
        exit(1);
    }
    fprintf(stdout, "\nFile: %s opened successfully...\n", filename);

    if(sigemptyset(&alarm_mask) == -1)
    {
        fprintf(stderr, "/nError while emptying the mask set: %s\n", strerror(errno));
        exit(1);
    }
    if(sigaddset(&alarm_mask, SIGALRM) == -1)
    {
        fprintf(stderr, "/nError while setting the SIGALRM in mask set: %s\n", strerror(errno));
        exit(1);
    }
    signal(SIGALRM, sig_alrm);
    while(1)
    {
        /* if all packets are received by reciver and all data is transferred - Mission Mayhem accomplished*/
        if(is_file_read_complete == 1 && sld_win_buff.no_of_seg_in_transit == 0 && sld_win_buff.fin_ack_received == 1)
        {
            fprintf(stdout, "\nFile transfer completed successfully!!!");
            fprintf(stdout,"Client acknowledged final data segement...\n");
            fprintf(stdout,"Connection is being closed...\n\n");
            win_seg_ptr = (struct win_segment*)malloc(sizeof (struct win_segment));
            memset(win_seg_ptr, 0, sizeof(struct win_segment));

            /* 0 sequence no is indicating connection close to client*/
            win_seg_ptr->segment.head.seq_num = 0;
            win_seg_ptr->segment.head.ack = 1;
            win_seg_ptr->segment.head.fin = 1;
            win_seg_ptr->next_segment = NULL;

            memset(&time_val, 0, sizeof(time_val));
            /* get current time */
            if(gettimeofday(&time_val, NULL))
            {
                fprintf(stderr, "\nError settign the timestamp for a tcp segment: %s\n", strerror(errno));
                exit(1);
            }
            /* first get the millisecond from second and microsecond elapsed since epoch */
            win_seg_ptr->time_of_send = (uint64_t)(time_val.tv_usec/factor) + (uint64_t)(time_val.tv_sec*factor);

            if(send(conn_socket_fd, &(win_seg_ptr->segment), 512, 0) == -1)
            {
                fprintf(stderr, "\nError while sending final acknowledgement: %s\n", strerror(errno) );
                exit(1);
            }

            fprintf(stdout, "Final segament informing connection close on server side sent...\n");
            close(conn_socket_fd);
            return;
        }

        if(sigsetjmp(jmpbuf, 1) != 0)
        {
                /* Process receiver deadlock by sending window probing message */
                if(sld_win_buff.no_of_seg_in_transit == 0 && sld_win_buff.advt_rcv_win_size == 0 && is_file_read_complete != 1 )
                {
                    fprintf(stdout, "\nReceiver window is locked...\n");

                    /* prepare window probe message */
                    struct win_segment probe_win_seg;
                    memset(&probe_win_seg, 0, sizeof(struct win_segment));
                    //tcp_seg_seq_num += 1;
                    probe_win_seg.segment.head.seq_num = 0;
                    probe_win_seg.next_segment = NULL;
                    memset(&time_val, 0, sizeof(time_val));
                    /* get current time */
                    if(gettimeofday(&time_val, NULL))
                    {
                        fprintf(stderr, "\nError setting the timestamp for a tcp segment: %s\n", strerror(errno));
                    }
                        /* first get the millisecond from second and microsecond elapsed since epoch */
                    probe_win_seg.time_of_send = (uint64_t)(time_val.tv_usec/1000) + (uint64_t)(time_val.tv_sec*1000);

                    /* send the probe window segement to the client */
                    if(send(conn_socket_fd, &(probe_win_seg.segment), sizeof(probe_win_seg.segment), 0) == -1)
                    {
                        fprintf(stderr, "\nError while sending the tcp segment with sequence no %d: %s\n", win_seg_ptr->segment.head.seq_num, strerror(errno) );
                        exit(1);
                    }
                    set_alarm(&sld_win_buff, &rtti, &win_prob_rti);
                    fprintf(stdout, "Window Probing segement sent...\n");
                }

                /* Normal time out use case */
                if(sld_win_buff.no_of_seg_in_transit != 0 && sld_win_buff.advt_rcv_win_size != 0)
                {

                    fprintf(stdout, "\nRetranmission time out happened...\n");

                    fprintf(stdout, "\nCurrent congestion window: %d\n", sld_win_buff.cwin_size);
                    fprintf(stdout, "Current congestion window threshold: %d\n", sld_win_buff.ssthresh);

                    fprintf(stdout, "\nSlow start threshold and congestion window size will be modified...\n");

                    flight_size = min(sld_win_buff.advt_rcv_win_size, sld_win_buff.cwin_size);
                    sld_win_buff.ssthresh = max(2, (int)(ceil(flight_size/2.0)) );
                    sld_win_buff.cwin_size = 1;
                    sld_win_buff.no_ack_in_rtt = 0;

                    fprintf(stdout, "New congestion window: %d\n", sld_win_buff.cwin_size);
                    fprintf(stdout, "New congestion window threshold: %d\n", sld_win_buff.ssthresh);

                    if(sld_win_buff.buffer_head != NULL)
                    {
                        if(sld_win_buff.buffer_head->no_of_sends == RTT_MAXNREXMT)
                        {
                            fprintf(stdout, "\nSegment %d has already been transmitted %d times,\n", sld_win_buff.buffer_head->segment.head.seq_num, RTT_MAXNREXMT);
                            fprintf(stdout, "File transfer connection is being dropped!!!\n");
                            close(conn_socket_fd);
                            return;
                        }
                        else
                        {
                            rtti.rto = rtt_minmax(rtti.rto*2);
                            fprintf(stdout, "\nRetransmitting the segment %d...\n", sld_win_buff.buffer_head->segment.head.seq_num);
                            fprintf(stdout, "New retransmission time out interval %d msec...\n", rtti.rto);
                            send_tcp_segment(&sld_win_buff, sld_win_buff.buffer_head, conn_socket_fd, clientaddr, &rtti, &win_prob_rti, 1);
                        }
                    }
                }
        }

        /* prevent the timeout to affect the sending of data and receving of acknowledgement */
        /* mask the sigalarm*/
        if(sigprocmask(SIG_BLOCK, &alarm_mask, NULL) == -1)
        {
            fprintf(stderr, "\nError while blocking the SIGALARM: %s\n", strerror(errno));
            exit(1);
        }
        /************************************* File read and segment send part start **********************************/

                    /* if file has been read completely => no need to read the file again*/
        if(is_file_read_complete == 1)
        {
            fprintf(stdout, "\nFile read is complete.\nNo new tcp segments will be trasmitted...\n");
            fprintf(stdout, "Old tcp segements may be retrasmitted, if required..\n");
        }
        else
        {
            /* Get the effectvie window size which represents no of segements that can be sent further */
            effective_win_size = min(sld_win_buff.cwin_size , sld_win_buff.advt_rcv_win_size ) - sld_win_buff.no_of_seg_in_transit;
            j=1;
            if(effective_win_size < 0)
                effective_win_size = 0;
            /* print window size information */
            fprintf(stdout, "\n Current congestion window size:%d\n", sld_win_buff.cwin_size );
            fprintf(stdout, "Advertised receiver window size:%d\n", sld_win_buff.advt_rcv_win_size);
            fprintf(stdout, "No of segment in transit: %d\n", sld_win_buff.no_of_seg_in_transit);
            fprintf(stdout, "Available window size for transmisson:%d\n", effective_win_size);


            while(j <= effective_win_size)
            {
                j++;
                /* prepare the window segment with tcp segement */
                win_seg_ptr = (struct win_segment*)malloc(sizeof (struct win_segment));
                memset(win_seg_ptr, 0, sizeof(struct win_segment));

                tcp_seg_seq_num += 1;
                win_seg_ptr->segment.head.seq_num = tcp_seg_seq_num;
                win_seg_ptr->next_segment = NULL;

                memset(&time_val, 0, sizeof(time_val));
                /* get current time */
                if(gettimeofday(&time_val, NULL))
                {
                    fprintf(stderr, "\nError settign the timestamp for a tcp segment: %s\n", strerror(errno));
                }
                    /* first get the millisecond from second and microsecond elapsed since epoch */
                win_seg_ptr->time_of_send = (uint64_t)(time_val.tv_usec/factor) + (uint64_t)(time_val.tv_sec*factor);



                /* read the requested file to create a tcp segement */
                if(fread(win_seg_ptr->segment.data, 1, sizeof(win_seg_ptr->segment.data), file_req) < sizeof(win_seg_ptr->segment.data))
                {
                    if(ferror(file_req))
                    {
                        fprintf(stderr, "\nError reading requested file: %s\n", strerror(errno));
                        exit(1);
                    }
                }

                /* If file read is over, set the is_file_read_complete indicator to true*/
                if(feof(file_req))
                {
                    win_seg_ptr->segment.head.fin = 1;
                    is_file_read_complete = 1;
                }

                /* add the tcp segment to sliding window buffer */
                add_win_seg_to_sld_win(&sld_win_buff, win_seg_ptr);

                /* send the added window segement to the client */
                send_tcp_segment(&sld_win_buff, win_seg_ptr, conn_socket_fd, clientaddr, &rtti, &win_prob_rti, 1);

                /* If file read is over, no need to read anymore */
                if(feof(file_req))
                {
                    fprintf(stdout, "\nFile read is complete.\nLast new tcp segments is being trasmitted\n");
                    fprintf(stdout, "Old tcp segements may be retrasmitted, if required in case of packet loss...\n");
                    break;
                }

            }
        }


            /************************************* File read and segment send part end **********************************/

            /* important processing complete: unmask the sigalarm*/
            if(sigprocmask(SIG_UNBLOCK, &alarm_mask, NULL) == -1)
            {
                fprintf(stderr, "\nError while blocking the SIGALARM: %s\n", strerror(errno));
                exit(1);
            }

            /********************************** ack read part start ******************************************************/
            memset(&tcp_seg, 0, sizeof(tcp_seg));
            fprintf(stdout, "\nWaiting to receive an acknowledgement\n...");


            memset(&test_itime_val, 0, sizeof(test_itime_val));

            getitimer(ITIMER_REAL, &test_itime_val);
            fprintf(stdout, "Remaining time left in timeout: %lu sec - %lu microsec...\n", test_itime_val.it_value.tv_sec, test_itime_val.it_value.tv_usec);

            if( recvfrom(conn_socket_fd, &tcp_seg, sizeof(tcp_seg), 0, (SA *)clientaddr, &len) < 0 )
            {
                fprintf(stderr, "\nError while receiving an acknowledgement from client:%s\n", strerror(errno));
                exit(1);
            }

            if(tcp_seg.head.ack == 1)
            {
                fprintf(stdout, "\nReceived an acknowledgement from client\n");
                fprintf(stdout, "Next sequance no. expected is:%d\n", tcp_seg.head.ack_num);
                process_client_ack(&sld_win_buff, &tcp_seg, &rtti, &win_prob_rti, conn_socket_fd, clientaddr);
                if(tcp_seg.head.fin == 1)
                {
                    sld_win_buff.fin_ack_received = 1;
                }
            }
        /********************************** ack read part end ********************************************************/
    }
    /***************************************Complete File Transfer Processing**********************************************/

}
 /********************************************completion of file_transfer function*****************************************************/



/**************************************** Start process_client_ack function ******************************************************************************/
void process_client_ack(struct sliding_win *sld_win_buff, struct tcp_segment *tcp_seg_ptr,
                        struct rtt_info *rtti, struct win_probe_rto_info *win_probe_rti,
                        int conn_sock_fd, struct sockaddr_in* cliaddr)
{

    struct win_segment *win_seg_lcl_ptr = sld_win_buff->buffer_head;
    struct win_segment prev_win_seg;
    struct timeval time_val;
    int flight_size;
    sld_win_buff->advt_rcv_win_size = tcp_seg_ptr->head.rcv_wnd_size;
    uint64_t factor = 1000;
    memset(&prev_win_seg, 0, sizeof(prev_win_seg));

    /* If sliding window head is null, and an ack is received
    then it must be the ack updating the advertised window from
    0 to a certain value and explicitely asking server to  send a tcp segment, if availble*/
    if( sld_win_buff->buffer_head == NULL)
    {
        return;
    }
    else
    {
        /* Remove all the sent segment till the sequence no specified in ack*/
        while(win_seg_lcl_ptr->segment.head.seq_num < tcp_seg_ptr->head.ack_num)
        {
            /* Implement congestion control protocol*/
            /* Increase the cwin size for every ack received in an RTT => exponential increase */
            if(sld_win_buff->cwin_size < sld_win_buff->ssthresh)
            {
                sld_win_buff->cwin_size += 1;
                fprintf(stdout, "\nCongestion control is in slow start phase...\n");
                fprintf(stdout, "After receving an ack during slow start phase, new congestion window: %d\n", sld_win_buff->cwin_size);
            }
            else
            {
                /* Congestion aviodance phase has been reached */
                /* Now congestion window will increase linerarly */
                fprintf(stdout, "\nCongestion control is in congestion aviodance phase...\n");
                sld_win_buff->no_ack_in_rtt++;
                if(sld_win_buff->no_ack_in_rtt%sld_win_buff->cwin_size == 0)
                {
                    sld_win_buff->cwin_size += 1;
                    fprintf(stdout, "After receving %d no of acknowledge in an RTT, new congestion window size: %d\n", sld_win_buff->no_ack_in_rtt, sld_win_buff->cwin_size);
                    /* To initiate next RTT processing */
                    sld_win_buff->no_ack_in_rtt = 0;
                }
                else
                {
                    fprintf(stdout, "Integer based calculaton is being used...\n");
                    fprintf(stdout, "Since no of ack received in current RTT %d is less than current congestion window size %d - no increase in cognestion window size...\n",sld_win_buff->no_ack_in_rtt, sld_win_buff->cwin_size);
                }
            }

            memset(&prev_win_seg, 0, sizeof(prev_win_seg));
            prev_win_seg = *(win_seg_lcl_ptr);
            /* remove the segment from the sliding window */
            fprintf(stdout, "\nSegment %d will be removed from the sliding window...\n", win_seg_lcl_ptr->segment.head.seq_num);
            remove_win_seg_from_sld_win(sld_win_buff, win_seg_lcl_ptr);
            if(sld_win_buff->buffer_head != NULL)
            {
                win_seg_lcl_ptr = sld_win_buff->buffer_head;
            }
            else
                break;
        }

        if(prev_win_seg.no_of_sends > 1)
        {
            /* do not recalculate RTO */
        }
        else
        {
           /* determine RTO based on current measured RTT */
            memset(&time_val, 0, sizeof(time_val));
            /* get current time */
            if(gettimeofday(&time_val, NULL))
            {
                fprintf(stderr, "\nError getting the current timestamp for RTT determination: %s\n", strerror(errno));
                exit(1);
            }

            rtti->rtt = (int)(time_val.tv_usec/factor + time_val.tv_sec*factor  - prev_win_seg.time_of_send);
            fprintf(stdout, "Measured RTT after receving an acknowledge from client: %d millisec\n", rtti->rtt);
            determine_rto(rtti);

            fprintf(stdout, "Measured RTO after receving an acknowledge from client: %d millisec\n", rtti->rto);
        }
        /* Fast retransmit/ Fast recovery processing for congestion control*/
        if(sld_win_buff->buffer_head != NULL)
        {
            if(sld_win_buff->buffer_head->segment.head.seq_num == tcp_seg_ptr->head.ack_num)
            {
                ++sld_win_buff->buffer_head->segment.head.ack_num;
                fprintf(stdout, "\nReceived %d duplicate acknowledgement for segment %d...\n", sld_win_buff->buffer_head->segment.head.ack_num, sld_win_buff->buffer_head->segment.head.seq_num);
                if( (sld_win_buff->buffer_head->segment.head.ack_num) == 3 )
                {


                    fprintf(stdout, "\nPerforming fast recovery...\n");
                    fprintf(stdout, "Current congestion window: %d\n", sld_win_buff->cwin_size);
                    fprintf(stdout, "Current congestion window threshold: %d\n", sld_win_buff->ssthresh);

                    /* slash the threshold to half of the current window size*/
                    flight_size = min(sld_win_buff->advt_rcv_win_size, sld_win_buff->cwin_size);
                    sld_win_buff->ssthresh = max(2, (int)(ceil(flight_size/2.0)) );
                    sld_win_buff->cwin_size = sld_win_buff->ssthresh;
                    /* again we are in congestion avoidance phase */
                    sld_win_buff->no_ack_in_rtt = 0;

                    fprintf(stdout, "\nNew congestion window: %d\n", sld_win_buff->cwin_size);
                    fprintf(stdout, "New congestion window threshold: %d\n", sld_win_buff->ssthresh);

                    fprintf(stdout, "\nRetrasmitting the segment after three duplicate acks...\n");

                    send_tcp_segment(sld_win_buff, sld_win_buff->buffer_head, conn_sock_fd, cliaddr, rtti, win_probe_rti, 1);

                }
            }
        }
        else
        {
            /* If sliding window is empty and received window size 0, it means a receiver window deadlock*/
            /* in such case set, persistance timer*/
            if(sld_win_buff->advt_rcv_win_size == 0)
            {
                fprintf(stdout,"\nReceiver window deadlock, persistant timer will be set...\n");
                set_alarm(sld_win_buff, rtti, win_probe_rti);
            }

        }
    }
}
/**************************************** Start process_client_ack function ******************************************************************************/



 /********************************************start of add_win_seg_to_sld_win function*****************************************************/
void add_win_seg_to_sld_win(struct sliding_win* sld_win_buff_ptr, struct win_segment* win_seg_ptr)
{
    struct win_segment* current_win_seg_ptr;

    if(sld_win_buff_ptr->buffer_head == NULL )
    {
        sld_win_buff_ptr->buffer_head = win_seg_ptr;
    }
    else
    {
        current_win_seg_ptr = sld_win_buff_ptr->buffer_head;
        while(current_win_seg_ptr->next_segment != NULL)
            current_win_seg_ptr = current_win_seg_ptr->next_segment;

        current_win_seg_ptr->next_segment = win_seg_ptr;
    }

    sld_win_buff_ptr->no_of_seg_in_transit += 1;
}
 /********************************************end of add_win_seg_to_sld_win function*****************************************************/


/************************************* start of remove_win_seg_from_sld_win function*************************************/
void remove_win_seg_from_sld_win(struct sliding_win* sld_win_buff, struct win_segment* win_seg_ptr)
{
    if(sld_win_buff->buffer_head == NULL)
        return;
    else
    {
        sld_win_buff->no_of_seg_in_transit--;
        fprintf(stdout, "No of segment in transit: %d\n", sld_win_buff->no_of_seg_in_transit);
        if(sld_win_buff->buffer_head->segment.head.seq_num == win_seg_ptr->segment.head.seq_num)
            sld_win_buff->buffer_head = sld_win_buff->buffer_head->next_segment;

        /* release the memory consumed by the window segment*/
        free(win_seg_ptr);
    }
}
/************************************* end of remove_win_seg_from_sld_win function*************************************/

 /********************************************start of send_tcp_segment function*****************************************************/
void send_tcp_segment(struct sliding_win* sld_win_buff_ptr, struct win_segment *win_seg_ptr,
                        int conn_sock_fd, struct sockaddr_in* cliaddr, struct rtt_info *rtti_ptr,
                        struct win_probe_rto_info* win_probe_rti_ptr, int is_set_alarm_req)
{
    win_seg_ptr->no_of_sends += 1;

    if(send(conn_sock_fd, &(win_seg_ptr->segment), 512, 0) == -1)
    {
        fprintf(stderr, "\nError while sending the tcp segment with sequence no %d: %s\n", win_seg_ptr->segment.head.seq_num, strerror(errno) );
        exit(1);
    }

    fprintf(stdout, "\nSegment sent with sequence no: %d\n", win_seg_ptr->segment.head.seq_num);
    fprintf(stdout, "No of time segement sent: %d \n", win_seg_ptr->no_of_sends);

    /* if it is an opening segment for sliding window, set the alarm,
       which will be used for all segments sent in this RTT*/
    if(sld_win_buff_ptr->buffer_head->segment.head.seq_num == win_seg_ptr->segment.head.seq_num && is_set_alarm_req == 1)
    {
        set_alarm(sld_win_buff_ptr, rtti_ptr, win_probe_rti_ptr);
    }


}
 /********************************************end of send_tcp_segment function*****************************************************/



/********************************************start of rtt_minmax function*****************************************************/
int rtt_minmax(int rto)
{
    if (rto < RTT_RXTMIN)
        rto = RTT_RXTMIN;
    else if (rto > RTT_RXTMAX)
        rto = RTT_RXTMAX;

    return (rto);
}
/********************************************end of rtt_minmax function*****************************************************/



/********************************************start of determin_rto function*****************************************************/
void determine_rto(struct rtt_info* rtti_ptr)
{
    int measured_rtt = rtti_ptr->rtt;
    measured_rtt -= (rtti_ptr->srtt>>3);
    if(measured_rtt < 0)
        measured_rtt = 0 - measured_rtt;
    measured_rtt -= (rtti_ptr->rttvar>>2);
    rtti_ptr->rttvar += measured_rtt;
    rtti_ptr->rto = rtt_minmax((rtti_ptr->srtt>>3) + rtti_ptr->rttvar) ;
}
/********************************************end of determine_rto function*****************************************************/



/********************************************start of set_alarm function*****************************************************/
void set_alarm(struct sliding_win *sld_win_buff, struct rtt_info* rtti_ptr, struct win_probe_rto_info* win_prob_rti_ptr)
{
/*         Two alarms are implemented
 *         1. Persistance Alarm
 *         2. Transmission Time Out Alarm
 */

   struct itimerval itime_val;
   memset(&itime_val, 0, sizeof(itime_val));

/*    If sliding window header is NULL, then there are two possibilities. Either advertised window is 0, in which case
 *    Persistance alarm comes into the picture. If advertised window is not empty, then clear the existing timer as the
 *    timer will be set again while sending a new segment, in which case alarm will be set.
 */

   if(sld_win_buff->buffer_head == NULL)
   {
        if(sld_win_buff->advt_rcv_win_size == 0)
        {
            /* First get the seconds value by converting to microsec and then back to sec */
            itime_val.it_value.tv_sec = (int)((win_prob_rti_ptr->win_probe_time_out*1000)/1000000) ;
            /* Get the microseconds value, mod is required because previous sec calculation will omit fractional part */
            itime_val.it_value.tv_usec = (int)((win_prob_rti_ptr->win_probe_time_out*1000000)%1000000);

            fprintf(stdout, "Persistance timer interval set: %lu sec - %lu microsec\n...", itime_val.it_value.tv_sec, itime_val.it_value.tv_usec);

            if(setitimer(ITIMER_REAL, &itime_val, NULL) == -1)
            {
                fprintf(stderr, "\nError while setting persistance timer for window probing: %s\n", strerror(errno));
                exit(1);
            }
        }
        else
        {
            /* Clear the retransmission timer */
            if(setitimer(ITIMER_REAL, &itime_val, NULL) == -1)
            {
                fprintf(stderr, "\nError while clearing retransmission timer: %s\n", strerror(errno));
                exit(1);
            }
        }

   }
   else
   {    /* Set the timer based on current calculated RTO */
        /* First get the seconds value by converting to microsec and then back to sec */
            itime_val.it_value.tv_sec = (int)((rtti_ptr->rto*1000)/1000000) ;
            /* Get the microseconds value, mod is required because previous sec calculation will omit fractional part */
            itime_val.it_value.tv_usec = (int)((rtti_ptr->rto*1000000)%1000000);

            fprintf(stdout, "Retransmission timer interval set: %lu sec - %lu microsec\n...", itime_val.it_value.tv_sec, itime_val.it_value.tv_usec);

            if(setitimer(ITIMER_REAL, &itime_val, NULL) == -1)
            {
                fprintf(stderr, "\nError while setting persistance timer for window probing: %s\n", strerror(errno));
                exit(1);
            }
   }
}
/********************************************start of set_alarm function*****************************************************/


/******************************start og sig_alarm handling function*************************************/
void sig_alrm(int signo)
{

    siglongjmp(jmpbuf, 1);
}
/******************************start og sig_alarm handling function*************************************/
//***this is function to check whether a node is on the list
struct client* is_client_present(struct client* root,uint32_t ip,uint16_t port_num)
{
    if(root == NULL)
        return NULL;

    while(root != NULL)
    {
        if(root->ip == ip && root->port_num == port_num)
        {
            break;
        }
        root = root->next_client;
    }

    return root;

}


void rm_cli(pid_t child_pid)
{
    struct client *node = head, *prev_node = NULL;

    while(node != NULL)
    {
        if(node->pid == child_pid)
        {
            if(node == head)
            {
                head = node->next_client;
            }
            else
            {
                prev_node->next_client = node->next_client;
            }
            free(node);

        }
        prev_node = node;
        node = node->next_client;
    }
}

void add_cli(struct client **root,int32_t ip,uint16_t port_num,int pipefd,pid_t pid)
{
	struct client *client_node;
	if((*root)== NULL)
	{
		(*root) = create_cli(ip,port_num,pipefd,pid);
        return;
	}

	client_node = *root;

	while(client_node->next_client != NULL)
	{
		client_node = client_node->next_client;
	}

	client_node->next_client  = create_cli(ip,port_num,pipefd,pid);
    return;
}

struct client* create_cli(int32_t ip,uint16_t port_num,int pipefd,pid_t pid)
{
	struct client *node = (struct client*)malloc(sizeof(struct client));
	node->ip = ip;
	node->port_num = port_num;
	node->pipefd = pipefd;
	node->pid = pid;
	node->next_client = NULL;
	return node;
}



