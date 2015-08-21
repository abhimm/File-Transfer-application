#include <string.h>
#include <math.h>
#include <stdio.h>
#include "unpifiplus.h"

//------------------------ALL STRUCT DEFINITIONS-----------------------
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
    struct tcp_segment tcp_seg;
    struct win_segment *next_seg;
    uint32_t no_of_sent;	//no use on client side
    uint32_t timeout;    	//no use on client side
};

struct buffer{
    uint32_t window_size_max;
    uint32_t window_size_avilable;
    struct win_segment *buffer_head;
    struct win_segment *buffer_tail;
    uint32_t last_consumed_seg_seq;
};

struct consumed_list{
	int item;
	struct consumed_list* next;
};

//------------------END OF ALL STRUCT DEFINITIONS-----------------------

//-------------------GLOBAL VARIABLES-----------------------------------
//For thread safe
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

//Make the root global, otherwise consumer thread can't see
struct win_segment *root_win_seg = NULL;
struct consumed_list *root_consumed_list = NULL;
uint32_t cur_win_size; //make current window size global, so that everyone can access it
uint32_t cur_ack_num = 2;
uint32_t actual_win_size;
uint32_t last_consumed_num = 1;
int isFin = 0;
//------------------END OF GLOBAL VARIABLES-----------------------------


//------------------ALL METHODS DECLEARATIONS---------------------------
/*Function declerations*/
struct tcp_segment* create_pac(uint32_t seq_num, uint32_t ack_num, uint8_t ack,
 uint8_t syn, uint8_t fin,uint32_t rcv_wnd_size, char *msg);//my faking construction method to creat a tcp_segment
void reset_pac(struct tcp_segment *tcp_seg,uint32_t seq_num, uint32_t ack_num, uint8_t ack,
 uint8_t syn, uint8_t fin,uint32_t rcv_wnd_size,char *msg);// reset items inside tcp_segment struct
struct win_segment* create_win_seg(struct tcp_segment* tcp_segg);

void produce_window(struct win_segment **,struct tcp_segment *);
void update_ack_win_size(struct win_segment* node);

int consume_from_root(struct win_segment** root);

/*Methods to maintein the consumed list*/
struct consumed_list* creat_consume_list_node(int item);
void add_consumed_list_node(struct consumed_list **root,int item);
struct consumed_list* is_consume_list(struct consumed_list *root,int item);
/*Methods to maintein the consumed list*/

int drop(float i);
static void * consumer_thread(void *);
/*End of function declerations*/
//----------------END OF ALL METHODS DECLEARATIONS----------------------

