/******************************************************************************
*   TinTin++                                                                  *
*   Copyright (C) 2005 (See CREDITS file)                                     *
*                                                                             *
*   This program is protected under the GNU GPL (See COPYING)                 *
*                                                                             *
*   This program is free software; you can redistribute it and/or modify      *
*   it under the terms of the GNU General Public License as published by      *
*   the Free Software Foundation; either version 2 of the License, or         *
*   (at your option) any later version.                                       *
*                                                                             *
*   This program is distributed in the hope that it will be useful,           *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of            *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
*   GNU General Public License for more details.                              *
*                                                                             *
*   You should have received a copy of the GNU General Public License         *
*   along with this program; if not, write to the Free Software               *
*   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA *
******************************************************************************/

/******************************************************************************
*   file: chat.c - funtions related to p2p chatting                           *
*           (T)he K(I)cki(N) (T)ickin D(I)kumud Clie(N)t ++ 2.00              *
*                     coded by Sean Butler 1998                               *
*                   recoded by Igor van den Hoven 2005                        *
******************************************************************************/

#include "tintin.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>


#define CALL_TIMEOUT 5
#define BLOCK_SIZE 500
#define DEFAULT_PORT 4050
#define TINTIN_VERSION "TinTin++ 1.95.9"

#define RESTRING(point, value)   \
{                                \
	free(point);                \
	point = strdup((value));    \
}

#define STRFREE(point)           \
{                                \
	free((point));              \
	point = NULL;               \
}

DO_COMMAND(do_chat)
{
	char left[BUFFER_SIZE], right[BUFFER_SIZE];
	int cnt;

	arg = get_arg_in_braces(arg, left, FALSE);
	arg = get_arg_in_braces(arg, right, TRUE);

	if (*left == 0)
	{
		tintin_header(NULL, " CHAT COMMANDS ");

		for (cnt = 0 ; *chat_table[cnt].name != 0 ; cnt++)
		{
			if (strlen(left) + 20 > ses->cols)
			{
				tintin_printf(NULL, "%s", left);
				*left = 0;
			}
			cat_sprintf(left, "%-20s", chat_table[cnt].name);
		}
		if (*left)
		{
			tintin_printf(NULL, "%s", left);
		}
		tintin_header(NULL, "");

		return ses;
	}

	for (cnt = 0 ; cnt < *chat_table[cnt].name != 0 ; cnt++)
	{
		if (!is_abbrev(left, chat_table[cnt].name))
		{
			continue;
		}

		if (chat_table[cnt].chat != chat_initialize && gtd->chat == NULL)
		{
			tintin_printf(NULL, "\033[1;31m<CHAT> You must initialize a chat port first.");

			return ses;
		}

		chat_table[cnt].chat (right);

		return ses;
	}

	do_chat(ses, "");

	return ses;
}


/*
	Get ready to receive connections.
*/

DO_CHAT(chat_initialize)
{
	char hostname[BUFFER_SIZE];
	struct sockaddr_in sa;
	struct hostent *hp = NULL;
	const char *reuse = "1";
	int sock;

	if (gtd->chat)
	{
		chat_printf("Already initialised");

		return;
	}

	gtd->chat = calloc(1, sizeof(struct chat_data));

	gtd->chat->port     = *arg ? atoi(arg) : DEFAULT_PORT;
	gtd->chat->name     = strdup("TinTin");
	gtd->chat->ip       = strdup("<Unknown>");
	gtd->chat->reply    = strdup("");
	gtd->chat->color    = strdup("\033[1;31m");

	chat_downloaddir("");

	gethostname(hostname, BUFFER_SIZE);

	hp = gethostbyname(hostname);

	if (hp == NULL)
	{
		perror("chat_initialize: gethostbyname");

		return;
	}

	sa.sin_family      = hp->h_addrtype;
	sa.sin_port        = htons(gtd->chat->port);
	sa.sin_addr.s_addr = 0;

	sock = socket(AF_INET, SOCK_STREAM, 0);

	if (sock < 0)
	{
		perror("chat_initialize: socket");

		return;
	}

	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reuse, sizeof(reuse));

	if (bind(sock, (struct sockaddr *) &sa, sizeof(sa)) < 0)
	{
		chat_printf("Port in use, cannot initiate chat.");

		close(sock);

		return;
	}

/*	nonblock(sock); */

	if (listen(sock, 3) == -1)
	{
		perror("chat_initialize: listen:");

		close(sock);

		return;
	}
	gtd->chat->fd = sock;

	chat_printf("Initialized chat on port %d.", gtd->chat->port);
}


/*
	Accept an incoming chat connection.
*/

int chat_new(int s)
{
	struct chat_data *new_buddy;
	struct sockaddr_in isa;
	int i, fd;
	struct timeval to;

	to.tv_sec  = 0;
	to.tv_usec = 0;

	i = sizeof(isa);

	getsockname(s, (struct sockaddr *) &isa, &i);

	if ((fd = accept(s, (struct sockaddr *) &isa, &i)) < 0)
	{
		perror("chat_new: accept");

		return -1;
	}

	if (HAS_BIT(gtd->chat->flags, CHAT_FLAG_DND))
	{
		close(fd);

		return -1;
	}

/*	nonblock(fd); */

	new_buddy = calloc(1, sizeof(struct chat_data));

	new_buddy->ip   = strdup(inet_ntoa(isa.sin_addr));
	new_buddy->fd   = fd;
	new_buddy->name = strdup("Unknown");

	new_buddy->timeout = CALL_TIMEOUT + time(NULL);

	LINK(new_buddy, gtd->chat->next, gtd->chat->prev);

	chat_printf("New connection: %s D%d.", new_buddy->ip, new_buddy->fd);

	return 0;
}


/*
	Places a call to another chat client.
*/

