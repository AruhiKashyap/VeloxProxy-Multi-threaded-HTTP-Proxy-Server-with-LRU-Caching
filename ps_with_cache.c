// AUTHOR: Aruhi Kashyap



// include all the necessary header files
#include "proxy_parse.h"// for parsing the http request
#include <stdio.h>
#include <stdlib.h>//for malloc, free, exit ('Emergency Exits')
#include <string.h>// for string operations like strcpy, strcat, strcmp, strlen, bzero, bcopy
#include <time.h>// for time tracking in cache
#include <pthread.h>// for threading
#include <semaphore.h>// for semaphores to control concurrent connections
#include <sys/types.h>// for socket programming types
#include <sys/socket.h>// for socket programming
#include <netinet/in.h>// for sockaddr_in structure
#include <netdb.h>// for gethostbyname function
#include <arpa/inet.h>// for inet_addr, inet_ntoa functions
#include <unistd.h>// for close function
#include <fcntl.h>// for fcntl function
#include <sys/wait.h>// for wait function
#include <errno.h>// for errno variable


// Global Constants
#define CHUNK_SIZE 4096// size of buffer for reading/writing data
#define CONCURRENT_LIMIT 400// maximum number of concurrent connections allowed
#define TOTAL_CACHE_CAPACITY 200*(1<<20)// total cache size in bytes (200 MB)
#define SINGLE_ENTRY_LIMIT 10*(1<<20)// maximum size of a single cached entry

// to avoid writing struct Node multiple times, we use a typedef for cleaner code
typedef struct Node Node;


// Cache Node Structure
// Each node represents a cached response, linked in a list for eviction purposes
// content: the actual HTTP response data
// content_len: size of the response data
// key_url: the URL associated with this cached response
// last_access: timestamp of the last access for eviction policy
struct Node {
    char* content;
    int content_len;
    char* key_url;
    time_t last_access;
    Node* next_node;
};

// Global State Variables 
int service_port = 8080;// default port, can be overridden by command line argument
int master_socket;// socket file descriptor for the proxy server
pthread_t worker_threads[CONCURRENT_LIMIT];
sem_t flow_control;// semaphore to limit concurrent connections
pthread_mutex_t cache_guard;// mutex to protect cache access
Node* cache_root = NULL;// head of the linked list representing the cache
int current_cache_usage = 0;// tracks total bytes currently stored in cache

// Function Prototypes

Node* search_cache(char* url);// returns a pointer to the cache node if found, otherwise NULL
void evict_oldest_entry();// evicts the least recently accessed cache entry to free up space
int store_in_cache(char* data, int size, char* url);// stores a new response in the cache, evicting old entries if necessary, returns 1 on success, 0 on failure

int dispatch_error(int client_fd, int code);




// --- Networking Functions ---


// Helper to get the standard HTTP reason phrase
const char* get_reason_phrase(int mode) {
    switch(mode) {
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown Error";
    }
}

