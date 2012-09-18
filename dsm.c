#define _GNU_SOURCE

#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <fcntl.h>
#include<pthread.h>
#include<netinet/in.h>
#include<string.h>
#include<sys/socket.h>

#include "dsm.h"
#include <sys/time.h>

#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

#define ONE_PAGE 1
#define INVALID -2

void begin (void) __attribute__((constructor));
void end (void) __attribute__((destructor));


struct addressPageMappings
{
	long startAddress;
	int isValid; //0 -in valid, 1- valid
	int access;//R = 0, W = 1, NOT BEING ACCESSED = -1, INVALID = -2
	struct addressPageMappings *next;
};

static int isMaster;
static int numOfPagesToAlloc;
static int pagesize;
int serverSD,clientSD;
long pingedAddress;
char content[30];


int isReqForWrite;
int pageFetched;
int waitingForAPage;

void* buffer;
void* page;

struct addressPageMappings * AddressPageMappings;
struct addressPageMappings * currAddressPageMappings;

struct sockaddr_in server,client;

void getTime()
{
//	struct timeval tv;
//	struct tm* ptm;
//	char time_string[40];
//	long milliseconds;
//
//	/* Obtain the time of day, and convert it to a tm struct. */
//	gettimeofday (&tv, NULL);
//	ptm = localtime (&tv.tv_sec);
//	/* Format the date and time, down to a single second. */
//	strftime (time_string, sizeof (time_string), "%Y-%m-%d %H:%M:%S", ptm);
//	/* Compute milliseconds from microseconds. */
//	milliseconds = tv.tv_usec / 1000;
//	/* Print the formatted time, in seconds, followed by a decimal point and the milliseconds. */
//	printf ("%s.%03ld\n", time_string, milliseconds);
}

void changeAccessOfPageInPageTable(long startAddress, int access)
{
	currAddressPageMappings = AddressPageMappings;
	while(currAddressPageMappings != NULL)
	{
		//		printf("looking for: %ld, found: %ld\n",requestedAddress,currAddressPageMappings->startAddress);
		if((currAddressPageMappings->startAddress) == startAddress)
		{
			if(access == INVALID)
			{
//				printf("Changing Access of address to INVALID: %ld\n", currAddressPageMappings->startAddress);
				currAddressPageMappings->isValid = 0;
				currAddressPageMappings->access = -1;
			}
			else
			{
//				printf("Changing Access of address: %ld\n", currAddressPageMappings->startAddress);
				currAddressPageMappings->isValid = 1;
				currAddressPageMappings->access = access;
			}
			return;
		}
		currAddressPageMappings = currAddressPageMappings->next;
	}
	return; //requestedAddress not present
}

void mprotectPages(long startAddress, int numOfPages, int accessType)
{
	//accessType: -1: none, 0:read, 1:write
	int i;
	if(accessType == PROT_NONE)
	{
		 if(mprotect((void*)startAddress, pagesize * numOfPages, PROT_NONE) == -1)
			 handle_error("mprotect");
		 for(i = 0; i < numOfPages; i++)
			 changeAccessOfPageInPageTable(startAddress+(i*pagesize),-1);
	}
	else if(accessType == PROT_READ)
	{
		 if(mprotect((void*)startAddress, pagesize * numOfPages, PROT_READ) == -1)
			 handle_error("mprotect");
		 for(i = 0; i < numOfPages; i++)
			 changeAccessOfPageInPageTable(startAddress+(i*pagesize),0);
	}
	else if(accessType == PROT_WRITE)
	{
		if(mprotect((void*)startAddress, pagesize * numOfPages, PROT_READ | PROT_WRITE) == -1)
			 handle_error("mprotect");
		for(i = 0; i < numOfPages; i++)
			 changeAccessOfPageInPageTable(startAddress+(i*pagesize),1);
	}
}