void *threaded_chat_call(void *arg)
{
	int sock, dig;
	char ip[BUFFER_SIZE], pn[BUFFER_SIZE];
	struct sockaddr_in dest_addr;
	struct chat_data *new_buddy;
	struct timeval to;
	fd_set wds, rds;

	chat_printf("Attempting to call %s ...", arg);

	to.tv_sec = CALL_TIMEOUT;                
	to.tv_usec = 0;

	arg = (void *) get_arg_in_braces((const char *) arg, ip, FALSE);
	arg = (void *) get_arg_in_braces((const char *) arg, pn, FALSE);

	if (*pn == 0)
	{
		sprintf(pn, "%d", DEFAULT_PORT);
	}

	if (sscanf(ip, "%d.%d.%d.%d", &dig, &dig, &dig, &dig) == 4)
	{
		dest_addr.sin_addr.s_addr = inet_addr(ip);
	}
	else
	{
		struct hostent *hp;

		if ((hp = gethostbyname(ip)) == NULL)
		{
			chat_printf("Failed to call %s, unknown host.", ip);

			return NULL;
		}
		memcpy((char *)&dest_addr.sin_addr, hp->h_addr, sizeof(dest_addr.sin_addr));
	}

	if (is_number(pn))
	{
		dest_addr.sin_port = htons(atoi(pn));
	}
	else
	{
		chat_printf("The port should be a number.");

		return NULL;
	}

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		syserr("socket");
	}

	dest_addr.sin_family = AF_INET;

	if (connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0)
	{
		chat_printf("Failed to connect to %s:%s", ip, pn);

		return NULL;
	}

	FD_ZERO(&wds);

	FD_SET(sock, &wds); /* mother chat desc */

	if (select(FD_SETSIZE, NULL, &wds, NULL, &to) == -1)
	{
		chat_printf("Failed to connect to %s %s", ip, pn);

		return NULL;
	}

	if (!FD_ISSET(sock, &wds))
	{
		chat_printf("Connection timed out.");

		return NULL;
	}

/*	nonblock(sock); */

	new_buddy = calloc(1, sizeof(struct chat_data));

	chat_printf("Socket connected, negotiating protocol...");

	new_buddy->ip   = strdup(ip);
	new_buddy->port = atoi(pn);
	new_buddy->fd   = sock;
	new_buddy->name = strdup("");


	chat_socket_printf(new_buddy, "CHAT:%s\n%s%-5u", gtd->chat->name, gtd->chat->ip, gtd->chat->port);

	FD_ZERO(&rds);
	FD_SET(sock, &rds);

	to.tv_sec  = CALL_TIMEOUT;
	to.tv_usec = 0;

	if (select(FD_SETSIZE, &rds, NULL, NULL, &to) == -1)
	{
		close_chat(new_buddy, FALSE);

		return NULL;
	}

	if (process_chat_input(new_buddy) == -1)
	{
		close_chat(new_buddy, FALSE);

		return NULL;
	}

	if (*new_buddy->name == 0)
	{
		close_chat(new_buddy, FALSE);
	}
	else
	{
		LINK(new_buddy, gtd->chat->next, gtd->chat->prev);
		chat_printf("Connection made to %s.", new_buddy->name);
	}

	return NULL;
}

void chat_call(const char *arg)
{
	pthread_t thread;

	pthread_create(&thread, NULL, (void *) threaded_chat_call, (void *) arg);
}
	

/*
	Clean up and close a chat conneciton.
*/

void close_chat(struct chat_data *buddy, int unlink)
{
	buddy->flags = 0;

	if (*buddy->name == 0)
	{
		chat_printf("Closing connection to %s D%d", buddy->ip, buddy->fd);
	}
	else
	{
		chat_printf("Closing connection to %s@%s.", buddy->name, buddy->ip);
	}

	if (unlink)
	{
		UNLINK(buddy, gtd->chat->next, gtd->chat->prev);
	}
	STRFREE(buddy->name);
	STRFREE(buddy->ip);
	STRFREE(buddy->download);
	STRFREE(buddy->version);

	close(buddy->fd);

	free(buddy);
}


/*
	Set a socket to nonblocking.
*/

void nonblock(int s)
{
	int flags;

	if ((flags = fcntl(s, F_GETFL, 0)) == -1)
	{
		exit(1);
	}

	SET_BIT(flags, O_NONBLOCK);

	if (fcntl(s, F_SETFL, flags) == -1)
	{
		perror("nonblock: fcntl");
		exit(1);
	}
}

/*
	Set a socket to blocking.
*/

void block(int s)
{
	int flags;

	if ((flags = fcntl(s, F_GETFL, 0)) == -1)
	{
		exit(1);
  	}

	DEL_BIT(flags, O_NONBLOCK);

	if (fcntl(s, F_SETFL, flags) == -1)
  	{
  		perror("block: fcntl");
  		exit(1);
  	}
}

/*
	Check for incoming calls and poll for input on 
	                              connected sockets.
*/

void process_chat_connections(fd_set *input_set, fd_set *exc_set)
{
	struct chat_data *buddy, *buddy_next;

	push_call("process_chat_connections(%p,%p)",input_set,exc_set);

	/*
		accept incoming connections
	*/

	if (FD_ISSET(gtd->chat->fd, input_set))
	{
		chat_new(gtd->chat->fd);
	}

	/*
		read from sockets
	*/

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy_next)
	{
		buddy_next = buddy->next;

		if (FD_ISSET(buddy->fd, input_set))
		{
			if (process_chat_input(buddy) < 0)
			{
				close_chat(buddy, TRUE);
			}
		}
	}
	pop_call();
	return;
}

void chat_socket_printf(struct chat_data *buddy, const char *format, ...)
{
	char buf[BUFFER_SIZE];
	va_list args;

	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	if (write(buddy->fd, buf, strlen(buf)) < 0)
	{
		chat_printf("%s went link lost.", buddy->name);
		close_chat(buddy, TRUE);
	}
}

void chat_printf(const char *format, ...)
{
	struct chat_data *buddy;
	char buf[BUFFER_SIZE], tmp[BUFFER_SIZE];
	va_list args;

	va_start(args, format);
	vsprintf(buf, format, args);
	va_end(args);

	if (strncmp(buf, "<CHAT>", 6))
	{
		sprintf(tmp, "<CHAT> %s", buf);
	}
	else
	{
		sprintf(tmp, "%s", buf);
	}

	strip_vt102_codes_non_graph(tmp, buf);

	sprintf(tmp, "%c%s%c", CHAT_SNOOP_DATA, buf, CHAT_END_OF_COMMAND);

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (HAS_BIT(buddy->flags, CHAT_FLAG_FORWARD))
		{
			chat_socket_printf(buddy, "%s", tmp);
		}
	}
	sprintf(tmp, "%s%s%s", gtd->chat->color, buf, "\033[0m");

	tintin_puts(tmp, NULL);
}

/*
	Read and store for parsing all input on a socket.
	Then call get_chat_commands() to parse out the commands.
*/

