#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 

int next_seqno=0;
int send_base=0;
int window_size = 1;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
tcp_packet *window_buffer[1000]; //What is this?
int slowstart = 0;
int eof_window_number = 0;     //What is this?
int cong_avoid = 0;
int dupACK = 0;
int ssthresh = 64;
int ack_buffer[1000];
int dupACK_index = 0;
//MSS_SIZE = 1500   



void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Set window_size = 1
        window_size = 1;
        //Set ssthresh 
        if ( (window_size/2) > 2)
        {
            ssthresh = (window_size/2);
        }
        else
        {
            ssthresh = 2;
        }
        //Continue in slow start
        slowstart = 1;
        VLOG(INFO, "Timeout happend. Window = %d, Ssthresh = %d. Resending packet...", window_size, ssthresh);
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
    }
}


void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}


void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}


/*
 * init_timer: Initialize timeer
 * delay: delay in milli seconds
 * sig_handler: signal handler function for resending unacknoledge packets
 */
void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;    // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;  
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


int main (int argc, char **argv)
{
    int portno, len;
    int next_seqno;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");


    /* initialize server server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    //Stop and wait protocol

    init_timer(RETRY, resend_packets);
    next_seqno = 0;
    window_size = 1;

    while (1)
    {
       //------Put data into buffer X window_size------///
        for (int i = 0; i < window_size; i++)
        {
        len = fread(buffer, 1, DATA_SIZE, fp);
        if ( len <= 0)
        {
            VLOG(INFO, "End Of File has been reached");
            sndpkt = make_packet(0);
            sendto(sockfd, sndpkt, TCP_HDR_SIZE,  0,(const struct sockaddr *)&serveraddr, serverlen);
            break;
        }
        //Send_base = 0
        send_base = next_seqno;
        //Next_seqno = 0 + num bytes in 1 packet
        next_seqno = send_base + len;
        //Make a packet with num_bytes
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        //Seq num = 0
        sndpkt->hdr.seqno = send_base;

        //Put packet into window_buffer
        window_buffer[i] = sndpkt;

       //------Send all packets in window---------
      
        //Send packet
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, ( const struct sockaddr *)&serveraddr, serverlen) > 0)
        {
            VLOG(DEBUG, "Sent packet %d to %s", send_base, inet_ntoa(serveraddr.sin_addr));
            
        }
        else
        {
            error("sendto");
        }
        }

        //Start timer for ACK
        start_timer();

        //If timeout - resend function

        slowstart = 1;
       
    while(slowstart==1)
    { 

        //-------Wait for ACK to ACK last packet (Cumulative ACK) ------------

        //------- Receive ACKS-----------

        //If recv ACK
        for(int i = 0; i< window_size; i++){
            if(recvfrom(sockfd, buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) > 0)
            {
                //-----Process ACK--------
                recvpkt = (tcp_packet *)buffer;
                printf("Received %d, ACK = %d \n", get_data_size(recvpkt),recvpkt->hdr.ackno);
                assert(get_data_size(recvpkt) <= DATA_SIZE); //If FALSE, then error
                //Put into ACK buffer then compare window-size - 1 with next seq no 
                ack_buffer[i]=recvpkt->hdr.ackno;
                stop_timer();

                //Check for dup ACKsS - If last ACK is same as this ACK
                if (ack_buffer[i-1] == ack_buffer[i])
                {
                    dupACK++;
                    if (dupACK == 2)
                    {   
                        printf("3 DupACKs - need to retransmit");
                        dupACK_index = i - 2;
                        //TODO - Test if this break to for loop - Jump out of for loop
                        break;
                    }
                }


            }
           // If do not recv ACK
            else 
            {
                printf("Did not get ACK b/c timeout\n");
                error("recvfrom");
            }
        }

            //If ACK of LAST PACKET does not equal the next seq num
            // ----- Resend packet --------
            if (ack_buffer[window_size - 1] != next_seqno)
            {
                //Set ssthresh 
                if ( (window_size/2) > 2)
                {
                    ssthresh = (window_size/2);
                }
                else
                {
                    ssthresh = 2;
                }
        
                printf("ACK = %d, Next_Seqno = %d \n",recvpkt->hdr.ackno, next_seqno);
                
                //TODO - Test if ACK_index works!
                if (dupACK == 3)
                {
                    window_size = 1;
                    //Resend packet
                    if (sendto(sockfd, window_buffer[0], TCP_HDR_SIZE + get_data_size(sndpkt), 0,(const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto");
                    }
                    VLOG(DEBUG, "RESENDING: packet %d to %s, Base: %d, ACK: %d", send_base, inet_ntoa(serveraddr.sin_addr), window_buffer[dupACK_index]->hdr.seqno, window_buffer[dupACK_index]->hdr.ackno);
                }

                //Resend packet
                if (sendto(sockfd, window_buffer[0], TCP_HDR_SIZE + get_data_size(sndpkt), 0,(const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }

                VLOG(DEBUG, "RESENDING: packet %d to %s, Base: %d, ACK: %d", send_base, inet_ntoa(serveraddr.sin_addr), window_buffer[0]->hdr.seqno, window_buffer[0]->hdr.ackno);
            }

            //If ACK was correct and DOES equal next seq number

            if (recvpkt->hdr.ackno == next_seqno)
            {
                printf("ACK = next_seqno \n");
                
                //Increase window_size
                window_size++;
                printf("window_size = %d\n", window_size);

                if (window_size == ssthresh)
                {
                    printf("Entering congestion avoidance\n");
                    cong_avoid = 1;

                }

                //Next seq number = seq number of the ACK
                //window_buffer is tcp_packet
                //tcp_packet has DATA and HEADER
                //HEADER has 
                //window_buffer is array of 10 TCP_PACKETS
                //Next seq number is first packet's ACK

                //next_seqno = window_buffer[0]->hdr.ackno;

                printf("Recieved ACK: %d, Next Seq_no: %d\n, last_window: %d", recvpkt->hdr.ackno, next_seqno, eof_window_number);
                // update oldest unACKed byte
                // send_base = 
               
                free(sndpkt);

                //TODO: Update window (Part 2)
                //update_window(window_buffer, fp);
                // window_size = window_size + DATA_SIZE

                //---- Send next packets-----

                //slowstart = 0;

            

            }

        }   
        // //If do not recv ACK
        // else 
        // {
        //     error("recvfrom");
        // }

    }

    // }

    return 0;

}