int main()
{
    FILE    *fp_param;
    char    *mode = "r";
    char    data[32];
    char    ip_str[16];
    int     opt_val_set = 1;
    int     counter, read_count,i;
    unsigned long check1, check2;
    char    test[32];
    int     is_server_local = 0;
    char    *separator = "-----------------------------------------------------";
    struct  param{
                char serv_ip_addr[16];
                short serv_port;
                char fname[32];
                int rcv_win_size;
                int rnd_seed;
                float p_loss;
                int mean;

            } input_param;
    struct sockaddr_in
            *clientaddr_ptr,
            *client_snet_mask_ptr,
            fin_cli_smask,
            fin_cliaddr,
            clientaddr,
            client_snet_mask,
            fin_cliaddr_prnt,
            servaddr_prnt,
            servaddr_1,
            servaddr;


    struct  ifi_info *ifi, *ifihead;
    int sockfd;
    socklen_t addr_size;
    struct tcp_segment *send_seg,*recv_seg;
    fd_set rset;
    struct timeval timeout;
    int maxfd;
    pthread_t tid;
    int *iptr;//this int[] is the argument of the consuming thread
				//iptr[0] = mean, iptr[1] = sockfd




// TEST VARIABLE
    int conn_res = 0;
    int sel_res;

 // Read the parameter file, client.in

    if((fp_param = fopen("client.in", mode)) == NULL )
    {
        fprintf(stderr, "Parameter File, client.in can't be opened\n");
        exit(1);
    }

    counter = 0;
    memset(&input_param,0,sizeof(input_param));

    while( (read_count = fscanf(fp_param, "%s", data )) != EOF)
    {
        if( read_count != 1 )
        {
            fprintf(stderr, "Input parameter are not maintained correctly in client.in \n");
            exit(1);
        }

        ++counter;

        switch(counter)
        {
            case 1:
                strcpy(input_param.serv_ip_addr, data);
                printf("IP address: %s\n", input_param.serv_ip_addr );
                break;
            case 2:
                input_param.serv_port = (short)atoi(data);
                printf("Port: %d\n", input_param.serv_port);
                break;
            case 3:
                strcpy(input_param.fname, data);
                printf("File name: %s\n", input_param.fname );
                break;
            case 4:
                input_param.rcv_win_size = atoi(data);
                actual_win_size = cur_win_size = input_param.rcv_win_size;
                printf("Window size: %d\n", cur_win_size );
                break;
            case 5:
                input_param.rnd_seed = atoi(data);
                printf("seed: %d\n", input_param.rnd_seed);
                break;
            case 6:
                input_param.p_loss = atof(data);
                printf("Probability: %f\n", input_param.p_loss );
                break;
            case 7:
                input_param.mean = atoi(data);
                iptr = malloc(sizeof(int));
                *iptr = input_param.mean;
                printf("Mean: %d\n", *iptr );
                break;
            default:
                fprintf(stderr, "Invalid no of parameters\n");
                exit(1);
        }
        memset(data, 0, sizeof(data));
    }
    // Prepare server address information
    //call srand to initialize the random function
    srand(input_param.rnd_seed);

    printf("IP ADDRESS LENGTH: %d\n",(int)strlen(input_param.serv_ip_addr));
    if( input_param.serv_port != 0 && input_param.serv_ip_addr != 0)
    {
        bzero(&servaddr, sizeof(servaddr));
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(input_param.serv_port);
        if(inet_pton(AF_INET, input_param.serv_ip_addr, &servaddr.sin_addr) != 1  )
        {
            fprintf(stderr, "Presentation to network address conversion error:%s\n", strerror(errno));
            exit(1);
        }
    }
    // Read interface infomration
    memset(&clientaddr,0,sizeof(clientaddr));
    memset(&fin_cliaddr,0,sizeof(fin_cliaddr));
    memset(&client_snet_mask,0,sizeof(client_snet_mask));
    memset(&fin_cli_smask,0,sizeof(fin_cli_smask));
    memset(&fin_cliaddr_prnt,0,sizeof(fin_cliaddr_prnt));
    memset(&servaddr_prnt,0,sizeof(servaddr_prnt));

    fprintf(stdout, "%s\n", separator);
    // Check local interfaces to determine client IP address
    fprintf(stdout, "Available Local Interface:\n");
    for(ifi = ifihead = Get_ifi_info_plus(AF_INET, 1); ifi != NULL ; ifi = ifi->ifi_next)
    {
        clientaddr_ptr = (struct sockaddr_in *)ifi->ifi_addr;
        client_snet_mask_ptr = (struct sockaddr_in *)ifi->ifi_ntmaddr;

        memcpy(&clientaddr,clientaddr_ptr,sizeof(*clientaddr_ptr));
        memcpy(&client_snet_mask,client_snet_mask_ptr,sizeof(*client_snet_mask_ptr));

        fprintf(stdout, "Intarface Name: %s\n", ifi->ifi_name );
        if(ifi->ifi_ntmaddr != NULL)
        {
            fprintf(stdout, "Subnet Mask: %s\n", sock_ntop_host((SA *)&client_snet_mask, sizeof(client_snet_mask)) );
        }
        if(ifi->ifi_addr != NULL)
        {
            fprintf(stdout, "IP Address: %s\n", sock_ntop_host((SA *)&clientaddr, sizeof(clientaddr)) );
        }
        fprintf(stdout, "%s\n", separator);
        // check if server belongs to same network
        check1 = client_snet_mask.sin_addr.s_addr & clientaddr.sin_addr.s_addr;
        check2 = client_snet_mask.sin_addr.s_addr & servaddr.sin_addr.s_addr;

        if( check1 == check2 )
        {
                is_server_local = 1; // set if a match is found
                if(fin_cli_smask.sin_addr.s_addr == 0 && fin_cliaddr.sin_addr.s_addr == 0)
                {
                    fin_cli_smask = client_snet_mask;
                    fin_cliaddr = clientaddr;
                }
                if(fin_cli_smask.sin_addr.s_addr < client_snet_mask.sin_addr.s_addr)
                {
                    fin_cli_smask = client_snet_mask;
                    fin_cliaddr = clientaddr;
                }
        }
    }
    // If server is not local, assign last entry in the ifi_info result as client address
    if(is_server_local == 0)
    {
        fprintf(stdout, "%s\n", separator);
        fprintf(stdout, "Info: server is not local; client will be assigned IP address randomly\n");
        fin_cli_smask = client_snet_mask;
        fin_cliaddr = clientaddr;
    }
    else
    {
        fprintf(stdout, "%s\n", separator);
        fprintf(stdout, "Info: server is local to the client network\n");
    }
    fprintf(stdout, "Chosen Subnet Mask: %s\n", inet_ntop(AF_INET, &fin_cli_smask.sin_addr, ip_str, sizeof(ip_str)));

    fprintf(stdout, "Chosen IPclient: %s\n", inet_ntop(AF_INET, &fin_cliaddr.sin_addr, ip_str, sizeof(ip_str))) ;

    fprintf(stdout, "Given IPserver: %s\n", inet_ntop(AF_INET, &servaddr.sin_addr, ip_str, sizeof(ip_str))) ;

    fprintf(stdout, "Server Port no: %d\n", ntohs(servaddr.sin_port) );

    // Create UDP socket
    if((sockfd = socket(AF_INET, SOCK_DGRAM,0)) == -1)
    {
        fprintf(stderr, "Socket creation error: %s\n", strerror(errno));
        exit(1);
    }
    // give sockfd to iptr[1], consumer thread has to send server a msg
    //when window size changes from 0 to available

    // set DONOTROUTE option if server is local
    if(is_server_local == 1)
    {
        if(setsockopt(sockfd, SOL_SOCKET, SO_DONTROUTE, &opt_val_set, sizeof(opt_val_set)) == -1)
        {
            fprintf(stderr, "Socket set option error: %s\n", strerror(errno));
            exit(1);
        }
    }
    // Bind UDP socket
    fin_cliaddr.sin_port = htons(0); /*assign 0 as the port no which we be replaced by kernel so other ephemeral port*/
    if((bind(sockfd, (SA*)(&fin_cliaddr), sizeof(fin_cliaddr))) == -1)
    {
        fprintf(stdout, "Socket bind error: %s\n", strerror(errno));
    }

    // Get assigned port no after bind
    addr_size = sizeof(fin_cliaddr_prnt);
    if(getsockname(sockfd, (SA*)(&fin_cliaddr_prnt), &addr_size) == -1)
    {
        fprintf(stderr, "Get socket name error: %s\n", strerror(errno));
        exit(1);
    }
    // Print ephemeral port no assigned to client
    fprintf(stdout, "%s\nClient address information after binding:\n", separator);
    fprintf(stdout, "Client IP address: %s\n", inet_ntop(AF_INET, &fin_cliaddr_prnt.sin_addr, ip_str, sizeof(ip_str))) ;
    fprintf(stdout, "Client ephemeral port no: %d\n", ntohs(fin_cliaddr_prnt.sin_port));

    // Connect socket to server

    if((conn_res = connect(sockfd, (SA *)&servaddr, sizeof(servaddr))) == -1 )
    {
        fprintf(stderr, "Socket connects to servaddr error: %d\n", errno);
        exit(1);
    }


    // Get server socket information
    if(getpeername(sockfd, (struct sockaddr*)(&servaddr_prnt), &addr_size) == -1)
    {
        fprintf(stdout, "Get server/peer info error: %s\n", strerror(errno));
    }
    fprintf(stdout, "%s\nServer address information after connect:\n", separator);
    fprintf(stdout, "Server IP address: %s\n", inet_ntop(AF_INET, &servaddr_prnt.sin_addr, ip_str, sizeof(ip_str))) ;
    fprintf(stdout, "Server port no: %d\n", ntohs(servaddr_prnt.sin_port));

    // Initiate Handshaking, create a seg for sending
    // and dont forget to give recv_seg a memory space
    char tempfilename[MAXLINE];
	strcpy(tempfilename, input_param.fname);
    printf("\nFile requested: %s\n", tempfilename);
    send_seg = create_pac(0,0,0,1,0,0,tempfilename);
    recv_seg = create_pac(0,0,0,0,0,0,NULL);

	//as a purpose to avoid confusion, let's use a while(1) exclusively
	//for the three way handshake, one thing to notice is, if the client
	//damps the 1st datagram 3 times in a row, then print out what's
	//going on, and exit the program

	counter = 0;
	while(1){
		if(drop(input_param.p_loss)){ // drop the very 1st datagram
			counter++;
			printf("The very 1st SYN is lost: No.%d.\n\n",counter);
			if(counter == 12){
				printf("Sending 1st SYN failed 3 times in row, exit!!!\n");
				exit(1);
			}
			continue;
		}else{ // the very 1st datagram not dropped
			counter = 0;
send_syn:		if((send(sockfd, send_seg, sizeof(*send_seg), 0)) < 0 ){
				fprintf(stderr, "\nSYN message for connecttion establishment failed: %s\n", strerror(errno));
				exit(1);
			}
			else{
				fprintf(stdout, "\nFirst handshaking message for connection establishment sent successfully\n");
			}
		}
		fprintf(stdout, "\nWating for second handshake from server\n");

    //The ACK from server is dropped
    if(drop(input_param.p_loss)){
		if(recv(sockfd, recv_seg, sizeof(*recv_seg), 0) < 0){
			fprintf(stderr, "\nError in receiving second handshake from server: %s\n", strerror(errno));
			exit(1);
		}
		printf("ACK from server is dropped. Client times out, a new SYN will be send agian...\n\n");
		goto send_syn;
	}else{//The ACK from server not dropped
recv_ack:	if(recv(sockfd, recv_seg, sizeof(*recv_seg), 0) < 0){
				fprintf(stderr, "\nError in receiving second handshake from server: %s\n", strerror(errno));
				exit(1);
			}
		 // second handshake received, update server port address
		servaddr_prnt.sin_port = (atoi(recv_seg->data));
		printf("\nnew port num  from server is %d,\n", servaddr_prnt.sin_port);
		if((conn_res = connect(sockfd, (SA *)&servaddr_prnt, sizeof(servaddr_prnt))) == -1 ){
			fprintf(stderr, "Socket connect error (line 368): %s\n", strerror(errno));
			exit(1);
		}
		fprintf(stdout, "Socket connect result: %d\n", conn_res);

		// Get server socket information
		if(getpeername(sockfd, (struct sockaddr*)(&servaddr), &addr_size) == -1){
			fprintf(stdout, "Get server/peer info error: %s\n", strerror(errno));
		}
		fprintf(stdout, "%s\nServer address information after reconnect:\n", separator);
		fprintf(stdout, "Server IP address: %s\n", inet_ntop(AF_INET, &servaddr.sin_addr, ip_str, sizeof(ip_str))) ;
		fprintf(stdout, "Server port no: %d\n", ntohs(servaddr.sin_port));
	}

		//The second ACK from client is dropped
		if(drop(input_param.p_loss)){
			printf("The response ACK of client(3rd handshake) is dropped, wait again for ACK from server...\n\n");
			goto recv_ack;
		}else{//The second ACK from client is not dropped
			reset_pac(send_seg,0,2,1,0,0,input_param.rcv_win_size,NULL);
			if((send(sockfd, send_seg, sizeof(*send_seg), 0)) < 0 ){
				fprintf(stderr, "\n3rd handshaking message sent failed: %s\n", strerror(errno));
				exit(1);
			}
			else{
				fprintf(stdout, "\nThird handshaking message for connection establishment sent successfully\n");
				break;
			}
		}

	}
   /*File transfer starts from here!!!*/
	printf("\n3 way Handshake finished,File transfer gets started......\n");





	/****************File transmission starts from here*******************************/
	//1st: create a consumer thread to consume buffer
    pthread_create(&tid,NULL,&consumer_thread,iptr);
    //2nd: I know Steve is the least professional programmer, but I need
    //the timer to time out the Recv method here
//    struct timeval tv;
//    tv.tv_sec = 10;
//    tv.tv_usec = 0;
//    setsockopt(sockfd,SOL_SOCKET,SO_RCVTIMEO,(char *)&tv,sizeof(struct timeval));
        isFin = 0; //flag to check if fin is received
    int isFull = 0;//flag to check if window is full

    //2nd: a while(1), which read the incoming data packet and simulate drop
	while(1){
		//the received packet
		if(drop(input_param.p_loss)){

			if(recv(sockfd, recv_seg, sizeof(*recv_seg), 0)==-1){
			//	if( errno == EAGAIN && errno == EWOULDBLOCK){
			//		printf("Client recv time out, retransmit an ack\n");
			//		goto ack;
			//	}else{
					printf("recv error:%s\n", strerror(errno));
					exit(1);
			//}
			}
			printf("Received datagram is dropped.\n\n");
                if((recv_seg->head.fin == 1)&&(recv_seg->head.seq_num == 0)){
                sleep(3);
                printf("Time wait compeleted\n");

				break;
			}

			continue;
		}else{
			reset_pac(recv_seg,0,0,0,0,0,0,NULL);

			if(recv(sockfd, recv_seg, sizeof(*recv_seg), 0)==-1){
			//	if( errno == EAGAIN && errno == EWOULDBLOCK){
			//		printf("Client recv time out, retransmit an ack\n");
			//		goto ack;
			//	}else{
					printf("recv error:%s\n", strerror(errno));
					exit(1);
			//}
			}
			printf("Received datagram is accepted.\n");
			printf("Seqence num is %d\n\n.",recv_seg->head.seq_num);


			//Lock the producer thread, since produce is gonna
			//put the received seg into window
			if(pthread_mutex_lock(&mutex) != 0){
				printf("Lock error:%s\n",strerror(errno));
				exit(1);
			}

			//get the seq_num and give to cur_ack_num
			if(cur_ack_num == recv_seg->head.seq_num)
			{
                cur_ack_num = recv_seg->head.seq_num + 1;

                //check if it's a fin with seq_num != 0
                if((recv_seg->head.fin == 1)&&(recv_seg->head.seq_num != 0)){ //last datagram
                    //printf("Last Payload received from server, send a fin back to server.\n");
                    //reset_pac(send_seg,0,cur_ack_num,1,0,1,0,NULL);// ack_num, ack and fin
                    isFin = 1;
                }
			}

            //real fin from server
			if((recv_seg->head.fin == 1)&&(recv_seg->head.seq_num == 0)){
                printf("Fin received\n");
                	//unlock this thread
                if(pthread_mutex_unlock(&mutex) != 0){
                    printf("Unlock error:%s\n",strerror(errno));
                    exit(1);
                }
				break;
			}
            //means the window is full, try to send
			//an ACK by all means
			if(cur_win_size == 0)
			{
                printf("\nWindow is full\n");
				isFull = 1;
				reset_pac(send_seg,0,0,0,0,0,0,NULL);// ack_num, ack and fin
                Send(sockfd,send_seg,sizeof(*send_seg),0);
                //unlock this thread
                if(pthread_mutex_unlock(&mutex) != 0)
                {
                    printf("Unlock error:%s\n",strerror(errno));
                    exit(1);
                }
				continue;
			}
			else
			{
               	isFull = 0;
			}

			produce_window(&root_win_seg,recv_seg);
			printf("Current window size is:%d.",cur_win_size);




			//unlock this thread
			if(pthread_mutex_unlock(&mutex) != 0){
				printf("Unlock error:%s\n",strerror(errno));
				exit(1);
			}
		}


		//Send the ACK
ack:	if(drop(input_param.p_loss)){
			printf("The responding ACK is dropped.\n\n");
			continue;
		}else{
			//the reason to lock this up is because, two threads share
			// cur_ack_num and cur_win_size
            printf("The responding ACK will be sent.\n\n");
			if(pthread_mutex_lock(&mutex) != 0){
				printf("Sending ACK lock error:%s\n",strerror(errno));
				exit(1);
			}

			if(!isFin && !isFull){ //happens when it's not a fin nor a fullwindow
				reset_pac(send_seg,0,0,0,0,0,0,NULL);
				reset_pac(send_seg,0,cur_ack_num,1,0,0,cur_win_size,NULL);
				printf("Sending ACK, ack_num is %d, and current window size is %d.\n",cur_ack_num,cur_win_size);
			}
			//if(isFull)
			//	printf("Recv Window is full, send an alert to server\n");
			if(isFin)
			{
                reset_pac(send_seg,0,cur_ack_num,1,0,1,0,NULL);// ack_num, ack and fin
                printf("Last datapayload received, send the response to server\n");
			}

			Send(sockfd,send_seg,sizeof(*send_seg),0);


			if(pthread_mutex_unlock(&mutex) != 0){
				printf("Sending ACK Unlock error:%s\n",strerror(errno));
				exit(1);
			}
		}
	}

	//file transfer finished
	printf("File transmission finished, Main thread JOB DONE!!\n");

	pthread_join(tid,NULL);
    return 0;
}