int process_chat_input(struct chat_data *buddy)
{
	struct chat_data *node;

	char buf[BUFFER_SIZE], name[BUFFER_SIZE], ip[BUFFER_SIZE], *sep;
	int size;

	push_call("process_chat_input(%p)",buddy);

	size = read(buddy->fd, buf, BUFFER_SIZE - 1000);

	if (size <= 0)
	{
		pop_call();
		return -1;
	}

	buf[size] = 0;

	/* deal with connections individually */

	if (!strncmp(buf, "CHAT:", 5))
	{
		if ((sep = strchr(buf, '\n')) != NULL)
		{
			*sep = 0;

			strcpy(name, &buf[5]);
			strcpy(ip,   &sep[1]);

			if (strlen(ip) >= 5)
			{
				buddy->port = atoi(ip + strlen(ip) - 5);
			}

			RESTRING(buddy->name, name);

			buddy->timeout = 0;

			for (node = gtd->chat ; node ; node = node->next)
			{
				if (node != buddy && !strcasecmp(name, node->name))
				{
					chat_socket_printf(buddy, "%c\n%s is already connected to someone named %s.\n%c", CHAT_MESSAGE, gtd->chat->name, name, CHAT_END_OF_COMMAND);

					chat_printf("Refusing connection from %s:%d, already connected to someone named %s.", buddy->ip, buddy->port, name);

					pop_call();
					return -1;
				}
			}
		}

		chat_socket_printf(buddy, "YES:%s\n", gtd->chat->name);

		chat_printf("Connected to %s@%s:%d", buddy->name, buddy->ip, buddy->port);

		pop_call();
		return 1;
	}

	if (!strncmp(buf, "YES:", 4))
	{
		if (sscanf(buf, "YES:%s\n", name) == 1)
		{
			buddy->name = strdup(name);

			chat_socket_printf(buddy, "%c%s%c", CHAT_VERSION, TINTIN_VERSION, CHAT_END_OF_COMMAND);

			sep = strchr(buf, '\n');

			*sep++ = 0;

			get_chat_commands(buddy, sep, size - strlen(buf));

			pop_call();
			return 0;
		}
		else
		{
			pop_call();
			return -1;
		}
	}

	if (!strncmp(buf, "NO", 2))
	{
		pop_call();
		return -1;
	}

	get_chat_commands(buddy, buf, size);

	pop_call();
	return 0;
}

/*
	Parse the input queue for commands and execute the
	apropriate function.
*/

void get_chat_commands(struct chat_data *buddy, char *buf, int len)
{
	char txt[BUFFER_SIZE];
	unsigned char *pto, *pti, ptc;

	push_call("get_chat_commands(%s,%d,%s)",buddy->name,len,buf);

	pti = buf;
	pto = txt;

	/*
		have to deal with these first
	*/

	while (*pti == CHAT_FILE_BLOCK)
	{
		receive_block((pti + 1), buddy);

		len -= 502;

		if (len <= 0)
		{
			pop_call();
			return;
		}
		pti += 502;
	}

	while (*pti)
	{
		ptc = *pti++;
		pto = txt;

		while (isspace(*pti))
		{
			pti++;
		}

		while (*pti != CHAT_END_OF_COMMAND)
		{
			if (*pti == 0)
			{
				chat_printf("Invalid message: %d %s", ptc, buf);
				pop_call();
				return;
			}
			*pto++ = *pti++;
		}

		*pto-- = 0;

		if (strlen(txt))
		{
			while (isspace(*pto))
			{
				*pto-- = 0;
			}
		}

		switch (ptc)
		{
			case CHAT_NAME_CHANGE:
				buddy->name = strdup(txt);
				chat_printf("%s is now %s.\n", buddy->name, txt);
				break;

			case CHAT_REQUEST_CONNECTIONS:
				request_response(buddy);
				break;

			case CHAT_CONNECTION_LIST:
				parse_requested_connections(buddy, txt);
				break;

			case CHAT_TEXT_EVERYBODY:
				chat_receive_text_everybody(buddy, txt);
				break;

			case CHAT_TEXT_PERSONAL:
				chat_receive_text_personal(buddy, txt);
				break;

			case CHAT_TEXT_GROUP:
				chat_socket_printf(buddy, "%c%s%c", CHAT_MESSAGE, "\nTintin++ does not yet support this.\n", CHAT_END_OF_COMMAND);
				break;

			case CHAT_MESSAGE:
				chat_receive_message(buddy, txt);
				break;

			case CHAT_DO_NOT_DISTURB:
				chat_printf("%s has enabled DND.", buddy->name);
				break;

			case CHAT_SEND_ACTION:
			case CHAT_SEND_ALIAS:
			case CHAT_SEND_VARIABLE:
			case CHAT_SEND_EVENT:
			case CHAT_SEND_GAG:
			case CHAT_SEND_HIGHLIGHT:
			case CHAT_SEND_LIST:
			case CHAT_SEND_ARRAY:
			case CHAT_SEND_BARITEM:
				chat_socket_printf(buddy, "%c%s%c", CHAT_MESSAGE, "\nTintin++ does not support this.\n", CHAT_END_OF_COMMAND);
				break;

			case CHAT_VERSION:
				if (buddy->version == NULL)
				{
					chat_socket_printf(buddy, "%c%s%c", CHAT_VERSION, TINTIN_VERSION, CHAT_END_OF_COMMAND);

					buddy->version = strdup(txt);
				}
				break;

			case CHAT_FILE_START:
				chat_receive_file(txt, buddy);
				break;

			case CHAT_FILE_DENY:
				file_denied(buddy, txt);
				break;

			case CHAT_FILE_BLOCK_REQUEST:
				send_block(buddy);
				break;

			case CHAT_FILE_BLOCK:
				receive_block(txt, buddy);
				break;

			case CHAT_FILE_END:
				chat_printf("File transfer completion acknowledged.");
				break;

			case CHAT_FILE_CANCEL:
				chat_printf("File cancel request received.");
				file_cleanup(buddy);
				break;

			case CHAT_PING_REQUEST:
				chat_socket_printf(buddy, "%c%s%c", CHAT_PING_RESPONSE, txt, CHAT_END_OF_COMMAND);
				break;

			case CHAT_PING_RESPONSE:
				ping_response(buddy, txt);
				break;

			case CHAT_PEEK_CONNECTIONS:
				peek_response(buddy);
				break;

			case CHAT_PEEK_LIST:
				parse_peeked_connections(buddy, txt);
				break;

			case CHAT_SNOOP_START:
				break;

			case CHAT_SNOOP_DATA:
				SET_BIT(buddy->flags, CHAT_FLAG_FORWARDBY);
				chat_receive_snoop_data(buddy, txt);
				break;

			default:
				tintin_printf(NULL, "get_chat_commands: unknown option [%d] (%s)", ptc, txt);
				break;
		}
		pti++;
	}
	pop_call();
	return;
}

