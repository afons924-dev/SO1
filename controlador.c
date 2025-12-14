#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <ctype.h>

#define FIFO_PRINCIPAL "controlador_fifo"
#define MAX_UTILIZADORES 30
#define MAX_VIAGENS 100
#define MAX_VEICULOS_DEFAULT 10
#define BUFFER_SIZE 1024
#define LOCK_FILE "/tmp/controlador.lock"

typedef enum { AGENDADA, EM_CURSO, CONCLUIDA, CANCELADA } EstadoViagem;
typedef struct { char username[50]; char fifo_nome[100]; int fd_fifo; int em_viagem; } Utilizador;
typedef struct { int id; char username_cliente[50]; int hora_inicio; char local_partida[100]; int distancia; EstadoViagem estado; pid_t pid_veiculo; int fd_telemetria; int progresso; } Viagem;

Utilizador lista_utilizadores[MAX_UTILIZADORES]; Viagem lista_viagens[MAX_VIAGENS];
int num_utilizadores = 0, num_viagens = 0, proximo_id_viagem = 1, tempo_simulado = 0;
int nveiculos_max = 0, veiculos_em_servico = 0; long long total_kms = 0; int terminar_flag = 0;

void inicializar_controlador(); void loop_principal(); void terminar_sistema();
void processar_comandos_admin(char* cmd); void processar_comandos_clientes(char* cmd);
void lancar_veiculo(Viagem *v); void adicionar_utilizador(char* username, char* fifo_nome);
void remover_utilizador(char* username); void agendar_viagem(char* username, int hora, char* local, int dist);
void consultar_viagens(char* username); void cancelar_viagem(int id_viagem, char* requisitante);
Utilizador* encontrar_utilizador(char* username); Viagem* encontrar_viagem(int id);
void trim(char *str);

int main() {
    inicializar_controlador();
    loop_principal();
    terminar_sistema();
    return 0;
}

void handle_sigint(int sig) {
    (void)sig;
    terminar_flag = 1;
}

void inicializar_controlador() {
    int lock_fd = open(LOCK_FILE, O_CREAT | O_EXCL, 0666);
    if (lock_fd == -1) {
        if (errno == EEXIST) printf("Erro: Outra instancia do controlador ja esta em execucao.\n");
        else perror("Erro ao criar lock file");
        exit(1);
    }
    close(lock_fd);

    // Configura o handler para SIGINT (Ctrl+C)
    signal(SIGINT, handle_sigint);

    printf("A iniciar o controlador...\n");
    char *nveiculos_env = getenv("NVEICULOS");
    nveiculos_max = nveiculos_env ? atoi(nveiculos_env) : MAX_VEICULOS_DEFAULT;
    printf("Frota: %d veículos.\n", nveiculos_max);
    signal(SIGPIPE, SIG_IGN);
    unlink(FIFO_PRINCIPAL);
    if (mkfifo(FIFO_PRINCIPAL, 0666) == -1) { perror("mkfifo"); exit(1); }
}