void populateInitialPageTable()
{
	AddressPageMappings = NULL;
	int i, isSlave;
	if(isMaster)
		isSlave = 0;
	else
		isSlave = 1;
	i = 0;
	struct addressPageMappings * newAddressPageMappings = malloc(sizeof(struct addressPageMappings));
	AddressPageMappings = newAddressPageMappings;
	newAddressPageMappings->startAddress = i*pagesize;
	newAddressPageMappings->isValid = isMaster;
	newAddressPageMappings->access = -1;
	newAddressPageMappings->next = NULL;
	i++;

	for(; i < numOfPagesToAlloc/2; i++)
	{
		struct addressPageMappings * newAddressPageMappings = malloc(sizeof(struct addressPageMappings));
		newAddressPageMappings->startAddress = i*pagesize;
		newAddressPageMappings->isValid = isMaster;
		newAddressPageMappings->access = -1;
		newAddressPageMappings->next = AddressPageMappings;
		AddressPageMappings = newAddressPageMappings;
	}
	for(; i < numOfPagesToAlloc; i++)
	{
		struct addressPageMappings * newAddressPageMappings = malloc(sizeof(struct addressPageMappings));
		newAddressPageMappings->startAddress = i*pagesize;
		newAddressPageMappings->isValid = isSlave;
		newAddressPageMappings->access = -1;
		newAddressPageMappings->next = AddressPageMappings;
		AddressPageMappings = newAddressPageMappings;
	}
}

int isPageValid(long normalizedAddress)
{
	currAddressPageMappings = AddressPageMappings;
	while(currAddressPageMappings != NULL)
	{
//		printf("looking for: %ld, found: %ld\n",requestedAddress,currAddressPageMappings->startAddress);
		if((currAddressPageMappings->startAddress) == normalizedAddress)
		{
			if(currAddressPageMappings->isValid)
				return(1);
			break;
		}
		currAddressPageMappings = currAddressPageMappings->next;
	}
	return -1; //requestedAddress not present
}

void validateCurrPage(long normalizedAddress)
{
//	printf("in validateCurrPage\n");
	currAddressPageMappings = AddressPageMappings;
	while(currAddressPageMappings != NULL)
	{
//		printf("looking for: %ld, found: %ld\n",requestedAddress,currAddressPageMappings->startAddress);
		if((currAddressPageMappings->startAddress) == normalizedAddress)
		{
			currAddressPageMappings->isValid = 1;
			currAddressPageMappings->access = 1;
			break;
		}
		currAddressPageMappings = currAddressPageMappings->next;
	}
	return; //requestedAddress not present
}

void invalidateCurrPage(long normalizedAddress)
{
//	printf("in invalidateCurrPage\n");
	currAddressPageMappings = AddressPageMappings;
	while(currAddressPageMappings != NULL)
	{
//		printf("looking for: %ld, found: %ld\n",requestedAddress,currAddressPageMappings->startAddress);
		if((currAddressPageMappings->startAddress) == normalizedAddress)
		{
			currAddressPageMappings->isValid = 0;
			currAddressPageMappings->access = -2;
			break;
		}
		currAddressPageMappings = currAddressPageMappings->next;
	}
	return; //requestedAddress not present
}

int getPageAccess(long normalizedAddress)
{
	currAddressPageMappings = AddressPageMappings;
	while(currAddressPageMappings != NULL)
	{
//		printf("looking for: %ld, found: %ld\n",requestedAddress,currAddressPageMappings->startAddress);
		if((currAddressPageMappings->startAddress) == normalizedAddress)
		{
			if(currAddressPageMappings->isValid)
			{
				return currAddressPageMappings->access;
			}
			break;
		}
		currAddressPageMappings = currAddressPageMappings->next;
	}
	return -2; //requestedAddress not present
}

