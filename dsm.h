#ifndef DSM_H
#define DSM_H

	extern struct sockaddr_in server,client;
	extern int serverNSD,clientSD;
	
void initializeDSM(int ismaster, char * masterip, int mport, char *otherip, int oport, int numpagestoalloc);
void * getsharedregion();
//void destroyDSM();
#endif