/**************************************************************************
 *  CHAT COMMUNICATION ROUTINES                                           *
 *************************************************************************/


void chat_receive_text_everybody(struct chat_data *buddy, char *txt)
{
	struct chat_data *node;

	if (HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE))
	{
		return;
	}

	chat_printf("%s", txt);

	if (HAS_BIT(buddy->flags, CHAT_FLAG_SERVE))
	{
		for (node = gtd->chat->next ; node ; node = node->next)
		{
			if (node != buddy)
			{
				chat_socket_printf(node, "%c%s%c", CHAT_MESSAGE, txt, CHAT_END_OF_COMMAND);
			}
		}
	}
	else
	{
		for (node = gtd->chat->next ; node ; node = node->next)
		{
			if (HAS_BIT(node->flags, CHAT_FLAG_SERVE))
			{
				chat_socket_printf(node, "%c%s%c", CHAT_SNOOP_DATA, txt, CHAT_END_OF_COMMAND);
			}
		}
	}
}


void chat_receive_text_personal(struct chat_data *buddy, char *txt)
{
	if (HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE))
	{
		return;
	}
	RESTRING(gtd->chat->reply, buddy->name);

	chat_printf("%s", txt);
}


void chat_receive_message(struct chat_data *buddy, char *txt)
{
	if (HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE))
	{
		return;
	}

	chat_printf("%s", txt);
}


void chat_receive_snoop_data(struct chat_data *buddy, char *txt)
{
	if (HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE))
	{
		return;
	}

	chat_printf("%s", txt);
}


void peek_response(struct chat_data *peeker)
{
	struct chat_data *buddy;
	char buf[BUFFER_SIZE];

	for (buf[0] = 0, buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (!HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
		{
			cat_sprintf(buf, "%s~%d~%s~", buddy->ip, buddy->port, buddy->name);
		}
	}
	chat_socket_printf(peeker, "%c%s%c", CHAT_PEEK_LIST, buf, CHAT_END_OF_COMMAND);

	return;
}


void parse_peeked_connections(struct chat_data *buddy, char *txt)
{
	char *comma, ip[BUFFER_SIZE], port[BUFFER_SIZE], name[BUFFER_SIZE];

	if (*txt == 0)
	{
		chat_printf("%s has no public connections.", buddy->name);

		return;
	}

	ip[0] = port[0] = name[0] = 0;

	comma = txt;

	tintin_printf(NULL, " %-15s   %-15s   %-5s", "Name", "Address", "Port");
	tintin_printf(NULL, " ---------------   ---------------   ----- ");

	while (comma)
	{
		comma = strchr(txt, '~');

		if (comma)
		{
			*comma = 0;
		}

		if (*ip == 0)
		{
			strcpy(ip, txt);
		}
		else if (*port == 0)
		{
			strcpy(port, txt);
		}
		else
		{
			strcpy(name, txt);
		}

		if (comma)
		{
			txt += strlen(txt) + 1;
		}

		if (*ip && *port && *name)
		{
			tintin_printf(NULL, " %-15s   %-15s   %-5s", name, ip, port);

			*port = 0;
			*ip   = 0;
			*name = 0;
		}
	}
	return;
}


void ping_response(struct chat_data *ch, char *time)
{
	chat_printf("Ping response time for %s: %lld ms", ch->name, (utime() - atoll(time)) / 1000);
}


void request_response(struct chat_data *requester)
{
	struct chat_data *buddy;
	char buf[BUFFER_SIZE], tmp[BUFFER_SIZE];

	chat_printf("%s has requested your public connections.", requester->name);

	for (buf[0] = 0, buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (buddy != requester && !HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
		{
			sprintf(tmp, "%s,%-5u", buddy->ip, buddy->port);

			if (*buf)
			{
				strcat(buf, ",");
			}
			strcat(buf, tmp);
		}
	}
	chat_socket_printf(requester, "%c%s%c", CHAT_CONNECTION_LIST, buf, CHAT_END_OF_COMMAND);

	return;
}


void parse_requested_connections(struct chat_data *buddy, char *txt)
{
	struct chat_data *node;
	char *comma, ip[BUFFER_SIZE], port[BUFFER_SIZE], address[BUFFER_SIZE];

	if (!HAS_BIT(buddy->flags, CHAT_FLAG_REQUEST))
	{
		chat_printf("%s tried to force your client to connect to: %s.", buddy->name, txt);

		return;
	}



	ip[0] = 0;
	port[0] = 0;
	comma = txt;

	while (comma)
	{
		comma = strchr(txt, ',');

		if (comma)
		{
			*comma = 0;
		}

		if (*ip == 0)
		{
			strcpy(ip, txt);
		}
		else
		{
			strcpy(port, txt);
		}

		if (comma)
		{
			txt += strlen(txt) + 1;
		}

		if (*ip && *port)
		{
			for (node = gtd->chat->next ; node ; node = node->next)
			{
				if (!strcmp(ip, node->ip) && atoi(port) == node->port)
				{
					chat_printf("skipping known address: %s port %s", ip, port);
					break;
				}
			}
			if (node == NULL)
			{
				sprintf(address, "%s %s", ip, port);
				chat_call(address);
			}
			*port = 0;
			*ip   = 0;
		}
	}
	DEL_BIT(buddy->flags, CHAT_FLAG_REQUEST);

	return;
}



/*
	USER COMMANDS
*/


DO_CHAT(chat_downloaddir)
{
	char dir[BUFFER_SIZE];

	if (gtd->chat->download == NULL)
	{
		sprintf(dir, "%s/", getenv("HOME"));

		gtd->chat->download = strdup(dir);

		return;
	}

	sprintf(dir, "%s%s", arg, is_suffix("/", arg) ? "/" : "");

	RESTRING(gtd->chat->download, dir);

	chat_printf("Download directory set to '%s'", gtd->chat->download);
}


DO_CHAT(chat_emote)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE], right[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left,  FALSE);
	arg = get_arg_in_braces(arg, temp,  TRUE);

	substitute(gtd->ses, temp, right, SUB_COL|SUB_ESC);

	if (!strcasecmp(left, "ALL"))
	{
		chat_printf("You emote to everyone: %s %s", gtd->chat->name, right);

		for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
		{
			chat_socket_printf(buddy, "%c\n%s %s\n%c", CHAT_TEXT_EVERYBODY, gtd->chat->name, right, CHAT_END_OF_COMMAND);
		}
	}
	else
	{
		if ((buddy = find_buddy(left)) != NULL)
		{
			chat_printf("You emote to %s: %s %s", buddy->name, gtd->chat->name, right);

			chat_socket_printf(buddy, "%c\n%s %s\n%c", CHAT_TEXT_PERSONAL, gtd->chat->name, right, CHAT_END_OF_COMMAND);
		}
		else
		{
			chat_printf("You are not connected to anyone named '%s'.", left);
		}
	}
}


