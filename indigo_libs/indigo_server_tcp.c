// Copyright (c) 2016 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO TCP wire protocol server
 \file indigo_server_tcp.c
 */

#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <stdarg.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>

#ifdef INDIGO_LINUX
#include <netinet/tcp.h>
#endif

#if defined(INDIGO_LINUX) || defined(INDIGO_MACOS)
#include <dns_sd.h>
#endif

#include <indigo/indigo_bus.h>
#include <indigo/indigo_client.h>
#include <indigo/indigo_server_tcp.h>
#include <indigo/indigo_driver_xml.h>
#include <indigo/indigo_driver_json.h>
#include <indigo/indigo_client_xml.h>
#include <indigo/indigo_base64.h>
#include <indigo/indigo_io.h>

#define SHA1_SIZE 20
#if _MSC_VER
# define _sha1_restrict __restrict
#else
# define _sha1_restrict __restrict__
#endif

#define INDIGO_PRINTF(...) if (!indigo_printf(__VA_ARGS__)) goto failure

void sha1(unsigned char h[static SHA1_SIZE], const void *_sha1_restrict p, size_t n);

static int server_socket;
static bool startup_initiated = true;
static bool shutdown_initiated = false;
static int client_count = 0;
static indigo_server_tcp_callback server_callback;

int indigo_server_tcp_port = 7624;
bool indigo_use_bonjour = true;
bool indigo_is_ephemeral_port = false;
bool indigo_use_blob_buffering = true;
bool indigo_use_blob_compression = false;

static pthread_mutex_t resource_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct resource {
	const char *path;
	unsigned char *data;
	unsigned length;
	const char *file_name;
	char *content_type;
	bool (*handler)(int client_socket, char *method, char *path, char *params);
	struct resource *next;
} *resources = NULL;

#define BUFFER_SIZE	1024