int dispatch_error(int socket, int status_code)
{
    char str[2048]; // Increased slightly for safety
    char body[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", &data);

    const char* status_text = get_reason_phrase(status_code);
    
    // If the status code isn't one we handle, exit early as per your original logic
    if (strcmp(status_text, "Unknown Error") == 0) {
        return -1;
    }

    // 1. Generate the HTML body
    // This allows us to calculate the length dynamically
    int body_len = snprintf(body, sizeof(body), 
        "<HTML><HEAD><TITLE>%d %s</TITLE></HEAD>\n"
        "<BODY><H1>%d %s</H1>%s\n</BODY></HTML>", 
        status_code, status_text, 
        status_code, status_text,
        (status_code == 403) ? "<br>Permission Denied" : "");

    // 2. Format the full response using your original header structure
    int total_len = snprintf(str, sizeof(str), 
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: text/html\r\n"
        "Date: %s\r\n"
        "Server: VaibhavN/14785\r\n\r\n"
        "%s", 
        status_code, status_text, body_len, currentTime, body);

    // 3. Log and Send
    printf("%d %s\n", status_code, status_text);
    send(socket, str, total_len, 0);

    return 1;
}

Node* search_cache(char* url) {
    pthread_mutex_lock(&cache_guard);
    Node* current = cache_root;
    while(current) {
        if(!strcmp(current->key_url, url)) {
            current->last_access = time(NULL);
            pthread_mutex_unlock(&cache_guard);
            return current;
        }
        current = current->next_node;
    }
    pthread_mutex_unlock(&cache_guard);
    return NULL;
}

void evict_oldest_entry() {
    if(!cache_root) return;
    Node *curr = cache_root;
    Node *victim_prev = NULL, *victim = cache_root;

    while(curr->next_node) {
        if(curr->next_node->last_access < victim->last_access) {
            victim = curr->next_node;
            victim_prev = curr;
        }
        curr = curr->next_node;
    }

    if(victim == cache_root) cache_root = cache_root->next_node;
    else victim_prev->next_node = victim->next_node;

    current_cache_usage -= (victim->content_len + sizeof(Node) + strlen(victim->key_url));
    free(victim->content);
    free(victim->key_url);
    free(victim);
}

int store_in_cache(char* data, int size, char* url) {
    pthread_mutex_lock(&cache_guard);
    int entry_size = size + strlen(url) + sizeof(Node);
    
    if(entry_size > SINGLE_ENTRY_LIMIT) {
        pthread_mutex_unlock(&cache_guard);
        return 0;
    }

    while(current_cache_usage + entry_size > TOTAL_CACHE_CAPACITY) {
        evict_oldest_entry();
    }

    Node* new_node =  (Node*) malloc(sizeof(Node));
    new_node->content = strdup(data);
    new_node->key_url = strdup(url);
    new_node->content_len = size;
    new_node->last_access = time(NULL);
    new_node->next_node = cache_root;
    cache_root = new_node;
    current_cache_usage += entry_size;

    pthread_mutex_unlock(&cache_guard);
    return 1;
}


// checkHTTPversion checks if the HTTP version in the request is either 1.0 or 1.1, returns 1 for valid versions and -1 for invalid versions
int checkHTTPversion(char *msg)
{
	int version = -1;

	if(strncmp(msg, "HTTP/1.1", 8) == 0)
	{
		version = 1;
	}
	else if(strncmp(msg, "HTTP/1.0", 8) == 0)			
	{
		version = 1;										// Handling this similar to version 1.1
	}
	else
		version = -1;

	return version;
}


int connectRemoteServer(char* host_addr, int port_num)
{
	// Creating Socket for remote server ---------------------------

	int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);

	if( remoteSocket < 0)
	{
		printf("Error in Creating Socket.\n");
		return -1;
	}
	
	// Get host by the name or ip address provided

	struct hostent *host = gethostbyname(host_addr);	
	if(host == NULL)
	{
		fprintf(stderr, "No such host exists.\n");	
		return -1;
	}

	// inserts ip address and port number of host in struct `server_addr`
	struct sockaddr_in server_addr;

	bzero((char*)&server_addr, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port_num);

	bcopy((char *)host->h_addr_list[0],(char *)&server_addr.sin_addr.s_addr,host->h_length);

	// Connect to Remote server ----------------------------------------------------

	if( connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0 )
	{
		fprintf(stderr, "Error in connecting !\n"); 
		return -1;
	}
	// free(host_addr);
	return remoteSocket;
}


// Helper function to handle the GET request by forwarding it to the remote server and relaying the response back to the client, while also caching the response for future requests