//--------------------------All THE SUB ROUTINES-------------------------

/*Consumer thread, consume items on the receiver window*/
static void * consumer_thread(void *arg){

	struct timeval tv;
	struct timespec ts;
	double sleeptime;
	int *mean = (int *)arg;

//	struct tcp_segment *send_pac = NULL;
//	send_pac = create_pac(0,0,0,0,0,0,NULL);
/*     if(pthread_detach(pthread_self()))
 *         err_sys("Error detaching the thread: %s\n", strerror(errno)) ;
 */

while(1){
    printf("mean: %d\n", *mean);
	sleeptime = -1 * (*mean) * log((double)rand()/(double)RAND_MAX);
	if(gettimeofday(&tv,NULL) < 0){
  		printf("Get day time error:%s\n",strerror(errno));
  		exit(1);
  	}
   	ts.tv_sec = tv.tv_sec;
   	ts.tv_nsec = tv.tv_usec * 1000;
   	ts.tv_sec += ceil(sleeptime/1000.0);



	//lock the thread
	if(pthread_mutex_lock(&mutex) != 0){
		printf("Lock error:%s\n",strerror(errno));
		exit(1);
	}
	//let this thread sleep for ts amount of time
	printf("\n----------Consumer Thread sleeps %f milliseconds--------\n", sleeptime);
	pthread_cond_timedwait(&cond,&mutex,&ts);

	//sleep(sleeptime/1000);
	printf("\n----------Consumer Thread wakes up--------\n");
	//Call the consume_from_root method to consume the win_seg inside
	//the window, also update the cur_win_size variable

	// returns a 1, means that last datagram has been received
	if(consume_from_root(&root_win_seg)){
//		reset_pac(send_pac,0,0,1,0,0,cur_win_size,NULL);
//		Send(*sockfd,send_pac,sizeof(*send_pac), 0);
		printf("\nThe last datagram has been consumed,consumer thread JOB DONE!!!\n");
        if(pthread_mutex_unlock(&mutex) != 0)
        {
            printf("Unlock error:%s\n",strerror(errno));

        }
		break;
	}


//	printf("\n----------Consuming process ended--------\n");
	//unlock this thread
	if(pthread_mutex_unlock(&mutex) != 0){
		printf("Unlock error:%s\n",strerror(errno));
		exit(1);
	}

}//end of while(1)

	return NULL;
}