void loop_principal() {
    int fd_fifo_principal = open(FIFO_PRINCIPAL, O_RDONLY | O_NONBLOCK);
    open(FIFO_PRINCIPAL, O_WRONLY); fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    printf("Controlador operacional.\n> "); fflush(stdout);
    time_t ultima_atualizacao = 0;
    char admin_buffer[BUFFER_SIZE], cliente_buffer[BUFFER_SIZE];
    while (!terminar_flag) {
        memset(admin_buffer, 0, BUFFER_SIZE);
        if (read(STDIN_FILENO, admin_buffer, sizeof(admin_buffer)-1) > 0) processar_comandos_admin(admin_buffer);
        memset(cliente_buffer, 0, BUFFER_SIZE);
        if (read(fd_fifo_principal, cliente_buffer, sizeof(cliente_buffer)-1) > 0) processar_comandos_clientes(cliente_buffer);
        for (int i = 0; i < num_viagens; i++) {
            if (lista_viagens[i].estado == EM_CURSO && lista_viagens[i].fd_telemetria > 0) {
                 char t_buf[256]; memset(t_buf, 0, sizeof(t_buf)); int n = read(lista_viagens[i].fd_telemetria, t_buf, sizeof(t_buf)-1);
                 if (n > 0) {
                    t_buf[n] = '\0';
                    char* token = strtok(t_buf, "\n");
                    while(token != NULL) {
                        int id; char tipo[50];
                        if (sscanf(token, "%d %s", &id, tipo) >= 2) {
                             Viagem* v = encontrar_viagem(id);
                             if (v) {
                                if (strcmp(tipo, "PROGRESSO")==0){ sscanf(token, "%*d %*s %d", &v->progresso); }
                                else if (strcmp(tipo, "INICIOU")==0) { printf("\n[T] Veiculo %d iniciou.\n> ", id); fflush(stdout); }
                                else if (strcmp(tipo, "CLIENTE_ENTROU")==0) { printf("\n[T] Cliente entrou no veiculo %d.\n> ", id); fflush(stdout); }
                                else if (strcmp(tipo, "CONCLUIDA")==0 || strcmp(tipo, "CANCELADA")==0) {
                                    v->estado = (strcmp(tipo, "CONCLUIDA")==0) ? CONCLUIDA : CANCELADA;
                                    if(v->estado == CONCLUIDA) total_kms += v->distancia;
                                    veiculos_em_servico--; Utilizador* u = encontrar_utilizador(v->username_cliente);
                                    if (u) u->em_viagem = 0;
                                    close(v->fd_telemetria); v->fd_telemetria = -1;
                                    waitpid(v->pid_veiculo, NULL, 0);
                                    printf("\nViagem %d %s.\n> ", id, tipo); fflush(stdout);
                                } else if (strcmp(tipo, "ERRO")==0) {
                                    v->estado = CANCELADA;
                                    veiculos_em_servico--; Utilizador* u = encontrar_utilizador(v->username_cliente);
                                    if (u) u->em_viagem = 0;
                                    close(v->fd_telemetria); v->fd_telemetria = -1;
                                    waitpid(v->pid_veiculo, NULL, 0);
                                    printf("\nViagem %d CANCELADA (Erro no veiculo).\n> ", id); fflush(stdout);
                                }
                             }
                        }
                        token = strtok(NULL, "\n");
                    }
                 }
            }
        }
        if (time(NULL) > ultima_atualizacao) {
            ultima_atualizacao = time(NULL); tempo_simulado++;
            for (int i = 0; i < num_viagens; i++) {
                if (lista_viagens[i].estado == AGENDADA && lista_viagens[i].hora_inicio <= tempo_simulado) {
                    if (veiculos_em_servico < nveiculos_max) lancar_veiculo(&lista_viagens[i]);
                }
            }
        }
        usleep(100000);
    }
}

void processar_comandos_admin(char* cmd) {
    trim(cmd);
    printf("\n[ADMIN] %s\n", cmd); int id;
    if (strcmp(cmd, "help") == 0) {
        printf("--- Comandos do Administrador ---\n");
        printf("  listar         - Mostra a informacao de todos os servicos agendados/em curso.\n");
        printf("  utiliz         - Mostra a lista dos utilizadores atualmente ligados.\n");
        printf("  frota          - Mostra o progresso da viagem de cada veiculo em servico.\n");
        printf("  cancelar <id>  - Cancela um servico (se id for 0, cancela todos).\n");
        printf("  km             - Mostra o numero total de quilometros percorridos por todos os veiculos.\n");
        printf("  hora           - Mostra o valor atual do tempo simulado.\n");
        printf("  terminar       - Termina a execucao de todo o sistema.\n");
        printf("  help           - Mostra esta ajuda.\n");
    } else if (strcmp(cmd, "terminar") == 0) terminar_flag = 1;
    else if (strcmp(cmd, "hora") == 0) printf("Tempo simulado: %d\n", tempo_simulado);
    else if (strcmp(cmd, "listar") == 0) {
        printf("--- Viagens ---\n");
        for (int i=0; i<num_viagens; i++) printf("ID:%d U:%s H:%d E:%d P:%d%%\n", lista_viagens[i].id, lista_viagens[i].username_cliente, lista_viagens[i].hora_inicio, lista_viagens[i].estado, lista_viagens[i].progresso);
    } else if (strcmp(cmd, "utiliz") == 0) {
        printf("--- Utilizadores ---\n");
        for (int i=0; i<num_utilizadores; i++) printf("U:%s E:%s\n", lista_utilizadores[i].username, lista_utilizadores[i].em_viagem ? "Viagem" : "Espera");
    } else if (strcmp(cmd, "km") == 0) printf("KMs: %lld\n", total_kms);
    else if (sscanf(cmd, "cancelar %d", &id) == 1) cancelar_viagem(id, "admin");
    else if (strcmp(cmd, "frota") == 0) {
        printf("--- Frota (%d/%d) ---\n", veiculos_em_servico, nveiculos_max);
        for(int i=0; i<num_viagens; i++) if(lista_viagens[i].estado == EM_CURSO) printf("ID:%d P:%d%%\n", lista_viagens[i].id, lista_viagens[i].progresso);
    } else {
        printf("Comando de administrador desconhecido. Use 'help' para ver a lista.\n");
    }
    printf("> "); fflush(stdout);
}