int handle_request(int clientSocket, struct ParsedRequest *request, char *tempReq)
{
	char *buf = (char*)malloc(sizeof(char)*CHUNK_SIZE);
	strcpy(buf, "GET ");
	strcat(buf, request->path);
	strcat(buf, " ");
	strcat(buf, request->version);
	strcat(buf, "\r\n");

	size_t len = strlen(buf);

	if (ParsedHeader_set(request, "Connection", "close") < 0){
		printf("set header key not work\n");
	}

	if(ParsedHeader_get(request, "Host") == NULL)
	{
		if(ParsedHeader_set(request, "Host", request->host) < 0){
			printf("Set \"Host\" header key not working\n");
		}
	}

	if (ParsedRequest_unparse_headers(request, buf + len, (size_t)CHUNK_SIZE - len) < 0) {
		printf("unparse failed\n");
		//return -1;				// If this happens Still try to send request without header
	}

	int server_port = 80;				// Default Remote Server Port
	if(request->port != NULL)
		server_port = atoi(request->port);

	int remoteSocketID = connectRemoteServer(request->host, server_port);

	if(remoteSocketID < 0)
		return -1;

	int bytes_send = send(remoteSocketID, buf, strlen(buf), 0);

	bzero(buf, CHUNK_SIZE);

	bytes_send = recv(remoteSocketID, buf, CHUNK_SIZE-1, 0);
	char *temp_buffer = (char*)malloc(sizeof(char)*CHUNK_SIZE); //temp buffer
	int temp_buffer_size = CHUNK_SIZE;
	int temp_buffer_index = 0;

	while(bytes_send > 0)
	{
		bytes_send = send(clientSocket, buf, bytes_send, 0);
		
		for(long int i=0;i<bytes_send/sizeof(char);i++){
			temp_buffer[temp_buffer_index] = buf[i];
			// printf("%c",buf[i]); // Response Printing
			temp_buffer_index++;
		}
		temp_buffer_size += CHUNK_SIZE;
		temp_buffer=(char*)realloc(temp_buffer,temp_buffer_size);

		if(bytes_send < 0)
		{
			perror("Error in sending data to client socket.\n");
			break;
		}
		bzero(buf, CHUNK_SIZE);

		bytes_send = recv(remoteSocketID, buf, CHUNK_SIZE-1, 0);

	} 
	temp_buffer[temp_buffer_index]='\0';
	free(buf);
	store_in_cache(temp_buffer, strlen(temp_buffer), tempReq);
	printf("Done\n");
	free(temp_buffer);
	
	
 	close(remoteSocketID);
	return 0;
}




//Thread function to handle each client connection

void* thread_fn(void* socketNew)
{
	sem_wait(&flow_control); 
	int p;
	sem_getvalue(&flow_control,&p);
	printf("flow control(semaphore) value:%d\n",p);
    int* t= (int*)(socketNew);
	int socket=*t;           // Socket is socket descriptor of the connected Client
	int bytes_send_client,len;	  // Bytes Transferred

	
	char *buffer = (char*)calloc(CHUNK_SIZE,sizeof(char));	// Creating buffer of 4kb for a client
	
	
	bzero(buffer, CHUNK_SIZE);								// Making buffer zero
	bytes_send_client = recv(socket, buffer, CHUNK_SIZE, 0); // Receiving the Request of client by proxy server
	
	while(bytes_send_client > 0)
	{
		len = strlen(buffer);
        //loop until u find "\r\n\r\n" in the buffer
		if(strstr(buffer, "\r\n\r\n") == NULL)
		{	
			bytes_send_client = recv(socket, buffer + len, CHUNK_SIZE - len, 0);
		}
		else{
			break;
		}
	}

	
	char *tempReq = (char*)malloc(strlen(buffer)*sizeof(char)+1);
    //tempReq, buffer both store the http request sent by client
	for (long int i = 0; i < strlen(buffer); i++)
	{
		tempReq[i] = buffer[i];
	}
	
	//checking for the request in cache 
	struct Node* temp = search_cache(tempReq);

	if( temp != NULL){
        //request found in cache, so sending the response to client from proxy's cache
		int size=temp->content_len/sizeof(char);
		int pos=0;
		char response[CHUNK_SIZE];
		while(pos<size){
			bzero(response,CHUNK_SIZE);
			for(int i=0;i<CHUNK_SIZE;i++){
				response[i]=temp->content[pos];
				pos++;
			}
			send(socket,response,CHUNK_SIZE,0);
		}
		printf("Data retrived from the Cache\n\n");
		printf("%s\n\n",response);
		// close(socketNew);
		// sem_post(&seamaphore);
		// return NULL;
	}
	
	
	else if(bytes_send_client > 0)
	{
		len = strlen(buffer); 
		//Parsing the request
		struct ParsedRequest* request = ParsedRequest_create();
		
        //ParsedRequest_parse returns 0 on success and -1 on failure.On success it stores parsed request in
        // the request
		if (ParsedRequest_parse(request, buffer, len) < 0) 
		{
		   	printf("Parsing failed\n");
		}
		else
		{	
			bzero(buffer, CHUNK_SIZE);
			if(!strcmp(request->method,"GET"))							
			{
                
				if( request->host && request->path && (checkHTTPversion(request->version) == 1) )
				{
					bytes_send_client = handle_request(socket, request, tempReq);		// Handle GET request
					if(bytes_send_client == -1)
					{	
						dispatch_error(socket, 500);
					}

				}
				else
					dispatch_error(socket, 500);			// 500 Internal Error

			}
            else
            {
                printf("This code doesn't support any method other than GET\n");
            }
    
		}
        //freeing up the request pointer
		ParsedRequest_destroy(request);

	}

	else if( bytes_send_client < 0)
	{
		perror("Error in receiving from client.\n");
	}
	else if(bytes_send_client == 0)
	{
		printf("Client disconnected!\n");
	}

	shutdown(socket, SHUT_RDWR);
	close(socket);
	free(buffer);
	sem_post(&flow_control);	
	
	sem_getvalue(&flow_control,&p);
	printf("Flow control(semaphore) post value:%d\n",p);
	free(tempReq);
	return NULL;
}



