OPERATING SYSTEMS - SUDOKU SERVER AND MULTI PLAYERS



TO COMPILE:
1. Open terminal
2. Execute:
    make 

TO CLEAN (remove .o files and executables):
1. Open terminal
2. Execute:
    make clean

TO EXECUTE:
1. Start server:
    ./server

2. On another terminal, start client:
    ./client client_config.txt


To play normal mode select 'n' 
and to play tournament select 'y' , 
note that server enters you in a tournament only
when the lobby is empty and starts when there are sufficient players 
in this case if there are 2 clients.

Client receives a puzzle and sends their solution.

Server verifies the solution and checks if it's correct.






