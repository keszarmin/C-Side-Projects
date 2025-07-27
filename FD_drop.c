#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <dirent.h>
#include <termios.h>
#include <stdint.h>
#define PORT 9160


typedef enum {
    DESERIALIZE_OK = 0,
    DESERIALIZE_INVALID_INPUT = -1,
    DESERIALIZE_INVALID_SIZE = -2,
    DESERIALIZE_MEMORY_ERROR = -3
} DESERIALIZE_TYPE;

typedef enum {
    OK = 0,
    ERROR = -1,
    SEND_ERROR = -2,
    GET_ERROR = -3,
    READ_CONTINUE = 202,
    READ_END = 201,
    CONTINUE = 200,
    BREAK = 199,
} STATUS_CODE_TYPE;

char get_ch(void) {
    struct termios oldt,newt;
    char ch;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    read(STDIN_FILENO, &ch, 1);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return ch;
}

char *IP_ADDRES_REQ() {

    int sockfd;
    struct ifreq *ifr;
    struct ifconf ifc;
    int interfaces;
    char buf[1024];
    char *ip = (char *)malloc(INET_ADDRSTRLEN);

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(1);
    }

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sockfd, SIOCGIFCONF, &ifc) < 0) {
        perror("ioctl(SIOCGIFCONF)");
        close(sockfd);
        exit(0);
    }

    ifr = ifc.ifc_req;
    interfaces = ifc.ifc_len / sizeof(struct ifreq);

    for (int i = 0; i < interfaces; i++) {
        struct ifreq *item = &ifr[i];

        if (ioctl(sockfd, SIOCGIFADDR, item) == 0) {
            struct sockaddr_in *ipaddr = (struct sockaddr_in *)&item->ifr_addr;

            inet_ntop(AF_INET, &ipaddr->sin_addr, ip, INET_ADDRSTRLEN);
        }
    }

    close(sockfd);

    return ip;
    free(ip);
}

typedef struct 
{
    char FILE_NAME[64];
    uint32_t FILE_SIZE;
    uint32_t BUFFER_SIZE;
} FILE_INFO;

void serialize(unsigned char *out, const FILE_INFO *file_info_ptr) {
    memset(out, 0, 64);
    memcpy(out, file_info_ptr->FILE_NAME, 64);

    uint32_t net_fs = htonl(file_info_ptr->FILE_SIZE);
    memcpy(out + 64, &net_fs, 4); 
    
    uint32_t net_bs = htonl(file_info_ptr->BUFFER_SIZE);
    memcpy(out + 68,&net_bs, 4);
};

void deserialize(unsigned char *in, FILE_INFO *file_info_ptr) {
    memcpy(file_info_ptr->FILE_NAME, in, 64);
    file_info_ptr->FILE_NAME[63] = '\0'; 

    uint32_t temp1,temp2;
    memcpy(&temp1,in + 64,sizeof(uint32_t));
    memcpy(&temp2,in + 68,sizeof(uint32_t));
    
    file_info_ptr->FILE_SIZE = ntohl(temp1);
    file_info_ptr->BUFFER_SIZE = ntohl(temp2);
};

STATUS_CODE_TYPE SEND_STATUS(int sock,int code) {
    int status = htonl(code);
    int bytes = send(sock,&status,sizeof(int),0);
    if (bytes != sizeof(status)) return SEND_ERROR;
    
    return OK;
}

STATUS_CODE_TYPE GET_STATUS(int sock) {
    int code;
    
    int bytes = recv(sock,&code,sizeof(int),0);
    if (bytes != sizeof(code)) return GET_ERROR;

    return code;
}