/*This method consumers item on the window(win_segment linked list)   */
/*and update the global var cur_win_size, and add the comsumed seq_num*/
/*to the consumed list(struct consumed_list linked list)*/
/*return 1 if it's last datagram, otherwise 0*/
int consume_from_root(struct win_segment** root){
	int val = 0;
    if((*root) == NULL){
        printf("Nothing to consume at all!!\n");
        return val;
    }
    if((*root)->tcp_seg.head.seq_num - last_consumed_num != 1){

		printf("The first segment on window is not in order, can't consume\n");
		return val;
	}
	printf("there are things to consume!!\n");
    int last_item = 0,i = 1;
    struct win_segment *tempNode;
    while((*root)->next_seg != NULL){
        if((((*root)->tcp_seg.head.seq_num - last_item) != 1) && i != 1){
            printf("Item num not in sequence, stop consuming\n");
            return 0;
        }
        printf("The consumed sequence is %d,\nContent is \n\"%s\"\n\n",(*root)->tcp_seg.head.seq_num,(*root)->tcp_seg.data);
        add_consumed_list_node(&root_consumed_list,(*root)->tcp_seg.head.seq_num); // add this seq_num to the consumed_list
        last_item =(*root)->tcp_seg.head.seq_num;
        last_consumed_num = last_item;
        tempNode = (*root);
        i++;
        (*root) = (*root)->next_seg;
        free(tempNode);
        cur_win_size++;
        actual_win_size++;
    }
    if((*root)->tcp_seg.head.fin)
		val = 1;
    if(i==1){
	printf("Consumed only one item, No.%d,\nContent is \n\"%s\"\n",(*root)->tcp_seg.head.seq_num,(*root)->tcp_seg.data);
	add_consumed_list_node(&root_consumed_list,(*root)->tcp_seg.head.seq_num); // add this seq_num to the consumed_list
	last_consumed_num = (*root)->tcp_seg.head.seq_num;
    }
	else{
		if(((*root)->tcp_seg.head.seq_num - last_item) != 1)
			return 0;
		printf("The consumed sequence is %d,\ncontent is \n\"%s\"\n",(*root)->tcp_seg.head.seq_num,(*root)->tcp_seg.data);
		add_consumed_list_node(&root_consumed_list,(*root)->tcp_seg.head.seq_num); // add this seq_num to the consumed_list
		last_consumed_num = (*root)->tcp_seg.head.seq_num;
	}
    free((*root));
    (*root) = NULL;
    cur_win_size++;
    actual_win_size++;
    return val;
}


