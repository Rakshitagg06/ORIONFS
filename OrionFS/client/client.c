#include "../common/protocol.h"
#include "../common/utils.h"
#include "../common/network.h"
#include <ctype.h>

// Forward declaration for helper used in WRITE parsing
static bool is_int_str(const char* s);

char username[MAX_USERNAME_LEN];
int nm_socket = -1;

void execute_view(Command* cmd) {
    Message msg = {0};
    msg.type = MSG_VIEW_FILES;
    strcpy(msg.sender, username);
    msg.arg1 = cmd->flag_a ? 1 : 0;
    msg.arg2 = cmd->flag_l ? 1 : 0;
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s", response.data);
    }
}

void execute_create(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: CREATE <filename>\n");
        return;
    }
    
    Message msg = {0};
    msg.type = MSG_CREATE_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s\n", response.data);
    }
}

void execute_read(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: READ <filename>\n");
        return;
    }
    
    // Get SS info from NM
    Message msg = {0};
    msg.type = MSG_READ_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        // Print server-provided message as-is to match spec wording
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
        return;
    }
    
    // Connect to SS
    char* ss_ip = response.data;
    int ss_port = response.arg1;
    
    int ss_socket = connect_to_server(ss_ip, ss_port);
    if (ss_socket < 0) {
        printf("ERROR: Failed to connect to storage server\n");
        return;
    }
    
    // Request file
    Message read_msg = {0};
    read_msg.type = MSG_READ_REQUEST;
    strcpy(read_msg.sender, username);
    strcpy(read_msg.filename, cmd->args[0]);
    
    send_message(ss_socket, &read_msg);
    
    Message file_response;
    recv_message(ss_socket, &file_response);
    
    if (file_response.type == MSG_ERROR) {
        printf("%s\n", (strlen(file_response.data) ? file_response.data : error_to_string(file_response.error_code)));
    } else {
        printf("%s\n", file_response.data);
    }
    
    close_socket(ss_socket);
}