DO_CHAT(chat_info)
{
	if (*arg == 0)
	{
		tintin_printf2(NULL, "Name                 : %s", gtd->chat->name);
		tintin_printf2(NULL, "IP Address           : %s", gtd->chat->ip);
		tintin_printf2(NULL, "Chat Port            : %d", gtd->chat->port);
		tintin_printf2(NULL, "Download Dir         : %s", gtd->chat->download);
		tintin_printf2(NULL, "Reply                : %s", gtd->chat->reply);
		tintin_printf2(NULL, "DND                  : %s", HAS_BIT(gtd->chat->flags, CHAT_FLAG_DND) ? "Yes" : "No");
	}
}


DO_CHAT(chat_ip)
{
	RESTRING(gtd->chat->ip, arg);

	chat_printf("IP changed to %s", gtd->chat->ip);
}


DO_CHAT(chat_message)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE], right[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left, FALSE);
	arg = get_arg_in_braces(arg, temp, TRUE);

	substitute(gtd->ses, temp, right, SUB_COL|SUB_ESC);

	if (!strcasecmp(left, "ALL"))
	{
		chat_printf("You chat to everyone, '%s'", right);

		for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
		{
			chat_socket_printf(buddy, "%c\n%s chats to everyone, '%s'\n%c", CHAT_TEXT_EVERYBODY, gtd->chat->name, right, CHAT_END_OF_COMMAND);
		}
	}
	else
	{
		if ((buddy = find_buddy(left)) != NULL)
		{
			chat_printf("You chat to %s, '%s'", buddy->name, right);

			chat_socket_printf(buddy, "%c\n%s chats to you, '%s'\n%c", CHAT_TEXT_PERSONAL, gtd->chat->name, right, CHAT_END_OF_COMMAND);
		}
		else
		{
			chat_printf("You are not connected to anyone named '%s'.", left);
		}
	}
}


DO_CHAT(chat_name)
{
	struct chat_data *buddy;

	if (!strcmp(gtd->chat->name, arg))
	{
		chat_printf("Your name is already set to %s.", gtd->chat->name);

		return;
	}
	RESTRING(gtd->chat->name, arg);

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		chat_socket_printf(buddy, "%c%s%c", CHAT_NAME_CHANGE, gtd->chat->name, CHAT_END_OF_COMMAND);
	}
	chat_printf("Name changed to %s.", gtd->chat->name);
}


DO_CHAT(chat_paste)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE], right[BUFFER_SIZE];

	if (arg == NULL)
	{
		if (strlen(rl_line_buffer))
		{
			sprintf(temp, "%s\n%s", gtd->chat->paste_buf, rl_line_buffer);

			RESTRING(gtd->chat->paste_buf, temp);

			rl_point = 0;

			rl_delete_text(0, strlen(rl_line_buffer));

			rl_redisplay();
		}

		arg = get_arg_in_braces(gtd->chat->paste_buf, left, FALSE);

		sprintf(temp, "%s\n<078>======================================================================", arg);

		substitute(gtd->ses, temp, right, SUB_COL|SUB_ESC);

		RESTRING(gtd->chat->paste_buf, right);

		if (!strcasecmp(left, "ALL"))
		{
			chat_printf("You paste to everyone:\n%s", gtd->chat->paste_buf);

			for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
			{
				chat_socket_printf(buddy, "%c\n%s pastes to everyone:\n%s\n%c", CHAT_TEXT_EVERYBODY, gtd->chat->name, gtd->chat->paste_buf, CHAT_END_OF_COMMAND);
			}
		}
		else
		{
			if ((buddy = find_buddy(left)) != NULL)
			{
				chat_printf("You paste to %s:\n%s", buddy->name, gtd->chat->paste_buf);
	
				chat_socket_printf(buddy, "%c\n%s pastes to you:\n%s\n%c", CHAT_TEXT_EVERYBODY, gtd->chat->name, gtd->chat->paste_buf, CHAT_END_OF_COMMAND);
			}
			else
			{
				chat_printf("You are not connected to anyone named '%s'.", left);
			}
		}
		if (IS_SPLIT(gtd->ses))
		{
			erase_toeol();
		}
		gtd->chat->paste_time = 0;

		return;
	}

	if (gtd->chat->paste_time)
	{
		sprintf(temp, "%s\n%s", gtd->chat->paste_buf, arg);

		RESTRING(gtd->chat->paste_buf, temp);

		gtd->chat->paste_time = 200000LL + utime();

		return;
	}

	arg = get_arg_in_braces(arg, left, FALSE);
	arg = get_arg_in_braces(arg, right, TRUE);

	gtd->chat->paste_time = 400000LL + utime();

	sprintf(temp, "{%s}<078>======================================================================\n<068>%s", left, right);

	RESTRING(gtd->chat->paste_buf, temp);
}


DO_CHAT(chat_peek)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}
	chat_socket_printf(buddy, "%c%c", CHAT_PEEK_CONNECTIONS, CHAT_END_OF_COMMAND);
}


void chat_ping(const char *arg)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	chat_socket_printf(buddy, "%c%lld%c", CHAT_PING_REQUEST, utime(), CHAT_END_OF_COMMAND);

	chat_printf("Ping request sent to %s.", buddy->name);
}


DO_CHAT(chat_reply)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, temp, TRUE);

	substitute(gtd->ses, temp, left, SUB_COL|SUB_ESC);

	if ((buddy = find_buddy(gtd->chat->reply)) != NULL)
	{
		chat_printf("You reply to %s, '%s'", buddy->name, left);

		chat_socket_printf(buddy, "%c\n%s replies to you, '%s'\n%c", CHAT_TEXT_PERSONAL, gtd->chat->name, left, CHAT_END_OF_COMMAND);
	}
	else
	{
		chat_printf("You are not connected to anyone named '%s'.", gtd->chat->reply);
	}
}

