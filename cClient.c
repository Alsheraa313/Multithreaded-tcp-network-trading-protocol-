#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <pthread.h>
#endif

#define RESPONSE_SIZE 8192

static volatile int running = 1;

#ifdef _WIN32
#include <windows.h>
HANDLE print_mutex;
#else
#include <pthread.h>
pthread_mutex_t print_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void safe_print(const char *msg)
{
#ifdef _WIN32
    WaitForSingleObject(print_mutex, INFINITE);
    printf("%s", msg);
    fflush(stdout);
    ReleaseMutex(print_mutex);
#else
    pthread_mutex_lock(&print_mutex);
    printf("%s", msg);
    fflush(stdout);
    pthread_mutex_unlock(&print_mutex);
#endif
}

// Thread that monitors server socket using select() and prints messages as they arrive
void *receiver_thread(void *arg)
{
    int sock = *(int *)arg;

    while (running)
    {
        fd_set read_fds;
        struct timeval tv;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        tv.tv_sec = 1; // timeout so we can check the running flag periodically
        tv.tv_usec = 0;

        int rv = select(sock + 1, &read_fds, NULL, NULL, &tv);
        if (rv < 0)
        {
            perror("select");
            break;
        }
        else if (rv == 0)
        {
            continue; // if a timeout occurs,it loops again
        }

        if (FD_ISSET(sock, &read_fds))
        { // if a socket is ready to be read it receives the messahes and prints them
            char buffer[RESPONSE_SIZE];
            int bytes = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0)
            {
                running = 0;
                break;
            }
            buffer[bytes] = '\0';
            printf("\n%s\n", buffer);
            fflush(stdout);
        }
    }

    return NULL;
}

// helper function to send a command and wait for a single response to help for synchronous login
int send_command_and_wait_response(int sock, const char *cmd, char *out, size_t outlen)
{
    if (send(sock, cmd, strlen(cmd), 0) < 0)
    {
        perror("send");
        return -1;
    }

    int total = 0;
    while (total < (int)outlen - 1)
    {
        int n = recv(sock, out + total, outlen - total - 1, 0);
        if (n <= 0)
            break;
        total += n;
        if (n < (int)outlen - total - 1)
            break;
    }
    out[total] = '\0';
    return total;
}

int main()
{

#ifdef _WIN32
    // Initialize Winsock for Windows network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
    print_mutex = CreateMutex(NULL, FALSE, NULL);
#else
    pthread_mutex_init(&print_mutex, NULL);
#endif

    int netSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (netSocket < 0)
    {
#ifdef _WIN32
        printf("socket failed with error: %d\n", WSAGetLastError());
#else
        perror("socket");
#endif
        return 1;
    }
    // same process as last time
    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(9001);
    inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr);

    int connectStatus = connect(netSocket, (struct sockaddr *)&server_address, sizeof(server_address));
    if (connectStatus < 0)
    {
#ifdef _WIN32
        printf("Connection failed with error: %d\n", WSAGetLastError());
        closesocket(netSocket);
        WSACleanup();
#else
        perror("Connection failed");
        close(netSocket);
#endif
        return 1;
    }

    // the server prompot once the client connects
    char serverResponse[RESPONSE_SIZE] = {0};
    int r = recv(netSocket, serverResponse, sizeof(serverResponse) - 1, 0);
    if (r > 0)
    {
        serverResponse[r] = '\0';
        safe_print(serverResponse);
        safe_print("\n");
    }

    // here is where the helper function becomes useful
    char userInput[512];
    char response[RESPONSE_SIZE];
    while (running)
    {
        safe_print("Enter LOGIN <username> <password>: ");
        fflush(stdout);
        if (!fgets(userInput, sizeof(userInput), stdin))
        {
            running = 0;
            break;
        }
        userInput[strcspn(userInput, "\n")] = 0; // only allows for LOGIN in this phase
        if (strncmp(userInput, "LOGIN", 5) != 0)
        {
            safe_print("Please LOGIN first\n");
            continue;
        }

        // send and wait for response
        memset(response, 0, sizeof(response));
        int len = send_command_and_wait_response(netSocket, userInput, response, sizeof(response));
        if (len <= 0)
        {
            safe_print("Failed to receive response from server during login.\n");
            running = 0;
            break;
        }

        safe_print(response);
        safe_print("\n");
        if (strstr(response, "200 OK") != NULL)
        { // breaks out the loop if login is successful
            break;
        }
    }

    if (!running)
    {
#ifdef _WIN32
        closesocket(netSocket);
        WSACleanup();
#else
        close(netSocket);
#endif
        return 0;
    }

    // Start receiver thread to monitor server messages concurrently
#ifdef _WIN32
    HANDLE threadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)receiver_thread, &netSocket, 0, NULL);
    if (threadHandle == NULL)
    {
        printf("CreateThread failed with error: %lu\n", GetLastError());
        running = 0;
    }
#else
    pthread_t tid; // thread to recieve and print messages from  the server
    if (pthread_create(&tid, NULL, receiver_thread, &netSocket) != 0)
    {
        perror("pthread_create");
        running = 0;
    }
#endif

    fflush(stdout);

    while (running)
    {
#ifdef _WIN32
        if (_kbhit())
        {
            if (!fgets(userInput, sizeof(userInput), stdin))
            {
                running = 0;
                break;
            }
        }
        else
        {
            Sleep(100);
            continue;
        }
#else
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(STDIN_FILENO, &read_fds);

        struct timeval tv;
        tv.tv_sec = 1; // sets up the timeout so were not running forever
        tv.tv_usec = 0;

        int rv = select(STDIN_FILENO + 1, &read_fds, NULL, NULL, &tv);
        if (rv < 0)
        {
            perror("select");
            break;
        }
        else if (rv == 0)
        {
            continue;
        }

        if (FD_ISSET(STDIN_FILENO, &read_fds))
        { // reads the user input
            if (!fgets(userInput, sizeof(userInput), stdin))
            {
                running = 0;
                break;
            }
        }
        else
        {
            continue;
        }
#endif

        userInput[strcspn(userInput, "\n")] = 0; // grabs the user input
        if (strlen(userInput) == 0)
        {
            fflush(stdout);
            continue;
        }

        if (send(netSocket, userInput, strlen(userInput), 0) < 0)
        { // sends the users input to the server
#ifdef _WIN32
            printf("send failed with error: %d\n", WSAGetLastError());
#else
            perror("send");
#endif
            running = 0;
            break;
        }

        if (strcmp(userInput, "QUIT") == 0)
        { // the exit condition
            running = 0;
            break;
        }
    }

#ifdef _WIN32
    WaitForSingleObject(threadHandle, INFINITE);
    CloseHandle(threadHandle);
#else
    pthread_join(tid, NULL);
#endif

#ifdef _WIN32
    closesocket(netSocket);
    WSACleanup();
    CloseHandle(print_mutex);
#else
    close(netSocket);
    pthread_mutex_destroy(&print_mutex);
#endif

    return 0;
}