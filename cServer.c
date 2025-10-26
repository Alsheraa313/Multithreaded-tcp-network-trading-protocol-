#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "sqlite3.h"
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sqlite3.h"
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include "sqlite3.h"
#include <windows.h>

#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#define STRCMP_NOCASE _stricmp
#else
#include <strings.h>
#define STRCMP_NOCASE strcasecmp
#endif

// --- Windows equivalents of pthreads ---
typedef CRITICAL_SECTION pthread_mutex_t;

#define pthread_mutex_init(m, a) InitializeCriticalSection(m)
#define pthread_mutex_lock(m) EnterCriticalSection(m)
#define pthread_mutex_unlock(m) LeaveCriticalSection(m)
#define pthread_mutex_destroy(m) DeleteCriticalSection(m)

// Initialize the mutexes like pthread style
pthread_mutex_t active_clients_mutex;
pthread_mutex_t db_mutex;

sqlite3 *db;

static void init_mutexes()
{
    pthread_mutex_init(&active_clients_mutex, NULL);
    pthread_mutex_init(&db_mutex, NULL);
}

static void destroy_mutexes()
{
    pthread_mutex_destroy(&active_clients_mutex);
    pthread_mutex_destroy(&db_mutex);
}

#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_TASKS 100
#define NUM_WORKERS 4
// the mutex protects the database from being worked on by a bunch of threads at once
sqlite3 *db;
pthread_mutex_t db_mutex;
pthread_mutex_t active_clients_mutex;
pthread_mutex_t disconnect_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
int disconnect_list[FD_SETSIZE] = {0};
/*this comes in handy for tracking which clients need
to be disconnected with the shutdown
*/
int server_shutdown = 0;
// a bool/flag for the shutdown status

int client_sockets[FD_SETSIZE];

int serverSocket;
int login_status[FD_SETSIZE];
char username_by_slot[FD_SETSIZE][50];
int conn_count = 0;
fd_set master_set;

#define MAX_TASKS 100

typedef struct
{
    int client_socket;
    char message[512];
} Task;
/*for tasks it has the client socket a thread will work on
(down in the workerthread function) and the message thatll be worked with
*/
typedef struct
{
    Task tasks[MAX_TASKS];
    int front, rear, count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} TaskQueue;

/*were doing a queue for multithreading, the idea behind it is that we have a fixed size
amount of tasks, then the tasks with the aformentioned client socket and message gets queued in to be wokred on */
TaskQueue workQueue;

void initQueue(TaskQueue *q)
{
    q->front = q->rear = q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void enqueue(TaskQueue *q, Task t) // we enqueue a new task so it can be worked on, more on that in the workerthread below
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == MAX_TASKS)
        pthread_cond_wait(&q->cond, &q->mutex);

    q->tasks[q->rear] = t;
    q->rear++;
    if (q->rear == MAX_TASKS)
        q->rear = 0;

    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

Task dequeue(TaskQueue *q) // and once a worker thread is free it dequeues a task to work on
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0)
        pthread_cond_wait(&q->cond, &q->mutex);

    Task t = q->tasks[q->front];
    q->front++;
    if (q->front == MAX_TASKS)
        q->front = 0;

    q->count--;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return t;
}

pthread_mutex_t active_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t db_mutex = PTHREAD_MUTEX_INITIALIZER;
sqlite3 *db;

#endif

// basic struct to help track who is logged in at the given moment
typedef struct
{
    char username[50];
    char ip[50];
    int is_root;
} ActiveClient;

ActiveClient active_clients[10];

int active_count = 0;