DO_CHAT(chat_request)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}
	chat_socket_printf(buddy, "%c%c", CHAT_REQUEST_CONNECTIONS, CHAT_END_OF_COMMAND);

	chat_printf("You request %s's public connections.", buddy->name);

	SET_BIT(buddy->flags, CHAT_FLAG_REQUEST);

	return;
}


DO_CHAT(chat_serve)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	TOG_BIT(buddy->flags, CHAT_FLAG_SERVE);

	if (HAS_BIT(buddy->flags, CHAT_FLAG_SERVE))
	{
		chat_printf("You are now chat serving %s.", buddy->name);
		chat_socket_printf(buddy, "%c\n%s is now chat serving you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND); 
	}
	else
	{
		chat_printf("You are no longer chat serving %s.", buddy->name);
		chat_socket_printf(buddy, "%c\n%s is no longer chat serving you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND); 
	}
}


DO_CHAT(chat_who)
{
	struct chat_data *buddy;
	int cnt = 1;

	tintin_printf(NULL, "     %-15s  %-5s  %-20s  %-5s  %-15s", "Name", "Flags", "Address", "Port", "Client");
	tintin_printf(NULL, "     ===============  =====  ====================  =====  ==================== ");

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		tintin_printf(NULL, " %03d %-15s  %s%s%s%s%s  %-20s  %-5u  %-20s",
			cnt++,
			buddy->name,
			HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE)   ? "P" : " ",
			HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE)    ? "I" : " ",
			HAS_BIT(buddy->flags, CHAT_FLAG_SERVE)     ? "S" : " ",
			HAS_BIT(buddy->flags, CHAT_FLAG_FORWARD)   ? "F" :
			HAS_BIT(buddy->flags, CHAT_FLAG_FORWARDBY) ? "f" : " ",
			" ",
			buddy->ip,
			buddy->port,
			buddy->version);
	}
	tintin_printf(NULL, "     ===============  =====  ====================  =====  ==================== ");
}


DO_CHAT(chat_zap)
{
	struct chat_data *buddy;

	if (!strcasecmp(arg, "ALL"))
	{
		while (gtd->chat->next)
		{
			close_chat(gtd->chat->next, TRUE);
		}
	}
	else
	{
		if ((buddy = find_buddy(arg)))
		{
			close_chat(buddy, TRUE);
		}
		else
		{
			chat_printf("You are not connected to anyone named '%s'.", arg);
		}
	}
}



/**************************************************************************
 * FILE TRANSFER FUNCTIONS                                                *
 *************************************************************************/


DO_CHAT(chat_accept)
{
	char left[BUFFER_SIZE], right[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left, FALSE);
	arg = get_arg_in_braces(arg, right, TRUE);

	struct chat_data *buddy;

	if ((buddy = find_buddy(left)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	if (buddy->file_pt == NULL)
	{
		chat_printf("ERROR: You don't have a file transfer in progress with %s.", buddy->name);

		return;
	}

	if (buddy->file_start_time)
	{
		chat_printf("ERROR: You already have a file transfer in progress with %s.", buddy->name);

		return;
	}

	buddy->file_start_time = utime();

	/*
		I'm sending 3 requests here to speed things up. - Scandum
	*/

	if (buddy->file_block_tot >= 10 && !strcasecmp(right, "boost"))
	{
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
	}
	chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);

	chat_printf("Started %sfile transfer from %s, file: %s, size: %lld", !strcasecmp(right, "boost") ? "boosted " : "", buddy->name, buddy->file_name, buddy->file_size);
}


DO_CHAT(chat_decline)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	if (buddy->file_pt == NULL)
	{
		chat_printf("You don't have a file transfer in progress with %s.", buddy->name);

		return;
	}

	if (buddy->file_start_time)
	{
		chat_printf("You already have a file transfer in progress with %s.", buddy->name);

		return;
	}

	deny_file(buddy, "\nYour file transfer was rejected.\n");
}

/*
	Send the initial info about the transfer to take place.
	Can only send one file at a time to a chat connection.
	Cannot both send and receive on the same connection.
	One file to or from each chat connection is ok however.
*/