void getPage(long normalizedAddress)
{
//	printf("inside getpage\n");

	if(isMaster)
	{
		sprintf(content,"%s","page request");
//		printf("sending page Req for address: %lx from server to client\n", normalizedAddress);
		getTime();
		pageFetched = 0;
		send(serverSD,content,30,0);
		write(serverSD,&normalizedAddress,sizeof(long));
		write(serverSD,&isReqForWrite,sizeof(int));

		//Wait for the page to arrive
/*		while(pageFetched == 0)
		{
			waitingForAPage = 1;
		}
		waitingForAPage = 0;
		*/
	}
	else
	{
		sprintf(content,"%s","page request");
//		printf("content: %s\n",content);
//		printf("sending page Req for address: %lx from client to server\n", normalizedAddress);
		getTime();
		pageFetched = 0;
		send(clientSD,content,30,0);
		write(clientSD,&normalizedAddress,sizeof(long));
		write(clientSD,&isReqForWrite,sizeof(int));
		//Wait for the page to arrive
/*		while(pageFetched == 0)
		{
			waitingForAPage = 1;
		}
		waitingForAPage = 0;
		*/
	}
	return;
}

void preparePage()
{
	int access;//R = 0, W = 1, NOT BEING ACCESSED = -1
//	printf("in prepare page : pingedAddress: %ld",pingedAddress);
	if(isPageValid(pingedAddress) == -1)
	{
//		printf("Page %ld INVALID\n", pingedAddress/pagesize);
		exit(EXIT_FAILURE);
	}

	page = malloc(pagesize);

//	printf("address: %ld, access: %d\n",pingedAddress, isReqForWrite);
	access = getPageAccess(pingedAddress);

	if(access == -1)//NONE
	{
//		printf("To Write - Curr None\n");
		//Set read permissions
		mprotectPages(pingedAddress+(long)buffer, ONE_PAGE, PROT_READ);

		//READ CONTENTS
		memcpy(page,(void*)(pingedAddress+(long)buffer),pagesize);
	}
	else if(access == 0)//READ
	{
//		printf("To Write - Curr Read\n");

		//READ CONTENTS
		memcpy(page,(void*)(pingedAddress+(long)buffer),pagesize);
	}
	else if(access == 1)//WRITE
	{
//		printf("To Write - Curr Write\n");

		//revoke write permissions
		mprotectPages(pingedAddress+(long)buffer, ONE_PAGE, PROT_NONE);

		//Set read permissions
		mprotectPages(pingedAddress+(long)buffer, ONE_PAGE, PROT_READ);

		//READ CONTENTS
		memcpy(page,(void*)(pingedAddress+(long)buffer),pagesize);
	}
	//revoke read permissions
	mprotectPages(pingedAddress+(long)buffer, ONE_PAGE, PROT_NONE);

	invalidateCurrPage(pingedAddress);
	return;
}

void sendPage()
{
	int bytesSent;
    if(isMaster)
    {
//        printf("Sending Page from master\n");

        int size  = sizeof(client);
        sprintf(content,"%s","sending requested page");
//        printf("content: %s\n",content);
        send(serverSD,content,30,0);
        bytesSent= write(serverSD,page,pagesize);
//        printf("bytesSent from Master: %d\n", bytesSent);
    }
    else
    {
//        printf("Sending Page from slave\n");

        sprintf(content,"%s","sending requested page");
//        printf("content: %s\n",content);
        send(clientSD,content,30,0);
        bytesSent = write(clientSD,page,pagesize);
//        printf("bytesSent from Client: %d\n", bytesSent);
    }
}

