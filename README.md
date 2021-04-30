# Project2-TCP-Jumana-Lauren

To run:

./rdt_sender 127.0.0.1 6000 send_file.txt
./rdt_receiver port send_file.txt

Our simplified TCP sender has three main loops, which it enters and breaks out of at the appropriate times: Sending packets, Slow Start, and Congestion Avoidance. All timeout resending is handled by the resender() function, aided by the start_timer() and stop_timer() functions. During both Slow Start and Congestion Avoidance mode, while the sender is waiting to receive an ACK, duplicate ACKS are stored in the ACK_buffer. Each time an ACk is recieved, we loop through this ack_buffer to check if there are 2 ACKs matching the current one. If so, the packet is resent. Finally, We implemented a timer function in the receiever that functions in the same way as in the sender, so that if the receiver does not receieve a packet it sends another ACK.