DO_CHAT(chat_sendfile)
{
	char left[BUFFER_SIZE];
	char right[BUFFER_SIZE];
	struct chat_data *buddy;

	/*
		Determine the chat connection refered to
	*/

	arg = get_arg_in_braces(arg, left,  0);
	arg = get_arg_in_braces(arg, right, 0);

	if (*left == 0 || *right == 0)
	{
		chat_printf("USAGE: #sendfile <person> <filename>");

		return;
	}

	if ((buddy = find_buddy(left)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", left);

		return;
	}

	if (buddy->file_pt)
	{
		chat_printf("ERROR: You already have a file transfer in progress with that person.");

		return;
	}

	buddy->file_block_cnt = 0;
	buddy->file_block_tot = 0;

	buddy->file_name = strdup(fix_file_name(right));

	/*
		Open file for read
	*/

	if ((buddy->file_pt = fopen(right, "r")) == NULL)
	{
		chat_printf("ERROR: No such file.");

		file_cleanup(buddy);
		return;
	}

	/*
		determine its size
	*/

	if ((buddy->file_size = get_file_size(right)) == 0)
	{
		chat_printf("Cannot send an empty file.");

		file_cleanup(buddy);

		return;
	}

	buddy->file_block_tot = buddy->file_size / BLOCK_SIZE + (buddy->file_size % BLOCK_SIZE ? 1 : 0);

	/*
		Strip the dir info from the file name
	*/

	if (*buddy->file_name == 0)
	{
		chat_printf("Must be a file, directories not accepted.");

		file_cleanup(buddy);

		return;
	}

	buddy->file_start_time = utime();

	/*
		send notification about the pending xfer
	*/

	chat_socket_printf(buddy, "%c%s,%lld%c", CHAT_FILE_START, buddy->file_name, buddy->file_size, CHAT_END_OF_COMMAND);

	chat_printf("Sending file to: %s, File: %s, Size: %lld", buddy->name, buddy->file_name, buddy->file_size);

	return;
}


/*
	Prepare to receive a file from sender and then send the
	request for the first block of data.
*/

void chat_receive_file(char *arg, struct chat_data *buddy)
{
	char path[BUFFER_SIZE], *comma;

	push_call("chat_receive_file(%p,%p)",arg,buddy);

	if (buddy->file_pt)
	{
		deny_file(buddy, "\nThere is a transfer already in progress.\n");

		pop_call();
		return;
	}
	buddy->file_block_cnt = 0;
	buddy->file_block_tot = 0;
	buddy->file_size      = 0;

	/*
		Parse the args
	*/


	if ((comma = strchr(arg, ',')) == NULL)
	{
		deny_file(buddy, "\nFile protocol error. (no file size was transmitted)\n");

		pop_call();
		return;	
	}
	*comma = 0;

	buddy->file_name = strdup(arg);
	buddy->file_size = atoll(&comma[1]);

	if (strcmp(fix_file_name(buddy->file_name), buddy->file_name))
	{
		deny_file(buddy, "\nFilename sent with directory info. (rejected)\n");

		file_cleanup(buddy);

		pop_call();
		return;
	}

	if (buddy->file_size == 0)
	{
		deny_file(buddy, "\nFile protocol error. (no file size was transmitted)\n");

		file_cleanup(buddy);

		pop_call();
		return;
	}

	buddy->file_block_tot = buddy->file_size / BLOCK_SIZE + (buddy->file_size % BLOCK_SIZE ? 1 : 0);


	sprintf(path, "%s%s", gtd->chat->download, buddy->file_name);

	if ((buddy->file_pt = fopen(path, "w")) == NULL)
	{
		deny_file(buddy, "\nCould not create that file on receiver's end.\n");

		file_cleanup(buddy);

		pop_call();
		return;
	}

	chat_printf("File transfer from %s, file: %s, size: %d.", buddy->name, buddy->file_name, buddy->file_size);
	chat_printf("#accept or #decline?");

	buddy->file_start_time = 0;

	pop_call();
	return;
}

/*
	Send BLOCK_SIZE bytes of data to buddy in one block. 
*/

void send_block(struct chat_data *buddy)
{
	unsigned char block[BUFFER_SIZE], *pto;
	int i, c;

	if (buddy->file_pt == NULL)
	{
		return;
	}

	if (buddy->file_block_cnt == 0)
	{
		buddy->file_start_time = utime();

		chat_printf("%s started a file transfer, file: %s, size: %lld", buddy->name, buddy->file_name, buddy->file_size);
	}

	pto = block;

	*pto++ = CHAT_FILE_BLOCK;

	/*
		Read until we get BLOCK_SIZE bytes, or EOF
	*/

	for (i = 0; i < BLOCK_SIZE; i++)
	{
		c = fgetc(buddy->file_pt);

		if (c == EOF)
		{
			break;
		}
		*pto++ = (unsigned char) c;
	}
	*pto++ = CHAT_END_OF_COMMAND;
	*pto++ = 0;

	write(buddy->fd, block, 502);

	buddy->file_block_cnt++;

	/*
		if at the end, close the file and notify user
	*/

	if (i < BLOCK_SIZE)
	{
		chat_printf("File transfer: %s, to %s completed at %lld.%lld KB/s.",
			buddy->file_name,
			buddy->name,
			1000LL * buddy->file_size / (utime() - buddy->file_start_time),
			10000LL * buddy->file_size / (utime() - buddy->file_start_time) % 10);
			
			

		chat_socket_printf(buddy, "%c%c", CHAT_FILE_END, CHAT_END_OF_COMMAND);

		file_cleanup(buddy);
	}
}

/*
	Receive BLOCK_SIZE bytes of data for file sent by buddy.
*/

void receive_block(unsigned char *s, struct chat_data *buddy)
{
	int len;

	if (buddy->file_pt == NULL)
	{
		return;
	}

	/*
		keep track of blocks recvd so we know when we are done
	*/

	buddy->file_block_cnt++;

	/*
		compare the number of blocks received to the number needed
	*/

	if (buddy->file_block_cnt == buddy->file_block_tot)
	{
		len = buddy->file_size % BLOCK_SIZE;
		fwrite(s, 1, len, buddy->file_pt);

		chat_printf("Transfer of %s completed, size: %lld, speed: %lld.%lld KB/s.",
			buddy->file_name,
			buddy->file_size,
			1000LL * buddy->file_size / (utime() - buddy->file_start_time),
			10000LL * buddy->file_size / (utime() - buddy->file_start_time) % 10);

		file_cleanup(buddy);
	}
	else
	{
		fwrite(s, 1, BLOCK_SIZE, buddy->file_pt);

		chat_socket_printf(buddy, "%c%c", CHAT_FILE_BLOCK_REQUEST, CHAT_END_OF_COMMAND);
	}
}

/*
	Inform the sender that you will not accept a transfer.
*/

void deny_file(struct chat_data *ch, char *arg)
{
	chat_socket_printf(ch, "%c%s%c", CHAT_FILE_DENY, arg, CHAT_END_OF_COMMAND);
}

/*
	After a file deny message is received, clean up the file data.
*/

void file_denied(struct chat_data *buddy, char *txt)
{
	chat_printf("%s", txt);

	file_cleanup(buddy);
}

void file_cleanup(struct chat_data *buddy)
{
	if (buddy->file_pt)
	{
		fclose(buddy->file_pt);

		buddy->file_pt = NULL;
	}
	if (buddy->file_name)
	{
		STRFREE(buddy->file_name);
	}
}

/*
	Cancel a file transfer in progress.
*/

DO_CHAT(chat_cancelfile)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	if (buddy->file_pt == NULL)
	{
		return;
	}

	fclose(buddy->file_pt);

	buddy->file_pt = NULL;

	chat_printf("Okay, file transfer canceled");

	chat_socket_printf(buddy, "%c%c", CHAT_FILE_CANCEL, CHAT_END_OF_COMMAND);
}

DO_CHAT(chat_color)
{
	char left[BUFFER_SIZE], color[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left, TRUE);

	if (*left == 0 || get_highlight_codes(left, color) == FALSE)
	{
		chat_printf("Valid colors are:\r\n\r\nreset, bold, faint, underscore, blink, reverse, dim, black, red, green, yellow, blue, magenta, cyan, white, b black, b red, b green, b yellow, b blue, b magenta, b cyan, b white");

		return;
	}
	RESTRING(gtd->chat->color, color);

	chat_printf("Color has been set to %s", left);
}

DO_CHAT(chat_dnd)
{
	TOG_BIT(gtd->chat->flags, CHAT_FLAG_DND);

	if (HAS_BIT(gtd->chat->flags, CHAT_FLAG_DND))
	{
		chat_printf("New connections are no longer accepted.");
	}
	else
	{
		chat_printf("New connections are accepted.");
	}
}

/*
	Get statistics on the file transfer in progress.
*/