void clientReceiveMethod (void *tempsd)
{
//	printf("in clientReceiveMethod\n");
	int sd = (int *)tempsd;
	clientSD = sd;
	int fetchedSize,j;
	for (;;)
	{
		int con=connect(clientSD,(struct sockaddr*)&client,sizeof(client));

		if(con==-1)
		{
			printf("\nConnection error\n");
			exit(EXIT_FAILURE);
		}
		else
		{
			int i=recv(clientSD,content,30,0);
			while(1)
			{
				if(strcmp(content,"page request")==0)
				{
//					printf("received  page request from server \n");
					getTime();
					read(clientSD,&pingedAddress,sizeof(long));
					read(clientSD,&isReqForWrite,sizeof(int));
					if(waitingForAPage == 0)
					{
						preparePage();
						sendPage();
					}
					else
					{
						//IGNORE
//						printf("************************Waiting for page  cannot proceed ***************\n");
//						exit(EXIT_FAILURE);
							while(waitingForAPage != 0){}
							
//							printf("waitingForAPage: %d \n",waitingForAPage);
							preparePage();
							sendPage();
					}
				}

				else if(strcmp(content,"sending requested page")==0)
				{
//					printf("received requested page from server\n");
					getTime();
					page = malloc(pagesize);
					fetchedSize = read(clientSD,page,pagesize);
					int * p=(int *) page;
					pageFetched = 1;
				}
				else{}
				sprintf(content,"%s","");
				i=recv(sd,content,30,0);
			}
		}
	}
}

void serverReceiveMethod(void * tempnsd)
{
//	printf("in serverReceiveMethod\n");

	int sd = (int *)tempnsd,nsd;
	int fetchedSize;
	int i=sizeof(client);
	for(;;)
	{
		nsd = accept(sd,((struct sockaddr *)&client),&i);
		serverSD = nsd;
//        printf("original server sd  %d\n",serverSD);
		if(nsd==-1)
		{
			printf("Check the description parameter\n");
			exit(EXIT_FAILURE);
		}
		getTime();
//		printf("Connection accepted!\n");

		int i = recv(serverSD,content,30,0);
		while(1)
		{
			if(strcmp(content,"page request")==0)
			{
//				printf("received page request from client\n");
				getTime();
				read(serverSD,&pingedAddress,sizeof(long));
				read(nsd,&isReqForWrite,sizeof(int));
				if(waitingForAPage == 0)
				{
					preparePage();
					sendPage();
				}
				else
				{
					//IGNORE
//					printf("************************Waiting for page  cannot proceed ***************\n");
					//exit(EXIT_FAILURE);
					while(waitingForAPage != 0){}
					preparePage();
					sendPage();
					
				}
			}
			else if(strcmp(content,"sending requested page")==0)
			{
				page = malloc(pagesize);
				fetchedSize = read(serverSD,page,pagesize);
//				printf("received requested page from client\n");
				pageFetched = 1;
				getTime();
			}
			else{}
			sprintf(content,"%s","");
			i=recv(serverSD,content,30,0);
		}
	}
}


void setUpConnection(int ismaster, char * masterip, int mport, char *otherip, int oport)
{
	int sd,bi;
	pthread_t thread;
	if(ismaster)
	{
		if((sd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))==-1)
		{
			printf("\nSocket creation error\n");
			exit(EXIT_FAILURE);
		}
//		printf("\nSocket created\n");
		bzero((char*)&server,sizeof(server));
		bzero((char*)&client,sizeof(client));
		server.sin_family=AF_INET;
		server.sin_port=htons(mport);
		server.sin_addr.s_addr =inet_addr(masterip);

		//server.sin_addr.s_addr=htonl(INADDR_ANY);
		bi=bind(sd,(struct sockaddr *)&server,sizeof(server));

		if(bi==-1)
		{
			printf("\nBind error, Port busy, Plz change port in client and server\n");
			exit(EXIT_FAILURE) ;
		}
		listen(sd,5);
		serverSD = sd;
//        printf("original server sd  %d\n",sd);
		if(pthread_create(&thread, NULL, serverReceiveMethod, (void *) sd))
			perror("receive error");
//		printf("sleeping till thread is created\n");
		sleep(2);
	}
	else
	{
		if((sd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))==-1)
		{
			printf("\nSocket problem\n");
			exit(EXIT_FAILURE);
		}

		bzero((char*)&client,sizeof(client));
		client.sin_family = AF_INET;
		client.sin_port=htons(mport);
		client.sin_addr.s_addr=inet_addr(masterip);

		//client.sin_addr.s_addr=htonl(INADDR_ANY);
		clientSD = sd; 
