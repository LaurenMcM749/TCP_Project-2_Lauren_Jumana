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

#include"packet.h"
#include"common.h"

#define STDIN_FD    0
#define RETRY  120 //milli second 

//Server vars
int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
sigset_t sigmask;
int i;

//TCP vars
tcp_packet *sndpkt;
tcp_packet *recvpkt;
int next_seqno = 0;
int send_base = 0; //base window
int send_end = 0;  //base end
int oldest_unACKED = 0;
//cwnd = send_base + send_end
int cwnd = 10;
int eof_window_number = 0;     //What is this?
tcp_packet *window_buffer[10]; //What is this?
int slowstart = 0;
int cong_avoid = 0;
int dupACK = 0;
int ssthresh = 0;
//MSS_SIZE = 1500   


void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        //Resend all packets range between 
        //sendBase and nextSeqNum
        VLOG(INFO, "Timout happend");
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, 
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
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
    printf("in main");
   

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

    //init_timer(RETRY, resend_packets);
    next_seqno = 0;
    cwnd = 10;
    
    printf("outside while loop");
    while (1)
    {
        printf("in while loop");
        //------Send first packets---------
        for (int i = 0; i < cwnd; i++)
        {
            printf("in loop");
            len = fread(buffer, 1, DATA_SIZE, fp);
            //If return differs from DATA_SIZE, then error or end of file
            if (len <= 0)
            {
                VLOG(INFO, "End Of File has been reached");
                sndpkt = make_packet(0);
                sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (const struct sockaddr *)&serveraddr, serverlen);
                break;
            }
            send_base = next_seqno;
            next_seqno = send_base + len;
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = send_base;
        }
        //Send packet
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, (const struct sockaddr *)&serveraddr, serverlen) > 0)
        {
            VLOG(DEBUG, "Packet %d to %s. Window: %d, Base: %d, ACK: %d, EOF Window: %d", send_base, inet_ntoa(serveraddr.sin_addr), i, window_buffer[i]->hdr.seqno, window_buffer[i]->hdr.ackno, eof_window_number);
        }
        else
        {
            error("sendto");
        }
    
        //Start timer for ACK
        start_timer();
        //Next seq num = the ACK of the first packet ???
        next_seqno = window_buffer[0]->hdr.ackno;
        slowstart = 1;
        //-------------Slowstart------------------///
        //While (base < end && ACK = 0)
        while (slowstart == 1)
        {
            //-------Wait for ACK------------

            //------- Receive ACKS-----------

            //If recv ACK
            if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr, (socklen_t *)&serverlen) > 0)
            {
                //-----Process ACK--------

                //Take data from buffer --> turn it into recvpkt
                recvpkt = (tcp_packet *)buffer;

                //Check if size of recvpkt less than or equal to SIZE (If no - gives perror)
                assert(get_data_size(recvpkt) <= DATA_SIZE);

                //Stop timer for ACK once get ACK
                stop_timer();

                //If ACK does not equal the next seq num
                // ----- Resend packet --------
                if (recvpkt->hdr.ackno != next_seqno)
                {
                    //Resend packet
                    if (sendto(sockfd, window_buffer[0], TCP_HDR_SIZE + get_data_size(sndpkt), 0,(const struct sockaddr *)&serveraddr, serverlen) < 0)
                    {
                        error("sendto");
                    }

                    VLOG(DEBUG, "RESENDING: packet %d to %s, Base: %d, ACK: %d", send_base, inet_ntoa(serveraddr.sin_addr), window_buffer[0]->hdr.seqno, window_buffer[0]->hdr.ackno);
                }

                //If ACK was correct and DOES equal next seq number
                //---- Send next packets-----
                printf("recieved ackno: %d, expecting %d\n, last_window: %d", recvpkt->hdr.ackno, next_seqno, eof_window_number);
                // update oldest unACKed byte
                // cwnd = cwnd + MSS

                //Free memory of sndpkt, because got to receive buffer
                free(sndpkt);

                //TODO: Update window (Part 2)
                //update_window(window_buffer, fp);

                //TODO: Next seq number = seq number of the ACK
                next_seqno = window_buffer[0]->hdr.ackno;
                slowstart = 0;

            }
            //If do not recv ACK
            else
            {
                error("recvfrom");
            }
        }

    }

    return 0;
}