/*this sub basically just add tcp_segment to root_win_seg in order	   */
/*This sub runs on the main thread, also update cur_win_size global var*/
void produce_window(struct win_segment **root,struct tcp_segment *seg){
	//before putting anything to recv window, check if the incoming seg_num
	//has been on the consumed list, if so, then just return without
	//producing anything
	if(seg->head.seq_num == 0 && seg->head.fin != 1 )
        return;
	if(is_consume_list(root_consumed_list,seg->head.seq_num) != NULL){// on the list, just return
		return;
	}

	int item = seg->head.seq_num;
	struct win_segment *tempNode,*rootcopy;
    if((*root) == NULL){
        tempNode = create_win_seg(seg);
        (*root) = tempNode;
        actual_win_size--;
        if(cur_ack_num == item + 1)
            cur_win_size--;
        printf("Received segment No.%d put in receiving window\n",seg->head.seq_num);
        return;
    }

    //if the incoming data has the same seq_num as root, then just return
    if(item == ((*root)->tcp_seg.head.seq_num))
		return;
    if(item < ((*root)->tcp_seg.head.seq_num)){
        tempNode = create_win_seg(seg);
        tempNode->next_seg = (*root);
        (*root) = tempNode;
        actual_win_size--;
        if(cur_ack_num == item + 1)
            cur_win_size--;
        printf("Received segment No.%d put in receiving window\n",seg->head.seq_num);
        update_ack_win_size(tempNode);
        return;
    }
    if((*root)->next_seg == NULL){
        tempNode = create_win_seg(seg);
        (*root)->next_seg = tempNode;
        actual_win_size--;
        if(cur_ack_num == item + 1)
            cur_win_size--;
        printf("Received segment No.%d put in receiving window\n",seg->head.seq_num);
        update_ack_win_size(tempNode);
        return;
    }

    rootcopy = *root;
    struct win_segment *tnode;
    while(rootcopy->next_seg != NULL){
		//if the incoming data has the same seq_num as root, then just return
		if(item == (rootcopy->next_seg->tcp_seg.head.seq_num))
			return;
		if(item < (rootcopy->next_seg->tcp_seg.head.seq_num)){
			tempNode = create_win_seg(seg);
			tnode = rootcopy->next_seg;
			rootcopy->next_seg = tempNode;
			tempNode->next_seg = tnode;
            actual_win_size--;
            if(cur_ack_num == item + 1)
                cur_win_size--;
			printf("Received segment No.%d put in receiving window\n",seg->head.seq_num);
            update_ack_win_size(tempNode);
			return;
		}
		else{
			rootcopy = rootcopy->next_seg;
		}
    }
    tempNode = create_win_seg(seg);
    rootcopy->next_seg = tempNode;
    actual_win_size--;
    if(cur_ack_num == item + 1)
        cur_win_size--;
    printf("Received segment No.%d\n",seg->head.seq_num);
    update_ack_win_size(tempNode);
}