STATUS_CODE_TYPE SEND_FILE(int sock,char *file_path) {
    FILE_INFO file_i;
    FILE *fptr;

    const char *file_name = strrchr(file_path,'/');
    
    if (file_name != NULL) file_name++;
    else file_name = file_path;

    strncpy(file_i.FILE_NAME,file_name,64);
    file_i.BUFFER_SIZE = 4096;

    fptr = fopen(file_path,"rb");
    if (fptr == NULL) {
        perror("╠ ❌ FILE ERROR");
        exit(1);
    } 
    
    fseek(fptr, 0, SEEK_END);
    long fsize = ftell(fptr);
    rewind(fptr);

    if (fsize >= 0 && fsize <= UINT32_MAX) {
        file_i.FILE_SIZE = (uint32_t)fsize;
    } else {
        perror("╠ ❌ The File size is more than 4GB\n");
        return ERROR;
    }
    unsigned char buffer[fsize + 1]; 

    size_t read = fread(buffer,1,fsize,fptr);
    buffer[read] = '\0';
    fclose(fptr); 

    unsigned char info_buffer[72];
    serialize(info_buffer,&file_i);

    if (send(sock,info_buffer,72,0) < 0) {
        return SEND_ERROR;
    }

    uint32_t sended = 0;
    while ((uint32_t)fsize > sended) {
        size_t remaning = (uint32_t)fsize - sended;
        size_t chunk = remaning < 4096 ? remaning : 4096;

        ssize_t bytes = send(sock,buffer + sended,chunk,0);
        if (bytes <= 0) {
            return SEND_ERROR;
        }
        sended += bytes;
    }

    if (GET_STATUS(sock) != OK) {
        return GET_ERROR;
    }
    printf("╠ ✅ %s SENT\n",file_path);
    return OK;
};

STATUS_CODE_TYPE GET_FILE(int sock,char *dir_path,int usage) {
    FILE_INFO file_i;

    unsigned char info_buffer[72];

    if (recv(sock,info_buffer,72,0) < 0) return GET_ERROR;
    

    deserialize(info_buffer,&file_i);

    char buffer[file_i.FILE_SIZE + 1]; 

    uint32_t received = 0;
    while (received < file_i.FILE_SIZE)
    {
        ssize_t bytes = recv(sock,buffer + received,file_i.FILE_SIZE - received,0);
        if (bytes <= 0) return GET_ERROR;
        received += bytes;
    }

    FILE *fptr;

    if (usage == 1) {
        char full_dir_path[128];
        snprintf(full_dir_path,sizeof(full_dir_path),"./%s/%s",dir_path,file_i.FILE_NAME);
        
        printf("%s\n",full_dir_path);
        
        fptr = fopen(full_dir_path,"wb");
    } else {
        fptr = fopen(file_i.FILE_NAME,"wb");
    }

    size_t written_bytes = fwrite(buffer,file_i.FILE_SIZE,1,fptr);
    if (written_bytes != 1) {
        if (SEND_STATUS(sock,ERROR) != OK) return SEND_ERROR;
    } else {
        if (SEND_STATUS(sock,OK) != OK) return SEND_ERROR;
    }

    fclose(fptr);
    return OK;
};

