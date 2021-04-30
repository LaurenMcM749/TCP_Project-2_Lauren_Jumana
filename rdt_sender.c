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
int dupACKbreak = 0;
int ackno= 0;
int freepkt = 1;
int dangerACK;
int ack_base = -1;
int ack;
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
        VLOG(INFO, "Timeout happend. Window = %d, Ssthresh = %d. Resending packet %d...", window_size, ssthresh,sndpkt->hdr.seqno);
        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0, ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }
        if (cong_avoid == 1){
            slowstart = 0;
        }
        if (slowstart == 1){
            cong_avoid = 0;
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

        //If still in slowstart, stay in slowstart
        if (cong_avoid == 0){
            slowstart = 1;
        }

        //If still in cong_avoid, stay in cong_aboid
        if (slowstart == 0){
            cong_avoid = 1;
        }

        //If reach sstrhesh -> enter congestion avoidance
        if (window_size == ssthresh)
        {  
            cong_avoid = 1;
            slowstart = 0;
        }

        freepkt = 1;
       
        while(slowstart==1)
        { 
            dupACKbreak = 0;

            //-------Wait for ACK to ACK last packet (Cumulative ACK) ------------

            //------- Receive ACKS-----------

            //If recv ACK and there are not 3 dupACK
                for(int i = 0; i< window_size; i++)
                {
                    if(recvfrom(sockfd, buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) > 0)
                    {

                        //-----Process ACK--------
                        recvpkt = (tcp_packet *)buffer;
                        printf("S - Received ACK = %d \n",recvpkt->hdr.ackno);                        
                        assert(get_data_size(recvpkt) <= DATA_SIZE); //If FALSE, then error

                        //Every ack, increase ack_base
                        ack_base++;

                        //Put ACK into buffer 
                        ack_buffer[ack_base]=recvpkt->hdr.ackno;

                        stop_timer();

                        
                        for(int j = 0; j < ack_base; j++){
                            //If ACK in buffer equal to current ACK, dupACK ++
                            if (ack_buffer[j] == ack_buffer[ack_base]){
                                dupACK ++;
                            }
                            if (dupACK == 2){
                                printf("3 DupACKs Detected: %d \n", ack_buffer[ack_base]);
                                dupACKbreak = 1;
                                ack_base++;
                                break;
                            }
                        }

                        if (dupACKbreak == 1) 
                        {
                            break;
                        }

                    }
                    
                   // If do not recv ACK
                    else 
                    {
                        printf("Did not get ACK b/c timeout\n");
                        error("recvfrom");
                    }
                }


                // ----- Resend packet --------
                if ( dupACKbreak == 1 )
                {
                    //printf("S - NO: ACK = %d, Next_Seqno = %d \n",recvpkt->hdr.ackno, next_seqno);

                    //1) If timeout - resend()
                    
                    //2) If 3 dup ACKs
                    if (dupACKbreak == 1)
                    {
                        dupACKbreak = 0;
                        dupACK = 0;

                        printf("3 DupACKs: %d\n",recvpkt->hdr.ackno);

                        //Identify packet to send
                        for(int x = 0; x < window_size+1; x++){
                            printf("S: %d\n", window_buffer[x]->hdr.seqno);
                            if( (window_buffer[x]->hdr.seqno) == (recvpkt->hdr.ackno)){
                                sndpkt = window_buffer[x];
                                printf("Need to send %d\n",sndpkt->hdr.seqno);
                                break;
                            }
                        }

                        //Resend packet
                        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,(const struct sockaddr *)&serveraddr, serverlen) > 0)
                        {
                            VLOG(DEBUG, "RESENDING: packet %d", sndpkt->hdr.seqno);
                        }
                        else 
                        {
                            error("sendto");
                        }

                         // Set window to 1
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

                        printf("Window_size = %d, SSthresh = %d\n", window_size, ssthresh);

                        slowstart = 0;
                    }

                }

                //If ACK was correct and DOES equal next seq number
                if (recvpkt->hdr.ackno == next_seqno)
                { 

                    printf("S - OK- ACK: %d, Next Seq_no: %d\n", recvpkt->hdr.ackno, next_seqno);

                    //Increase window_size
                    window_size++;
                    printf("Window_size = %d\n", window_size);

                   
                    if (freepkt == 1) 
                    {
                        free(sndpkt);
                        freepkt  = 0;
                    }


                    //---- Send next packets-----

                    cong_avoid = 0;
                    slowstart = 0;


                } 

        }

        while (cong_avoid == 1)
        {
            printf("Entered congestion avoidance\n");
            dupACKbreak = 0;
            dupACK = 0;

            //-------Wait for ACK to ACK last packet (Cumulative ACK) ------------

            //------- Receive ACKS-----------

            //If recv ACK and there are not 3 dupACK
                for(int i = 0; i< window_size; i++)
                {
                    if(recvfrom(sockfd, buffer, MSS_SIZE, 0,(struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) > 0)
                    {
                        //-----Process ACK--------
                        recvpkt = (tcp_packet *)buffer;
                        printf("CA - Received ACK = %d \n",recvpkt->hdr.ackno);                        
                        assert(get_data_size(recvpkt) <= DATA_SIZE); //If FALSE, then error
                          //Every ack, increase ack_base
                        ack_base++;

                        //Put ACK into buffer 
                        ack_buffer[ack_base]=recvpkt->hdr.ackno;

                        stop_timer();

                        
                        for(int j = 0; j < ack_base; j++){
                            //If ACK in buffer equal to current ACK, dupACK ++
                            if (ack_buffer[j] == ack_buffer[ack_base]){
                                dupACK ++;
                            }
                            if (dupACK == 2){
                                printf("3 DupACKs Detected: %d \n", ack_buffer[ack_base]);
                                ack = ack_buffer[ack_base];
                                dupACKbreak = 1;
                                break;
                            }
                        }


                        // //Check for dup ACKs
                        // if ((ack_buffer[ack_base + (i-1)] == ack_buffer[ack_base + i]) && (ack_buffer[ack_base + (i-2)] == ack_buffer[ack_base + i])) 
                        // {
                        //     printf("3 DupACKs Detected: %d \n", ack_buffer[ack_base + i]);
                        //     //dupACK_index = i - 2;
                        //     dupACKbreak = 1;
                        // }

                        ack_base++;

                        if (dupACKbreak == 1) 
                        {
                            break;
                        }

                    }
                   // If do not recv ACK
                    else 
                    {
                        printf("Did not get ACK b/c timeout\n");
                        error("recvfrom");
                    }
                }


                // ----- Resend packet --------
                if ((recvpkt->hdr.ackno != next_seqno) || (dupACKbreak == 1))
                {
                    printf("CA: ACK = %d, Next_Seqno = %d \n",recvpkt->hdr.ackno, next_seqno);

                    //1) If timeout - resend()
                    
                    //2) If 3 dup ACKs
                    if (dupACKbreak == 1)
                    {
                        dupACKbreak = 0;
                        dupACK = 0;

                        printf("3 DupACKs: %d\n",recvpkt->hdr.ackno);

                          //Identify packet to send
                        for(int x = 0; x < window_size + 1; x++){
                            printf("CA: %d\n", window_buffer[x]->hdr.seqno);
                            if( (window_buffer[x]->hdr.seqno) == (recvpkt->hdr.ackno)){
                                sndpkt = window_buffer[x];
                                printf("Need to send %d\n",sndpkt->hdr.seqno);
                                break;
                            }
                        }
    
                        //Resend packet
                        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,(const struct sockaddr *)&serveraddr, serverlen) > 0)
                        {
                            VLOG(DEBUG, "RESENDING: packet %d", sndpkt->hdr.seqno);
                        }
                        else 
                        {
                            error("sendto");
                        }


                        // Set window to 1
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

                        printf("Window_size = %d, SSthresh = %d\n", window_size, ssthresh);

                        cong_avoid = 0;
                        slowstart = 1;
                    }

                }

                //If ACK was correct and DOES equal next seq number
                if (recvpkt->hdr.ackno == next_seqno)
                { 

                    printf("CA: OK- Recieved ACK: %d, Next Seq_no: %d\n", recvpkt->hdr.ackno, next_seqno);

                    //Increase window_size
                    window_size = window_size + (1/window_size);
                    printf("Window_size = %d\n", window_size);

                 
                    if (freepkt == 1) 
                    {
                        free(sndpkt);
                        freepkt = 0;
                    }
                   
                    //---- Send next packets-----

                    cong_avoid = 0;
                    slowstart = 0;

                } 

        } //congavoid while

    } //while1

    // free(sndpkt);
    return 0; 

} //main