void update_ack_win_size(struct win_segment* node)
{
    if(node->tcp_seg.head.seq_num == cur_ack_num - 1)
    {   node = node->next_seg;
        while(node != NULL)
        {
            if(node->tcp_seg.head.seq_num == cur_ack_num)
            {
                cur_ack_num++;
                actual_win_size--;
                cur_win_size--;

                if(node->tcp_seg.head.fin == 1 && node->tcp_seg.head.seq_num != 0)
                    isFin = 1;

                node = node->next_seg;
            }
            else
            {
                break;
            }




        }
    }

}

/*Faking constructor to create a tcp_segment*/
struct tcp_segment* create_pac(uint32_t seq_num, uint32_t ack_num,
uint8_t ack, uint8_t syn, uint8_t fin,uint32_t rcv_wnd_size,char *msg){
	struct tcp_segment *node = malloc(sizeof(struct tcp_segment));
	node->head.seq_num = (seq_num);
	node->head.ack_num = (ack_num);
	node->head.ack = (ack);
	node->head.syn = (syn);
	node->head.fin = (fin);
	node->head.rcv_wnd_size = (rcv_wnd_size);
	if(msg!=NULL){
		strcpy(node->data,msg);
	}
	else
		node->data[0] = '\0';
	return node;
}

/*Faking constructor to create a win_segment*/
struct win_segment* create_win_seg(struct tcp_segment* tcp_segg){
	struct win_segment *node = malloc(sizeof(struct win_segment));
	node->tcp_seg = *tcp_segg;
	node->next_seg = NULL;
	return node;
}