void processar_comandos_clientes(char* cmd) {
    trim(cmd);
    printf("\n[CLIENTE] %s\n", cmd);
    char* cmd_copy = strdup(cmd);
    char* token = strtok(cmd_copy, " ");
    if (!token) { free(cmd_copy); return; }

    // Comando LOGIN é um caso especial
    if (strcmp(token, "LOGIN") == 0) {
        char *user = strtok(NULL, " "); char *fifo = strtok(NULL, " ");
        if (user && fifo) adicionar_utilizador(user, fifo);
        free(cmd_copy);
        return;
    }

    // Para os outros comandos, o primeiro token é o username
    char username[50];
    strncpy(username, token, sizeof(username) - 1);
    username[sizeof(username)-1] = '\0';

    char* tipo_cmd = strtok(NULL, " ");
    if (!tipo_cmd) { // Comandos de uma só palavra (consultar, terminar)
        // O cliente envia "pedro consultar", logo o token original 'cmd' tem o tipo de cmd
        char* original_cmd = strchr(cmd, ' ');
        if (original_cmd) {
            original_cmd++; // Pula o espaço
            if(strcmp(original_cmd, "consultar") == 0) consultar_viagens(username);
            else if(strcmp(original_cmd, "terminar") == 0) remover_utilizador(username);
        }
    } else { // Comandos com argumentos
        if (strcmp(tipo_cmd, "agendar") == 0) {
            char* hora_str = strtok(NULL, " ");
            if (hora_str) {
                int hora = atoi(hora_str);
                char* resto = hora_str + strlen(hora_str) + 1;
                char* p_dist = strrchr(resto, ' ');
                if (p_dist) {
                    int dist = atoi(p_dist + 1);
                    *p_dist = '\0';
                    char* local = resto;
                    while(isspace((unsigned char)*local)) local++;
                    agendar_viagem(username, hora, local, dist);
                }
            }
        } else if (strcmp(tipo_cmd, "cancelar") == 0) {
            char* id_str = strtok(NULL, " ");
            if (id_str) cancelar_viagem(atoi(id_str), username);
        }
    }

    free(cmd_copy);
    printf("> "); fflush(stdout);
}