void execute_write(Command* cmd) {
    if (cmd->arg_count < 2) {
        printf("Usage: WRITE <filename> <sentence_number>\n");
        return;
    }
    
    char* filename = cmd->args[0];
    int sentence_idx = atoi(cmd->args[1]);
    
    // Get SS info from NM
    Message msg = {0};
    msg.type = MSG_WRITE_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, filename);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
        return;
    }
    
    // Connect to SS
    char* ss_ip = response.data;
    int ss_port = response.arg1;
    
    int ss_socket = connect_to_server(ss_ip, ss_port);
    if (ss_socket < 0) {
        printf("ERROR: Failed to connect to storage server\n");
        return;
    }
    
    // Request write
    Message write_msg = {0};
    write_msg.type = MSG_WRITE_REQUEST;
    strcpy(write_msg.sender, username);
    strcpy(write_msg.filename, filename);
    write_msg.arg1 = sentence_idx;
    
    send_message(ss_socket, &write_msg);
    
    // Wait for READY
    Message ready_response;
    recv_message(ss_socket, &ready_response);
    
    if (ready_response.type == MSG_ERROR) {
        printf("%s\n", (strlen(ready_response.data) ? ready_response.data : error_to_string(ready_response.error_code)));
        close_socket(ss_socket);
        return;
    }
    
    printf("%s\n", ready_response.data);
    
    // Interactive mode
    printf("Enter: <word_index> <content>\n");
    printf("Type 'ETIRW' on a line by itself to finish\n\n");
    
    while (1) {
        char input[1024];
        printf("> ");
        if (!fgets(input, sizeof(input), stdin)) {
            Message finish_msg = (Message){0};
            finish_msg.type = MSG_WRITE_REQUEST;
            strcpy(finish_msg.data, "ETIRW");
            send_message(ss_socket, &finish_msg);
            break;
        }
        input[strcspn(input, "\n")] = 0;
        if (strlen(input) == 0) continue;

        // Detect ETIRW possibly appended in the same line
        bool finish_now = false;
        char *etpos = strstr(input, "ETIRW");
        if (etpos) {
            finish_now = true;
            *etpos = '\0';
            // trim spaces at end
            while (etpos > input && isspace((unsigned char)*(etpos-1))) { *(--etpos)='\0'; }
        }

        // Parse possibly multiple pairs: <word_index> <content> [<word_index> <content>] ...
        char buf[1024];
        strncpy(buf, input, sizeof(buf)-1); buf[sizeof(buf)-1] = '\0';
        char *tok = strtok(buf, " ");
        bool have_idx = false; int cur_idx = -1; char cur_content[1024]; cur_content[0] = '\0';

        while (tok) {
            if (strcmp(tok, "ETIRW") == 0) { finish_now = true; break; }
            if (is_int_str(tok)) {
                // flush previous pair if any
                if (have_idx && strlen(cur_content) > 0) {
                    Message update_msg = {0};
                    update_msg.type = MSG_WRITE_REQUEST;
                    update_msg.arg1 = cur_idx;
                    strncpy(update_msg.data, cur_content, sizeof(update_msg.data)-1);
                    send_message(ss_socket, &update_msg);
                    cur_content[0] = '\0';
                }
                cur_idx = atoi(tok);
                have_idx = true;
            } else {
                if (have_idx) {
                    if (cur_content[0]) strncat(cur_content, " ", sizeof(cur_content)-strlen(cur_content)-1);
                    strncat(cur_content, tok, sizeof(cur_content)-strlen(cur_content)-1);
                } else {
                    // ignore stray content before first index
                }
            }
            tok = strtok(NULL, " ");
        }

        // Flush last pending pair
        if (have_idx && strlen(cur_content) > 0) {
            Message update_msg = {0};
            update_msg.type = MSG_WRITE_REQUEST;
            update_msg.arg1 = cur_idx;
            strncpy(update_msg.data, cur_content, sizeof(update_msg.data)-1);
            send_message(ss_socket, &update_msg);
        } else if (!have_idx && strlen(input) > 0) {
            printf("Invalid format. Use: <word_index> <content>\n");
        }

        if (finish_now) {
            Message finish_msg = {0};
            finish_msg.type = MSG_WRITE_REQUEST;
            strcpy(finish_msg.data, "ETIRW");
            send_message(ss_socket, &finish_msg);
            break;
        }
    }
    
    // Get final response
    Message final_response;
    recv_message(ss_socket, &final_response);
    printf("%s\n", final_response.data);
    
    close_socket(ss_socket);
}

void execute_delete(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: DELETE <filename>\n");
        return;
    }
    
    Message msg = {0};
    msg.type = MSG_DELETE_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s\n", response.data);
    }
}

void execute_info(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: INFO <filename>\n");
        return;
    }
    
    Message msg = {0};
    msg.type = MSG_INFO_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s", response.data);
    }
}

void execute_list(Command* cmd) {
    Message msg = {0};
    msg.type = MSG_LIST_USERS;
    strcpy(msg.sender, username);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    printf("%s", response.data);
}

void execute_addaccess(Command* cmd) {
    if (cmd->arg_count < 2) {
        printf("Usage: ADDACCESS -R|-W <filename> <username>\n");
        return;
    }
    if (!cmd->flag_r && !cmd->flag_w) {
        printf("Usage: ADDACCESS -R|-W <filename> <username>\n");
        return;
    }
    
    Message msg = (Message){0};
    msg.type = MSG_ADDACCESS;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    strcpy(msg.data, cmd->args[1]);
    // Prefer WRITE if both flags present
    msg.arg1 = cmd->flag_w ? 2 : 1; // 1=READ, 2=WRITE
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s\n", response.data);
    }
}

void execute_remaccess(Command* cmd) {
    if (cmd->arg_count < 2) {
        printf("Usage: REMACCESS <filename> <username>\n");
        return;
    }
    
    Message msg = {0};
    msg.type = MSG_REMACCESS;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    strcpy(msg.data, cmd->args[1]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
    } else {
        printf("%s\n", response.data);
    }
}