int main(void) {
    
    printf("╬ (SERVER/CLIENT): \n");
    char ch = get_ch();
    
    if (ch == 'c') {
        int sock;
        struct sockaddr_in addr;
        
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            printf("╠ ❌ Socket error\n");
            exit(1);
        } 
        printf("╠ ✅ TCP client socket created\n");

        char ip[16];
        printf("╠ IP address\n╠ (Type y if u want see your ip address)\n╬ :");
        scanf("%s",ip);
        if (strncmp(ip,"y",1) == 0) {
            printf("╠ %s\n",IP_ADDRES_REQ());
            exit(1);
        }

        memset(&addr, '\0', sizeof(addr));
        addr.sin_addr.s_addr = inet_addr(ip);
        addr.sin_family = AF_INET;
        addr.sin_port = PORT;

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            printf("╠ ❌ Connection error\n");
            exit(1);
        }
        printf("╠ Connected to the server✨✨✨\n");
        
        while (1) {
            puts("╬ (FILE/DIRECTORY): ");
                char c = get_ch();
                if (c == 'f') {
    
                    if (send(sock, "f",2,0) < 0) {
                        printf("╠ ❌ Error in sending request.\n");
                        exit(1);
                    } 
    
                    puts("╬ Enter file name: ");
                    char filename[64];
                    scanf("%s", filename);
                    if (SEND_FILE(sock, filename) != OK) {
                        printf("╠ ❌ Error in sending file.\n");
                        
                    }
                } else if (c == 'd') {
                    
                    if (send(sock, "d",2,0) < 0) {
                        printf("╠ ❌ Error in sending request.\n");
                        exit(1);
                    } 

                    puts("╬ Enter directory name:");
                    char dir_name[16];
                    scanf("%s",dir_name);
                    
                    puts("╬ Enter direvtory path:");
                    char dir_path[64];
                    scanf("%s",dir_path);
    
                    DIR *dr = opendir(dir_path);
                
                    if (dr == NULL) perror("╠ ⫸ Directory reading erorr.");
                    
                    if (send(sock,dir_name,16,0) < 0) {
                        perror("╠ ⫸ Error in the dir configuration.");
                        exit(1);
                    }

                    struct dirent *dir;
                    char file_path[64];
                    while ((dir = readdir(dr)) != 0)
                    {
                        if (dir->d_type == DT_REG) {
                            snprintf(file_path,sizeof(file_path),"%s/%s",dir_path,dir->d_name);
                            if (SEND_FILE(sock,file_path) < 0) {
                                perror("╠ ⫸ Error in sending the file.");
                            }
                            bzero(file_path,64);
                            SEND_STATUS(sock,CONTINUE);
                        }
                    }
                    if (dr != NULL) closedir(dr);
                    if (SEND_STATUS(sock,BREAK) != ERROR) printf("╠ ✅ Directory successfully sent sent!\n");

                } else if (c == 'q') {
                    if (send(sock,"q",2,0) < 0) {
                        printf("╠ ❌ Error in shoutdowning the server.\n");
                        break;
                    }
                    printf("╩\n");
                    exit(0);    
                } else {
                    printf("╠ ❌ Invalid option.\n");
                }
        }

    } else if (ch == 's') {
        char *ip = IP_ADDRES_REQ();
        int server_sock, client_sock;
        struct sockaddr_in server_addr, client_addr;
        socklen_t addr_size;


        server_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (server_sock < 0) {
            perror("╠ ❌ Socket error\n");
            exit(1);
        } 
        printf("╠ ✅ TCP server socket created\n");

        memset(&server_addr, '\0', sizeof(server_addr));
        server_addr.sin_addr.s_addr = inet_addr(ip);
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = PORT;

        free(ip);

        int n = bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
        if (n < 0) {
            perror("╠ ❌ Bind error\n");
            exit(1);
        }
        printf("╠ ✅ Bind to port:%d\n",PORT);

        listen(server_sock, 5);
        printf("╠ Listening✨✨✨\n");
        
        addr_size = sizeof(client_addr);
        client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_size);
        
        if (client_sock < 0) {
            perror("╠ ❌ Socket error\n");
        } else printf("╠ ✅ Client connected!\n");

        while (1) {

            char *run = malloc(sizeof(char));
            if (recv(client_sock, run, sizeof(run), 0) < 0) {
                perror("╠ ❌ Error in receiving");
                break;
            };
    
            if (strncmp(run, "f", 1) == 0) {        
                if (GET_FILE(client_sock,NULL,0) != OK) {
                    perror("╠ ❌ Error in reading file");
                    exit(1);
                }
                printf("╠ ✅ File ready!\n");
            } else if (strncmp(run,"d",1) == 0) {

                char dir_name[16];

                if (recv(client_sock,dir_name,16,0) < 0) printf("╠ ❌ Error in receiving\n");

                int dir_crt = mkdir(dir_name,777);

                if (dir_crt == 0) {
                    printf("╠ ✅ Directory ready!\n");
                } else {
                    printf("╠ ❌ Error in creating the directory\n.");
                    exit(1);
                }

                STATUS_CODE_TYPE end;
                while (end != BREAK) {
                    if (GET_FILE(client_sock,dir_name,1) < 0) {
                        perror("╠ ❌ Error in reading file.");
                    }
                    end = GET_STATUS(client_sock);
                }

                printf("╠ ✅ Directory ready!\n");

            } else if (strncmp(run,"q",1) == 0) {
                free(run);
                close(client_sock);
                close(server_sock);
                printf("╩\n");
                exit(0);        
            } else puts("╠ ❌ Invalid option.");
        }

    } else if (ch == 'q') {
        printf("╩\n");
        exit(0);
    } else {
        printf("╠ ❌ Invalid option.\n");
        exit(1);
    }

    return 0;
}

// gcc-15 ./FD_drop.c -o ./FD_drop -Wall 