void adicionar_utilizador(char* username, char* fifo_nome) {
    if (num_utilizadores >= MAX_UTILIZADORES) {
        int fd_temp = open(fifo_nome, O_WRONLY);
        if (fd_temp != -1) { write(fd_temp, "Erro: Limite de utilizadores atingido.", 38); close(fd_temp); }
        return;
    }
    if (encontrar_utilizador(username) != NULL) {
        // Verifica se o user existente está "vivo"
        Utilizador* u_existente = encontrar_utilizador(username);
        // Tenta escrever um espaço para ver se dá EPIPE (write de 0 bytes não deteta erro)
        // Nota: signal(SIGPIPE, SIG_IGN) foi definido em inicializar_controlador()
        if (write(u_existente->fd_fifo, " ", 1) == -1 && errno == EPIPE) {
            // Cliente antigo morreu. Vamos removê-lo silenciosamente para permitir o novo login.
            printf("Detetada sessao morta para '%s'. A limpar...\n", username);
            close(u_existente->fd_fifo);
            // Reutiliza o slot removendo
            // (Chamar remover_utilizador tem side effects de print e cancelamento de viagens,
            //  talvez seja melhor fazer aqui a limpeza especifica ou chamar remover e lidar com prints)
            remover_utilizador(username);
            // Agora prossegue para adicionar o novo.
        } else {
            // Cliente está vivo. Rejeita novo login.
            int fd_temp = open(fifo_nome, O_WRONLY);
            if (fd_temp != -1) { write(fd_temp, "Erro: Username ja esta em uso.", 30); close(fd_temp); }
            return;
        }
    }
    Utilizador* u = &lista_utilizadores[num_utilizadores];
    strcpy(u->username, username); strcpy(u->fifo_nome, fifo_nome); u->em_viagem = 0;
    u->fd_fifo = open(fifo_nome, O_WRONLY); if (u->fd_fifo == -1) return;
    num_utilizadores++; printf("User '%s' adicionado.\n", username);
    write(u->fd_fifo, "Login bem-sucedido!", 19);
}

void remover_utilizador(char* username) {
    Utilizador* u = encontrar_utilizador(username); if (!u) return;
    if (u->em_viagem) {
        write(u->fd_fifo, "Erro: Nao pode sair enquanto estiver em viagem.", 46);
        return;
    }
    for (int i=0; i<num_viagens; i++) {
        if (strcmp(lista_viagens[i].username_cliente, username) == 0 && lista_viagens[i].estado == AGENDADA) {
            lista_viagens[i].estado = CANCELADA;
        }
    }
    printf("Viagens agendadas para '%s' canceladas.\n", username);
    int idx = u - lista_utilizadores;
    close(u->fd_fifo);
    for(int i=idx; i<num_utilizadores-1; i++) lista_utilizadores[i] = lista_utilizadores[i+1];
    num_utilizadores--;
    printf("User '%s' removido.\n", username);
}

void agendar_viagem(char* username, int hora, char* local, int dist) {
    Utilizador* u = encontrar_utilizador(username); if (!u) return;
    if (hora < tempo_simulado) {
        write(u->fd_fifo, "Erro: Nao pode agendar viagens para o passado.", 46);
        return;
    }
    if (dist <= 0) {
        write(u->fd_fifo, "Erro: A distancia deve ser um numero positivo.", 46);
        return;
    }

    // Verificar duplicados (mesmo user, mesma hora)
    for (int i=0; i<num_viagens; i++) {
        if (strcmp(lista_viagens[i].username_cliente, username) == 0 &&
            (lista_viagens[i].estado == AGENDADA || lista_viagens[i].estado == EM_CURSO)) {
            if (lista_viagens[i].hora_inicio == hora) {
                 write(u->fd_fifo, "Erro: Ja tem viagem agendada para essa hora.", 44);
                 return;
            }
        }
    }

    if (num_viagens >= MAX_VIAGENS) {
        write(u->fd_fifo, "Erro: Sistema de agendamento cheio.", 35);
        return;
    }
    Viagem* v = &lista_viagens[num_viagens];
    v->id = proximo_id_viagem++; strcpy(v->username_cliente, username); v->hora_inicio = hora;
    strncpy(v->local_partida, local, sizeof(v->local_partida) - 1);
    v->local_partida[sizeof(v->local_partida) - 1] = '\0';
    v->distancia = dist; v->estado = AGENDADA; v->progresso = 0;
    num_viagens++; printf("Viagem %d agendada para %s.\n", v->id, username);
    char msg[100]; snprintf(msg, 100, "Viagem agendada com ID %d", v->id);
    write(u->fd_fifo, msg, strlen(msg));
}