DO_CHAT(chat_filestat)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	if (buddy->file_pt == NULL)
	{
		chat_printf("You have no file transfer in progress with %s.", buddy->name);

		return;
	}

	tintin_printf(NULL, "  Contact: %s",   buddy->name);
	tintin_printf(NULL, " Filename: %s",   buddy->file_name);
	tintin_printf(NULL, " Filesize: %lld", buddy->file_size);
	tintin_printf(NULL, " Received: %d",   buddy->file_block_cnt * BLOCK_SIZE);
	tintin_printf(NULL, "    Speed: %lld KB/s", (1000 * buddy->file_block_cnt * BLOCK_SIZE) / (utime() - buddy->file_start_time));
}



/*
	TOGGLE FUNCTIONS
*/


DO_CHAT(chat_forward)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	TOG_BIT(buddy->flags, CHAT_FLAG_FORWARD);

	if (HAS_BIT(buddy->flags, CHAT_FLAG_FORWARD))
	{
		chat_socket_printf(buddy, "%c\n%s is now forwarding to you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

		chat_printf("You are now forwarding to %s.", buddy->name);
	}
	else
	{
		chat_socket_printf(buddy, "%c\n%s is no longer forwarding to you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

		chat_printf("You are no longer forwarding to %s.", buddy->name);
	}
}

DO_CHAT(chat_forwardall)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	TOG_BIT(buddy->flags, CHAT_FLAG_FORWARDALL);

	if (HAS_BIT(buddy->flags, CHAT_FLAG_FORWARDALL))
	{
		chat_socket_printf(buddy, "%c\n%s is now forwarding session output to you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

		chat_printf("You are now forwarding session output to %s.", buddy->name);
	}
	else
	{
		chat_socket_printf(buddy, "%c\n%s is no longer forwarding session output to you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

		chat_printf("You are no longer forwarding session output to %s.", buddy->name);
	}
}

void chat_forward_session(struct session *ses, char *linelog)
{
	char tmp[BUFFER_SIZE];
	struct chat_data *buddy;

	if (ses != gtd->ses)
	{
		return;
	}

	sprintf(tmp, "%c%s%c", CHAT_SNOOP_DATA, linelog, CHAT_END_OF_COMMAND);

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (HAS_BIT(buddy->flags, CHAT_FLAG_FORWARDALL))
		{
			chat_socket_printf(buddy, "%s", tmp);
		}
	}
}

/*
	Pussy version of the ignore feature
*/

DO_CHAT(chat_ignore)
{
	struct chat_data *buddy;

	if ((buddy = find_buddy(arg)) == NULL)
	{
		chat_printf("You are not connected to anyone named '%s'.", arg);

		return;
	}

	TOG_BIT(buddy->flags, CHAT_FLAG_IGNORE);

	if (HAS_BIT(buddy->flags, CHAT_FLAG_IGNORE))
	{
/*		chat_socket_printf(buddy, "%c\n%s is now ignoring you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND); */

		chat_printf("You are now ignoring %s.", buddy->name);
	}
	else
	{
/*		chat_socket_printf(buddy, "%c\n%s is no longer ignoring you.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND); */

		chat_printf("You are no longer ignoring %s.", buddy->name);
	}
}


DO_CHAT(chat_private)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE], right[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left,  FALSE);
	arg = get_arg_in_braces(arg, temp,  TRUE);

	substitute(gtd->ses, temp, right, SUB_COL|SUB_ESC);

	if (!strcasecmp(left, "ALL"))
	{
		for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
		{
			if (!HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
			{
				chat_socket_printf(buddy, "%c\n%s marked your connection private.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

				chat_printf("Your connection with %s is now private.", buddy->name);				

				SET_BIT(buddy->flags, CHAT_FLAG_PRIVATE);
			}
		}
	}
	else
	{
		if ((buddy = find_buddy(left)) != NULL)
		{
			if (!HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
			{
				chat_socket_printf(buddy, "%c\n%s marked your connection private.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

				chat_printf("Your connection with %s is now private.", buddy->name);

				SET_BIT(buddy->flags, CHAT_FLAG_PRIVATE);
			}
			else
			{
				chat_printf("Your connection with %s is already private.", buddy->name);
			}
		}
	}
}

DO_CHAT(chat_public)
{
	struct chat_data *buddy;
	char temp[BUFFER_SIZE], left[BUFFER_SIZE], right[BUFFER_SIZE];

	arg = get_arg_in_braces(arg, left,  FALSE);
	arg = get_arg_in_braces(arg, temp,  TRUE);

	substitute(gtd->ses, temp, right, SUB_COL|SUB_ESC);

	if (!strcasecmp(left, "ALL"))
	{
		for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
		{
			if (HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
			{
				chat_socket_printf(buddy, "%c\n%s marked your connection public.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

				chat_printf("Your connection with %s is now public.", buddy->name);				

				DEL_BIT(buddy->flags, CHAT_FLAG_PRIVATE);
			}
		}
	}
	else
	{
		if ((buddy = find_buddy(left)) != NULL)
		{
			if (HAS_BIT(buddy->flags, CHAT_FLAG_PRIVATE))
			{
				chat_socket_printf(buddy, "%c\n%s marked your connection public.\n%c", CHAT_MESSAGE, gtd->chat->name, CHAT_END_OF_COMMAND);

				chat_printf("Your connection with %s is now public.", buddy->name);

				DEL_BIT(buddy->flags, CHAT_FLAG_PRIVATE);
			}
			else
			{
				chat_printf("Your connection with %s is already public.", buddy->name);
			}
		}
	}
}

/*
	INTERNAL UTILITY ROUTINES
*/


int get_file_size(char *fpath)
{
	struct stat statbuf;

	if (stat(fpath, &statbuf) == -1)
	{
		return 0;
	}
	return statbuf.st_size;
}


struct chat_data *find_buddy(const char *arg)
{
	struct chat_data *buddy;
	int cnt = 1;

	if (*arg == 0)
	{
		return NULL;
	}

	if (is_number(arg))
	{
		for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
		{
			if (atoi(arg) == cnt++)
			{
				return buddy;
			}
		}
	}

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (!strcmp(arg, buddy->ip))
		{
			return buddy;
		}
	}

	for (buddy = gtd->chat->next ; buddy ; buddy = buddy->next)
	{
		if (is_abbrev(arg, buddy->name))
		{
			return buddy;
		}
	}

	return NULL;
}


char *fix_file_name(char *name)
{
	int len;

	for (len = strlen(name) ; len > 0 ; len--)
	{
		switch (name[len])
		{
			case '/':
			case '\\':
			case ':':
				return &name[len + 1];
		}
	}
	return name;
}