void execute_undo(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: UNDO <filename>\n");
        return;
    }
    
    // Get SS info from NM
    Message msg = {0};
    msg.type = MSG_UNDO_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
        return;
    }
    
    // Connect to SS
    char* ss_ip = response.data;
    int ss_port = response.arg1;
    
    int ss_socket = connect_to_server(ss_ip, ss_port);
    if (ss_socket < 0) {
        printf("ERROR: Failed to connect to storage server\n");
        return;
    }
    
    // Request undo
    Message undo_msg = {0};
    undo_msg.type = MSG_UNDO_FILE;
    strcpy(undo_msg.sender, username);
    strcpy(undo_msg.filename, cmd->args[0]);
    
    send_message(ss_socket, &undo_msg);
    
    Message undo_response;
    recv_message(ss_socket, &undo_response);
    
    if (undo_response.type == MSG_ERROR) {
        printf("%s\n", (strlen(undo_response.data) ? undo_response.data : error_to_string(undo_response.error_code)));
    } else {
        printf("%s\n", undo_response.data);
    }
    
    close_socket(ss_socket);
}

void execute_stream(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: STREAM <filename>\n");
        return;
    }
    
    // Get SS info from NM
    Message msg = {0};
    msg.type = MSG_STREAM_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    
    send_message(nm_socket, &msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type == MSG_ERROR) {
        printf("%s\n", (strlen(response.data) ? response.data : error_to_string(response.error_code)));
        return;
    }
    
    // Connect to SS
    char* ss_ip = response.data;
    int ss_port = response.arg1;
    
    int ss_socket = connect_to_server(ss_ip, ss_port);
    if (ss_socket < 0) {
        printf("ERROR: Failed to connect to storage server\n");
        return;
    }
    
    // Request stream
    Message stream_msg = {0};
    stream_msg.type = MSG_STREAM_REQUEST;
    strcpy(stream_msg.sender, username);
    strcpy(stream_msg.filename, cmd->args[0]);
    
    send_message(ss_socket, &stream_msg);
    
    // Receive words
    while (1) {
        Message word_msg;
        int received = recv_message(ss_socket, &word_msg);
        
        if (received <= 0) {
            // SS disconnected mid-stream
            printf("\nERROR: Storage Server disconnected during streaming\n");
            break;
        }
        
        if (word_msg.type == MSG_ERROR) {
            // Propagate server-side error if any
            printf("\n%s\n", (strlen(word_msg.data) ? word_msg.data : error_to_string(word_msg.error_code)));
            break;
        }
        
        if (strcmp(word_msg.data, "END_OF_FILE") == 0) {
            printf("\n");
            break;
        }
        
        printf("%s ", word_msg.data);
        fflush(stdout);
    }
    
    close_socket(ss_socket);
}

void execute_exec(Command* cmd) {
    if (cmd->arg_count < 1) {
        printf("Usage: EXEC <filename>\n");
        return;
    }

    Message msg = {0};
    msg.type = MSG_EXEC_FILE;
    strcpy(msg.sender, username);
    strcpy(msg.filename, cmd->args[0]);
    send_message(nm_socket, &msg);

    while (1) {
        Message resp;
        int received = recv_message(nm_socket, &resp);
        if (received <= 0) {
            printf("ERROR: Disconnected during EXEC\n");
            break;
        }
        if (resp.type == MSG_ERROR) {
            printf("%s\n", (strlen(resp.data) ? resp.data : error_to_string(resp.error_code)));
            break;
        }
        if (resp.type == MSG_EXEC_OUTPUT) {
            // print as we receive
            printf("%s", resp.data);
            fflush(stdout);
            continue;
        }
        if (resp.type == MSG_EXEC_DONE) {
            // End of exec
            break;
        }
        // Unexpected message, stop
        break;
    }
}