// standard sqlite callback function
static int callback(void *data, int argc, char **argv, char **azColName)
{
    int i;
    for (i = 0; i < argc; i++)
    {
        printf("%s = %s\n", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    printf("\n");
    return 0;
}

/*login function, checks input first and then capacity, after which itll pull the user from the sql db, copy it and
add it to the activeClient array and increment the count. we had to move the menu here */
/*if a user isnt found in the db, sends out an error*/

int handleLoginCommand(sqlite3 *db, int clientSocket, char *args)
{
    char username[50], password[50];

    if (sscanf(args, "%49s %49s", username, password) != 2)
    {
        const char *msg = "403 Wrong UserID or Password\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    if (active_count >= 10)
    {
        const char *msg = "503 server is full\n";
        send(clientSocket, msg, strlen(msg), 0);
        return 0;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT ID, is_root FROM users WHERE user_name = ? AND password = ?;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
    {
        fprintf(stderr, "SQLite prepare failed: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_STATIC);

    char response[256];

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int isRoot = sqlite3_column_int(stmt, 1);
        sqlite3_finalize(stmt);

        ActiveClient newClient;
        memset(&newClient, 0, sizeof(newClient));

        strncpy(newClient.username, username, sizeof(newClient.username) - 1);
        newClient.is_root = isRoot;

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        if (getpeername(clientSocket, (struct sockaddr *)&addr, &addr_len) == 0)
        {
            inet_ntop(AF_INET, &addr.sin_addr, newClient.ip, sizeof(newClient.ip));
        }
        else
        { // local IP fallback as a just incase
            strncpy(newClient.ip, "127.0.0.1", sizeof(newClient.ip) - 1);
            newClient.ip[sizeof(newClient.ip) - 1] = '\0';
        }

        // where any new client gets added to the active clients array mentioned above
        active_clients[active_count++] = newClient;

        printf("User '%s' logged in from %s | Active clients: %d\n",
               newClient.username, newClient.ip, active_count);

        return 1;
    }
    else
    {
        sqlite3_finalize(stmt);
        snprintf(response, sizeof(response), "403 Wrong UserID or Password\n");
        send(clientSocket, response, strlen(response), 0);
        return 0;
    }
}

// Handles the SELL command from the client.
// This command allows a user to sell a certain number of Pokemon cards.
// It updates both the Pokemon_Cards table and the user's USD balance.
void handleSellCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    char cardName[50];
    int quantity, userID;
    double price;

    // Expected format: SELL <cardName> <quantity> <price> <userID>
    // If the format is wrong, return a 403 message format error.
    if (sscanf(args, "%49s %d %lf %d", cardName, &quantity, &price, &userID) != 4)
    {
        send(clientSocket, "403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n",
             strlen("403 message format error: Usage -> SELL <cardName> <quantity> <price> <userID>\n"), 0);
        return;
    }

    sqlite3_stmt *stmt;

    // Check if the user exists
    const char *userCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, userCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error.\n",
             strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, userID);

    double currentBalance = 0.0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        currentBalance = sqlite3_column_double(stmt, 0);
    }
    else
    {
        // User ID not found in the database
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: User does not exist.\n",
             strlen("400 invalid command: User does not exist.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if the user owns the card and how many they have
    int currentQuantity = 0;
    const char *cardCheck = "SELECT count FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
    if (sqlite3_prepare_v2(db, cardCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error.\n",
             strlen("400 invalid command: Database error.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        currentQuantity = sqlite3_column_int(stmt, 0);
    }
    else
    {
        // The specified card was not found for this user
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: Card not found for this user.\n",
             strlen("400 invalid command: Card not found for this user.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Check if user has enough cards to sell
    if (currentQuantity < quantity)
    {
        send(clientSocket, "400 invalid command: Not enough cards to sell.\n",
             strlen("400 invalid command: Not enough cards to sell.\n"), 0);
        return;
    }

    // If user still has cards left after selling, update the count
    if (currentQuantity - quantity > 0)
    {
        const char *updateCard = "UPDATE pokemon_cards SET count=count-? WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, updateCard, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Otherwise, delete the card entry since the user sold them all
    else
    {
        const char *deleteCard = "DELETE FROM pokemon_cards WHERE card_name=? AND owner_id=?;";
        sqlite3_prepare_v2(db, deleteCard, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, userID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Calculate total money earned from selling the cards
    double proceeds = price * quantity;

    // Add proceeds to the user's balance
    const char *updateBalance = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, proceeds);
    sqlite3_bind_int(stmt, 2, userID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Compute new balance manually for confirmation
    double newBalance = currentBalance + proceeds;

    // Send a success message back to the client
    char response[256];
    snprintf(response, sizeof(response),
             "200 OK\n %d %s.balance USD $%.2f\n%s\n",
             quantity, cardName, newBalance, serverPrompt);
    send(clientSocket, response, strlen(response), 0);
}

// Handles the BUY command from the client.
// This command allows a user to buy Pokemon cards from another user (seller).
// It checks balances, updates both users' balances, and updates the card counts.
void handleBuyCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    char cardName[50], cardType[50], rarity[50];
    double price;
    int quantity, buyerID;

    // Expected format: BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>
    // If the input format is wrong, send a 403 format error message.
    if (sscanf(args, "%49s %49s %49s %lf %d %d", cardName, cardType, rarity, &price, &quantity, &buyerID) != 6)
    {
        const char *usageMsg = "403 message format error: Usage -> BUY <cardName> <cardType> <rarity> <price> <quantity> <buyerID>\n";
        send(clientSocket, usageMsg, strlen(usageMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;

    // Find a seller who has at least one of the requested card
    // Exclude the buyer from being their own seller
    const char *findSellerSQL =
        "SELECT owner_id, count FROM pokemon_cards "
        "WHERE card_name=? AND card_type=? AND rarity=? AND count>0 AND owner_id != ? "
        "LIMIT 1;";
    if (sqlite3_prepare_v2(db, findSellerSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error when finding seller.\n",
             strlen("400 invalid command: Database error when finding seller.\n"), 0);
        return;
    }
    sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, buyerID);

    int sellerID = -1;
    int sellerCount = 0;

    // If seller found, store their ID and how many cards they have
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        sellerID = sqlite3_column_int(stmt, 0);
        sellerCount = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    // If no seller found, send an error message
    if (sellerID == -1)
    {
        send(clientSocket, "400 invalid command: No seller found for this card.\n",
             strlen("400 invalid command: No seller found for this card.\n"), 0);
        return;
    }

    // Check buyer exists and get buyer's balance
    double buyerBalance = 0.0;
    double totalPrice = 0.0;
    const char *buyerCheck = "SELECT usd_balance FROM users WHERE ID=?;";
    if (sqlite3_prepare_v2(db, buyerCheck, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error while checking buyer balance.\n",
             strlen("400 invalid command: Database error while checking buyer balance.\n"), 0);
        return;
    }
    sqlite3_bind_int(stmt, 1, buyerID);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        buyerBalance = sqlite3_column_double(stmt, 0);
    }
    else
    {
        sqlite3_finalize(stmt);
        send(clientSocket, "400 invalid command: Buyer does not exist.\n",
             strlen("400 invalid command: Buyer does not exist.\n"), 0);
        return;
    }
    sqlite3_finalize(stmt);

    // Calculate total cost of the transaction
    totalPrice = price * quantity;
    // If buyer cannot afford, cancel transaction
    if (buyerBalance < totalPrice)
    {
        const char *msg = "400 invalid command: Insufficient balance for this purchase.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    // Update or delete seller's card record depending on remaining count
    const char *updateSellerSQL;
    if (sellerCount == quantity)
    {
        updateSellerSQL = "DELETE FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }
    else
    {
        updateSellerSQL = "UPDATE pokemon_cards SET count=count-? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    }

    if (sqlite3_prepare_v2(db, updateSellerSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        send(clientSocket, "400 invalid command: Database error when updating seller.\n",
             strlen("400 invalid command: Database error when updating seller.\n"), 0);
        return;
    }

    // Bind values depending on which SQL query was chosen
    if (sellerCount == quantity)
    {
        sqlite3_bind_int(stmt, 1, sellerID);
        sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, rarity, -1, SQLITE_STATIC);
    }
    else
    {
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_int(stmt, 2, sellerID);
        sqlite3_bind_text(stmt, 3, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, rarity, -1, SQLITE_STATIC);
    }
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Check if the buyer already owns the same type of card
    const char *checkBuyerSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
    sqlite3_prepare_v2(db, checkBuyerSQL, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, buyerID);
    sqlite3_bind_text(stmt, 2, cardName, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, cardType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, rarity, -1, SQLITE_STATIC);

    int buyerCount = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        buyerCount = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    // If buyer already owns the card, increase their count
    if (buyerCount > 0)
    {
        const char *updateBuyerSQL = "UPDATE pokemon_cards SET count=count+? WHERE owner_id=? AND card_name=? AND card_type=? AND rarity=?;";
        sqlite3_prepare_v2(db, updateBuyerSQL, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, quantity);
        sqlite3_bind_int(stmt, 2, buyerID);
        sqlite3_bind_text(stmt, 3, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, rarity, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    // Otherwise, insert a new record for the buyer
    else
    {
        const char *insertBuyerSQL = "INSERT INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES (?, ?, ?, ?, ?);";
        sqlite3_prepare_v2(db, insertBuyerSQL, -1, &stmt, NULL);
        sqlite3_bind_text(stmt, 1, cardName, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, cardType, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, rarity, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 4, quantity);
        sqlite3_bind_int(stmt, 5, buyerID);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Calculate total cost of the transaction
    totalPrice = price * quantity;

    // Deduct total price from buyer's balance
    const char *updateBuyerBalance = "UPDATE users SET usd_balance=usd_balance-? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateBuyerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, buyerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Add total price to seller's balance
    const char *updateSellerBalance = "UPDATE users SET usd_balance=usd_balance+? WHERE ID=?;";
    sqlite3_prepare_v2(db, updateSellerBalance, -1, &stmt, NULL);
    sqlite3_bind_double(stmt, 1, totalPrice);
    sqlite3_bind_int(stmt, 2, sellerID);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Get updated buyer balance
    double newBuyerBalance = 0.0;
    sqlite3_stmt *balanceStmt;
    const char *getNewBalanceSQL = "SELECT usd_balance FROM users WHERE ID=?;";
    sqlite3_prepare_v2(db, getNewBalanceSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW)
    {
        newBuyerBalance = sqlite3_column_double(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    // Get buyer's new total card count for this card
    int newBuyerCount = 0;
    const char *getBuyerCountSQL = "SELECT count FROM pokemon_cards WHERE owner_id=? AND card_name=?;";
    sqlite3_prepare_v2(db, getBuyerCountSQL, -1, &balanceStmt, NULL);
    sqlite3_bind_int(balanceStmt, 1, buyerID);
    sqlite3_bind_text(balanceStmt, 2, cardName, -1, SQLITE_STATIC);
    if (sqlite3_step(balanceStmt) == SQLITE_ROW)
    {
        newBuyerCount = sqlite3_column_int(balanceStmt, 0);
    }
    sqlite3_finalize(balanceStmt);

    char response[256];
    snprintf(response, sizeof(response),
             "200 OK\nBOUGHT: New balance: %d %s. User USD balance $%.2f\n%s",
             newBuyerCount, cardName, newBuyerBalance, serverPrompt);

    send(clientSocket, response, strlen(response), 0);
}

// Handles the BALANCE command.
// Expected format: BALANCE <userID>
// This command checks a user's USD balance in the database and returns it to the client.
void handleBalanceCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    int userID;

    if (sscanf(args, "%d", &userID) != 1)
    {
        const char *errMsg = "403 message format error\nUsage: BALANCE <OwnerID>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;
    const char *query = "SELECT usd_balance FROM users WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while checking balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    // Bind user ID value into the prepared SQL statement
    sqlite3_bind_int(stmt, 1, userID);

    char response[256];

    // Execute the query and check if a record is found
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double balance = sqlite3_column_double(stmt, 0);

        snprintf(response, sizeof(response), "200 OK\nBalance: %.2f USD\n%s", balance, serverPrompt);
    }
    else
    {
        snprintf(response, sizeof(response),
                 "400 invalid command\n");
    }

    // Finalize the SQL statement and send response
    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}
// Handles the BALANCE command.
// Expected format: BALANCE <userID>
// This command takes input of a user ID and amount and then updates the USD balance of that user in the users table
void handleDepositCommand(sqlite3 *db, int clientSocket, char *args, const char *serverPrompt)
{
    int userID;
    double amount;

    if (sscanf(args, "%d %lf", &userID, &amount) != 2 || amount <= 0)
    {
        const char *errMsg = "403 message format error\nUsage: DEPOSIT <OwnerID> <amount>\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_stmt *stmt;
    const char *updateSQL = "UPDATE users SET usd_balance = usd_balance + ? WHERE ID = ?;";

    if (sqlite3_prepare_v2(db, updateSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while processing deposit.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_bind_double(stmt, 1, amount);
    sqlite3_bind_int(stmt, 2, userID);

    if (sqlite3_step(stmt) != SQLITE_DONE)
    {
        const char *errMsg = "400 invalid command\nFailed to update balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);

    const char *querySQL = "SELECT usd_balance FROM users WHERE ID = ?;";
    if (sqlite3_prepare_v2(db, querySQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *errMsg = "400 invalid command\nDatabase error while retrieving balance.\n";
        send(clientSocket, errMsg, strlen(errMsg), 0);
        return;
    }

    sqlite3_bind_int(stmt, 1, userID);

    char response[256];

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double newBalance = sqlite3_column_double(stmt, 0);
        snprintf(response, sizeof(response), "deposit successfully. New User balance: $ %.2f USD\n%s", newBalance, serverPrompt);
    }
    else
    {
        snprintf(response, sizeof(response), "400 invalid command\n");
    }

    sqlite3_finalize(stmt);
    send(clientSocket, response, strlen(response), 0);
}

// Handles the LOOKUP command
//  Expected format: LOOKUP <card_name> || <type> || <rarity>
//  This command checks if there are any cards that matches the card the user entered, if so it returns the ID,
//  Card Name, Type, Rarity, Count, and Owner of the card
void handleLookupCommand(sqlite3 *db, int clientSocket, char *args)
{
    char arg1[50] = "", arg2[50] = "", arg3[50] = "";

    // Trim leading whitespace
    while (*args == ' ' || *args == '\t' || *args == '\n' || *args == '\r')
    {
        args++;
    }

    // Parse up to three arguments
    int count = sscanf(args, "%49s %49s %49s", arg1, arg2, arg3);

    // If no arguments were provided, send format error
    if (count < 1)
    {
        send(clientSocket,
             "403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n",
             strlen("403 message format error: Usage -> LOOKUP <card_name> or <card_type> or <rarity>\n"),
             0);
        return;
    }

    char response[4096];
    memset(response, 0, sizeof(response));

    // Build SQL query depending on how many arguments there are
    const char *baseSQL =
        "SELECT id, card_name, card_type, rarity, count, owner_id "
        "FROM pokemon_cards WHERE "
        "(card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?)";

    if (count >= 2)
    {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }
    if (count == 3)
    {
        baseSQL =
            "SELECT id, card_name, card_type, rarity, count, owner_id "
            "FROM pokemon_cards WHERE "
            "((card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?) "
            "AND (card_name LIKE ? OR card_type LIKE ? OR rarity LIKE ?))";
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, baseSQL, -1, &stmt, NULL) != SQLITE_OK)
    {
        snprintf(response, sizeof(response), "500 Internal Server Error: %s\n", sqlite3_errmsg(db));
        send(clientSocket, response, strlen(response), 0);
        return;
    }

#define BIND_LIKE(index, value)                                        \
    do                                                                 \
    {                                                                  \
        char pattern[64];                                              \
        snprintf(pattern, sizeof(pattern), "%%%s%%", value);           \
        sqlite3_bind_text(stmt, index, pattern, -1, SQLITE_TRANSIENT); \
    } while (0)

    if (count >= 1)
    {
        BIND_LIKE(1, arg1);
        BIND_LIKE(2, arg1);
        BIND_LIKE(3, arg1);
    }
    if (count >= 2)
    {
        BIND_LIKE(4, arg2);
        BIND_LIKE(5, arg2);
        BIND_LIKE(6, arg2);
    }
    if (count == 3)
    {
        BIND_LIKE(7, arg3);
        BIND_LIKE(8, arg3);
        BIND_LIKE(9, arg3);
    }

    strcat(response, "200 OK\n\n");
    strcat(response, "ID     Card Name           Type       Rarity     Count     Owner\n");
    strcat(response, "--------------------------------------------------------------------\n");

    int rowsFound = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        rowsFound++;

        int id = sqlite3_column_int(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *type = sqlite3_column_text(stmt, 2);
        const unsigned char *rarity = sqlite3_column_text(stmt, 3);
        int countValue = sqlite3_column_int(stmt, 4);
        const unsigned char *owner = sqlite3_column_text(stmt, 5);

        char line[256];
        snprintf(line, sizeof(line),
                 "%-6d %-18s %-10s %-10s %-9d %-10s\n",
                 id, name, type, rarity, countValue, owner);

        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }

    sqlite3_finalize(stmt);

    if (rowsFound == 0)
    {
        snprintf(response, sizeof(response), "404 error Your search did not match any records.\n");
    }

    send(clientSocket, response, strlen(response), 0);
}

// Handles the WHO command
// Expected format: WHO
// This command (WHICH CAN ONLY BE USED BY THE ROOT USER) will display a list of the active users and the user's IP address
void handleWhoCommand(sqlite3 *db, int clientSocket, char *username, const char *serverPrompt)
{
    char response[4096];
    memset(response, 0, sizeof(response));

    (void)db;

    int is_root_user = 0;
    const char *found_username = NULL;

    pthread_mutex_lock(&active_clients_mutex);
    for (int i = 0; i < active_count; ++i)
    {
        if (strcmp(active_clients[i].username, username) == 0)
        {
            is_root_user = active_clients[i].is_root;
            found_username = active_clients[i].username;
            break;
        }
    }
    pthread_mutex_unlock(&active_clients_mutex);

    if (!is_root_user)
    {
        snprintf(response, sizeof(response), "403 Error, Only root user can use WHO command\n%s", serverPrompt);
        send(clientSocket, response, strlen(response), 0);
        return;
    }

    snprintf(response, sizeof(response), "200 OK\nThe list of the active users:\n");

    pthread_mutex_lock(&active_clients_mutex);
    for (int i = 0; i < active_count; ++i)
    {
        if (strlen(active_clients[i].username) == 0)
            continue;

        char line[128];
        snprintf(line, sizeof(line), "%s %s\n", active_clients[i].username, active_clients[i].ip);
        strncat(response, line, sizeof(response) - strlen(response) - 1);
    }
    pthread_mutex_unlock(&active_clients_mutex);

    // Add server prompt at the end
    strncat(response, serverPrompt, sizeof(response) - strlen(response) - 1);

    send(clientSocket, response, strlen(response), 0);
}
// Handles the LIST command.
// Expected format: LIST
// This command shows all Pokemon cards owned by the current user (or all if root).
void handleListCommand(sqlite3 *db, int clientSocket, const char *username, const char *serverPrompt)
{
    sqlite3_stmt *stmt;
    char response[4096];
    memset(response, 0, sizeof(response));

    int is_root_user = 0;
    int user_id = -1;

    // First, get the user's ID and check if they are root
    const char *userQuery = "SELECT ID, is_root FROM users WHERE user_name = ?;";
    if (sqlite3_prepare_v2(db, userQuery, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *msg = "400 invalid command\nDatabase error while getting user info.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    sqlite3_bind_text(stmt, 1, username, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        user_id = sqlite3_column_int(stmt, 0);
        is_root_user = sqlite3_column_int(stmt, 1);
    }
    else
    {
        sqlite3_finalize(stmt);
        const char *msg = "400 invalid command\nUser not found.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }
    sqlite3_finalize(stmt);

    const char *query;
    if (is_root_user)
    {
        // Outputs all cards if the user is root
        query = "SELECT pokemon_cards.id, card_name, card_type, rarity, count, users.user_name "
                "FROM pokemon_cards JOIN users ON pokemon_cards.owner_id = users.ID;";
    }
    else
    {
        // if the user isnt root, this selects only from their own records
        query = "SELECT id, card_name, card_type, rarity, count, owner_id "
                "FROM pokemon_cards WHERE owner_id = ?;";
    }

    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK)
    {
        const char *msg = "400 invalid command\nDatabase error while listing cards.\n";
        send(clientSocket, msg, strlen(msg), 0);
        return;
    }

    if (!is_root_user)
        sqlite3_bind_int(stmt, 1, user_id);

    int response_len = 0;

    if (is_root_user)
    {
        response_len = snprintf(response, sizeof(response),
                                "200 OK\nThe list of records in the cards database:\n"
                                "ID   Card Name        Type        Rarity      Count     Owner\n");
    }
    else
    {
        response_len = snprintf(response, sizeof(response),
                                "200 OK\nThe list of records in the Pokemon cards table for current user, %s:\n"
                                "ID   Card Name        Type        Rarity      Count     Owner\n",
                                username);
    }

    int found = 0;

    // Loop through each result row  and then formats card info
    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int id = sqlite3_column_int(stmt, 0);
        const char *cardName = (const char *)sqlite3_column_text(stmt, 1);
        const char *cardType = (const char *)sqlite3_column_text(stmt, 2);
        const char *rarity = (const char *)sqlite3_column_text(stmt, 3);
        int count = sqlite3_column_int(stmt, 4);

        if (is_root_user)
        {
            const char *owner_name = (const char *)sqlite3_column_text(stmt, 5);
            int written = snprintf(response + response_len, sizeof(response) - response_len,
                                   "%-4d %-15s %-11s %-11s %-6d %-6s\n",
                                   id, cardName, cardType, rarity, count, owner_name);
            if (written < 0 || written >= (int)(sizeof(response) - response_len - 1))
                break;
            response_len += written;
        }
        else
        {
            int written = snprintf(response + response_len, sizeof(response) - response_len,
                                   "%-4d %-15s %-11s %-11s %-6d\n",
                                   id, cardName, cardType, rarity, count);
            if (written < 0 || written >= (int)(sizeof(response) - response_len - 1))
                break;
            response_len += written;
        }
        found = 1;
    }

    sqlite3_finalize(stmt);

    if (!found)
    {
        response_len = snprintf(response, sizeof(response),
                                "200 OK\nNo cards found for user %s.\n", username);
    }

    // Add server prompt at the end
    strncat(response, serverPrompt, sizeof(response) - strlen(response) - 1);

    send(clientSocket, response, strlen(response), 0);
}
/*main worker thread function, whenever a client joins and sends a request, it gets queued up and then upon being dequeud it goes
through the worker function here.*/
void *workerThread(void *arg)
{
    const char *loginPrompt = "Enter LOGIN followed by username and password\n";
    const char *serverPrompt = "Available commands: BUY, SELL, BALANCE, DEPOSIT, LIST, QUIT, LOGOUT, WHO, LOOKUP, SHUTDOWN\n";

    while (1)
    {
        Task t = dequeue(&workQueue); // that dequeue that was mentioned earlier utilized the task intialized way above
        int sd = t.client_socket;
        char *buf = t.message;

        buf[strcspn(buf, "\r\n")] = 0;

        printf("Client %d Command received: %s\n", sd, buf);
        fflush(stdout); // prints to server console

        int slot = -1; // this checks for which slot a client is in and doubles as error handling for if theyve disconnected or not
        for (int i = 0; i < FD_SETSIZE; ++i)
        {
            if (client_sockets[i] == sd)
            {
                slot = i;
                break;
            }
        }

        if (slot == -1)
            continue;

        if (login_status[slot] == 0) // stopgate to make sure they login before making the server do anything else
        {
            if (strncmp(buf, "LOGIN", 5) == 0)
            {
                char user[50] = {0}, pass[50] = {0};
                if (strlen(buf) <= 6)
                {
                    send(sd, "LOGIN:\n", 36, 0);
                }
                else
                {
                    int text = sscanf(buf + 6, "%49s %49s", user, pass);
                    if (text != 2)
                    {
                        send(sd, "LOGIN:\n", 36, 0);
                    }
                    else
                    {
                        pthread_mutex_lock(&db_mutex);
                        pthread_mutex_lock(&active_clients_mutex);
                        int success = handleLoginCommand(db, sd, buf + 6);
                        pthread_mutex_unlock(&active_clients_mutex);
                        pthread_mutex_unlock(&db_mutex);

                        if (success)
                        {
                            login_status[slot] = 1;
                            strncpy(username_by_slot[slot], user, sizeof(username_by_slot[slot]) - 1);
                            username_by_slot[slot][sizeof(username_by_slot[slot]) - 1] = '\0';
                            send(sd, "200 OK\n", 7, 0);
                            send(sd, serverPrompt, strlen(serverPrompt), 0);
                        }
                    }
                }
            }
            else
            {
                send(sd, "403 Wrong UserID or Password\n", 29, 0);
            }
        }
        else
        { /*the menu the process the different commands that have been queued in */
            char current_username[50];
            strncpy(current_username, username_by_slot[slot], sizeof(current_username) - 1);
            current_username[sizeof(current_username) - 1] = '\0';

            if (strncmp(buf, "BALANCE", 7) == 0)
                handleBalanceCommand(db, sd, buf + 8, serverPrompt);
            else if (strncmp(buf, "LIST", 4) == 0)
                handleListCommand(db, sd, current_username, serverPrompt);
            else if (strncmp(buf, "BUY", 3) == 0)
                handleBuyCommand(db, sd, buf + 4, serverPrompt);
            else if (strncmp(buf, "SELL", 4) == 0)
                handleSellCommand(db, sd, buf + 5, serverPrompt);
            else if (strncmp(buf, "DEPOSIT", 7) == 0)
                handleDepositCommand(db, sd, buf + 8, serverPrompt);
            else if (strncmp(buf, "WHO", 3) == 0)
                handleWhoCommand(db, sd, current_username, serverPrompt);
            else if (strncmp(buf, "LOOKUP", 6) == 0)
                handleLookupCommand(db, sd, buf + 7);
            else if (strcmp(buf, "LOGOUT") == 0)
            {
                send(sd, "You have been logged out.\n", 26, 0);
                pthread_mutex_lock(&active_clients_mutex); // pops out the user if theyve elected to logout
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, current_username) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);
                login_status[slot] = 0;
                username_by_slot[slot][0] = '\0';
                send(sd, loginPrompt, strlen(loginPrompt), 0);
            }
            else if (strcmp(buf, "QUIT") == 0)
            {
                send(sd, "Goodbye\n", 9, 0);
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k) // same process as logout
                {
                    if (strcmp(active_clients[k].username, current_username) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);
                close(sd);
                FD_CLR(sd, &master_set);
                client_sockets[slot] = -1;
                login_status[slot] = 0;
                username_by_slot[slot][0] = '\0';
                conn_count--;
            }
            else if (strcmp(buf, "SHUTDOWN") == 0)
            {
                char requester[50] = "";
                int slot_index = -1;

                // the below block of functions gets the requesters name to check if theyre a root and then verifies it
                for (int i = 0; i < FD_SETSIZE; ++i)
                {
                    if (client_sockets[i] == sd)
                    {
                        slot_index = i;
                        break;
                    }
                }

                if (slot_index != -1 && username_by_slot[slot_index][0] != '\0')
                {
                    strncpy(requester, username_by_slot[slot_index], sizeof(requester) - 1);
                    requester[sizeof(requester) - 1] = '\0';
                }

                int is_root_requester = 0;
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, requester) == 0)
                    {
                        if (active_clients[k].is_root)
                            is_root_requester = 1;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);

                if (is_root_requester)
                {
                    send(sd, "200 OK\nServer shutting down\n", 28, 0);

                    // Set shutdown flag for root
                    pthread_mutex_lock(&shutdown_mutex);
                    server_shutdown = 1;
                    pthread_mutex_unlock(&shutdown_mutex);

                    printf("Server will terminate\n");
                }
                else
                {
                    send(sd, "401 Error, Only root can shutdown the server\n", 53, 0);
                }
            }

            else
            {
                send(sd, "Invalid command\n", 16, 0);
            }
        }
    }

    return NULL;
}

void startWorkerPool() // creates the worker threads that handle all those commands aboce
{
    pthread_t threads[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; ++i)
    {
        pthread_create(&threads[i], NULL, workerThread, NULL);
        pthread_detach(threads[i]);
    }
}
int main()
{
#ifdef _WIN32
    // Initialize Winsock on Windows for network communication
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        printf("Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // creates the db
    char *zErrMsg = 0;
    int rc = sqlite3_open("users.db", &db);
    if (rc)
    {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    pthread_mutex_init(&db_mutex, NULL);
    pthread_mutex_init(&active_clients_mutex, NULL);
    pthread_mutex_init(&shutdown_mutex, NULL);
    pthread_mutex_init(&disconnect_mutex, NULL);

    fprintf(stderr, "Opened database successfully\n");

    // Create "users" table if it doesn't already exist
    char *sql = "CREATE TABLE IF NOT EXISTS users ("
                "ID INTEGER PRIMARY KEY,"
                "first_name TEXT,"
                "last_name TEXT,"
                "user_name TEXT NOT NULL,"
                "password TEXT,"
                "usd_balance DOUBLE NOT NULL,"
                "is_root INTEGER NOT NULL DEFAULT 0);";

    rc = sqlite3_exec(db, sql, callback, 0, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (create table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Insert users into the table
    sql = "INSERT OR IGNORE INTO users (ID, first_name, last_name, user_name, password, usd_balance, is_root) VALUES "
          "(1, 'Branch', 'Tree', 'Root', 'Root01', 100, 1),"
          "(2, 'Ms', 'Partner', 'Mary', 'Mary01', 50, 0),"
          "(3, 'Mr', 'Dude', 'John', 'John01', 200, 0),"
          "(4, 'Ms', 'Misses', 'Moe', 'Moe01', 300, 0);";

    rc = sqlite3_exec(db, sql, 0, 0, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (insert users): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Create "pokemon_cards" table if it doesn't exist
    const char *create_pokemon_cards_table =
        "CREATE TABLE IF NOT EXISTS pokemon_cards ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "card_name TEXT NOT NULL, "
        "card_type TEXT NOT NULL, "
        "rarity TEXT NOT NULL, "
        "count INTEGER NOT NULL, "
        "owner_id INTEGER NOT NULL, "
        "FOREIGN KEY(owner_id) REFERENCES users(ID), "
        "UNIQUE(card_name, card_type, rarity, owner_id)"
        ");";

    rc = sqlite3_exec(db, create_pokemon_cards_table, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (create pokemon table): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    // Insert default Pokémon cards if they don't already exist
    sql = "INSERT OR IGNORE INTO pokemon_cards (card_name, card_type, rarity, count, owner_id) VALUES "
          "('Pikachu', 'Electric', 'Common', 2, 1),"
          "('Charizard', 'Fire', 'Rare', 3, 2),"
          "('Squirtle', 'Water', 'Uncommon', 30, 2);";

    rc = sqlite3_exec(db, sql, NULL, NULL, &zErrMsg);
    if (rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error (insert pokemon): %s\n", zErrMsg);
        sqlite3_free(zErrMsg);
    }

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0)
    {
        perror("socket");
        exit(1);
    }
    // creating the server socket and then subseqently binding it
    struct sockaddr_in serverAddress;
    memset(&serverAddress, 0, sizeof(serverAddress));

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(9001);
    serverAddress.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (struct sockaddr *)&serverAddress, sizeof(serverAddress)) < 0)
    {
        perror("bind");
        exit(1);
    }

    if (listen(serverSocket, 10) < 0)
    {
        perror("listen");
        exit(1);
    }

    printf("Server listening on port 9001...\n");

    fd_set master_set, read_fds;         // everything monnitored by select
    for (int i = 0; i < FD_SETSIZE; ++i) // error handling to make sure nothing is in a state it shouldnt be in
    {
        client_sockets[i] = -1;
        login_status[i] = 0;
        username_by_slot[i][0] = '\0';
        disconnect_list[i] = 0;
    }

    FD_ZERO(&master_set);
    FD_SET(serverSocket, &master_set);
    int max_fd = serverSocket;

    // Initialize and start worker threads above
    initQueue(&workQueue);
    startWorkerPool();

    const char *loginPrompt = "Enter LOGIN followed by username and password\n";

    while (1)
    {
        // Checks if that server shutdown has been set
        pthread_mutex_lock(&shutdown_mutex);
        if (server_shutdown)
        {
            pthread_mutex_unlock(&shutdown_mutex);
            printf("Server has been shutdown\n");
            break;
        }
        pthread_mutex_unlock(&shutdown_mutex);

        read_fds = master_set;                                        // here is that coveted select() use vvvvv
        int nready = select(max_fd + 1, &read_fds, NULL, NULL, NULL); // monitoers the server socket for new connections and the client socket for messages
        if (nready < 0)
        {
            perror("select");
            break;
        }

        if (FD_ISSET(serverSocket, &read_fds)) // once a new client is ready to connect, it goes through this accept loop
        {
            struct sockaddr_in clientAddr;
            socklen_t addrLen = sizeof(clientAddr);
            int newsock = accept(serverSocket, (struct sockaddr *)&clientAddr, &addrLen);
            if (newsock < 0)
            {
                perror("accept");
                continue;
            }

            if (conn_count >= 10)
            {
                const char *msg = "Server is full\n";
                send(newsock, msg, strlen(msg), 0);
                close(newsock);
                printf("Server is full\n");
            }
            else
            {
                int slot = -1;
                for (int i = 0; i < FD_SETSIZE; ++i) // if the server isnt full, this loops to find a slot available for the connecting client
                {
                    if (client_sockets[i] == -1)
                    {
                        slot = i;
                        break;
                    }
                }
                if (slot == -1)
                {
                    close(newsock);
                }
                else
                { // these are initalzions for the new connecting client
                    client_sockets[slot] = newsock;
                    login_status[slot] = 0;
                    username_by_slot[slot][0] = '\0';
                    FD_SET(newsock, &master_set);
                    max_fd = serverSocket;
                    for (int i = 0; i < FD_SETSIZE; ++i) // makes sure select() checks everything
                    {
                        if (client_sockets[i] > max_fd)
                            max_fd = client_sockets[i];
                    }
                    if (newsock > max_fd)
                        max_fd = newsock;
                    conn_count++;
                    send(newsock, loginPrompt, strlen(loginPrompt), 0);
                    printf("Accepted connection on socket %d (slot %d)\n", newsock, slot);
                }
            }
            if (--nready <= 0)
                continue;
        }

        // processes client messages from sockets that are ready to be read
        for (int i = 0; i < FD_SETSIZE && nready > 0; ++i)
        {
            int sd = client_sockets[i];
            if (sd == -1)
                continue;
            if (!FD_ISSET(sd, &read_fds))
                continue;

            nready--;
            char buf[512];
            memset(buf, 0, sizeof(buf));
            int bytes = recv(sd, buf, sizeof(buf) - 1, 0);
            if (bytes <= 0) // if any sort of disconnect happens from any means, this cleans it up and pops them out from the active clients
            {

                printf("Client on socket %d disconnected\n", sd);
                pthread_mutex_lock(&active_clients_mutex);
                for (int k = 0; k < active_count; ++k)
                {
                    if (strcmp(active_clients[k].username, username_by_slot[i]) == 0)
                    {
                        for (int m = k; m < active_count - 1; ++m)
                            active_clients[m] = active_clients[m + 1];
                        active_count--;
                        break;
                    }
                }
                pthread_mutex_unlock(&active_clients_mutex);

                close(sd);
                FD_CLR(sd, &master_set);
                client_sockets[i] = -1;
                login_status[i] = 0;
                username_by_slot[i][0] = '\0';
                conn_count--;
                continue;
            }

            // stuffs the clients message into a the task struct, which then gets enqueued for processed by the above workerthread function
            Task t = {.client_socket = sd};
            strncpy(t.message, buf, sizeof(t.message) - 1);
            t.message[sizeof(t.message) - 1] = '\0';
            enqueue(&workQueue, t);
        }
    }

    close(serverSocket);
    sqlite3_close(db);

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}