static void start_worker_thread(int *client_socket) {
	int socket = *client_socket;
	INDIGO_TRACE(indigo_trace("%d <- // Worker thread started", socket));
	server_callback(++client_count);
	int res = 0;
	char c;
	void *free_on_exit = NULL;
	pthread_mutex_t *unlock_at_exit = NULL;
	
	if (recv(socket, &c, 1, MSG_PEEK) == 1) {
		if (c == '<') {
			INDIGO_TRACE(indigo_trace("%d <- // Protocol switched to XML", socket));
			indigo_client *protocol_adapter = indigo_xml_device_adapter(socket, socket);
			assert(protocol_adapter != NULL);
			indigo_attach_client(protocol_adapter);
			indigo_xml_parse(NULL, protocol_adapter);
			indigo_detach_client(protocol_adapter);
			indigo_release_xml_device_adapter(protocol_adapter);
		} else if (c == '{') {
			INDIGO_TRACE(indigo_trace("%d <- // Protocol switched to JSON", socket));
			indigo_client *protocol_adapter = indigo_json_device_adapter(socket, socket, false);
			assert(protocol_adapter != NULL);
			indigo_attach_client(protocol_adapter);
			indigo_json_parse(NULL, protocol_adapter);
			indigo_detach_client(protocol_adapter);
			indigo_release_json_device_adapter(protocol_adapter);
		} else if (c == 'G' || c == 'P') {
			char request[BUFFER_SIZE];
			char header[BUFFER_SIZE];
			while ((res = indigo_read_line(socket, request, BUFFER_SIZE)) >= 0) {
				bool keep_alive = true;
				if (!strncmp(request, "GET /", 5)) {
					char *path = request + 4;
					char *space = strchr(path, ' ');
					if (space)
						*space = 0;
					char *params = strchr(path, '?');
					if (params)
						*params++ = 0;
					char websocket_key[256] = "";
					bool use_gzip = false;
					bool use_imagebytes = false;
					while (indigo_read_line(socket, header, BUFFER_SIZE) > 0) {
						if (!strncasecmp(header, "Sec-WebSocket-Key: ", 19))
							strncpy(websocket_key, header + 19, sizeof(websocket_key));
						if (!strcasecmp(header, "Connection: close"))
							keep_alive = false;
						if (!strncasecmp(header, "Accept-Encoding:", 16)) {
							if (strstr(header + 16, "gzip"))
								use_gzip = true;
						}
						if (!strncasecmp(header, "Accept:", 7)) {
							if (strstr(header + 7, "application/imagebytes"))
								use_imagebytes = true;
						}
					}
					if (!strcmp(path, "/")) {
						if (*websocket_key) {
							unsigned char shaHash[SHA1_SIZE];
							memset(shaHash, 0, sizeof(shaHash));
							strcat(websocket_key, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
							sha1(shaHash, websocket_key, strlen(websocket_key));
							INDIGO_PRINTF(socket, "HTTP/1.1 101 Switching Protocols\r\n");
							INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
							INDIGO_PRINTF(socket, "Upgrade: websocket\r\n");
							INDIGO_PRINTF(socket, "Connection: upgrade\r\n");
							base64_encode((unsigned char *)websocket_key, shaHash, 20);
							INDIGO_PRINTF(socket, "Sec-WebSocket-Accept: %s\r\n", websocket_key);
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_TRACE(indigo_trace("%d <- // Protocol switched to JSON-over-WebSockets", socket));
							indigo_client *protocol_adapter = indigo_json_device_adapter(socket, socket, true);
							assert(protocol_adapter != NULL);
							indigo_attach_client(protocol_adapter);
							indigo_json_parse(NULL, protocol_adapter);
							indigo_detach_client(protocol_adapter);
							indigo_release_json_device_adapter(protocol_adapter);
						} else {
							INDIGO_PRINTF(socket, "HTTP/1.1 301 OK\r\n");
							INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
							INDIGO_PRINTF(socket, "Location: /mng.html\r\n");
							INDIGO_PRINTF(socket, "Content-type: text/html\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_PRINTF(socket, "<a href='/mng.html'>INDIGO Server Manager</a>");
						}
						keep_alive = false;
					} else if (!strncmp(path, "/blob/", 6)) {
						indigo_item *item;
						indigo_blob_entry *entry;
						if (sscanf(path, "/blob/%p.", &item) && (entry = indigo_validate_blob(item))) {
							pthread_mutex_lock(unlock_at_exit = &entry->mutext);
							long working_size = entry->size;
							if (working_size == 0) {
								assert(entry->content == NULL);
								indigo_item item_copy = *item;
								item_copy.blob.size = 0;
								item_copy.blob.value = NULL;
								if (indigo_populate_http_blob_item(&item_copy)) {
									working_size = entry->size = item_copy.blob.size;
									entry->content = item_copy.blob.value;
								} else {
									indigo_error("%d <- // Failed to populate BLOB", socket);
								}
							}
							void *working_copy = indigo_use_blob_buffering ? (free_on_exit = malloc(working_size)) : entry->content;
							if (working_copy) {
								char working_format[INDIGO_NAME_SIZE];
								strcpy(working_format, entry->format);
								INDIGO_PRINTF(socket, "HTTP/1.1 200 OK\r\n");
								if (indigo_use_blob_buffering) {
									if (use_gzip && indigo_use_blob_compression && strcmp(entry->format, ".jpeg")) {
										unsigned compressed_size = (unsigned)working_size;
										indigo_compress("image", entry->content, (unsigned)working_size, working_copy, &compressed_size);
										INDIGO_PRINTF(socket, "Content-Encoding: gzip\r\n");
										INDIGO_PRINTF(socket, "X-Uncompressed-Content-Length: %ld\r\n", working_size);
										working_size = compressed_size;
									} else {
										memcpy(working_copy, entry->content, working_size);
									}
									pthread_mutex_unlock(&entry->mutext);
									unlock_at_exit = NULL;
								}
								INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
								if (!strcmp(entry->format, ".jpeg")) {
									INDIGO_PRINTF(socket, "Content-Type: image/jpeg\r\n");
								} else {
									INDIGO_PRINTF(socket, "Content-Type: application/octet-stream\r\n");
									INDIGO_PRINTF(socket, "Content-Disposition: attachment; filename=\"%p%s\"\r\n", item, working_format);
								}
								if (keep_alive)
									INDIGO_PRINTF(socket, "Connection: keep-alive\r\n");
								INDIGO_PRINTF(socket, "Content-Length: %ld\r\n", working_size);
								INDIGO_PRINTF(socket, "\r\n");
								if (indigo_write(socket, working_copy, working_size)) {
									INDIGO_TRACE(indigo_trace("%d <- // %ld bytes", socket, working_size));
								} else {
									indigo_error("%d <- // %s", socket, strerror(errno));
									goto failure;
								}
								if (indigo_use_blob_buffering) {
									free(working_copy);
									free_on_exit = NULL;
								} else {
									pthread_mutex_unlock(&entry->mutext);
									unlock_at_exit = NULL;
								}
							} else {
								pthread_mutex_unlock(&entry->mutext);
								unlock_at_exit = NULL;
								INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
								INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
								INDIGO_PRINTF(socket, "\r\n");
								INDIGO_PRINTF(socket, "Out of buffer memory!\r\n");
								INDIGO_TRACE(indigo_trace("%d <- // Out of buffer memory", socket));
								goto failure;
							}
						} else {
							INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
							INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_PRINTF(socket, "BLOB not found!\r\n");
							INDIGO_TRACE(indigo_trace("%d <- // BLOB not found", socket));
							goto failure;
						}
					} else {
						pthread_mutex_lock(&resource_list_mutex);
						struct resource *resource = resources;
						while (resource) {
							if (!strncmp(resource->path, path, strlen(resource->path)))
								break;
							resource = resource->next;
						}
						pthread_mutex_unlock(&resource_list_mutex);
						if (resource == NULL) {
							INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
							INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_PRINTF(socket, "%s not found!\r\n", path);
							INDIGO_TRACE(indigo_trace("%d <- // %s not found", socket, path));
							goto failure;
						} else if (resource->handler) {
							keep_alive = resource->handler(socket, use_imagebytes ? "GET/IMAGEBYTES" : (use_gzip ? "GET/GZIP" : "GET"), path, params);
						} else if (resource->data) {
							INDIGO_PRINTF(socket, "HTTP/1.1 200 OK\r\n");
							INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
							INDIGO_PRINTF(socket, "Content-Type: %s\r\n", resource->content_type);
							INDIGO_PRINTF(socket, "Content-Length: %d\r\n", resource->length);
							INDIGO_PRINTF(socket, "Content-Encoding: gzip\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							indigo_write(socket, (const char *)resource->data, resource->length);
							INDIGO_TRACE(indigo_trace("%d <- // %d bytes", socket, resource->length));
						} else if (resource->file_name) {
							char file_name[256];
							struct stat file_stat;
							int handle;
							if (*resource->file_name == '/') {
								strcpy(file_name, resource->file_name);
							} else {
								sprintf(file_name, "%s/%s", getenv("HOME"), resource->file_name);
							}
							if (stat(file_name, &file_stat) < 0 || (handle = open(file_name, O_RDONLY)) < 0) {
								INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
								INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
								INDIGO_PRINTF(socket, "\r\n");
								INDIGO_PRINTF(socket, "%s not found (%s)\r\n", file_name, strerror(errno));
								INDIGO_TRACE(indigo_trace("%d <- // Failed to stat/open file (%s, %s)", socket, file_name, strerror(errno)));
								goto failure;
							} else {
								const char *base_name = strrchr(file_name, '/');
								base_name = base_name ? base_name + 1 : file_name;
								INDIGO_PRINTF(socket, "HTTP/1.1 200 OK\r\n");
								INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
								INDIGO_PRINTF(socket, "Content-Type: %s\r\n", resource->content_type);
								INDIGO_PRINTF(socket, "Content-Disposition: attachment; filename=%s\r\n", base_name);
								INDIGO_PRINTF(socket, "Content-Length: %d\r\n", file_stat.st_size);
								INDIGO_PRINTF(socket, "\r\n");
								long remaining = file_stat.st_size;
								char buffer[128 * 1024];
								while (remaining > 0) {
									long count = read(handle, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
									if (count < 0) {
										INDIGO_TRACE(indigo_trace("%d -> // %s", socket, strerror(errno)));
										break;
									}
									if (indigo_write(socket, buffer, count)) {
										INDIGO_TRACE(indigo_trace("%d <- // %ld bytes", socket, count));
									} else {
										INDIGO_TRACE(indigo_trace("%d <- // %s", socket, strerror(errno)));
										goto failure;
									}
									remaining -= count;
								}
								close(handle);
							}
						}
					}
				} else if (!strncmp(request, "PUT /", 5)) {
					char *path = request + 4;
					char *space = strchr(path, ' ');
					if (space)
						*space = 0;
					if (!strncmp(path, "/blob/", 6)) {
						indigo_item *item;
						indigo_blob_entry *entry;
						if (sscanf(path, "/blob/%p.", &item) && (entry = indigo_validate_blob(item))) {
							int content_length = 0;
							char header[BUFFER_SIZE];
							while (indigo_read_line(socket, header, INDIGO_BUFFER_SIZE) > 0) {
								if (!strncasecmp(header, "Content-Length:", 15)) {
									content_length = atoi(header + 15);
								}
							}
							pthread_mutex_lock(unlock_at_exit = &entry->mutext);
							entry->content = indigo_safe_realloc(entry->content, entry->size = content_length);
							if (entry->content) {
								if (!indigo_read(socket, entry->content, content_length))
									goto failure;
								pthread_mutex_unlock(&entry->mutext);
								unlock_at_exit = NULL;
								INDIGO_PRINTF(socket, "HTTP/1.1 200 OK\r\n");
								INDIGO_PRINTF(socket, "Server: INDIGO/%d.%d-%s\r\n", (INDIGO_VERSION_CURRENT >> 8) & 0xFF, INDIGO_VERSION_CURRENT & 0xFF, INDIGO_BUILD);
								INDIGO_PRINTF(socket, "Content-Length: 0\r\n");
								INDIGO_PRINTF(socket, "\r\n");
							} else {
								INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
								INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
								INDIGO_PRINTF(socket, "\r\n");
								INDIGO_PRINTF(socket, "Out of buffer memory!\r\n");
								INDIGO_TRACE(indigo_trace("%d <- // Out of buffer memory", socket));
								goto failure;
							}
						} else {
							INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
							INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_PRINTF(socket, "BLOB not found!\r\n");
							INDIGO_TRACE(indigo_trace("%d <- // BLOB not found", socket));
							goto failure;
						}
					} else {
						pthread_mutex_lock(&resource_list_mutex);
						struct resource *resource = resources;
						while (resource) {
							if (!strncmp(resource->path, path, strlen(resource->path)))
								break;
							resource = resource->next;
						}
						pthread_mutex_unlock(&resource_list_mutex);
						if (resource == NULL) {
							INDIGO_PRINTF(socket, "HTTP/1.1 404 Not found\r\n");
							INDIGO_PRINTF(socket, "Content-Type: text/plain\r\n");
							INDIGO_PRINTF(socket, "\r\n");
							INDIGO_PRINTF(socket, "%s not found!\r\n", path);
							INDIGO_TRACE(indigo_trace("%d <- // %s not found", socket, path));
							goto failure;
						} else if (resource->handler) {
							keep_alive = resource->handler(socket, "PUT", path, NULL);
						}
					}
				}
				if (!keep_alive) {
					break;
				}
			}
		} else {
			INDIGO_TRACE(indigo_trace("%d -> // Unrecognised protocol", socket));
		}
	}
failure:
	shutdown(socket, SHUT_RDWR);
	indigo_usleep(ONE_SECOND_DELAY); // ???
	close(socket);
	server_callback(--client_count);
	free(client_socket);
	if (free_on_exit) {
		free(free_on_exit);
	}
	if (unlock_at_exit) {
		pthread_mutex_unlock(unlock_at_exit);
	}
	INDIGO_TRACE(indigo_trace("%d <- // Worker thread finished", socket));
}

void indigo_server_shutdown() {
	if (!shutdown_initiated) {
		shutdown_initiated = true;
		shutdown(server_socket, SHUT_RDWR);
		close(server_socket);
	}
}

void indigo_server_add_resource(const char *path, unsigned char *data, unsigned length, const char *content_type) {
	pthread_mutex_lock(&resource_list_mutex);
	struct resource *resource = indigo_safe_malloc(sizeof(struct resource));
	resource->path = path;
	resource->data = data;
	resource->length = length;
	resource->content_type = (char *)content_type;
	resource->handler = NULL;
	resource->next = resources;
	resources = resource;
	pthread_mutex_unlock(&resource_list_mutex);
	INDIGO_TRACE(indigo_trace("Resource %s (%d, %s) added", path, length, content_type));
}

void indigo_server_add_file_resource(const char *path, const char *file_name, const char *content_type) {
	pthread_mutex_lock(&resource_list_mutex);
	struct resource *resource = indigo_safe_malloc(sizeof(struct resource));
	resource->path = path;
	resource->file_name = file_name;
	resource->content_type = (char *)content_type;
	resource->handler = NULL;
	resource->next = resources;
	resources = resource;
	pthread_mutex_unlock(&resource_list_mutex);
	INDIGO_TRACE(indigo_trace("Resource %s (%s, %s) added", path, file_name, content_type));
}

void indigo_server_add_handler(const char *path, bool (*handler)(int client_socket, char *method, char *path, char *params)) {
	pthread_mutex_lock(&resource_list_mutex);
	struct resource *resource = indigo_safe_malloc(sizeof(struct resource));
	resource->path = path;
	resource->file_name = NULL;
	resource->content_type = NULL;
	resource->handler = handler;
	resource->next = resources;
	resources = resource;
	pthread_mutex_unlock(&resource_list_mutex);
	INDIGO_TRACE(indigo_trace("Resource %s handler added", path));
}

void indigo_server_remove_resource(const char *path) {
	pthread_mutex_lock(&resource_list_mutex);
	struct resource *resource = resources;
	struct resource *prev = NULL;
	while (resource) {
		if (!strcmp(resource->path, path)) {
			if (prev == NULL)
				resources = resource->next;
			else
				prev->next = resource->next;
			free(resource);
			pthread_mutex_unlock(&resource_list_mutex);
			INDIGO_TRACE(indigo_trace("Resource %s removed", path));
			return;
		}
		prev = resource;
		resource = resource->next;
	}
	pthread_mutex_unlock(&resource_list_mutex);
}

void indigo_server_remove_resources(void) {
	pthread_mutex_lock(&resource_list_mutex);
	struct resource *resource = resources;
	while (resource) {
		struct resource *tmp = resource;
		resource = resource->next;
		INDIGO_TRACE(indigo_trace("Resource %s removed", tmp->path));
		free(tmp);
	}
	resources = NULL;
	pthread_mutex_unlock(&resource_list_mutex);
}

static void default_server_callback(int count) {
	static DNSServiceRef sd_http;
	static DNSServiceRef sd_indigo;
	if (startup_initiated) {
		if (indigo_use_bonjour) {
			/* UGLY but the only way to suppress compat mode warning messages on Linux */
			setenv("AVAHI_COMPAT_NOWARN", "1", 1);
			DNSServiceRegister(&sd_http, 0, 0, indigo_local_service_name, MDNS_HTTP_TYPE, NULL, NULL, htons(indigo_server_tcp_port), 0, NULL, NULL, NULL);
			DNSServiceRegister(&sd_indigo, 0, 0, indigo_local_service_name, MDNS_INDIGO_TYPE, NULL, NULL, htons(indigo_server_tcp_port), 0, NULL, NULL, NULL);
			INDIGO_LOG(indigo_log("Service registered as %s", indigo_local_service_name));
		}
	} else if (shutdown_initiated) {
		if (indigo_use_bonjour) {
			DNSServiceRefDeallocate(sd_indigo);
			DNSServiceRefDeallocate(sd_http);
		}
		INDIGO_LOG(indigo_log("Service unregistered"));
	} else {
		INDIGO_TRACE(indigo_trace("%d clients", count));
	}
}


indigo_result indigo_server_start(indigo_server_tcp_callback callback) {
	indigo_use_blob_caching = true;
	startup_initiated = true;
	shutdown_initiated = false;
	server_callback = callback ? callback : default_server_callback;
	int client_socket;
	server_socket = socket(PF_INET, SOCK_STREAM, 0);
	if (server_socket == -1) {
		indigo_error("Can't open server socket (%s)", strerror(errno));
		return INDIGO_CANT_START_SERVER;
	}
	int reuse = 1;
	if (setsockopt(server_socket, SOL_SOCKET,SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		indigo_error("Can't setsockopt for server socket (%s)", strerror(errno));
		return INDIGO_CANT_START_SERVER;
	}
	struct sockaddr_in client_name;
	unsigned int name_len = sizeof(client_name);
	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(indigo_server_tcp_port);
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) < 0) {
		indigo_error("Can't bind server socket (%s)", strerror(errno));
		return INDIGO_CANT_START_SERVER;
	}
	
#ifdef INDIGO_LINUX
	int val = 1;
	if (setsockopt(server_socket, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0) {
		indigo_error("Can't setsockopt TCP_NODELAY, for server socket (%s)", strerror(errno));
		return INDIGO_CANT_START_SERVER;
	}
#endif
	
	unsigned int length = sizeof(server_address);
	if (getsockname(server_socket, (struct sockaddr *)&server_address, &length) == -1) {
		close(server_socket);
		return INDIGO_CANT_START_SERVER;
	}
	if (listen(server_socket, 64) < 0) {
		indigo_error("Can't listen on server socket (%s)", strerror(errno));
		close(server_socket);
		return INDIGO_CANT_START_SERVER;
	}
	indigo_is_ephemeral_port = indigo_server_tcp_port == 0;
	indigo_server_tcp_port = ntohs(server_address.sin_port);
	INDIGO_LOG(indigo_log("Server started on port %d", indigo_server_tcp_port));
	server_callback(0);
	startup_initiated = false;
	signal(SIGPIPE, SIG_IGN);
	while (1) {
		client_socket = accept(server_socket, (struct sockaddr *)&client_name, &name_len);
		if (client_socket == -1) {
			if (shutdown_initiated) {
				break;
			}
			indigo_error("Can't accept connection (%s)", strerror(errno));
		} else {
			struct timeval timeout;
			timeout.tv_sec = 0;
			timeout.tv_usec = 0;
			if (setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
				indigo_error("Can't set recv() timeout (%s)", strerror(errno));
			timeout.tv_sec = 5;
			if (setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
				indigo_error("Can't set send() timeout (%s)", strerror(errno));
			int *pointer = indigo_safe_malloc(sizeof(int));
			*pointer = client_socket;
			if (!indigo_async((void *(*)(void *))&start_worker_thread, pointer))
				indigo_error("Can't create worker thread for connection (%s)", strerror(errno));
		}
	}
	shutdown_initiated = false;
	server_callback(0);
	return INDIGO_OK;
}

/*
 Copyright (c) 2014 Malte Hildingsson, malte (at) afterwi.se

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */

static inline void sha1mix(unsigned *_sha1_restrict r, unsigned *_sha1_restrict w) {
	unsigned a = r[0];
	unsigned b = r[1];
	unsigned c = r[2];
	unsigned d = r[3];
	unsigned e = r[4];
	unsigned t, i = 0;

#define rol(x,s) ((x) << (s) | (unsigned) (x) >> (32 - (s)))
#define mix(f,v) do { \
t = (f) + (v) + rol(a, 5) + e + w[i & 0xf]; \
e = d; \
d = c; \
c = rol(b, 30); \
b = a; \
a = t; \
} while (0)

	for (; i < 16; ++i)
  mix(d ^ (b & (c ^ d)), 0x5a827999);

	for (; i < 20; ++i) {
		w[i & 0xf] = rol(w[i + 13 & 0xf] ^ w[i + 8 & 0xf] ^ w[i + 2 & 0xf] ^ w[i & 0xf], 1);
		mix(d ^ (b & (c ^ d)), 0x5a827999);
	}

	for (; i < 40; ++i) {
		w[i & 0xf] = rol(w[i + 13 & 0xf] ^ w[i + 8 & 0xf] ^ w[i + 2 & 0xf] ^ w[i & 0xf], 1);
		mix(b ^ c ^ d, 0x6ed9eba1);
	}

	for (; i < 60; ++i) {
		w[i & 0xf] = rol(w[i + 13 & 0xf] ^ w[i + 8 & 0xf] ^ w[i + 2 & 0xf] ^ w[i & 0xf], 1);
		mix((b & c) | (d & (b | c)), 0x8f1bbcdc);
	}

	for (; i < 80; ++i) {
		w[i & 0xf] = rol(w[i + 13 & 0xf] ^ w[i + 8 & 0xf] ^ w[i + 2 & 0xf] ^ w[i & 0xf], 1);
		mix(b ^ c ^ d, 0xca62c1d6);
	}

#undef mix
#undef rol

	r[0] += a;
	r[1] += b;
	r[2] += c;
	r[3] += d;
	r[4] += e;
}

void sha1(unsigned char h[static SHA1_SIZE], const void *_sha1_restrict p, size_t n) {
	size_t i = 0;
	unsigned w[16], r[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};

	for (; i < (n & ~0x3f);) {
		do w[i >> 2 & 0xf] =
			((const unsigned char *) p)[i + 3] << 0x00 |
			((const unsigned char *) p)[i + 2] << 0x08 |
			((const unsigned char *) p)[i + 1] << 0x10 |
			((const unsigned char *) p)[i + 0] << 0x18;
		while ((i += 4) & 0x3f);
		sha1mix(r, w);
	}

	memset(w, 0, sizeof w);

	for (; i < n; ++i)
  w[i >> 2 & 0xf] |= ((const unsigned char *) p)[i] << ((3 ^ (i & 3)) << 3);

	w[i >> 2 & 0xf] |= 0x80 << ((3 ^ (i & 3)) << 3);

	if ((n & 0x3f) > 56) {
		sha1mix(r, w);
		memset(w, 0, sizeof w);
	}

	w[15] = (unsigned) n << 3;
	sha1mix(r, w);

	for (i = 0; i < 5; ++i) {
		h[(i << 2) + 0] = (unsigned char) (r[i] >> 0x18);
		h[(i << 2) + 1] = (unsigned char) (r[i] >> 0x10);
		h[(i << 2) + 2] = (unsigned char) (r[i] >> 0x08);
		h[(i << 2) + 3] = (unsigned char) (r[i] >> 0x00);
	}
}