void execute_command(char* input) {
    Command cmd = parse_command(input);
    
    if (strlen(cmd.cmd) == 0) return;
    
    if (strcmp(cmd.cmd, "EXIT") == 0 || strcmp(cmd.cmd, "QUIT") == 0) {
        printf("👋 Goodbye!\n");
        close_socket(nm_socket);
        exit(0);
    }
    else if (strcmp(cmd.cmd, "VIEW") == 0) {
        execute_view(&cmd);
    }
    else if (strcmp(cmd.cmd, "CREATE") == 0) {
        execute_create(&cmd);
    }
    else if (strcmp(cmd.cmd, "READ") == 0) {
        execute_read(&cmd);
    }
    else if (strcmp(cmd.cmd, "WRITE") == 0) {
        execute_write(&cmd);
    }
    else if (strcmp(cmd.cmd, "DELETE") == 0) {
        execute_delete(&cmd);
    }
    else if (strcmp(cmd.cmd, "INFO") == 0) {
        execute_info(&cmd);
    }
    else if (strcmp(cmd.cmd, "LIST") == 0) {
        execute_list(&cmd);
    }
    else if (strcmp(cmd.cmd, "ADDACCESS") == 0) {
        execute_addaccess(&cmd);
    }
    else if (strcmp(cmd.cmd, "REMACCESS") == 0) {
        execute_remaccess(&cmd);
    }
    else if (strcmp(cmd.cmd, "UNDO") == 0) {
        execute_undo(&cmd);
    }
    else if (strcmp(cmd.cmd, "STREAM") == 0) {
        execute_stream(&cmd);
    }
    else if (strcmp(cmd.cmd, "EXEC") == 0) {
        execute_exec(&cmd);
    }
    else if (strcmp(cmd.cmd, "CREATEFOLDER") == 0) {
        if (cmd.arg_count < 1) { printf("Usage: CREATEFOLDER <foldername>\n"); return; }
        Message msg = {0};
        msg.type = MSG_CREATE_FOLDER;
        strcpy(msg.sender, username);
        strncpy(msg.data, cmd.args[0], sizeof(msg.data)-1);
        send_message(nm_socket, &msg);
        Message resp; recv_message(nm_socket, &resp);
        printf("%s\n", (strlen(resp.data) ? resp.data : (resp.type==MSG_ERROR? error_to_string(resp.error_code):"")));
    }
    else if (strcmp(cmd.cmd, "MOVE") == 0) {
        if (cmd.arg_count < 2) { printf("Usage: MOVE <filename> <foldername>\n"); return; }
        Message msg = {0};
        msg.type = MSG_MOVE_FILE;
        strcpy(msg.sender, username);
        strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1);
        strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1);
        send_message(nm_socket, &msg);
        Message resp; recv_message(nm_socket, &resp);
        printf("%s\n", (strlen(resp.data) ? resp.data : (resp.type==MSG_ERROR? error_to_string(resp.error_code):"")));
    }
    else if (strcmp(cmd.cmd, "VIEWFOLDER") == 0) {
        if (cmd.arg_count < 1) { printf("Usage: VIEWFOLDER <foldername>\n"); return; }
        Message msg = {0};
        msg.type = MSG_VIEW_FOLDER;
        strcpy(msg.sender, username);
        strncpy(msg.data, cmd.args[0], sizeof(msg.data)-1);
        send_message(nm_socket, &msg);
        Message resp; recv_message(nm_socket, &resp);
        printf("%s", resp.data);
    }
    else if (strcmp(cmd.cmd, "CHECKPOINT") == 0) {
        if (cmd.arg_count < 2) { printf("Usage: CHECKPOINT <filename> <tag>\n"); return; }
        // Ask NM for SS info
        Message msg = {0}; msg.type = MSG_CP_CREATE; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1);
        send_message(nm_socket, &msg); Message resp; recv_message(nm_socket, &resp);
        if (resp.type == MSG_ERROR) { printf("%s\n", resp.data); return; }
        int ss_port = resp.arg1; char ss_ip[MAX_IP_LEN]; strncpy(ss_ip, resp.data, sizeof(ss_ip)-1);
        int ss_socket = connect_to_server(ss_ip, ss_port); if (ss_socket < 0) { printf("ERROR: Failed to connect to storage server\n"); return; }
        Message cmsg = {0}; cmsg.type = MSG_CP_CREATE; strcpy(cmsg.sender, username); strncpy(cmsg.filename, cmd.args[0], sizeof(cmsg.filename)-1); strncpy(cmsg.data, cmd.args[1], sizeof(cmsg.data)-1); send_message(ss_socket, &cmsg);
        Message cresp; recv_message(ss_socket, &cresp); printf("%s\n", (strlen(cresp.data)?cresp.data:(cresp.type==MSG_ERROR?error_to_string(cresp.error_code):""))); close_socket(ss_socket);
    }
    else if (strcmp(cmd.cmd, "VIEWCHECKPOINT") == 0) {
        if (cmd.arg_count < 2) { printf("Usage: VIEWCHECKPOINT <filename> <tag>\n"); return; }
        Message msg = {0}; msg.type = MSG_CP_VIEW; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1);
        send_message(nm_socket, &msg); Message resp; recv_message(nm_socket, &resp); if (resp.type == MSG_ERROR) { printf("%s\n", resp.data); return; }
        int ss_port = resp.arg1; char ss_ip[MAX_IP_LEN]; strncpy(ss_ip, resp.data, sizeof(ss_ip)-1);
        int ss_socket = connect_to_server(ss_ip, ss_port); if (ss_socket < 0) { printf("ERROR: Failed to connect to storage server\n"); return; }
        Message vmsg = {0}; vmsg.type = MSG_CP_VIEW; strcpy(vmsg.sender, username); strncpy(vmsg.filename, cmd.args[0], sizeof(vmsg.filename)-1); strncpy(vmsg.data, cmd.args[1], sizeof(vmsg.data)-1); send_message(ss_socket, &vmsg);
        Message vresp; recv_message(ss_socket, &vresp); if (vresp.type == MSG_ERROR) printf("%s\n", vresp.data); else printf("%s\n", vresp.data); close_socket(ss_socket);
    }
    else if (strcmp(cmd.cmd, "REVERT") == 0) {
        if (cmd.arg_count < 2) { printf("Usage: REVERT <filename> <tag>\n"); return; }
        Message msg = {0}; msg.type = MSG_CP_REVERT; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1);
        send_message(nm_socket, &msg); Message resp; recv_message(nm_socket, &resp); if (resp.type == MSG_ERROR) { printf("%s\n", resp.data); return; }
        int ss_port = resp.arg1; char ss_ip[MAX_IP_LEN]; strncpy(ss_ip, resp.data, sizeof(ss_ip)-1);
        int ss_socket = connect_to_server(ss_ip, ss_port); if (ss_socket < 0) { printf("ERROR: Failed to connect to storage server\n"); return; }
        Message rmsg = {0}; rmsg.type = MSG_CP_REVERT; strcpy(rmsg.sender, username); strncpy(rmsg.filename, cmd.args[0], sizeof(rmsg.filename)-1); strncpy(rmsg.data, cmd.args[1], sizeof(rmsg.data)-1); send_message(ss_socket, &rmsg);
        Message rresp; recv_message(ss_socket, &rresp); printf("%s\n", (strlen(rresp.data)?rresp.data:(rresp.type==MSG_ERROR?error_to_string(rresp.error_code):""))); close_socket(ss_socket);
    }
    else if (strcmp(cmd.cmd, "LISTCHECKPOINTS") == 0) {
        if (cmd.arg_count < 1) { printf("Usage: LISTCHECKPOINTS <filename>\n"); return; }
        Message msg = {0}; msg.type = MSG_CP_LIST; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1);
        send_message(nm_socket, &msg); Message resp; recv_message(nm_socket, &resp); if (resp.type == MSG_ERROR) { printf("%s\n", resp.data); return; }
        int ss_port = resp.arg1; char ss_ip[MAX_IP_LEN]; strncpy(ss_ip, resp.data, sizeof(ss_ip)-1);
        int ss_socket = connect_to_server(ss_ip, ss_port); if (ss_socket < 0) { printf("ERROR: Failed to connect to storage server\n"); return; }
        Message lmsg = {0}; lmsg.type = MSG_CP_LIST; strcpy(lmsg.sender, username); strncpy(lmsg.filename, cmd.args[0], sizeof(lmsg.filename)-1); send_message(ss_socket, &lmsg);
        Message lresp; recv_message(ss_socket, &lresp); printf("%s", lresp.data); close_socket(ss_socket);
    }
    else if (strcmp(cmd.cmd, "REQUESTACCESS") == 0) {
        if (cmd.arg_count < 2 || (!cmd.flag_r && !cmd.flag_w)) { printf("Usage: REQUESTACCESS <filename> -R|-W\n"); return; }
        Message msg={0}; msg.type=MSG_REQUEST_ACCESS; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); msg.arg1 = cmd.flag_w?2:1; // desired access
        send_message(nm_socket, &msg); Message resp; recv_message(nm_socket, &resp);
        printf("%s\n", (strlen(resp.data)?resp.data:(resp.type==MSG_ERROR? error_to_string(resp.error_code):"")) );
    }
    else if (strcmp(cmd.cmd, "VIEWREQUESTS") == 0) {
        Message msg={0}; msg.type=MSG_VIEW_REQUESTS; strcpy(msg.sender, username); send_message(nm_socket,&msg); Message resp; recv_message(nm_socket,&resp);
        if (resp.type==MSG_ERROR) printf("%s\n", (strlen(resp.data)?resp.data:error_to_string(resp.error_code))); else printf("%s", resp.data);
    }
    else if (strcmp(cmd.cmd, "APPROVEREQUEST") == 0) {
        if (cmd.arg_count < 2 || (!cmd.flag_r && !cmd.flag_w)) { printf("Usage: APPROVEREQUEST <filename> <user> -R|-W\n"); return; }
        Message msg={0}; msg.type=MSG_APPROVE_REQUEST; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1); msg.arg1 = cmd.flag_w?2:1; send_message(nm_socket,&msg); Message resp; recv_message(nm_socket,&resp);
        printf("%s\n", (strlen(resp.data)?resp.data:(resp.type==MSG_ERROR? error_to_string(resp.error_code):"")) );
    }
    else if (strcmp(cmd.cmd, "DENYREQUEST") == 0) {
        if (cmd.arg_count < 2) { printf("Usage: DENYREQUEST <filename> <user>\n"); return; }
        Message msg={0}; msg.type=MSG_DENY_REQUEST; strcpy(msg.sender, username); strncpy(msg.filename, cmd.args[0], sizeof(msg.filename)-1); strncpy(msg.data, cmd.args[1], sizeof(msg.data)-1); send_message(nm_socket,&msg); Message resp; recv_message(nm_socket,&resp);
        printf("%s\n", (strlen(resp.data)?resp.data:(resp.type==MSG_ERROR? error_to_string(resp.error_code):"")) );
    }
    else if (strcmp(cmd.cmd, "HELP") == 0) {
        printf("\nAvailable Commands:\n");
        printf("  VIEW [-a] [-l]          - List files\n");
        printf("  CREATE <filename>       - Create new file\n");
        printf("  READ <filename>         - Read file content\n");
        printf("  WRITE <file> <sent#>    - Write to file\n");
        printf("  DELETE <filename>       - Delete file\n");
        printf("  INFO <filename>         - File information\n");
        printf("  LIST                    - List all users\n");
        printf("  ADDACCESS -R|-W <file> <user> - Add access\n");
        printf("  REMACCESS <file> <user> - Remove access\n");
        printf("  UNDO <filename>         - Undo last change\n");
        printf("  STREAM <filename>       - Stream file\n");
        printf("  CREATEFOLDER <name>     - Create a folder\n");
        printf("  MOVE <file> <folder>    - Move file to folder\n");
        printf("  VIEWFOLDER <name>       - List files in folder\n");
    printf("  CHECKPOINT <file> <tag> - Create checkpoint\n");
    printf("  VIEWCHECKPOINT <file> <tag> - View checkpoint content\n");
    printf("  REVERT <file> <tag>     - Revert to checkpoint\n");
    printf("  LISTCHECKPOINTS <file>  - List checkpoints\n");
    printf("  REQUESTACCESS <file> -R|-W        - Request access\n");
    printf("  VIEWREQUESTS                   - View your pending incoming requests (as owner)\n");
    printf("  APPROVEREQUEST <file> <user> -R|-W - Approve request\n");
    printf("  DENYREQUEST <file> <user>      - Deny request\n");
        printf("  EXIT                    - Exit client\n\n");
    }
    else {
        printf("Unknown command: %s\n", cmd.cmd);
        printf("Type HELP for available commands\n");
    }
}