void consultar_viagens(char* username) {
    Utilizador* u = encontrar_utilizador(username); if (!u) return;
    char buffer[BUFFER_SIZE]; int len = 0;
    len += snprintf(buffer + len, BUFFER_SIZE - len, "--- As suas viagens ---\n");
    int encontrou = 0;
    for(int i=0; i<num_viagens; i++) {
        if(strcmp(lista_viagens[i].username_cliente, username)==0) {
            encontrou = 1;
            len += snprintf(buffer + len, BUFFER_SIZE - len, "ID:%d H:%d E:%d P:%d%%\n", lista_viagens[i].id, lista_viagens[i].hora_inicio, lista_viagens[i].estado, lista_viagens[i].progresso);
            if (len >= BUFFER_SIZE) break;
        }
    }
    if (!encontrou) len += snprintf(buffer + len, BUFFER_SIZE - len, "Nao tem viagens agendadas.\n");
    write(u->fd_fifo, buffer, len);
}

void cancelar_viagem(int id_viagem, char* requisitante) {
    if (id_viagem == 0) {
        for(int i=0; i<num_viagens; i++) {
            if (strcmp(requisitante, "admin") == 0 || strcmp(lista_viagens[i].username_cliente, requisitante) == 0) {
                 if(lista_viagens[i].estado == AGENDADA || lista_viagens[i].estado == EM_CURSO)
                    cancelar_viagem(lista_viagens[i].id, requisitante);
            }
        }
        return;
    }
    Viagem* v = encontrar_viagem(id_viagem); if (!v) return;
    if (strcmp(requisitante, "admin") != 0 && strcmp(requisitante, v->username_cliente) != 0) return;
    if (v->estado == AGENDADA) {
        v->estado = CANCELADA; printf("Viagem %d (agendada) cancelada.\n", id_viagem);
    } else if (v->estado == EM_CURSO) {
        kill(v->pid_veiculo, SIGUSR1);
        printf("Sinal de cancelamento enviado para veiculo da viagem %d.\n", id_viagem);
    }
}

void lancar_veiculo(Viagem *v) {
    int p[2]; pipe(p); pid_t pid = fork();
    if (pid == 0) {
        close(p[0]); dup2(p[1], STDOUT_FILENO); close(p[1]);
        char d[10], id[10], fifo[100];
        snprintf(d, 10, "%d", v->distancia); snprintf(id, 10, "%d", v->id);
        Utilizador* u = encontrar_utilizador(v->username_cliente);
        snprintf(fifo, 100, "%s", u ? u->fifo_nome : "");
        execl("./veiculo", "veiculo", d, fifo, id, NULL); exit(1);
    }
    close(p[1]); v->pid_veiculo = pid; v->estado = EM_CURSO;
    v->fd_telemetria = p[0]; fcntl(v->fd_telemetria, F_SETFL, O_NONBLOCK);
    veiculos_em_servico++; Utilizador* u = encontrar_utilizador(v->username_cliente);
    if (u) u->em_viagem = 1;
    printf("Veiculo para viagem %d (PID %d) lancado.\n", v->id, pid);
}

void terminar_sistema() {
    printf("\nA terminar...\n");
    for (int i=0; i<num_utilizadores; i++) {
        write(lista_utilizadores[i].fd_fifo, "Sistema a encerrar", 18);
        close(lista_utilizadores[i].fd_fifo);
    }
    for (int i=0; i<num_viagens; i++) if (lista_viagens[i].estado == EM_CURSO) kill(lista_viagens[i].pid_veiculo, SIGUSR1);
    unlink(FIFO_PRINCIPAL); unlink(LOCK_FILE);
    printf("Sistema terminado.\n");
}

Utilizador* encontrar_utilizador(char* username) {
    for (int i=0; i<num_utilizadores; i++) if (strcmp(lista_utilizadores[i].username, username)==0) return &lista_utilizadores[i];
    return NULL;
}

Viagem* encontrar_viagem(int id) {
    for (int i=0; i<num_viagens; i++) if (lista_viagens[i].id == id) return &lista_viagens[i];
    return NULL;
}

void trim(char *str) {
    char *start, *end;
    for (start = str; *start && isspace(*start); ++start);
    memmove(str, start, strlen(start) + 1);
    for (end = str + strlen(str) - 1; end >= str && isspace(*end); --end);
    *(end + 1) = '\0';
}