// --- Main Execution ---

int main(int argc, char* argv[]) {
    int client_socketID ,client_len;

    struct sockaddr_in client_info;
    struct sockaddr_in srv_addr;

    // Initialize synchronization primitives
    sem_init(&flow_control, 0, CONCURRENT_LIMIT);//initialize semaphore with the maximum number of concurrent connections
    pthread_mutex_init(&cache_guard, NULL);//initialize mutex for cache access control
    


    if(argc == 2)        //checking whether two arguments are received or not
	{
		service_port = atoi(argv[1]);
	}
	else
	{
		printf("Too few arguments\n");
		exit(1);
	}

    printf("Setting Proxy Server Port : %d\n",service_port);



// Set up the master socket for listening to incoming connections
    master_socket = socket(AF_INET, SOCK_STREAM, 0);
    if( master_socket < 0)
	{
		perror("Failed to create socket.\n");
		exit(1);
	}
    int opt = 1;// for setsockopt to reuse address
    if(setsockopt(master_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed\n");
    }

    bzero((char*)&srv_addr, sizeof(srv_addr));  
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_port = htons(service_port);// Assigning port to the Proxy
    srv_addr.sin_addr.s_addr = INADDR_ANY;// Any available address assigned
  
	
   // binding the master socket to the specified port and address

    if(bind(master_socket, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) < 0) 
    {
        perror("Bind failed--port is not free.\n");
        return 1;//exit(1);
    }
    printf("Binding on port: %d\n",service_port);

    // Start listening for incoming connections with a specified backlog limit
    int listen_status=listen(master_socket, CONCURRENT_LIMIT);
    if(listen_status < 0) 
    {
        perror("Listen failed--error while listening!\n");
        return 1;//exit(1);
    }
    printf("Proxy active on port %d\n", service_port);

    int client_descriptors[CONCURRENT_LIMIT];// to store client socket descriptors for worker threads of connected clients
    int idx = 0;// index to keep track of the next available slot in client_descriptors and worker_threads arrays
     

    // infinite loop to accept incoming client connections and dispatch them to worker threads for processing
    while(1) {
        bzero((char*)&client_info, sizeof(client_info));		
        client_len = sizeof(client_info);
        // Accept an incoming connection and obtain a new socket descriptor for communication with the client
        client_socketID= accept(master_socket, (struct sockaddr*)&client_info,(socklen_t*)&client_len);
        
        if(client_socketID <0) 
        {
			fprintf(stderr, "Error in Accepting connection !\n");
			exit(1);
		}
        else
        {
            client_descriptors[idx % CONCURRENT_LIMIT] = client_socketID;
            // Create a new thread to handle the client's request, passing the client socket descriptor as an argument
        }

        // Getting IP address and port number of client
		struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_info;
		struct in_addr ip_addr = client_pt->sin_addr;
		char str[INET_ADDRSTRLEN];										// INET_ADDRSTRLEN: Default ip address size
		inet_ntop( AF_INET, &ip_addr, str, INET_ADDRSTRLEN );
		printf("Client is connected with port number: %d and ip address: %s \n",ntohs(client_info.sin_port), str);


		//printf("Socket values of index %d in main function is %d\n",i, client_socketId);
		pthread_create(&worker_threads[idx],NULL,thread_fn, (void*)&client_descriptors[idx]); // Creating a thread for each client accepted
		idx++; 
    }

    close(master_socket);
    return 0;
}