/*Reset the content inside a tcp_segment*/
void reset_pac(struct tcp_segment *tcp_seg,uint32_t seq_num, uint32_t ack_num,
uint8_t ack, uint8_t syn, uint8_t fin,uint32_t rcv_wnd_size,char *msg){
	tcp_seg->head.seq_num = (seq_num);
	tcp_seg->head.ack_num = (ack_num);
	tcp_seg->head.ack = (ack);
	tcp_seg->head.syn = (syn);
	tcp_seg->head.fin = (fin);
	tcp_seg->head.rcv_wnd_size = (rcv_wnd_size);
	if(msg!=NULL){
		strcpy(tcp_seg->data,msg);
	}
	else
		tcp_seg->data[0] = '\0';
}

/*Faking constructor to create a consumed_list node*/
struct consumed_list* creat_consume_list_node(int item){
	struct consumed_list *node;
	node = malloc(sizeof(struct consumed_list));
	node->item = item;
	node->next = NULL;
	return node;
}

/*Method to add an item to a consumed_list(linked list)*/
void add_consumed_list_node(struct consumed_list **root,int item){
	struct consumed_list *tempNode,*nextNode;
    if((*root) == NULL){
        tempNode = creat_consume_list_node(item);
        (*root) = tempNode;
        return;
    }
    nextNode = *root;
    while(nextNode->next != NULL){
        nextNode = nextNode->next;
    }
    tempNode = creat_consume_list_node(item);
    nextNode->next = tempNode;
}

/*Method to check if a given item(seq_num) is on the consumed_list*/
/*if yes, return the addr or that item, otherwise return NULL*/
struct consumed_list* is_consume_list(struct consumed_list *root,int item){
	if(root==NULL)
		return NULL;
	if(root->item == item){
        return root;
    }else{
        while(root->next != NULL){
            root = root->next;
            if(root->item == item)
                return root;
        }
        return NULL;
    }
}


/*very simple function for probablistic test*/
/* return 1 means drop, 0 means not*/
int drop(float i){
	float tempf = (double)rand()/(double)RAND_MAX;
	printf("\n\n\nCurrent Probability is %f\n",tempf);
	if(tempf < i)	return 1;
	else return 0;
}