//        printf("original client sd: %d\n",sd);
        //TODO: wait till thread is created
		if(pthread_create(&thread, NULL, clientReceiveMethod, (void *) sd))
			perror("receive error");

//		printf("sleeping till thread is created\n");
		sleep(2);
	}
}

static void handler(int sig, siginfo_t *si, void *_context)
{
	long requestedAddress, normalizedAddress;
	ucontext_t *context = _context;
	int isWrite;
	int access;
	int offsetInFile;
	void* map;

//	isWrite = (int)context->uc_mcontext.gregs[REG_ERR]&2;
	isWrite = 2;
	sleep(1);
//	printf("context in handler: %d\n",isWrite);
	waitingForAPage = 0;
	if(isWrite == 0)
		access = PROT_READ;
	else if(isWrite == 2)
		access = PROT_WRITE;

	requestedAddress = (((long)si->si_addr) & ~(pagesize-1));
	normalizedAddress = requestedAddress-(long)buffer;

//	printf("Got SIGSEGV at address: %ld, page: %ld\n", (long) si->si_addr, normalizedAddress/pagesize);

	if(isPageValid(normalizedAddress) == -1)
	{
//		printf("Requested page not present\n");
		waitingForAPage = 1;
		getPage(normalizedAddress);
		while(pageFetched == 0){}
//		printf("\nGot the requested page. Setting up appropriate permissions.\n");
		validateCurrPage(normalizedAddress);
		mprotectPages(requestedAddress,ONE_PAGE,PROT_WRITE);
		memcpy((void*)requestedAddress, page, pagesize);
	
//		sendReceivedPageAck();
		waitingForAPage = 0;
	}
	else
	{
//		printf("Requested page present. Setting up appropriate permissions.\n");
		mprotectPages(requestedAddress,ONE_PAGE,access);
	}
}

void initializeDSM(int ismaster, char * masterip, int mport, char *otherip, int oport, int numpagestoalloc)
{
	waitingForAPage = 0;
	getTime();
//	printf("in initializeDSM\n");
	struct sigaction sa;

	pagesize = sysconf(_SC_PAGE_SIZE);
	if (pagesize == -1)
		handle_error("sysconf");

	isMaster = ismaster;
	numOfPagesToAlloc = numpagestoalloc;

	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = handler;
	if (sigaction(SIGSEGV, &sa, NULL) == -1)
		handle_error("sigaction");


	long dummyAddress = 2147483648;
	long dummyAddress2 = (((long)dummyAddress) & ~(pagesize-1));

	buffer = mmap((void*)dummyAddress2,numpagestoalloc * pagesize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON  ,-1, 0);

//	printf("Start of region:        %lx\n", (long) buffer);

	if (buffer == NULL)
		handle_error("memalign");

	memset(buffer, 0,numpagestoalloc * pagesize);

	mprotectPages((long)buffer,numpagestoalloc,PROT_NONE);

	populateInitialPageTable();
	getTime();
//	printf("SETTING CONNECTION \n");
	setUpConnection(ismaster,masterip,mport,otherip,oport);

	return;
}

void * getsharedregion()
{
//	printf("in getsharedregion\n");
	return buffer;
}

//TODO: DIsconnect all existing connections & close the port

void begin (void)
{
//	printf ("\nIn begin ()");
}

void destroyDSM()
{
//	printf ("\nIn destroyDSM\n");
	close(serverSD);
	close(clientSD);
//	printf ("\nAll sockets closed\n");
}
void end (void)
{
//	printf ("\nIn destructor\n");
	close(serverSD);
	close(clientSD);
}