static bool is_int_str(const char* s) {
    if (!s || !*s) return false;
    for (const char* p = s; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

int main(int argc, char* argv[]) {
    char nm_ip[MAX_IP_LEN] = ""; // do not hardcode
    int nm_port = 8080;
    
    if (argc >= 3) {
        strncpy(nm_ip, argv[1], sizeof(nm_ip)-1);
        nm_port = atoi(argv[2]);
    } else {
        // If not provided, ask interactively to support multi-host setups
        printf("Enter Naming Server IP (e.g., 192.168.1.10): ");
        if (!fgets(nm_ip, sizeof(nm_ip), stdin)) return 1;
        nm_ip[strcspn(nm_ip, "\n")] = 0;
        if (!*nm_ip) strcpy(nm_ip, "127.0.0.1");
        char portbuf[16]={0};
        printf("Enter Naming Server Port [8080]: ");
        if (fgets(portbuf, sizeof(portbuf), stdin)) {
            int p = atoi(portbuf); if (p>0) nm_port = p;
        }
    }
    
    printf("🚀 NFS Client Starting...\n\n");
    
    // Get username
    printf("Enter username: ");
    fgets(username, sizeof(username), stdin);
    username[strcspn(username, "\n")] = 0;
    
    if (!is_valid_username(username)) {
        printf("❌ Invalid username\n");
        return 1;
    }
    
    // Connect to NM
    printf("Connecting to Naming Server at %s:%d...\n", nm_ip, nm_port);
    nm_socket = connect_to_server(nm_ip, nm_port);
    
    if (nm_socket < 0) {
        printf("❌ Failed to connect to Naming Server\n");
        return 1;
    }
    
    printf("✅ Connected!\n\n");
    
    // Register
    Message reg_msg = {0};
    reg_msg.type = MSG_CLIENT_REGISTER;
    strcpy(reg_msg.sender, username);
    // Include client IP and ports as per spec
    char local_ip[MAX_IP_LEN] = {0};
    get_local_ip(local_ip, sizeof(local_ip));
    strncpy(reg_msg.data, local_ip, sizeof(reg_msg.data) - 1);
    reg_msg.arg1 = nm_port; // NM port client connected to
    reg_msg.arg2 = 0;       // Optional: last-used SS port (unknown at register time)
    
    send_message(nm_socket, &reg_msg);
    
    Message response;
    recv_message(nm_socket, &response);
    
    if (response.type != MSG_ACK) {
        printf("❌ Registration failed\n");
        return 1;
    }
    
    printf("✅ Registered as '%s'\n", username);
    printf("Type HELP for available commands\n\n");
    
    // Interactive loop
    while (1) {
        printf("%s> ", username);
        
        char input[1024];
        if (!fgets(input, sizeof(input), stdin)) {
            break;
        }
        
        input[strcspn(input, "\n")] = 0;
        
        if (strlen(input) == 0) continue;
        
        execute_command(input);
        printf("\n");
    }
    
    close_socket(nm_socket);
    return 0;
}
